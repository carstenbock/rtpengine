# Phase 1 Findings — Licensing-Safe EVS Bridging (EVS-IO re-framing)

**Scope:** Investigation only. No code was written. All claims are cited as
`file:line`. The repository under investigation is the `sipwise/rtpengine`
clone at `apps/rtpengine`.

**Goal recap (for context):** Provide a deployment mode in which the
patent-encumbered EVS codec DSP inside the reference `.so` is *never* invoked,
while still bridging EVS callers:

1. **EVS AMR-WB IO ⇄ AMR-WB / G.722** by pure RE-FRAMING (EVS-IO frames are
   bit-identical to AMR-WB frames). Onward bridging to G.722 reuses the
   existing AMR-WB decoder + G.722 encoder.
2. **Native EVS primary modes:** passthrough end-to-end only.
3. **Legacy EVS-primary→PCM→G.722 (via the `.so`)** kept behind an explicit
   opt-in flag, OFF by default.

---

## 0. TL;DR — answer to THE critical question

> *Does rtpengine's EVS PAYLOAD parsing depend on the EVS `.so`, or is it pure
> rtpengine C code?*

**It is pure rtpengine C code.** The entire EVS RTP payload parser —
compact-vs-header-full detection, 7-bit AMR-IO compact CMR, header-full CMR,
ToC iteration, AMR-WB-IO vs primary detection, per-mode bit/rate lookup,
SID/NO_DATA handling, and the compact-format bit de-shuffle — lives in
`evs_decoder_input()` and its lookup tables in `lib/codeclib.c`, and touches the
`.so` **nowhere**.

- Parser entry: `evs_decoder_input()` — `lib/codeclib.c:4813-4932`.
- The **only** place the parser reaches the `.so` is the per-frame call to
  `evs_push_frame()` (`lib/codeclib.c:4771-4811`), which runs the actual DSP
  decode (`evs_dec_in` / `evs_dec_out` / `evs_amr_dec_out`,
  `lib/codeclib.c:4786,4793,4795,4800,4802`). Skip that call and **no DSP runs**.
- All mode/rate/bit identification is table-driven pure C:
  `evs_mode_from_bytes()` (`lib/codeclib.c:4214-4266`),
  `evs_mode_bits[2][16]` (`lib/codeclib.c:4307-4346`),
  `evs_mode_bitrates[2][16]` (`lib/codeclib.c:4347-4386`).

**Conclusion: the licensing benefit stands.** EVS-IO frames can be identified
and extracted, and the CMR can be read, with the DSP `.so` absent.

A second, independent confirmation: the `.so` is *only* dlopen'd/dlsym'd in
`evs_load_so()` (`lib/codeclib.c:4935-4992`), and `evs_def_init()`
(`lib/codeclib.c:4994-5001`) only flips `support_encoding`/`support_decoding`
to 1 *if the handle loaded*. With `--evs-lib-path` absent, `evs_load_so()`
early-returns at `lib/codeclib.c:4936-4937` and the daemon does **not** die —
so payload parsing code is compiled and linked regardless of the `.so`.

---

## 1. Codec definitions and handler/codec-type wiring

### 1.1 `codec_def_t` table entries

- **EVS** — `lib/codeclib.c:578-602`:
  - `.avcodec_id = -1` (no libavcodec backing; DSP via `.so` only).
  - `.packetizer = packetizer_passthrough` (`:592`).
  - `.evs = 1` (`:594`), `.codec_type = &codec_type_evs` (`:596`).
  - SDP/fmtp callbacks: `evs_format_parse/cmp/print/answer`,
    `evs_select_encoder_format` (`:587-591`).
  - `default_fmtp = "dtx=0;dtx-recv=0"` (`:586`).
- **AMR** — `lib/codeclib.c:705-730`; **AMR-WB** — `lib/codeclib.c:731-756`:
  - AMR-WB: `.avcodec_id = AV_CODEC_ID_AMR_WB`,
    `.avcodec_name_enc = "libvo_amrwbenc"`,
    `.avcodec_name_dec = "libopencore_amrwb"` (`:733-735`).
  - `.packetizer = packetizer_amr` (`:744`), `.amr = 1` (`:750`),
    `.codec_type = &codec_type_amr` (`:747`).
  - `default_fmtp = "octet-align=1;mode-change-capability=2"` (`:743`).
  - `.set_enc_options = amr_set_enc_options`,
    `.set_dec_options = amr_set_dec_options` (`:748-749`).

So **AMR-WB encode/decode is libavcodec** (`libopencore_amrwb` decoder,
`libvo_amrwbenc` encoder) — completely independent of the EVS `.so`. This is the
"existing AMR-WB↔G.722 transcoder" the task refers to.

### 1.2 `codec_type_t` dispatch structs

- `codec_type_evs` — `lib/codeclib.c:283-292`: `decoder_input = evs_decoder_input`,
  `encoder_input = evs_encoder_input`, plus `evs_decoder_init/close`,
  `evs_encoder_init/close`.
- `codec_type_amr` — `lib/codeclib.c:273-282`: `decoder_input = amr_decoder_input`,
  but `encoder_init/input/close = avc_*` (libavcodec) and
  `encoder_got_packet = amr_encoder_got_packet`.

### 1.3 Decoder dispatch chain (where parsing → DSP happens)

`__decoder_input_data()` (`lib/codeclib.c:1257-1319`) calls
`dec->def->codec_type->decoder_input(dec, data, &frames)` at
`lib/codeclib.c:1292`, then resamples each produced `AVFrame` and hands it to the
caller's callback (`:1299-1313`). For EVS this is `evs_decoder_input`, which
produces PCM frames only by calling `evs_push_frame` → DSP.

Public entry points used by the daemon: `decoder_input_data()`
(`lib/codeclib.c:1320`), `decoder_input_data_ptime()` (`lib/codeclib.c:1327`).

---

## 2. EVS depacketizer — line-by-line, with `.so` touch annotations

`evs_decoder_input()` — `lib/codeclib.c:4813-4932`:

| Step | Lines | Touches `.so`? |
|------|-------|----------------|
| Compact single-frame detection via `evs_mode_from_bytes(input.len)` | `4824` | **No** (pure table) |
| Special clause A.2.1.3 (AMR HF with CMR, 1-byte ambiguity) | `4826-4832` | No |
| Compact: extract `bits`, `is_amr`, `q_bit`, `mode` from packed return | `4838-4842` | No |
| Compact **AMR-IO**: read & strip 3-bit CMR, map via `evs_amr_io_compact_cmr[]` | `4844-4852` | No |
| Compact AMR-IO: 2-bit payload de-shuffle + first-bit restore | `4854-4863` | No |
| Header-full: first byte is CMR (`*toc & 0x80`) vs ToC | `4866-4880` | No |
| Header-full: iterate ToC entries (F-bit `0x40`) | `4882-4890` | No |
| Per-frame loop: HF mode/`is_amr`/`q_bit`/`bits` via `evs_mode_bits[is_amr][mode]` | `4893-4921` | No |
| **DSP decode** of each frame | `evs_push_frame()` `4897` | **YES → `.so`** |
| Emit `CE_EVS_CMR_RECV` if CMR present | `4923-4924` | No (event only) |

`evs_push_frame()` — `lib/codeclib.c:4771-4811` — is the sole DSP site:
`evs_dec_in` (`:4786`), `evs_dec_out` / `evs_amr_dec_out` (`:4793,4795,4800,4802`),
`evs_dec_inc_frame` (`:4805`).

**Key data structures consumed by the parser (all pure C):**

- `evs_mode_from_bytes()` — `lib/codeclib.c:4214-4266`. Packs `mode |
  (bits<<8) | (is_amr<<24)`. Recognises EVS-primary lengths (7,18,20,24,33,41,
  61,80,120,160,240,320 + 6 = SID) and **AMR-IO** lengths
  (17,23,32,36,40,46,50,58,60 + 5 = SID), i.e. the 9 AMR-WB rates plus SID.
- `evs_mode_bits[2][16]` — `lib/codeclib.c:4307-4346`. Row 1 (AMR-IO):
  `132,177,253,285,317,365,397,461,477,40(SID)…` — **identical** to AMR-WB.
- `evs_mode_bitrates[2][16]` — `lib/codeclib.c:4347-4386`. Row 1 (AMR-IO):
  `6600,8850,12650,14250,15850,18250,19850,23050,23850` — the 9 AMR-WB rates.

### 2.1 EVS packetizer (encode side, for completeness)

The packetizing logic lives **inside `evs_encoder_input()`**
(`lib/codeclib.c:4618-4734`), not in a standalone packetizer (the codec_def uses
`packetizer_passthrough`). It builds compact vs header-full, optional CMR byte,
ToC byte, AMR-IO `0x30` ToC marker, and the AMR-IO compact 2-bit forward
shuffle (`:4701-4716`). It only runs **after** the DSP encode
(`evs_enc_in`/`evs_amr_enc_in` `:4631,4633`, `evs_enc_out` `:4684`). The
forward/reverse bit-shuffle pair and the CMR table `evs_amr_io_compact_cmr[]`
(`lib/codeclib.c:4738-4747`) are reusable, DSP-free building blocks for the new
re-frame path.

---

## 3. AMR / AMR-WB packetizer & depacketizer

### 3.1 Depacketizer — `amr_decoder_input()` `lib/codeclib.c:3130-3279`

- Pure `bitstr` bitstream parsing (`bitstr_init` `:3138`; `lib/bitstr.h`).
- **Octet-aligned vs bandwidth-efficient** controlled by
  `dec->format_options.amr.octet_aligned` (`:3165,3196,3245`).
- **CMR** = top 4 bits (`:3142-3163`); emits `CE_AMR_CMR_RECV` (`:3150,3160`).
- **ToC** loop with F-bit (`:3187-3215`); FT range/validity checks against
  `amr_bits_per_frame[ft]` (`:3200-3208`).
- Optional CRC skip (`:3217-3223`).
- Per frame: builds an **octet-aligned TOC byte + payload** buffer
  (`frame.s[0] = toc_byte & 0x7c`, `:3243`) and feeds it to
  `avc_decoder_input()` (libavcodec) at `:3255,3265`. SID (40 bits) routed to
  native or generated DTX (`:3252-3263`).
- `amr_bitrate_tracker()` (`:3078-3128`) may emit `CE_AMR_SEND_CMR` (`:3114`).

### 3.2 Packetizer — `packetizer_amr()` `lib/codeclib.c:3370-3447`

- Reads encoder output TOC byte (`pkt->data[0]`), FT (`:3382-3383`); drops
  NO_DATA/SPEECH_LOST FT≥14 (`:3388-3391`).
- Writes CMR (4 bits, `:3404`); optional outgoing CMR sequencing
  (`:3406-3418`).
- **Octet-aligned** branch: byte-copies TOC+payload (`:3420-3429`).
- **Bandwidth-efficient** branch: 10-bit (CMR+ToC) shift of the payload
  (`:3431-3444`).

### 3.3 AMR tables

`amr_wb_bitrates[]` (`:2880-2895`) and `amr_wb_bits_per_frame[]`
(`:2896-2911`) — the canonical AMR-WB 9-rate table (6.60–23.85 kbit/s) plus
40-bit SID. These match the EVS-IO tables in §2 bit-for-bit, which is the
mechanical basis for re-framing.

### 3.4 fmtp parsing — `amr_format_parse()` / `amr_parse_format_cb()`

`lib/codeclib.c:2912-2957`: handles `octet-align`, `crc`, `robust-sorting`,
`interleaving`, `mode-set`, `mode-change-period`, `mode-change-neighbor`.
Comparison/answer compatibility in `amr_format_cmp()` (`:3043-3076`) and
`amr_mode_set_cmp()` (`:3026-3042`).

---

## 4. CMR / event handling

Event enum — `lib/codeclib.h:256-260`:
`CE_AMR_CMR_RECV`, `CE_AMR_SEND_CMR`, `CE_EVS_CMR_RECV`.

- **Emitters (lib/codeclib.c):**
  - `CE_AMR_CMR_RECV` — `:3150` (received CMR), `:3160` (own mode-change tick).
  - `CE_AMR_SEND_CMR` — `:3114` (bitrate tracker wants a step-up).
  - `CE_EVS_CMR_RECV` — `:4924` (EVS payload carried a CMR).
- **Consumer:** `codec_decoder_event()` — `daemon/codec.c:3232-3257`. Writes
  into the *sink media's* `encoder_callback`:
  - `amr.cmr_in/cmr_in_ts` (`:3240-3241`), `amr.cmr_out/cmr_out_ts`
    (`:3245-3246`), `evs.cmr_in/cmr_in_ts` (`:3250-3251`).
  - Wired in via `dec->event_func = codec_decoder_event`
    (`daemon/codec.c:4426`) and `dec->event_data = h->media` (`:4425`).
- **Encoder-side use:**
  - AMR: `amr_encoder_mode_change()` (`lib/codeclib.c:3315-3365`),
    `packetizer_amr` outgoing CMR (`:3406-3418`).
  - EVS: `evs_handle_cmr()` (`lib/codeclib.c:4562-4616`) maps the 7-bit EVS-CMR
    (`type = cmr>>4 & 0x7`, `req = cmr & 0xf`, `:4572-4573`) onto a bitrate and
    re-arms the DSP encoder (`evs_set_encoder_brate`, `:4604-4605`).

`encoder_callback_s` layout — `lib/codeclib.h:133-146` (separate `amr` and
`evs` sub-structs). The CMR semantics differ: AMR CMR is a 4-bit mode index;
EVS-CMR is a 7-bit (type,value) tuple (TS 26.445 Annex A Table A.3). A mapping
table will be needed in Phase 2 (note `evs_amr_io_compact_cmr[]` at
`lib/codeclib.c:4738-4747` already encodes the AMR-IO compact CMR ↔ mode
relation).

---

## 5. Passthrough vs transcode decision (where a new path plugs in)

Master routine: `__codec_handlers_update()` — `daemon/codec.c:1435-1864`.
For each source codec it picks a sink codec then decides passthrough vs
transcode:

- Self-support gate: `codec_def_supported(pt->codec_def)`
  (`daemon/codec.c:1572`). `codec_def_supported()` =
  `support_encoding && support_decoding` (`lib/codeclib.h:517-520`). **For EVS
  this is false unless the `.so` loaded** (§1, `lib/codeclib.c:4994-5001`).
  When unsupported → `__make_passthrough_gsl()` (`:1577`).
- If codecs differ in PT or format → `goto transcode` (`:1735-1738`), which
  calls `__make_transcoder()` (`:1850`).
- Identical → `__make_passthrough_gsl()` (`:1807`).

Transcoder construction: `__make_transcoder` → `__make_transcoder_full`
(`daemon/codec.c:783-891`) → SSRC handler `__ssrc_handler_transcode_new`
(`:4432-4510`). **Hard `.so` dependency today:** `__ssrc_handler_transcode_new`
returns NULL if either side is not `codec_def_supported` (`:4435-4436`), and
`evs_decoder_init`/`evs_encoder_init` dereference DSP symbols
(`lib/codeclib.c:4188-4199`, `4436-4552`). So **a vanilla EVS↔AMR-WB transcode
cannot even be instantiated without the `.so`** — this is exactly the hard
requirement Phase 3 must avoid on the new path.

Data path of a running transcode (the DSP-bearing pipeline we must bypass):

```
handler_func_transcode            daemon/codec.c:5085
  -> packet_decode                daemon/codec.c:4899  (packet->packet_func)
    -> __rtp_decode_direct        daemon/codec.c:4847
      -> decoder_input_data_ptime daemon/codec.c:4867
        -> __decoder_input_data   lib/codeclib.c:1257
          -> decoder_input()      lib/codeclib.c:1292  (= evs_decoder_input)
            -> evs_push_frame()   lib/codeclib.c:4897  ***EVS DSP***
          -> callback packet_decoded_* (PCM AVFrame) lib/codeclib.c:1308
            -> packet_decoded_common  daemon/codec.c:4753
              -> encoder_input_*  -> packetizer (e.g. packetizer_amr / G.722)
```

(There is also an optional `libcodec-chain.so` fast path —
`codec_cc_new`/`cc_run`, `daemon/codec.c:4465-4480`, `lib/codeclib.c:5014-5029`
— a *separate* accelerated chain library, unrelated to the EVS reference `.so`
licensing question.)

### Where the new re-frame path plugs in

Two viable insertion points (to be chosen in Phase 2):

1. **Packet/handler level (preferred for EVS-IO → AMR-WB):** a dedicated
   `handler_func` (sibling of `handler_func_transcode`) that runs the
   *parser-only* portion of `evs_decoder_input` (`lib/codeclib.c:4833-4921`,
   minus `evs_push_frame`) to extract `{is_amr, mode, bits, frame bits, q_bit,
   cmr}`, then emits an RFC 4867 AMR-WB payload using the same packing logic as
   `packetizer_amr` (`lib/codeclib.c:3370-3447`). No decoder, no encoder, no
   PCM — pure repacketization. CMR is mapped EVS-CMR → AMR-CMR.

2. **Decoder-substitution level (needed for EVS-IO → G.722):** reuse the
   parser to produce **AMR-WB octet-aligned `{TOC byte + payload}` buffers**
   (exactly the buffer `amr_decoder_input` builds at `lib/codeclib.c:3234-3243`)
   and feed them into the existing **AMR-WB libavcodec decoder**
   (`avc_decoder_input`) → PCM → existing G.722 encoder. The EVS `.so` is never
   touched; the AMR-WB DSP (`libopencore_amrwb`) does the decode.

Both paths require a refactor of `evs_decoder_input` to separate
*parse/extract* from *DSP decode* (today they are interleaved in one function).

---

## 6. Is there an existing EVS ⇄ AMR-WB-IO re-frame path?

**No.** Searches for `reframe`/`re-frame` return nothing, and every code path
that consumes an EVS payload (`evs_decoder_input`) unconditionally calls
`evs_push_frame` → DSP for every speech/SID frame
(`lib/codeclib.c:4896-4897`, `5003-5006`). There is no code that converts an
EVS-IO payload into an AMR-WB payload, nor any that feeds EVS-IO frames into the
AMR-WB libavcodec decoder. The feature is genuinely new; the reusable
primitives are the parser body, the AMR/EVS bit tables, the
`evs_amr_io_compact_cmr[]` map, and `packetizer_amr`.

---

## 7. `.so`-independence summary (the licensing assertion, verified)

| Capability | Needs EVS `.so`? | Evidence |
|---|---|---|
| EVS payload parse (compact/HF, CMR, ToC, AMR-IO detect, SID) | **No** | `lib/codeclib.c:4813-4921`; tables `:4214-4386` |
| EVS-IO frame extraction (raw speech bits + mode) | **No** | `lib/codeclib.c:4844-4921` |
| Read 7-bit EVS-CMR | **No** | `lib/codeclib.c:4847-4852`, `4871-4872`, `4924` |
| Build RFC 4867 AMR-WB payload | **No** | `packetizer_amr` `:3370-3447` |
| AMR-WB decode → PCM | **No** (uses `libopencore_amrwb`) | `:3255,3265`; def `:733-735` |
| G.722 encode | **No** (libavcodec) | generic avcodec path |
| **EVS primary/AMR-IO DSP encode** | **YES** | `evs_enc_in`/`evs_enc_out` `:4631,4684` |
| **EVS primary/AMR-IO DSP decode** | **YES** | `evs_push_frame` `:4786-4805` |
| EVS codec marked "supported" (enables transcode handler) | **YES** | `evs_def_init` `:4994-5001` |
| Daemon boots without `--evs-lib-path` | **Yes, fine** | `evs_load_so` early return `:4936-4937` |

The only EVS operations that require the `.so` are the encode and decode DSP.
Everything the re-frame path and passthrough need is `.so`-free.

---

## 8. Risks / open questions to resolve in Phase 2

1. **`codec_def_supported` gating.** EVS is "unsupported" without the `.so`, so
   `__codec_handlers_update` currently routes it to passthrough only. The new
   re-frame mode must establish a handler for EVS-IO → {AMR-WB, G.722} that does
   **not** depend on `support_encoding/decoding` (which today require the
   `.so`). Need a new "supported for re-framing" notion or an explicit codec
   option that activates the re-frame handler independently of the DSP flags.
2. **Constraining the EVS leg to AMR-IO.** The re-frame path *cannot* handle
   EVS primary frames. SDP offer/answer must force `evs-mode-switch=1`
   (`amr_io`, parsed at `lib/codeclib.c:4005-4008`) and an AMR-WB-compatible
   `mode-set` when the far leg is AMR-WB/G.722-only. Behaviour for an
   unexpected primary frame: **drop + rate-limited log, never call the DSP**
   (the parser already classifies primary vs AMR-IO via `is_amr`, so this is a
   cheap guard at `lib/codeclib.c:4840/4907`).
3. **CMR mapping fidelity.** EVS-CMR (7-bit type/value, Table A.3) ⇄ AMR-WB CMR
   (4-bit mode). Need a complete, spec-checked table; reconcile with
   `evs_handle_cmr` (`:4562-4616`) and `evs_amr_io_compact_cmr` (`:4738-4747`).
4. **Refactor shape.** Split `evs_decoder_input` into `evs_parse_payload()`
   (DSP-free, reusable) + the existing DSP decode, without regressing the
   legacy path. The legacy EVS-primary→PCM→G.722 mode stays behind its own
   opt-in flag.
5. **SID/DTX semantics on both sides.** EVS-IO SID = 40-bit length-5 (`:4262`),
   AMR-WB SID = 40-bit FT 9 (`:2906`); map NO_DATA/DTX consistently with the
   `dtx`/`dtx-recv` fmtp already parsed (`:4009-4015`).

---

## 9. Test & docs landmarks for Phase 4 (noted, not built)

- Daemon SDP/RTP harness: `t/auto-daemon-tests-evs.pl` — **currently gated on
  `RTPENGINE_3GPP_EVS_LIB`** (`:12-17`); the new re-frame/passthrough tests
  must run with the `.so` *absent* (i.e. not gated on that env var).
- C-level codec unit harnesses: `t/test-transcode.c`, `t/test-amr-decode.c`,
  `t/test-amr-encode.c` — candidates for a DSP-free re-frame unit test.
- pcap fixtures: `fixtures/` currently holds opus/pcma/pcmu raw assets only —
  new EVS-IO pcap fixtures must be added.
- `--codecs` listing flag: `daemon/main.c` (`"codecs"` option).
- Transcoding docs to update: `docs/transcoding.md`.

---

**STOP — awaiting your review before starting Phase 2 (design note).**
