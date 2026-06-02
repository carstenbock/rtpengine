#include "codeclib.h"
#include "str.h"
#include "main.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

// This test exercises the licensing-safe EVS AMR-WB-IO <-> AMR-WB re-framing
// (codec_evs_io_to_amr_wb / codec_amr_wb_to_evs_io). It deliberately runs
// WITHOUT loading the 3GPP EVS reference .so: the EVS DSP must never be invoked
// on these paths. codeclib_init(0) is called with no library path, so any
// accidental DSP call would crash or fail loudly.

struct rtpengine_config rtpe_config;
struct rtpengine_config initial_rtpe_config;

static void hexdump(const char *label, const unsigned char *buf, size_t len) {
	printf("%s (%zu): ", label, len);
	for (size_t i = 0; i < len; i++)
		printf("%02x", buf[i]);
	printf("\n");
}

// AMR-WB significant speech bits per mode (0..8) and SID (9).
static const int amr_wb_bits[10] = { 132, 177, 253, 285, 317, 365, 397, 461, 477, 40 };

// Build a canonical octet-aligned AMR-WB single-frame RTP payload for `mode`
// with CMR `cmr_nibble`. Speech bytes get a deterministic pattern with the
// trailing partial-byte bits zeroed so the round-trip is byte-exact.
static size_t build_amr_wb_octet(unsigned char *out, int mode, unsigned char cmr_nibble) {
	int bits = amr_wb_bits[mode];
	int nbytes = (bits + 7) / 8;
	size_t pos = 0;
	out[pos++] = (cmr_nibble & 0xf) << 4;
	out[pos++] = (mode << 3) | (1 << 2); // F=0, FT=mode, Q=1
	for (int i = 0; i < nbytes; i++)
		out[pos + i] = (unsigned char) (0x80 + i * 7);
	if (bits % 8)
		out[pos + nbytes - 1] &= 0xff << (8 - (bits % 8));
	pos += nbytes;
	return pos;
}

#define CHECK(cond, msg) do { \
		if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); exit(1); } \
	} while (0)

// AMR-WB octet -> EVS-IO -> AMR-WB octet must reproduce the input.
static void test_octet_roundtrip(int mode, unsigned char cmr) {
	unsigned char amr_in[256];
	size_t amr_in_len = build_amr_wb_octet(amr_in, mode, cmr);
	str amr_in_s = STR_LEN((char *) amr_in, amr_in_len);

	char evs[256];
	int evs_len = codec_amr_wb_to_evs_io(&amr_in_s, evs, sizeof(evs), true, false);
	CHECK(evs_len > 0, "amr_wb_to_evs_io failed");
	str evs_s = STR_LEN(evs, evs_len);

	char amr_out[256];
	int amr_out_len = codec_evs_io_to_amr_wb(&evs_s, amr_out, sizeof(amr_out), true);
	CHECK(amr_out_len > 0, "evs_io_to_amr_wb failed");

	if ((size_t) amr_out_len != amr_in_len || memcmp(amr_out, amr_in, amr_in_len)) {
		printf("octet round-trip mismatch, mode %d cmr 0x%x\n", mode, cmr);
		hexdump("in ", amr_in, amr_in_len);
		hexdump("evs", (unsigned char *) evs, evs_len);
		hexdump("out", (unsigned char *) amr_out, amr_out_len);
		exit(1);
	}
	printf("ok octet round-trip mode %d cmr 0x%x (evs %d bytes)\n", mode, cmr, evs_len);
}

// AMR-WB octet -> EVS-IO -> AMR-WB BE -> EVS-IO -> AMR-WB octet must reproduce
// the input. Exercises the bandwidth-efficient pack and parse as inverses.
static void test_be_roundtrip(int mode) {
	unsigned char amr_in[256];
	size_t amr_in_len = build_amr_wb_octet(amr_in, mode, 0xf);
	str amr_in_s = STR_LEN((char *) amr_in, amr_in_len);

	char evs1[256];
	int evs1_len = codec_amr_wb_to_evs_io(&amr_in_s, evs1, sizeof(evs1), true, false);
	CHECK(evs1_len > 0, "amr_wb_to_evs_io (1) failed");
	str evs1_s = STR_LEN(evs1, evs1_len);

	char be[256];
	int be_len = codec_evs_io_to_amr_wb(&evs1_s, be, sizeof(be), false); // bandwidth-efficient
	CHECK(be_len > 0, "evs_io_to_amr_wb BE failed");
	str be_s = STR_LEN(be, be_len);

	char evs2[256];
	int evs2_len = codec_amr_wb_to_evs_io(&be_s, evs2, sizeof(evs2), false, false);
	CHECK(evs2_len > 0, "amr_wb_to_evs_io (2, BE) failed");
	str evs2_s = STR_LEN(evs2, evs2_len);

	char amr_out[256];
	int amr_out_len = codec_evs_io_to_amr_wb(&evs2_s, amr_out, sizeof(amr_out), true);
	CHECK(amr_out_len > 0, "evs_io_to_amr_wb (final octet) failed");

	if ((size_t) amr_out_len != amr_in_len || memcmp(amr_out, amr_in, amr_in_len)) {
		printf("BE round-trip mismatch, mode %d\n", mode);
		hexdump("in ", amr_in, amr_in_len);
		hexdump("be ", (unsigned char *) be, be_len);
		hexdump("out", (unsigned char *) amr_out, amr_out_len);
		exit(1);
	}
	printf("ok BE round-trip mode %d\n", mode);
}

// Two AMR-WB frames in one packet -> EVS-IO (header-full) -> back.
static void test_multiframe_roundtrip(void) {
	unsigned char amr_in[256];
	int bits = amr_wb_bits[2];
	int nbytes = (bits + 7) / 8;
	size_t pos = 0;
	amr_in[pos++] = 0xf0; // CMR = no req
	amr_in[pos++] = 0x80 | (2 << 3) | (1 << 2); // F=1, FT=2, Q=1
	amr_in[pos++] = (2 << 3) | (1 << 2); // F=0, FT=2, Q=1
	for (int f = 0; f < 2; f++) {
		for (int i = 0; i < nbytes; i++)
			amr_in[pos + i] = (unsigned char) (0x33 + f * 17 + i * 5);
		if (bits % 8)
			amr_in[pos + nbytes - 1] &= 0xff << (8 - (bits % 8));
		pos += nbytes;
	}
	str amr_in_s = STR_LEN((char *) amr_in, pos);

	char evs[256];
	int evs_len = codec_amr_wb_to_evs_io(&amr_in_s, evs, sizeof(evs), true, false);
	CHECK(evs_len > 0, "multiframe amr_wb_to_evs_io failed");
	// must be header-full: first byte is a TOC (bit 7 clear) with continuation bit set
	CHECK(!(evs[0] & 0x80), "multiframe EVS-IO should be header-full TOC");
	str evs_s = STR_LEN(evs, evs_len);

	char amr_out[256];
	int amr_out_len = codec_evs_io_to_amr_wb(&evs_s, amr_out, sizeof(amr_out), true);
	CHECK(amr_out_len > 0, "multiframe evs_io_to_amr_wb failed");
	if ((size_t) amr_out_len != pos || memcmp(amr_out, amr_in, pos)) {
		printf("multiframe round-trip mismatch\n");
		hexdump("in ", amr_in, pos);
		hexdump("out", (unsigned char *) amr_out, amr_out_len);
		exit(1);
	}
	printf("ok multiframe (header-full) round-trip\n");
}

// A native EVS primary frame must be rejected, never re-framed, never DSP'd.
static void test_primary_rejected(void) {
	// 33-byte payload, first byte's MSB clear so it is parsed as a compact frame.
	// 33 bytes is an EVS primary (13.2 kbit/s) size, not an AMR-WB-IO size.
	unsigned char primary[33];
	memset(primary, 0, sizeof(primary));
	primary[0] = 0x20; // MSB clear -> not forced header-full
	str primary_s = STR_LEN((char *) primary, sizeof(primary));

	char out[256];
	int r = codec_evs_io_to_amr_wb(&primary_s, out, sizeof(out), true);
	CHECK(r == -1, "primary EVS frame must be rejected (-1)");
	printf("ok primary frame rejected\n");
}

int main(void) {
	rtpe_common_config_ptr = &rtpe_config.common;
	codeclib_init(0); // no EVS .so -> any DSP call would fail loudly

	for (unsigned char cmr = 0xf, done = 0; !done; done = 1)
		for (int m = 0; m <= 9; m++)
			test_octet_roundtrip(m, cmr);

	// CMR requests that are representable in the compact format must survive.
	unsigned char repr[] = { 0, 1, 2, 4, 5, 7, 8 };
	for (size_t i = 0; i < sizeof(repr); i++)
		test_octet_roundtrip(2, repr[i]);

	for (int m = 0; m <= 9; m++)
		test_be_roundtrip(m);

	test_multiframe_roundtrip();
	test_primary_rejected();

	printf("all EVS re-frame tests passed\n");
	return 0;
}

int get_local_log_level(unsigned int u) {
	return -1;
}
