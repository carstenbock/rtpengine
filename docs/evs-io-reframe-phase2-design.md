# Phase 2 — Design Note: Licensing-Safe EVS Bridging

**Status:** Design only. No code written. Builds on
`docs/evs-io-reframe-phase1-findings.md` (read that first; all `file:line`
citations there remain valid).

**One-line model (from the task):** *Normalise EVS to AMR-WB at the payload
boundary.* An EVS AMR-WB-IO leg is treated as an AMR-WB leg the moment its RTP
payload is parsed. From there, everything is existing AMR-WB machinery. The EVS
DSP `.so` is never instantiated on the new paths.

---

## 1. The three handling modes and how each is realised

| # | Scenario | Mechanism | DSP `.so`? |
|---|----------|-----------|-----------|
| 1a | EVS-IO ⇄ AMR-WB | **Pure re-frame** (packet-level repacketization) | never |
| 1b | EVS-IO → G.722 (and any non-AMR-WB PCM codec) | EVS-IO depacketize → **AMR-WB libavcodec decoder** → PCM → existing G.722 encoder | never (uses `libopencore_amrwb`) |
| 2 | Native EVS primary modes | **Passthrough** end-to-end only | never |
| 3 | Legacy EVS-primary → PCM → G.722 | EVS DSP decode (`.so`) → PCM → encoder | **yes, opt-in only** |

Modes 1a/1b/2 are the **new default** and need no `.so`. Mode 3 is the existing
behaviour, demoted behind an explicit, clearly-labelled opt-in.

---

## 2. Gating / configuration design

### 2.1 New global flag for the licensing-sensitive path

Add one boolean daemon option (X-macro list `include/main.h:~140`, parsed in
`daemon/main.c`, option table near the other `evs-*` options):

```
--evs-allow-dsp-transcode      (default: false)
```

- `rtpe_config.evs_allow_dsp_transcode` (new field via the `X(...)` bool macro
  in `include/main.h`, mirroring `evs_cn_dtx` at `include/main.h:141`).
- **OFF (default):** the EVS codec's DSP encode/decode are never used for
  transcoding. EVS-IO is bridged by re-frame (1a/1b); native EVS is passthrough
  only (2). Works with `--evs-lib-path` present **or absent**.
- **ON:** restores today's behaviour — EVS-primary↔PCM↔{G.722,…} transcode via
  the `.so` (mode 3). Requires `--evs-lib-path`. Help text must flag it:
  *"licensing-sensitive: invokes the patent-encumbered EVS codec DSP"*.

Rationale: whether the patented DSP may run is a **deployment-wide licensing
decision**, so a global flag is the right granularity and is unambiguous in
audits. (A per-call ng codec option override can be layered later if needed; not
in initial scope.)

### 2.2 Decoupling re-frame from `codec_def_supported`

Today `codec_def_supported(EVS)` is false without the `.so`
(`lib/codeclib.c:4994-5001`, `lib/codeclib.h:517-520`), and
`__codec_handlers_update` only transcodes supported codecs
(`daemon/codec.c:1572`). The re-frame path must be reachable **without** the
`.so`. Design choice:

- Introduce a separate capability predicate, e.g. `codec_def_evs_reframe(def)`
  → true for the EVS `codec_def` regardless of `.so` (it only needs the pure-C
  parser + the AMR-WB codec, which is always built). Do **not** set
  `support_encoding/decoding` for EVS based on re-frame — keep those tied to the
  `.so` so mode 3 gating stays correct.
- In `__codec_handlers_update`, before the existing `codec_def_supported` punt
  at `daemon/codec.c:1572`, add an EVS-IO re-frame branch (see §5).

---

## 3. EVS-IO ⇄ AMR-WB frame extraction / insertion (the bit mechanics)

The Phase-1 tables prove EVS-IO speech/SID payload **bits** equal AMR-WB payload
bits for the 9 shared modes + SID (`evs_mode_bits[1]` ==
`amr_wb_bits_per_frame`, `lib/codeclib.c:4327-4345` vs `2896-2911`). Re-framing
is therefore a packetization-format translation only. Three packetization
formats are involved:

- **EVS-IO compact** (TS 26.445 A.2.1.2/A.2.2.1): 3-bit CMR prefix + payload,
  payload shifted; size unambiguously identifies the mode
  (`evs_mode_from_bytes`, `lib/codeclib.c:4214-4266`).
- **EVS-IO header-full** (A.2.2.1.x): optional 1-byte CMR (`0x80`-flagged) +
  1-byte ToC (`0x30`-marked AMR-IO) + payload, byte-aligned.
- **AMR-WB RFC 4867**: 4-bit CMR + ToC entries (F/FT/Q + padding) + payload,
  either **octet-aligned** or **bandwidth-efficient**.

### 3.1 EVS-IO → AMR-WB

Reuse the *parse-only* portion of `evs_decoder_input`
(`lib/codeclib.c:4833-4921`) to obtain, per frame:
`{ is_amr (must be 1), mode (0..9), bits, q_bit, frame_data (octet-aligned
speech bits) }` and the leg CMR. Then build the AMR-WB payload exactly as
`packetizer_amr` does (`lib/codeclib.c:3370-3447`):

- Output CMR nibble = mapped from EVS CMR (§4); `0xF` = no request.
- ToC byte: `FT=mode`, `Q=q_bit`, `F=0` for the last frame (multi-frame ptime
  support: set `F=1` on all but last).
- Honour the sink's negotiated `octet-align` (octet-aligned: byte-copy
  TOC+payload, `:3420-3429`; bandwidth-efficient: 10-bit shift, `:3431-3444`).

The compact-format de-shuffle (the `<<2` payload realignment +
first-bit restore at `lib/codeclib.c:4854-4863`) yields the byte-aligned speech
payload that AMR-WB expects, so the same extracted `frame_data` feeds both 1a
and 1b unchanged.

### 3.2 AMR-WB → EVS-IO (reverse direction)

Reuse the *parse-only* portion of `amr_decoder_input`
(`lib/codeclib.c:3187-3243`) to obtain `{ ft (0..9), bits, q_bit, speech
payload }` + AMR CMR. Then build an EVS-IO payload reusing the packing logic
inside `evs_encoder_input` (`lib/codeclib.c:4641-4727`), specifically the
AMR-IO compact path (CMR `0xE0|...`, the forward `<<6`/`>>2` shuffle at
`:4701-4716`) or header-full (ToC `mode | 0x30`, `:4696-4698`), driven by the
EVS leg's negotiated `hf-only`/`cmr` fmtp. Outgoing EVS CMR ← mapped from AMR
CMR (§4) via `evs_amr_io_compact_cmr[]` (`lib/codeclib.c:4738-4747`).

### 3.3 The 9 AMR-WB rates and SID/DTX/NO_DATA

| Mode | kbit/s | bits | EVS-IO len (compact) | AMR-WB FT |
|------|--------|------|----------------------|-----------|
| 0 | 6.60 | 132 | 17 | 0 |
| 1 | 8.85 | 177 | 23 | 1 |
| 2 | 12.65 | 253 | 32 | 2 |
| 3 | 14.25 | 285 | 36 | 3 |
| 4 | 15.85 | 317 | 40 | 4 |
| 5 | 18.25 | 365 | 46 | 5 |
| 6 | 19.85 | 397 | 50 | 6 |
| 7 | 23.05 | 461 | 58 | 7 |
| 8 | 23.85 | 477 | 60 | 8 |
| SID | — | 40 | 5 | 9 (AMR-WB SID) |

(Cross-checked: `evs_mode_from_bytes` `:4244-4263` ↔ `amr_wb_bitrates`/
`amr_wb_bits_per_frame` `:2880-2911`.)

- **SID:** EVS-IO SID (compact len 5 / 40 bits, `:4262-4263`) ↔ AMR-WB SID
  (FT 9, 40 bits, `:2906`). 1:1 byte translation; both directions preserve the
  comfort-noise payload verbatim. No DTX generator on the re-frame path.
- **NO_DATA / SPEECH_LOST:** AMR-WB FT 14/15 are dropped by `packetizer_amr`
  (`:3388-3391`); on the re-frame path, an empty/absent EVS frame maps to AMR-WB
  NO_DATA (FT 15) → emit an empty AMR-WB packet or skip per ptime. EVS-IO has no
  in-band NO_DATA in compact; absence of payload ⇒ nothing forwarded.
- **DTX policy:** carried by fmtp `dtx`/`dtx-recv` (already parsed,
  `:4009-4015`); re-frame passes SID frames through unchanged, so DTX behaviour
  is preserved end-to-end without DSP.

---

## 4. CMR mapping: 7-bit EVS-CMR ⇄ 4-bit AMR CMR

EVS-CMR is a 7-bit `(type[3], value[4])` field (TS 26.445 Annex A, Table A.3);
AMR CMR is a 4-bit mode index. We only map the **AMR-WB-IO** CMR types, since
the re-frame leg is constrained to AMR-IO (§6).

- The platform already encodes the AMR-IO compact-CMR ↔ mode relation in
  `evs_amr_io_compact_cmr[8]` (`lib/codeclib.c:4738-4747`): index = AMR-WB mode
  0..6 (+ "no req"), value = the EVS 7-bit CMR byte (`0x90|n` family). The
  existing receiver `evs_handle_cmr` decodes EVS-CMR as
  `type = (cmr>>4)&0x7`, `req = cmr&0xf` (`:4572-4573`).
- **EVS-CMR → AMR CMR** (EVS-IO source): if `type` indicates AMR-WB-IO
  (per Table A.3, the `NB_AMR-WB`/`AMR-WB IO` rows), `amr_cmr_nibble = req`
  (already a mode index 0..8); "no request" (`0x7F`/`0xFF`) → AMR `0xF`.
  Modes outside the sink's `mode-set` are clamped/ignored (mirror
  `amr_encoder_mode_change`, `:3327`).
- **AMR CMR → EVS-CMR** (EVS-IO sink): `evs_cmr_byte =
  evs_amr_io_compact_cmr[amr_nibble<=6 ? amr_nibble : 7]` (note table covers
  0..6 + no-req; modes 7/8 = 23.05/23.85 need an explicit extension to the table
  in Phase 3). AMR `0xF` (no req) → EVS "no request".
- Routing reuses the existing event plumbing: parsed CMR fires
  `CE_AMR_CMR_RECV` / `CE_EVS_CMR_RECV` (`lib/codeclib.c:3150`, `4924`) →
  `codec_decoder_event` stores into the sink media's `encoder_callback`
  (`daemon/codec.c:3232-3257`). On the re-frame path the same callbacks carry
  the mapped CMR to the outbound packetizer.

A single shared table in Phase 3 (`evs_amrwb_io_cmr[]`, full 0..8 + no-req,
both directions) supersedes the current 0..6 `evs_amr_io_compact_cmr[]`.

---

## 5. Handler selection & data path (where it plugs into `daemon/codec.c`)

### 5.1 Two new handler kinds

**(A) Re-frame handler — `handler_func_reframe`** (new; sibling of
`handler_func_passthrough`/`handler_func_transcode`, declared near
`daemon/codec.c:309-316`). Used for **1a** (EVS-IO ⇄ AMR-WB). Per RTP packet:

```
handler_func_reframe(h, mp):
    parse source payload  (EVS-IO parse  OR  AMR-WB parse, by source codec)
    for each extracted frame {mode, q, speech_bits, cmr}:
        guard: if source is EVS and frame is PRIMARY (is_amr==0):
            drop + rate-limited WARN; never touch DSP   # see §6
        append to dest payload (AMR-WB writer OR EVS-IO writer)
    map + attach CMR
    emit one RTP packet with dest PT (reuse codec_output_rtp_seq_* + media_socket TX)
```

No `decoder_t`/`encoder_t`, no `AVFrame`, no `ssrc` transcode context. This is
the "repacketization only, no DSP" path. It can borrow the passthrough output
machinery (`__make_passthrough` set up the dest PT; the handler rewrites the
payload before TX).

**(B) Re-frame-fed transcode — reuse `handler_func_transcode`** for **1b**
(EVS-IO → G.722 / other PCM sink). Mechanism: the EVS source decoder runs in
**re-frame mode**, i.e. `evs_decoder_input` is refactored (§7) so that, when
re-frame mode is set, it parses EVS-IO and calls the **AMR-WB libavcodec
decoder** (`avc_decoder_input`, the same call AMR-WB uses at
`lib/codeclib.c:3255/3265`) on the extracted `{TOC+payload}` buffer — instead of
`evs_push_frame`. PCM then flows through the unchanged encoder/G.722 path. No
EVS DSP, no `.so`.

### 5.2 Decision insertion in `__codec_handlers_update`

Insert, inside the per-source-codec loop (around `daemon/codec.c:1569-1638`,
before/at the `codec_def_supported` punt at `:1572`):

```
if source_pt is EVS and source negotiated AMR-IO (fmtp amr_io==1):
    if sink_pt is AMR-WB and rates compatible:
        -> install handler_func_reframe (EVS-IO <-> AMR-WB)     # 1a
        continue
    if sink_pt is PCM-family (e.g. G.722) and !evs_allow_dsp_transcode:
        -> __make_transcoder(...) but force EVS decoder re-frame mode   # 1b
        continue
if source_pt is EVS (primary capable) and !evs_allow_dsp_transcode:
    -> passthrough only (mode 2); if sink can't take EVS, no bridge     # 2
if source_pt is EVS and evs_allow_dsp_transcode and .so present:
    -> existing behaviour (mode 3)
```

The reverse direction (sink EVS-IO) is handled symmetrically when the function
is called for the other leg (it is called per direction;
`codec_handlers_update`/`__codec_handlers_update` at `:4953/4985`).

### 5.3 Output emission

Re-frame handler reuses the existing per-handler RTP output:
`codec_output_rtp_seq_passthrough` / `_own` (`daemon/codec.c:291`, `4450/4477`)
and the standard `media_socket` TX used by passthrough/transcode, with the dest
PT number from `handler->dest_pt`.

---

## 6. SDP / offer-answer: constraining the EVS leg to AMR-IO

**Invariant the re-frame path depends on:** when an EVS leg is bridged to an
AMR-WB/G.722-only leg via re-frame, **only AMR-IO frames may arrive** — the
re-frame path cannot decode EVS primary frames (that needs the DSP).

Negotiation rules (applied in offer/answer fmtp handling; EVS fmtp parse at
`lib/codeclib.c:3996-4070`, answer at `4071-4094`, AMR side at `2912-2957`):

1. When the *other* leg offers only AMR-WB and/or G.722 (no EVS), the EVS leg's
   answer/offer toward the EVS endpoint **must set `evs-mode-switch=1`**
   (`amr_io`, `:4005-4008`) and an AMR-WB-compatible `mode-set` (intersection of
   the EVS leg's modes and the AMR-WB leg's `mode-set`). This forces the EVS
   endpoint into AMR-WB IO so no primary frames are produced.
2. If the EVS endpoint refuses AMR-IO (no `evs-mode-switch`), the bridge to an
   AMR-WB/G.722-only leg is not possible without the DSP → either (a) reject /
   no media bridge, or (b) fall back to mode 3 **only if**
   `--evs-allow-dsp-transcode` is set. Default = (a) with a clear log.
3. The EVS leg's `cmr`/`hf-only` choices must be reflected so the re-frame
   writer emits a format the endpoint accepts (`evs_format_print`,
   `:3945-3995`).

**Defensive runtime guard (belt-and-braces):** even with correct negotiation, a
misbehaving endpoint could send a primary frame. The parser already distinguishes
primary vs AMR-IO via `is_amr` (`lib/codeclib.c:4840` compact, `4907` HF). On
the re-frame path, `is_amr==0` ⇒ **drop the frame, emit a rate-limited
`LOG_WARN`, and never call `evs_push_frame`/DSP**. This is the single most
important safety property and is trivially enforceable because classification is
pure-C and happens before any DSP call.

---

## 7. Refactor of `evs_decoder_input` (enabling the above)

Split `lib/codeclib.c:4813-4932` into:

- `evs_parse_payload(const str *data, struct evs_io_frame out[], int *n, uint8_t *cmr)`
  — DSP-free; returns the per-frame `{is_amr, mode, bits, q_bit, frame_data}`
  list and the leg CMR. Body = current lines `4817-4921` minus the
  `evs_push_frame` call.
- Existing DSP path: `evs_decoder_input` keeps calling `evs_push_frame` per
  frame (mode 3 / when EVS decoder is a real DSP decoder).
- Re-frame consumers call `evs_parse_payload` and then either build AMR-WB
  payload (1a) or feed `avc_decoder_input` (1b).

No behaviour change for existing callers; the legacy path is byte-for-byte the
same sequence of operations.

A small new struct (private to `codeclib.c`):

```c
struct evs_io_frame {
    bool is_amr;      // must be true on the re-frame path
    int  mode;        // 0..9 (9 = SID)
    int  bits;        // evs_mode_bits[1][mode]
    int  q_bit;
    str  frame_data;  // octet-aligned speech bits (post de-shuffle)
};
```

Symmetric helpers for the reverse/encode side reuse the existing shuffle code in
`evs_encoder_input` (`:4701-4716`) factored into `evs_io_pack_compact()`.

---

## 8. What stays untouched (regression guard)

- Existing EVS primary transcode (mode 3) — only reachable when
  `--evs-allow-dsp-transcode` is set; code path unchanged.
- AMR / AMR-WB existing transcode & passthrough — unchanged; re-frame reuses
  their parsers/packetizers as libraries, not by modifying them.
- `evs_handle_cmr`, `amr_encoder_mode_change`, DTX methods — unchanged; re-frame
  reuses the CMR event plumbing.
- libcodec-chain accelerated path (`codec_cc_*`) — orthogonal, unchanged.

---

## 9. Phase 3 work breakdown (preview)

1. `lib/codeclib.c`: split `evs_parse_payload` out of `evs_decoder_input`; add
   `evs_io_to_amrwb()` (writer) and `amrwb_to_evs_io()` (writer); extend the CMR
   table to 0..8+no-req; add `evs_decoder` re-frame mode flag.
2. `daemon/codec.c`: `handler_func_reframe`; EVS-IO branch in
   `__codec_handlers_update`; EVS-decoder-re-frame wiring for the G.722 case.
3. `daemon/main.c` + `include/main.h`: `--evs-allow-dsp-transcode` flag (default
   false) with licensing-sensitive help text.
4. SDP/offer-answer: AMR-IO constraint logic (§6) in the EVS fmtp answer.
5. CHANGELOG + version bump (rtpengine app) + `docs/transcoding.md`.

## 10. Phase 4 test plan (preview)

- pcap fixtures: EVS-IO compact + header-full streams → assert AMR-WB out
  (octet-aligned and bandwidth-efficient) and G.722 out, with **no
  `--evs-lib-path`** (proves no DSP). New `t/` perl test NOT gated on
  `RTPENGINE_3GPP_EVS_LIB`.
- Native-EVS passthrough test (no `.so`).
- Primary-frame-on-reframe-path guard test (assert drop + no crash, no `.so`).
- `rtpengine --codecs` shows EVS without `.so`; update `docs/transcoding.md`.

---

## 11. Decisions I made (flagging for confirmation)

1. **Legacy DSP gating = single global flag `--evs-allow-dsp-transcode`
   (default off).** Alternative: a per-call ng codec option. I chose global
   because it is a deployment-wide licensing posture and is auditable.
2. **EVS-IO↔AMR-WB = packet-level pure re-frame (1a), separate from the
   PCM-transcode pipeline.** This honours "repacketization only, no DSP" and
   avoids a lossy decode/re-encode. EVS-IO→G.722 (1b) reuses the PCM pipeline
   with the AMR-WB DSP substituted in.
3. **No primary-frame support on the re-frame path**, by design; mismatches are
   dropped + logged, never DSP-decoded.

**STOP — please confirm §11 (especially the gating flag) before I begin
Phase 3 implementation, or tell me to proceed as designed.**
