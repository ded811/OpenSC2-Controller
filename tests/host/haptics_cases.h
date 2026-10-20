/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only, in both binaries: the haptics synthesiser's pure parts (haptics_dsp.c,
 * haptics_core.c, haptics_rules.c, haptics_sounds.c, haptics_run.c, haptics_help.c), measured on
 * rendered samples: frequencies,
 * envelopes, gains, mixing, the 0x81 special cases, the rumble emulation, the PCM rings and their
 * 0x44 flags, the scripts, the cues and the drop sounds; and the shared vectors of rendered blocks
 * (tests/vectors/hwtest/haptics.txt), which the hardware test suite compares with the board's.
 * Included after vectors_cases.h (it uses its line reader).
 */
#include <opensc2/codec.h>
#include <opensc2/haptics.h>
#include "haptics_core.h"
#include "haptics_rules.h"
#include "haptics_run.h"
#include "sys_rules.h"
#include "retained.h"

/* ---- helpers ---------------------------------------------------------------------------------- */

#define HAP_MAX_FRAMES 8000   /* a second of grip frames */

struct hap_capture {
	int32_t s[2][HAP_MAX_FRAMES];   /* both channels as Q15 values (pads converted back) */
	uint32_t frames;
};

static struct osc2_hap hap;
static struct hap_capture cap;

static int32_t hap_sample_of(uint8_t path, const uint8_t *block, uint32_t frame, uint8_t c)
{
	if (path == OSC2_HAP_PADS) {
		return ((int32_t)block[frame * 2 + c] - 128) * 32767 / 127;
	}
	return (int16_t)(uint16_t)(block[frame * 4 + c * 2] | (block[frame * 4 + c * 2 + 1] << 8));
}

/* Render `blocks` blocks of `path` (applying the queue before each, as the thread does), the
 * first one a first block when `first`; the samples go into `cap` from frame `cap.frames` on. */
static void hap_run(uint8_t path, uint32_t blocks, bool first)
{
	uint8_t block[OSC2_HAP_BLOCK_MAX_BYTES];
	uint8_t frames = hap.seq[path].frames;

	for (uint32_t b = 0; b < blocks; b++) {
		osc2_hap_apply(&hap, path);
		osc2_hap_render(&hap, path, first && b == 0, block);
		for (uint32_t f = 0; f < frames && cap.frames < HAP_MAX_FRAMES; f++) {
			cap.s[0][cap.frames] = hap_sample_of(path, block, f, 0);
			cap.s[1][cap.frames] = hap_sample_of(path, block, f, 1);
			cap.frames++;
		}
	}
}

static void hap_fresh(void)
{
	osc2_hap_init(&hap);
	memset(&cap, 0, sizeof(cap));
}

static void hap_queue_ok(const struct osc2_hap_cmd *c)
{
	CHECK(osc2_hap_queue(&hap, c));
}

static struct osc2_hap_cmd hap_tone(uint8_t channel, int8_t gain, uint32_t hz, uint16_t ms,
				    uint32_t lfo_hz, uint8_t depth)
{
	struct osc2_hap_cmd c;

	memset(&c, 0, sizeof(c));
	c.op = OSC2_HAP_OP_START;
	c.channel = channel;
	c.type = OSC2_HAP_GEN_TONE;
	c.gain_db = gain;
	c.duration_ms = ms;
	c.tone.freq_mhz = hz * 1000u;
	c.tone.lfo_mhz = lfo_hz * 1000u;
	c.tone.depth = depth;
	return c;
}

/* Sign changes of channel c in frames [from, to). */
static int hap_crossings(uint8_t c, uint32_t from, uint32_t to)
{
	int n = 0;
	int32_t last = 0;

	for (uint32_t i = from; i < to; i++) {
		int32_t v = cap.s[c][i];

		if (v != 0) {
			if (last != 0 && (v > 0) != (last > 0)) {
				n++;
			}
			last = v;
		}
	}
	return n;
}

static int32_t hap_peak(uint8_t c, uint32_t from, uint32_t to)
{
	int32_t p = 0;

	for (uint32_t i = from; i < to; i++) {
		int32_t v = cap.s[c][i] < 0 ? -cap.s[c][i] : cap.s[c][i];

		p = v > p ? v : p;
	}
	return p;
}

static uint32_t hap_last_nonzero(uint8_t c)
{
	uint32_t last = 0;

	for (uint32_t i = 0; i < cap.frames; i++) {
		if (cap.s[c][i] != 0) {
			last = i + 1;
		}
	}
	return last;
}

static int hap_events_of(uint8_t path, uint8_t kind, uint8_t what, struct osc2_hap_event *out,
			 int max)
{
	struct osc2_hap_event ev[OSC2_HAP_EVENTS_MAX];
	uint8_t n = osc2_hap_take_events(&hap, path, ev, OSC2_HAP_EVENTS_MAX);
	int found = 0;

	for (uint8_t i = 0; i < n; i++) {
		if (ev[i].kind == kind && (what == 0 || ev[i].what == what)) {
			if (found < max) {
				out[found] = ev[i];
			}
			found++;
		}
	}
	return found;
}

/* ---- the arithmetic --------------------------------------------------------------------------- */

OPENSC2_TEST(haptics_dsp)
{
	CHECK_EQ(osc2_hap_sin(0), 0);
	CHECK_EQ(osc2_hap_sin(0x40000000u), 32767);
	CHECK_EQ(osc2_hap_sin(0x80000000u), 0);
	CHECK_EQ(osc2_hap_sin(0xC0000000u), -32767);
	CHECK_EQ(osc2_hap_sin(0x20000000u), 23170);           /* sin(pi/4) */
	CHECK_EQ(osc2_hap_sin(0xE0000000u), -23170);          /* the LFO's start, -pi/4 */
	/* odd symmetry, and the interpolation stays within one count of the true sine */
	for (uint32_t p = 0; p < 0x80000000u; p += 0x01234567u) {
		double want = 32767.0 * sin(2.0 * 3.14159265358979323846 * p / 4294967296.0);
		int32_t got = osc2_hap_sin(p);

		CHECK(fabs(got - want) <= 1.5);
		CHECK_EQ(osc2_hap_sin(p + 0x80000000u), -got);
	}
	CHECK_EQ(osc2_hap_gain_q16_db(0), 65536);
	CHECK_EQ(osc2_hap_gain_q16_db(-6), 32846);
	CHECK_EQ(osc2_hap_gain_q16_db(6), 130762);
	CHECK_EQ(osc2_hap_gain_q16_db(-30), 2072);
	CHECK_EQ(osc2_hap_gain_q16_db(-128), 2072);            /* a gain never mutes */
	CHECK_EQ(osc2_hap_gain_q16_db(127), osc2_hap_gain_q16_db(30));
	CHECK_EQ(osc2_hap_db_add(120, 20), 127);
	CHECK_EQ(osc2_hap_db_add(-120, -20), -128);
	CHECK_EQ(osc2_hap_db_add(-3, 8), 5);
	/* log2 and 2^x: within 0.01 % */
	CHECK(abs(osc2_hap_log2(1000) - (int32_t)(9.965784 * 65536)) <= 7);
	CHECK_EQ(osc2_hap_log2(1), 0);
	CHECK_EQ(osc2_hap_log2(65536), 16 * 65536);
	CHECK(llabs((long long)osc2_hap_scale_exp2(1000000, 65536 / 2) - 1414214) <= 141);
	CHECK(llabs((long long)osc2_hap_scale_exp2(1000000, -65536 * 3) - 125000) <= 13);
	CHECK_EQ(osc2_hap_scale_exp2(12345, 0), 12345);
	/* the phase step of 50 Hz at 8 kHz: 2^32 / 160 */
	CHECK_EQ(osc2_hap_phase_step(50000, 8000), 26843546u);
	/* G.711 mu-law: silence, the extremes, a mid code */
	CHECK_EQ(osc2_hap_mulaw(0xFF), 0);
	CHECK_EQ(osc2_hap_mulaw(0x7F), 0);
	CHECK_EQ(osc2_hap_mulaw(0x80), 32124);
	CHECK_EQ(osc2_hap_mulaw(0x00), -32124);
	CHECK_EQ(osc2_hap_mulaw(0xEF), 132);     /* exponent 1, mantissa 0: (132 << 1) - 132 */
	for (int u = 0; u < 256; u++) {
		/* the code with the sign bit cleared is the negative one, same magnitude */
		CHECK_EQ(osc2_hap_mulaw((uint8_t)(u & 0x7F)), -osc2_hap_mulaw((uint8_t)(u | 0x80)));
	}
	{
		static const uint8_t s16[2] = { 0x00, 0x80 }, s16b[2] = { 0xFF, 0x7F };
		static const uint8_t s8a = 0x80, s8b = 0x7F, s8c = 0x01;

		CHECK_EQ(osc2_hap_pcm_sample(s16, OSC2_HAP_ENC_S16LE), -32767);
		CHECK_EQ(osc2_hap_pcm_sample(s16b, OSC2_HAP_ENC_S16LE), 32767);
		CHECK_EQ(osc2_hap_pcm_sample(&s8a, OSC2_HAP_ENC_S8), -32767);
		CHECK_EQ(osc2_hap_pcm_sample(&s8b, OSC2_HAP_ENC_S8), 32767);
		CHECK_EQ(osc2_hap_pcm_sample(&s8c, OSC2_HAP_ENC_S8), 258);
	}
	CHECK_EQ(osc2_hap_to_u8(0), 128);
	CHECK_EQ(osc2_hap_to_u8(32767), 255);
	CHECK_EQ(osc2_hap_to_u8(-32767), 0);
	CHECK_EQ(osc2_hap_status_id(OSC2_HAP_CH_LEFT_PAD), 3);
	CHECK_EQ(osc2_hap_status_id(OSC2_HAP_CH_RIGHT_PAD), 4);
	CHECK_EQ(osc2_hap_status_id(OSC2_HAP_CH_LEFT_GRIP), 0);
	CHECK_EQ(osc2_hap_status_id(OSC2_HAP_CH_RIGHT_GRIP), 1);
}

OPENSC2_TEST(haptics_clicks_are_own)
{
	/* OpenSC2's own click shapes, at the real clicks' lengths: they start and end silent and
	 * peak at full scale. */
	int32_t peak_a = 0, peak_b = 0;

	CHECK_EQ(osc2_hap_click_a[0], 0);
	CHECK_EQ(osc2_hap_click_b[0], 0);
	CHECK_EQ(osc2_hap_click_a[OSC2_HAP_CLICK_A_LEN - 1], 0);
	CHECK_EQ(osc2_hap_click_b[OSC2_HAP_CLICK_B_LEN - 1], 0);
	for (int i = 0; i < OSC2_HAP_CLICK_A_LEN; i++) {
		peak_a = abs(osc2_hap_click_a[i]) > peak_a ? abs(osc2_hap_click_a[i]) : peak_a;
	}
	for (int i = 0; i < OSC2_HAP_CLICK_B_LEN; i++) {
		peak_b = abs(osc2_hap_click_b[i]) > peak_b ? abs(osc2_hap_click_b[i]) : peak_b;
	}
	CHECK_EQ(peak_a, 127);
	CHECK_EQ(peak_b, 127);
}

/* ---- the sequencers --------------------------------------------------------------------------- */

OPENSC2_TEST(haptics_tone_frequency_and_lead_in)
{
	struct osc2_hap_cmd c = hap_tone(OSC2_HAP_CH_LEFT_GRIP, 0, 200, 100, 0, 0);
	struct osc2_hap_event ev[4];

	hap_fresh();
	hap.master_db = 0;
	hap.notes = true;
	hap_queue_ok(&c);
	CHECK(osc2_hap_pending(&hap, OSC2_HAP_GRIPS));
	CHECK(!osc2_hap_busy(&hap, OSC2_HAP_GRIPS));
	hap_run(OSC2_HAP_GRIPS, 16, true);
	/* 32 silent frames first (4 ms), then 800 samples of tone (100 ms at 8 kHz) */
	for (int i = 0; i < 32; i++) {
		CHECK_EQ(cap.s[0][i], 0);
	}
	CHECK_EQ(cap.s[0][32], 0);               /* the sine starts at phase 0 */
	CHECK(cap.s[0][33] > 0);
	CHECK_EQ(hap_last_nonzero(0), 32 + 800);       /* frames 32..831 */
	/* 200 Hz for 100 ms: 40 sign changes, give or take one */
	CHECK(abs(hap_crossings(0, 32, 32 + 800) - 39) <= 1);
	CHECK(abs(hap_peak(0, 32, 832) - 32767) <= 2);
	CHECK_EQ(hap_peak(1, 0, cap.frames), 0);        /* the right grip is silent */
	CHECK(!osc2_hap_busy(&hap, OSC2_HAP_GRIPS));
	CHECK_EQ(hap_events_of(OSC2_HAP_GRIPS, OSC2_HAP_EV_GEN, 0, ev, 4), 2);
	CHECK_EQ(ev[0].what, OSC2_HAP_GEN_EV_START);
	CHECK_EQ(ev[0].type, OSC2_HAP_GEN_TONE);
	CHECK_EQ(ev[0].a, 200);
	CHECK_EQ(ev[0].b, 100);
	CHECK_EQ(ev[1].what, OSC2_HAP_GEN_EV_END);

	/* the pads: 16 silent frames, 4 kHz */
	hap_fresh();
	hap.master_db = 0;
	c = hap_tone(OSC2_HAP_CH_RIGHT_PAD, 0, 100, 100, 0, 0);
	hap_queue_ok(&c);
	hap_run(OSC2_HAP_PADS, 16, true);
	for (int i = 0; i < 16; i++) {
		CHECK_EQ(cap.s[1][i], 0);
	}
	CHECK(abs(hap_crossings(1, 16, 16 + 400) - 19) <= 1);
	CHECK(hap_last_nonzero(1) <= 16 + 400);
	CHECK(hap_last_nonzero(1) >= 16 + 390);
}

OPENSC2_TEST(haptics_gains_and_signs)
{
	struct osc2_hap_cmd c = hap_tone(OSC2_HAP_CH_LEFT_GRIP, -6, 100, 50, 0, 0);
	struct osc2_hap_cmd d = hap_tone(OSC2_HAP_CH_RIGHT_GRIP, -6, 100, 50, 0, 0);

	/* the master gain (setting 76) on top of the generator's: -3 - 6 = -9 dB = 0.355 */
	hap_fresh();
	hap_queue_ok(&c);
	hap_queue_ok(&d);
	hap_run(OSC2_HAP_GRIPS, 8, true);
	CHECK(abs(hap_peak(0, 0, cap.frames) - 11627) <= 30);
	/* the right grip's channel gain is negated: the same tone comes out inverted */
	for (uint32_t i = 0; i < cap.frames; i++) {
		CHECK(abs(cap.s[1][i] + cap.s[0][i]) <= 1);
	}
	/* the pads: the left pad is the negated one */
	hap_fresh();
	hap.master_db = 0;
	c = hap_tone(OSC2_HAP_CH_LEFT_PAD, 0, 100, 50, 0, 0);
	d = hap_tone(OSC2_HAP_CH_RIGHT_PAD, 0, 100, 50, 0, 0);
	hap_queue_ok(&c);
	hap_queue_ok(&d);
	hap_run(OSC2_HAP_PADS, 4, true);
	CHECK(cap.s[1][17] > 0);
	CHECK(cap.s[0][17] < 0);
	/* two tones on one channel add up and clip at full scale */
	hap_fresh();
	hap.master_db = 6;
	c = hap_tone(OSC2_HAP_CH_LEFT_GRIP, 0, 100, 50, 0, 0);
	hap_queue_ok(&c);
	c.type = OSC2_HAP_GEN_SWEEP;
	c.sweep.f0_hz = 100;
	c.sweep.f1_hz = 100;
	c.duration_ms = 200;
	hap_queue_ok(&c);
	hap_run(OSC2_HAP_GRIPS, 10, true);
	CHECK_EQ(hap_peak(0, 0, cap.frames), 32767);
}

OPENSC2_TEST(haptics_tone_lfo_and_phase)
{
	struct osc2_hap_cmd c = hap_tone(OSC2_HAP_CH_LEFT_GRIP, 0, 400, 400, 10, 100);
	int32_t lo = 32767, hi = 0;

	hap_fresh();
	hap.master_db = 0;
	hap_queue_ok(&c);
	hap_run(OSC2_HAP_GRIPS, 52, true);
	/* 100 % depth swings the amplitude between 0 and 1 at 10 Hz: per 10 ms window */
	for (uint32_t w = 32; w + 80 <= 32 + 3200; w += 80) {
		int32_t p = hap_peak(0, w, w + 80);

		lo = p < lo ? p : lo;
		hi = p > hi ? p : hi;
	}
	CHECK(hi > 31000);
	CHECK(lo < 3000);
	/* it starts at about 15 % (the LFO at -pi/4) */
	CHECK(hap_peak(0, 32, 52) < 6500);
	CHECK(hap_peak(0, 32, 52) > 3000);

	/* a tone started over a running one keeps the phase: no jump at the join */
	hap_fresh();
	hap.master_db = 0;
	c = hap_tone(OSC2_HAP_CH_LEFT_GRIP, 0, 100, 0, 0, 0);   /* endless */
	hap_queue_ok(&c);
	hap_run(OSC2_HAP_GRIPS, 3, true);
	c = hap_tone(OSC2_HAP_CH_LEFT_GRIP, 0, 100, 20, 0, 0);
	hap_queue_ok(&c);
	hap_run(OSC2_HAP_GRIPS, 4, false);
	/* (100 Hz at 8 kHz moves at most 2573 counts a sample) */
	CHECK(abs(cap.s[0][192] - cap.s[0][191]) <= 2574);
	CHECK(!osc2_hap_busy(&hap, OSC2_HAP_GRIPS));     /* the new one has a duration */
	/* duration 0 never ends by itself */
	hap_fresh();
	c = hap_tone(OSC2_HAP_CH_LEFT_GRIP, 0, 100, 0, 0, 0);
	hap_queue_ok(&c);
	hap_run(OSC2_HAP_GRIPS, 200, true);
	CHECK(osc2_hap_busy(&hap, OSC2_HAP_GRIPS));
	c.op = OSC2_HAP_OP_STOP;
	c.type = OSC2_HAP_GEN_ALL;
	hap_queue_ok(&c);
	osc2_hap_apply(&hap, OSC2_HAP_GRIPS);
	CHECK(!osc2_hap_busy(&hap, OSC2_HAP_GRIPS));
}

OPENSC2_TEST(haptics_pulse_and_clicks)
{
	struct osc2_hap_cmd c;
	struct osc2_hap_event ev[8];

	/* a pulse train on a grip: 125 us samples, +1 for on, -1 for off, `count` periods */
	hap_fresh();
	hap.master_db = 0;
	memset(&c, 0, sizeof(c));
	c.op = OSC2_HAP_OP_START;
	c.channel = OSC2_HAP_CH_LEFT_GRIP;
	c.type = OSC2_HAP_GEN_PULSE;
	c.gain_db = -20;                  /* ignored: full scale */
	c.pulse.on_us = 500;
	c.pulse.off_us = 375;
	c.pulse.count = 5;
	hap_queue_ok(&c);
	hap_run(OSC2_HAP_GRIPS, 2, true);
	for (int k = 0; k < 5; k++) {
		for (int i = 0; i < 7; i++) {
			CHECK_EQ(cap.s[0][32 + k * 7 + i], i < 4 ? 32767 : -32767);
		}
	}
	CHECK_EQ(cap.s[0][32 + 35], 0);
	CHECK(!osc2_hap_busy(&hap, OSC2_HAP_GRIPS));
	/* on + off shorter than a sample: nothing (noted as ignored) */
	hap_fresh();
	hap.notes = true;
	c.pulse.on_us = 50;
	c.pulse.off_us = 50;
	hap_queue_ok(&c);
	osc2_hap_apply(&hap, OSC2_HAP_GRIPS);
	CHECK(!osc2_hap_busy(&hap, OSC2_HAP_GRIPS));
	CHECK_EQ(hap_events_of(OSC2_HAP_GRIPS, OSC2_HAP_EV_GEN, OSC2_HAP_GEN_EV_IGNORED, ev, 8), 1);

	/* click A on a pad: 70 samples at 4 kHz, then it ends; the grips refuse it */
	hap_fresh();
	hap.master_db = 0;
	hap.notes = true;
	memset(&c, 0, sizeof(c));
	c.op = OSC2_HAP_OP_START;
	c.channel = OSC2_HAP_CH_RIGHT_PAD;
	c.type = OSC2_HAP_GEN_CLICK_A;
	hap_queue_ok(&c);
	hap_run(OSC2_HAP_PADS, 4, true);
	CHECK_EQ(hap_last_nonzero(1) <= 16 + 70, 1);
	CHECK(hap_last_nonzero(1) >= 16 + 60);
	CHECK(abs(hap_peak(1, 0, cap.frames) - 32767) <= 300);
	CHECK(!osc2_hap_busy(&hap, OSC2_HAP_PADS));
	c.channel = OSC2_HAP_CH_LEFT_GRIP;
	CHECK(!osc2_hap_queue(&hap, &c));
	CHECK_EQ(hap_events_of(OSC2_HAP_GRIPS, OSC2_HAP_EV_GEN, OSC2_HAP_GEN_EV_REFUSED, ev, 8), 1);
	CHECK_EQ(ev[0].type, OSC2_HAP_GEN_CLICK_A);
	/* the pads refuse the rumble generator */
	c.channel = OSC2_HAP_CH_LEFT_PAD;
	c.type = OSC2_HAP_GEN_RUMBLE;
	CHECK(!osc2_hap_queue(&hap, &c));
	/* click B: 93 samples, played at -6 dB is half as loud */
	hap_fresh();
	hap.master_db = 0;
	c.channel = OSC2_HAP_CH_LEFT_PAD;
	c.type = OSC2_HAP_GEN_CLICK_B;
	c.gain_db = -6;
	hap_queue_ok(&c);
	hap_run(OSC2_HAP_PADS, 5, true);
	CHECK(hap_last_nonzero(0) <= 16 + 93);
	CHECK(hap_last_nonzero(0) >= 16 + 80);
	CHECK(abs(hap_peak(0, 0, cap.frames) - 16423) <= 300);
}

OPENSC2_TEST(haptics_sweep)
{
	struct osc2_hap_cmd c;
	struct osc2_hap_event ev[4];
	int early, late;

	hap_fresh();
	hap.master_db = 0;
	hap.notes = true;
	memset(&c, 0, sizeof(c));
	c.op = OSC2_HAP_OP_START;
	c.channel = OSC2_HAP_CH_RIGHT_GRIP;
	c.type = OSC2_HAP_GEN_SWEEP;
	c.duration_ms = 500;
	c.sweep.f0_hz = 100;
	c.sweep.f1_hz = 800;
	hap_queue_ok(&c);
	hap_run(OSC2_HAP_GRIPS, 70, true);
	/* exponential: 100 Hz at the start, 800 Hz at the end; over the first and last 50 ms the
	 * rate of sign changes is about 2 * f */
	early = hap_crossings(1, 32 + 0, 32 + 400);      /* 100..123 Hz: about 11 */
	late = hap_crossings(1, 32 + 3600, 32 + 4000);   /* 649..800 Hz: about 72 */
	CHECK(early >= 9 && early <= 13);
	CHECK(late >= 66 && late <= 78);
	/* the 80 ms fade-in and the 5 ms fade-out */
	CHECK(hap_peak(1, 32, 32 + 80) < 6000);
	CHECK(hap_peak(1, 32 + 700, 32 + 1000) > 30000);
	CHECK(hap_peak(1, 32 + 3990, 32 + 4000) < 9000);
	CHECK(hap_last_nonzero(1) <= 32 + 4000);
	CHECK(!osc2_hap_busy(&hap, OSC2_HAP_GRIPS));
	/* 0 Hz at one end plays nothing; equal ends are a tone with the fades */
	(void)hap_events_of(OSC2_HAP_GRIPS, OSC2_HAP_EV_GEN, 0, ev, 4);
	c.sweep.f0_hz = 0;
	hap_queue_ok(&c);
	osc2_hap_apply(&hap, OSC2_HAP_GRIPS);
	CHECK(!osc2_hap_busy(&hap, OSC2_HAP_GRIPS));
	CHECK_EQ(hap_events_of(OSC2_HAP_GRIPS, OSC2_HAP_EV_GEN, OSC2_HAP_GEN_EV_IGNORED, ev, 4), 1);
	hap_fresh();
	hap.master_db = 0;
	c.sweep.f0_hz = 200;
	c.sweep.f1_hz = 200;
	c.duration_ms = 200;
	hap_queue_ok(&c);
	hap_run(OSC2_HAP_GRIPS, 30, true);
	CHECK(abs(hap_crossings(1, 32, 32 + 1600) - 79) <= 1);
	/* the antiphase flag inverts it */
	hap_fresh();
	hap.master_db = 0;
	c.channel = OSC2_HAP_CH_LEFT_GRIP;
	hap_queue_ok(&c);
	c.flags = OSC2_HAP_CMD_INVERT;
	c.channel = OSC2_HAP_CH_RIGHT_GRIP;
	hap_queue_ok(&c);
	hap_run(OSC2_HAP_GRIPS, 30, true);
	for (uint32_t i = 0; i < cap.frames; i++) {
		CHECK(abs(cap.s[0][i] - cap.s[1][i]) <= 1);   /* the negated channel gain undone */
	}
}

static struct osc2_hap_cmd hap_clip(uint8_t channel, const uint8_t *data, uint16_t samples,
				    uint8_t format)
{
	struct osc2_hap_cmd c;

	memset(&c, 0, sizeof(c));
	c.op = OSC2_HAP_OP_START;
	c.channel = channel;
	c.type = OSC2_HAP_GEN_CLIP;
	c.format = format;
	c.clip.data = data;
	c.clip.samples = samples;
	return c;
}

OPENSC2_TEST(haptics_clip)
{
	/* a recording in a stream format (opensc2_haptics_play_clip()): s8 at 2 kHz on a grip, each
	 * sample and three steps towards the next, the last one held through its steps */
	static const uint8_t s8[3] = { 64, (uint8_t)-64, 127 };
	static const int32_t want[12] = { 16513, 8257, 0, -8256, -16513, -4193, 8127, 20447,
					  32767, 32767, 32767, 32767 };
	static const uint8_t mu[3] = { 0x80, 0x00, 0xFF };
	struct osc2_hap_cmd c = hap_clip(OSC2_HAP_CH_LEFT_GRIP, s8, 3, 6);
	struct osc2_hap_event ev[4];

	hap_fresh();
	hap.master_db = 0;
	hap.notes = true;
	hap_queue_ok(&c);
	hap_run(OSC2_HAP_GRIPS, 2, true);
	for (int i = 0; i < 12; i++) {
		CHECK_EQ(cap.s[0][32 + i], want[i]);
	}
	CHECK_EQ(hap_last_nonzero(0), 32 + 12);
	CHECK(!osc2_hap_busy(&hap, OSC2_HAP_GRIPS));
	CHECK_EQ(hap_events_of(OSC2_HAP_GRIPS, OSC2_HAP_EV_GEN, 0, ev, 4), 2);
	CHECK_EQ(ev[0].what, OSC2_HAP_GEN_EV_START);
	CHECK_EQ(ev[0].type, OSC2_HAP_GEN_CLIP);
	CHECK_EQ(ev[0].a, 3);                  /* samples */
	CHECK_EQ(ev[0].b, 6);                  /* format */
	CHECK_EQ(ev[1].what, OSC2_HAP_GEN_EV_END);
	/* mu-law at 8 kHz on the right grip: one sample each, through the negated channel gain */
	hap_fresh();
	hap.master_db = 0;
	c = hap_clip(OSC2_HAP_CH_RIGHT_GRIP, mu, 3, 8);
	hap_queue_ok(&c);
	hap_run(OSC2_HAP_GRIPS, 1, true);
	CHECK_EQ(cap.s[1][32], -32124);
	CHECK_EQ(cap.s[1][33], 32124);
	CHECK_EQ(cap.s[1][34], 0);
	CHECK_EQ(hap_last_nonzero(1), 32 + 2);
	CHECK(!osc2_hap_busy(&hap, OSC2_HAP_GRIPS));
	/* an 8 kHz clip on a 4 kHz pad is not slowed down to fit: one clip sample per output sample */
	hap_fresh();
	hap.master_db = 0;
	c = hap_clip(OSC2_HAP_CH_RIGHT_PAD, s8, 3, 4);
	hap_queue_ok(&c);
	hap_run(OSC2_HAP_PADS, 1, true);
	CHECK(cap.s[1][16] > 16000);
	CHECK(cap.s[1][17] < -16000);
	CHECK(cap.s[1][18] > 32000);
	CHECK_EQ(cap.s[1][19], 0);
	CHECK(!osc2_hap_busy(&hap, OSC2_HAP_PADS));
	/* formats 12 and above, and an empty clip, play nothing; a clip already playing goes on */
	hap_fresh();
	hap.notes = true;
	c = hap_clip(OSC2_HAP_CH_LEFT_GRIP, s8, 3, 12);
	hap_queue_ok(&c);
	osc2_hap_apply(&hap, OSC2_HAP_GRIPS);
	CHECK(!osc2_hap_busy(&hap, OSC2_HAP_GRIPS));
	c = hap_clip(OSC2_HAP_CH_LEFT_GRIP, s8, 3, 6);
	hap_queue_ok(&c);
	c = hap_clip(OSC2_HAP_CH_LEFT_GRIP, s8, 0, 6);
	hap_queue_ok(&c);
	osc2_hap_apply(&hap, OSC2_HAP_GRIPS);
	CHECK_EQ(osc2_hap_gen_mask(&hap, OSC2_HAP_CH_LEFT_GRIP), 1u << OSC2_HAP_GEN_CLIP);
	CHECK_EQ(hap_events_of(OSC2_HAP_GRIPS, OSC2_HAP_EV_GEN, OSC2_HAP_GEN_EV_IGNORED, ev, 4), 2);
}

OPENSC2_TEST(haptics_ring_params)
{
	/* the four u16 of a HAP_GEN entry: rates in 0.01 Hz, held at 0xFFFF from 655.35 Hz up */
	struct osc2_hap_cmd c = hap_tone(OSC2_HAP_CH_LEFT_GRIP, 0, 440, 100, 10, 50);
	uint16_t p[4];

	osc2_hap_cmd_params(&c, p);
	CHECK_EQ(p[0], 440);
	CHECK_EQ(p[1], 100);
	CHECK_EQ(p[2], 1000);
	CHECK_EQ(p[3], 50);
	c.tone.lfo_mhz = 655350u;
	osc2_hap_cmd_params(&c, p);
	CHECK_EQ(p[2], 65535);
	c.tone.lfo_mhz = 1000000u;               /* 1 kHz: does not wrap round */
	c.flags = OSC2_HAP_CMD_INVERT;
	osc2_hap_cmd_params(&c, p);
	CHECK_EQ(p[2], 0xFFFF);
	CHECK_EQ(p[3], 0x8000 | 50);
	memset(&c, 0, sizeof(c));
	c.op = OSC2_HAP_OP_START;
	c.type = OSC2_HAP_GEN_RUMBLE;
	c.duration_ms = 500;
	c.rumble.freq_mhz = 50000;
	c.rumble.burst_mhz = 12000;
	osc2_hap_cmd_params(&c, p);
	CHECK_EQ(p[0], 50);
	CHECK_EQ(p[1], 500);
	CHECK_EQ(p[2], 1200);
	CHECK_EQ(p[3], 0);
}

OPENSC2_TEST(haptics_rumble_numbers)
{
	/* the level step */
	static const uint16_t steps[] = { 16, 60, 100, 200, 1000, 8000, 16000, 0, 15, 2000 };
	static const int8_t dbs[] = { 16, 12, 8, 4, -4, -8, -12, 0, 0, 0 };

	for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		CHECK_EQ(osc2_hap_rumble_level_db(steps[i]), dbs[i]);
	}
	/* the right grip: -20 dB below 1000, -14 from 1000, 1 dB more per 10,000, -8 from 60,000 */
	CHECK_EQ(osc2_hap_rumble_right_db(999), -20);
	CHECK_EQ(osc2_hap_rumble_right_db(1000), -14);
	CHECK_EQ(osc2_hap_rumble_right_db(19999), -13);
	CHECK_EQ(osc2_hap_rumble_right_db(60000), -8);
	CHECK_EQ(osc2_hap_rumble_right_db(65535), -8);
	/* the left grip with setting 27 at its default: -26 below 1000, -20 from 1000, -5 at full */
	CHECK_EQ(osc2_hap_rumble_left_db(999, -10, 0), -26);
	CHECK_EQ(osc2_hap_rumble_left_db(1000, -10, 0), -20);
	CHECK_EQ(osc2_hap_rumble_left_db(65535, -10, 0), -5);
	CHECK_EQ(osc2_hap_rumble_left_db(12500, -10, 0), -17);   /* 12500 * 0.00024 = 3 exactly */
	CHECK_EQ(osc2_hap_rumble_left_db(12499, -10, 0), -18);
	CHECK_EQ(osc2_hap_rumble_left_db(1000, 0, 16), 6);        /* setting 27 and the level add */
	CHECK_EQ(osc2_hap_rumble_left_db(1000, -10, 16000), -32);
	/* the LFO: 10 Hz to a fifth, 8.8 Hz just above, 10 Hz at 40 %, 13.6 Hz at full speed */
	CHECK_EQ(osc2_hap_rumble_lfo_mhz(0), 10000);
	CHECK_EQ(osc2_hap_rumble_lfo_mhz(13107), 10000);
	CHECK_EQ(osc2_hap_rumble_lfo_mhz(13108), 8800);
	CHECK_EQ(osc2_hap_rumble_lfo_mhz(26214), 10000);
	CHECK_EQ(osc2_hap_rumble_lfo_mhz(65535), 13600);
	/* the bursts: 12 Hz whatever the speed */
	CHECK_EQ(osc2_hap_rumble_burst_mhz(0), 12000);
	CHECK_EQ(osc2_hap_rumble_burst_mhz(65535), 12000);
}

OPENSC2_TEST(haptics_rumble_bursts)
{
	struct opensc2_output o;
	struct osc2_hap_actions a;
	int32_t burst_peak[8];
	/* the documented first random bursts after the boot seed: +2, -1, +2, +2, -2, -4, +1, -3 dB */
	static const int8_t want_db[7] = { 2, -1, 2, 2, -2, -4, 1 };

	hap_fresh();
	hap.master_db = 0;
	memset(&o, 0, sizeof(o));
	o.type = OPENSC2_OUTPUT_RUMBLE;
	o.rumble.left_speed = 0;
	o.rumble.right_speed = 65535;       /* -8 dB */
	osc2_hap_actions(&o, -10, &a);
	CHECK_EQ(a.n_cmd, 2);
	CHECK_EQ(a.cmd[0].channel, OSC2_HAP_CH_RIGHT_GRIP);   /* the right grip first */
	CHECK_EQ(a.cmd[0].type, OSC2_HAP_GEN_RUMBLE);
	CHECK_EQ(a.cmd[0].gain_db, -8);
	CHECK_EQ(a.cmd[1].op, OSC2_HAP_OP_STOP);              /* left speed 0: a stop-all */
	CHECK_EQ(a.cmd[1].channel, OSC2_HAP_CH_LEFT_GRIP);
	hap_queue_ok(&a.cmd[0]);
	hap_queue_ok(&a.cmd[1]);
	hap_run(OSC2_HAP_GRIPS, 64, true);
	/* a burst every 668 samples (83.5 ms), two periods of 50 Hz (320 samples), then silence */
	for (int b = 0; b < 6; b++) {
		uint32_t at = 32 + (uint32_t)b * 668;

		CHECK(cap.s[1][at] != 0);                       /* the phase moves first: not 0 */
		CHECK_EQ(hap_peak(1, at + 322, at + 668), 0);
		burst_peak[b] = hap_peak(1, at, at + 320);
	}
	/* the first burst at 0 dB (-8 dB of the gain: 13045), the later ones random */
	CHECK(abs(burst_peak[0] - 13045) <= 40);
	for (int b = 1; b < 6; b++) {
		double ratio = (double)burst_peak[b] / burst_peak[0];

		CHECK(fabs(20.0 * log10(ratio) - want_db[b - 1]) < 0.1);
	}
	/* the inverted sine on a negated channel comes out upright: its first burst rises */
	CHECK(cap.s[1][33] > 0);
	/* 500 ms + 1 sample, then it is gone */
	CHECK(!osc2_hap_busy(&hap, OSC2_HAP_GRIPS));
	CHECK(hap_last_nonzero(1) <= 32 + 4001);

	/* a rumble report repeated over a running rumble keeps its bursts going */
	hap_fresh();
	osc2_hap_actions(&o, -10, &a);
	hap_queue_ok(&a.cmd[0]);
	hap_run(OSC2_HAP_GRIPS, 20, true);              /* 1280 frames: in the second burst */
	osc2_hap_actions(&o, -10, &a);
	hap_queue_ok(&a.cmd[0]);
	hap_run(OSC2_HAP_GRIPS, 20, false);
	CHECK(cap.s[1][32 + 1336] != 0);                 /* the third burst where it was due */
	CHECK_EQ(hap_peak(1, 32 + 668 + 322, 32 + 1336), 0);

	/* a timeout from the library does nothing */
	o.rumble.timeout = true;
	osc2_hap_actions(&o, -10, &a);
	CHECK_EQ(a.n_cmd, 0);
}

OPENSC2_TEST(haptics_rumble_left_tone)
{
	struct opensc2_output o;
	struct osc2_hap_actions a;

	hap_fresh();
	hap.master_db = 0;
	memset(&o, 0, sizeof(o));
	o.type = OPENSC2_OUTPUT_RUMBLE;
	o.rumble.left_speed = 65535;
	o.rumble.right_speed = 0;
	osc2_hap_actions(&o, -10, &a);
	CHECK_EQ(a.n_cmd, 2);
	CHECK_EQ(a.cmd[0].op, OSC2_HAP_OP_STOP);
	CHECK_EQ(a.cmd[0].channel, OSC2_HAP_CH_RIGHT_GRIP);
	CHECK_EQ(a.cmd[1].type, OSC2_HAP_GEN_TONE);
	CHECK_EQ(a.cmd[1].gain_db, -5);
	CHECK_EQ(a.cmd[1].tone.freq_mhz, 50000u);
	CHECK_EQ(a.cmd[1].tone.lfo_mhz, 13600u);
	CHECK_EQ(a.cmd[1].tone.depth, 100);
	CHECK_EQ(a.cmd[1].duration_ms, 500);
	hap_queue_ok(&a.cmd[0]);
	hap_queue_ok(&a.cmd[1]);
	hap_run(OSC2_HAP_GRIPS, 64, true);
	CHECK(abs(hap_crossings(0, 32, 32 + 4000) - 49) <= 1);   /* 50 Hz for 500 ms */
	CHECK(hap_peak(0, 0, cap.frames) <= 18420 + 40);           /* -5 dB */
	CHECK(hap_last_nonzero(0) <= 32 + 4000);
}

/* ---- the output reports' special cases -------------------------------------------------------- */

static void hap_decode(const uint8_t *report, size_t len, struct osc2_hap_actions *a)
{
	struct opensc2_output o[2];

	CHECK_EQ(opensc2_decode_output(report, len, o), 1);
	osc2_hap_actions(&o[0], -10, a);
}

OPENSC2_TEST(haptics_report_cases)
{
	struct osc2_hap_actions a;
	/* 0x81 target 2 (both pads): on 0 -> a stop-all on each, the left pad first */
	static const uint8_t stop[8] = { 0x81, 2, 0, 0, 0x10, 0, 5, 0 };
	static const uint8_t click_a[8] = { 0x81, 0, 0x10, 0, 0, 0, 5, 0 };     /* off 0 */
	static const uint8_t click_b[8] = { 0x81, 1, 0x10, 0, 0x10, 0, 4, 0 };  /* count 4 */
	static const uint8_t nothing[8] = { 0x81, 1, 0x10, 0, 0x10, 0, 0, 0 };  /* count 0 */
	static const uint8_t train[8] = { 0x81, 3, 0xE8, 3, 0xF4, 1, 7, 0 };    /* left grip */
	static const uint8_t cmd_a[4] = { 0x82, 0, 1, 0xFA };
	static const uint8_t cmd_bad[4] = { 0x82, 2, 3, 0 };
	static const uint8_t sweep7[9] = { 0x84, 7, 0, 100, 0, 100, 0, 0x2C, 1 };
	static const uint8_t pcm_stop[4] = { 0x86, 1, 5, 0 };
	static const uint8_t pcm_fmt[4] = { 0x86, 2, 2, 6 };
	static const uint8_t data89[7] = { 0x89, 3, 5, 1, 2, 3, 0 };
	static const uint8_t script[4] = { 0x85, 5, 9, 0xF6 };

	hap_decode(stop, sizeof(stop), &a);
	CHECK_EQ(a.n_cmd, 2);
	CHECK_EQ(a.cmd[0].channel, OSC2_HAP_CH_LEFT_PAD);
	CHECK_EQ(a.cmd[0].op, OSC2_HAP_OP_STOP);
	CHECK_EQ(a.cmd[0].type, OSC2_HAP_GEN_ALL);
	CHECK_EQ(a.cmd[1].channel, OSC2_HAP_CH_RIGHT_PAD);
	hap_decode(click_a, sizeof(click_a), &a);   /* target 0 of 0x81 is the right pad */
	CHECK_EQ(a.n_cmd, 1);
	CHECK_EQ(a.cmd[0].channel, OSC2_HAP_CH_RIGHT_PAD);
	CHECK_EQ(a.cmd[0].type, OSC2_HAP_GEN_CLICK_A);
	CHECK_EQ(a.cmd[0].gain_db, 8);
	hap_decode(click_b, sizeof(click_b), &a);
	CHECK_EQ(a.cmd[0].channel, OSC2_HAP_CH_LEFT_PAD);
	CHECK_EQ(a.cmd[0].type, OSC2_HAP_GEN_CLICK_B);
	CHECK_EQ(a.cmd[0].gain_db, 4);
	hap_decode(nothing, sizeof(nothing), &a);
	CHECK_EQ(a.n_cmd, 0);
	hap_decode(train, sizeof(train), &a);
	CHECK_EQ(a.n_cmd, 1);
	CHECK_EQ(a.cmd[0].type, OSC2_HAP_GEN_PULSE);
	CHECK_EQ(a.cmd[0].pulse.on_us, 1000);
	CHECK_EQ(a.cmd[0].pulse.off_us, 500);
	CHECK_EQ(a.cmd[0].pulse.count, 7);
	hap_decode(cmd_a, sizeof(cmd_a), &a);      /* click A 6 dB above the gain */
	CHECK_EQ(a.cmd[0].type, OSC2_HAP_GEN_CLICK_A);
	CHECK_EQ(a.cmd[0].gain_db, 0);
	hap_decode(cmd_bad, sizeof(cmd_bad), &a);  /* command 3: the report is ignored */
	CHECK_EQ(a.n_cmd, 0);
	hap_decode(sweep7, sizeof(sweep7), &a);    /* target 7: both grips, the right inverted */
	CHECK_EQ(a.n_cmd, 2);
	CHECK_EQ(a.cmd[0].channel, OSC2_HAP_CH_LEFT_GRIP);
	CHECK_EQ(a.cmd[0].flags, 0);
	CHECK_EQ(a.cmd[1].channel, OSC2_HAP_CH_RIGHT_GRIP);
	CHECK_EQ(a.cmd[1].flags, OSC2_HAP_CMD_INVERT);
	CHECK_EQ(a.cmd[1].sweep.f0_hz, 100);
	CHECK_EQ(a.cmd[1].sweep.f1_hz, 300);
	hap_decode(pcm_stop, sizeof(pcm_stop), &a);   /* 0x86 target 5: both pads, the right first */
	CHECK_EQ(a.n_cmd, 2);
	CHECK_EQ(a.cmd[0].channel, OSC2_HAP_CH_RIGHT_PAD);
	CHECK_EQ(a.cmd[0].type, OSC2_HAP_GEN_STREAM);
	CHECK_EQ(a.cmd[1].channel, OSC2_HAP_CH_LEFT_PAD);
	hap_decode(pcm_fmt, sizeof(pcm_fmt), &a);
	CHECK_EQ(a.n_cmd, 2);
	CHECK_EQ(a.cmd[0].op, OSC2_HAP_OP_FORMAT);
	CHECK_EQ(a.cmd[0].channel, OSC2_HAP_CH_RIGHT_GRIP);
	CHECK_EQ(a.cmd[0].format, 6);
	hap_decode(data89, sizeof(data89), &a);
	CHECK_EQ(a.n_write, 2);
	CHECK_EQ(a.write[0].channel, OSC2_HAP_CH_RIGHT_PAD);
	CHECK_EQ(a.write[1].channel, OSC2_HAP_CH_LEFT_PAD);
	CHECK_EQ(a.write[0].len, 3);
	hap_decode(script, sizeof(script), &a);
	CHECK_EQ(a.n_script, 2);
	CHECK_EQ(a.script[0].channel, OSC2_HAP_CH_LEFT_GRIP);
	CHECK_EQ(a.script[0].id, 9);
	CHECK_EQ(a.script[0].gain_db, -10);
}

OPENSC2_TEST(haptics_library_clicks)
{
	/* lizard mode's events: a tick (click A, +8 dB) on the left pad, a press (click B, +6 dB) on
	 * the right one, each at its gain as given; anything else plays nothing */
	struct opensc2_haptic h[3];
	struct osc2_hap_actions a;

	memset(h, 0, sizeof(h));
	h[0].kind = OPENSC2_HAPTIC_COMMAND;
	h[0].channels = OPENSC2_HAPTIC_LEFT_PAD;
	h[0].command.command = OPENSC2_HAPTIC_CMD_CLICK_A;
	h[0].command.gain_db = 8;
	h[1] = h[0];
	h[1].channels = OPENSC2_HAPTIC_RIGHT_PAD | OPENSC2_HAPTIC_LEFT_GRIP;   /* grips: not here */
	h[1].command.command = OPENSC2_HAPTIC_CMD_CLICK_B;
	h[1].command.gain_db = 6;
	h[2] = h[0];
	h[2].command.command = OPENSC2_HAPTIC_CMD_STOP_ALL;
	osc2_hap_library_clicks(h, 3, &a);
	CHECK_EQ(a.n_cmd, 2);
	CHECK_EQ(a.cmd[0].channel, OSC2_HAP_CH_LEFT_PAD);
	CHECK_EQ(a.cmd[0].type, OSC2_HAP_GEN_CLICK_A);
	CHECK_EQ(a.cmd[0].gain_db, 8);                 /* not 8 + 6 as for 0x82 */
	CHECK_EQ(a.cmd[1].channel, OSC2_HAP_CH_RIGHT_PAD);
	CHECK_EQ(a.cmd[1].type, OSC2_HAP_GEN_CLICK_B);
	CHECK_EQ(a.cmd[1].gain_db, 6);
	CHECK_EQ(a.n_write + a.n_script, 0);
}

OPENSC2_TEST(haptics_queue_limits)
{
	struct osc2_hap_cmd c = hap_tone(OSC2_HAP_CH_LEFT_GRIP, 0, 100, 10, 0, 0);
	struct osc2_hap_event ev[4];

	hap_fresh();
	hap.notes = true;
	for (int i = 0; i < OSC2_HAP_QUEUE_MAX; i++) {
		hap_queue_ok(&c);
	}
	CHECK(!osc2_hap_queue(&hap, &c));
	CHECK_EQ(hap.seq[OSC2_HAP_GRIPS].lost, 1);
	CHECK_EQ(hap_events_of(OSC2_HAP_GRIPS, OSC2_HAP_EV_GEN, OSC2_HAP_GEN_EV_LOST, ev, 4), 1);
	/* the other sequencer's queue is its own */
	c.channel = OSC2_HAP_CH_LEFT_PAD;
	hap_queue_ok(&c);
	osc2_hap_apply(&hap, OSC2_HAP_GRIPS);
	CHECK(!osc2_hap_pending(&hap, OSC2_HAP_GRIPS));
	CHECK(osc2_hap_pending(&hap, OSC2_HAP_PADS));
	CHECK_EQ(osc2_hap_gen_mask(&hap, OSC2_HAP_CH_LEFT_GRIP), 1u << OSC2_HAP_GEN_TONE);
	osc2_hap_reset(&hap);
	CHECK(!osc2_hap_busy(&hap, OSC2_HAP_GRIPS));
	CHECK(!osc2_hap_pending(&hap, OSC2_HAP_PADS));
}

/* ---- PCM streams ------------------------------------------------------------------------------ */

static uint32_t hap_status_flags(uint8_t path, uint8_t channel)
{
	struct osc2_hap_event ev[OSC2_HAP_EVENTS_MAX];
	uint8_t n = osc2_hap_take_events(&hap, path, ev, OSC2_HAP_EVENTS_MAX);
	uint32_t flags = 0;

	for (uint8_t i = 0; i < n; i++) {
		if (ev[i].kind == OSC2_HAP_EV_STATUS && ev[i].channel == channel) {
			flags |= ev[i].flags;
		}
	}
	return flags;
}

static void hap_format(uint8_t channel, uint8_t format)
{
	struct osc2_hap_cmd c;

	memset(&c, 0, sizeof(c));
	c.op = OSC2_HAP_OP_FORMAT;
	c.channel = channel;
	c.format = format;
	hap_queue_ok(&c);
	osc2_hap_apply(&hap, osc2_hap_path_of(channel));
}

OPENSC2_TEST(haptics_stream_formats)
{
	hap_fresh();
	CHECK_EQ(osc2_hap_stream_format(&hap, OSC2_HAP_CH_LEFT_PAD), 0xFF);
	/* an 8 kHz format on a pad: 0x10, not set */
	hap_format(OSC2_HAP_CH_LEFT_PAD, 4);
	CHECK_EQ(hap_status_flags(OSC2_HAP_PADS, OSC2_HAP_CH_LEFT_PAD), OSC2_HAP_FLAG_NEED_8K);
	CHECK_EQ(osc2_hap_stream_format(&hap, OSC2_HAP_CH_LEFT_PAD), 0xFF);
	/* s8 at 4 kHz on a pad: 0x20 */
	hap_format(OSC2_HAP_CH_LEFT_PAD, 5);
	CHECK_EQ(hap_status_flags(OSC2_HAP_PADS, OSC2_HAP_CH_LEFT_PAD), OSC2_HAP_FLAG_FORMAT);
	CHECK_EQ(osc2_hap_stream_format(&hap, OSC2_HAP_CH_LEFT_PAD), 5);
	/* formats 12 and above: reported as set, the previous format kept */
	hap_format(OSC2_HAP_CH_LEFT_PAD, 13);
	CHECK_EQ(hap_status_flags(OSC2_HAP_PADS, OSC2_HAP_CH_LEFT_PAD), OSC2_HAP_FLAG_FORMAT);
	CHECK_EQ(osc2_hap_stream_format(&hap, OSC2_HAP_CH_LEFT_PAD), 5);
	/* the water marks: 16 ms of audio in every format; the pads start at 8 ms */
	CHECK_EQ(osc2_hap_water_mark(0), 256);
	CHECK_EQ(osc2_hap_water_mark(1), 128);
	CHECK_EQ(osc2_hap_water_mark(3), 32);
	CHECK_EQ(osc2_hap_water_mark(4), 128);
	CHECK_EQ(osc2_hap_water_mark(11), 16);
	CHECK_EQ(osc2_hap_start_threshold(OSC2_HAP_GRIPS, 0), 256);
	CHECK_EQ(osc2_hap_start_threshold(OSC2_HAP_PADS, 5), 32);
}

OPENSC2_TEST(haptics_stream_flow)
{
	uint8_t bytes[64];
	uint32_t f;

	hap_fresh();
	hap.master_db = 0;
	hap.notes = true;
	for (int i = 0; i < 64; i++) {
		bytes[i] = (uint8_t)(i * 4);
	}
	/* without a format: writes only fill the ring; a full ring reports 0x01 */
	for (int i = 0; i < 8; i++) {
		f = osc2_hap_stream_write(&hap, OSC2_HAP_CH_LEFT_GRIP, bytes, 64);
		CHECK_EQ(f & OSC2_HAP_FLAG_DROPPED, 0);
	}
	CHECK_EQ(osc2_hap_stream_fill(&hap, OSC2_HAP_CH_LEFT_GRIP), 512);
	CHECK_EQ(osc2_hap_stream_write(&hap, OSC2_HAP_CH_LEFT_GRIP, bytes, 1), OSC2_HAP_FLAG_DROPPED);
	CHECK(!osc2_hap_pending(&hap, OSC2_HAP_GRIPS));

	/* s16le at 8 kHz on the left grip (water mark 256 bytes, start above 256 bytes held) */
	hap_fresh();
	hap.master_db = 0;
	hap_format(OSC2_HAP_CH_LEFT_GRIP, 0);
	CHECK_EQ(hap_status_flags(OSC2_HAP_GRIPS, OSC2_HAP_CH_LEFT_GRIP), OSC2_HAP_FLAG_FORMAT);
	for (int i = 0; i < 4; i++) {   /* 256 bytes: not more than the threshold yet */
		CHECK_EQ(osc2_hap_stream_write(&hap, OSC2_HAP_CH_LEFT_GRIP, bytes, 64), 0);
	}
	CHECK(!osc2_hap_pending(&hap, OSC2_HAP_GRIPS));
	/* this one leaves 192 bytes free (< 256): 0x08; and 256 were held before it: not > 256 */
	CHECK_EQ(osc2_hap_stream_write(&hap, OSC2_HAP_CH_LEFT_GRIP, bytes, 64), OSC2_HAP_FLAG_HIGH);
	CHECK(!osc2_hap_pending(&hap, OSC2_HAP_GRIPS));
	/* 320 held before this one: a start is queued */
	CHECK_EQ(osc2_hap_stream_write(&hap, OSC2_HAP_CH_LEFT_GRIP, bytes, 64), OSC2_HAP_FLAG_HIGH);
	CHECK(osc2_hap_pending(&hap, OSC2_HAP_GRIPS));
	/* playing: 384 bytes = 192 samples; 0x04 once below 256 bytes, then dry: 0x02 */
	hap_run(OSC2_HAP_GRIPS, 1, true);             /* 32 lead-in frames, 32 samples taken */
	CHECK_EQ(osc2_hap_stream_fill(&hap, OSC2_HAP_CH_LEFT_GRIP), 384 - 64);
	CHECK_EQ(hap_status_flags(OSC2_HAP_GRIPS, OSC2_HAP_CH_LEFT_GRIP), 0);
	hap_run(OSC2_HAP_GRIPS, 1, false);            /* 64 more: 192 bytes left (< 256) */
	CHECK_EQ(hap_status_flags(OSC2_HAP_GRIPS, OSC2_HAP_CH_LEFT_GRIP), OSC2_HAP_FLAG_LOW);
	hap_run(OSC2_HAP_GRIPS, 1, false);
	CHECK_EQ(hap_status_flags(OSC2_HAP_GRIPS, OSC2_HAP_CH_LEFT_GRIP), 0);   /* only once */
	hap_run(OSC2_HAP_GRIPS, 1, false);            /* the last 32 samples, then dry */
	CHECK_EQ(hap_status_flags(OSC2_HAP_GRIPS, OSC2_HAP_CH_LEFT_GRIP), OSC2_HAP_FLAG_STOPPED);
	CHECK(!osc2_hap_busy(&hap, OSC2_HAP_GRIPS));
	/* the samples come out as written, two bytes each, little endian */
	CHECK_EQ(cap.s[0][32], (int16_t)(uint16_t)(bytes[0] | (bytes[1] << 8)));
	CHECK_EQ(cap.s[0][33], (int16_t)(uint16_t)(bytes[2] | (bytes[3] << 8)));

	/* a format cannot change while the stream plays */
	hap_fresh();
	hap_format(OSC2_HAP_CH_RIGHT_GRIP, 0);
	(void)hap_status_flags(OSC2_HAP_GRIPS, OSC2_HAP_CH_RIGHT_GRIP);
	for (int i = 0; i < 6; i++) {
		(void)osc2_hap_stream_write(&hap, OSC2_HAP_CH_RIGHT_GRIP, bytes, 64);
	}
	hap_run(OSC2_HAP_GRIPS, 1, true);
	hap_format(OSC2_HAP_CH_RIGHT_GRIP, 1);
	CHECK_EQ(hap_status_flags(OSC2_HAP_GRIPS, OSC2_HAP_CH_RIGHT_GRIP), OSC2_HAP_FLAG_BUSY);
	/* a stop empties the ring (the 0x02 reply is the library's, not this engine's) */
	{
		struct osc2_hap_cmd c;

		memset(&c, 0, sizeof(c));
		c.op = OSC2_HAP_OP_STOP;
		c.channel = OSC2_HAP_CH_RIGHT_GRIP;
		c.type = OSC2_HAP_GEN_ALL;
		hap_queue_ok(&c);
		osc2_hap_apply(&hap, OSC2_HAP_GRIPS);
		CHECK_EQ(osc2_hap_stream_fill(&hap, OSC2_HAP_CH_RIGHT_GRIP), 0);
		CHECK(!osc2_hap_busy(&hap, OSC2_HAP_GRIPS));
		CHECK_EQ(hap_status_flags(OSC2_HAP_GRIPS, OSC2_HAP_CH_RIGHT_GRIP), 0);
		CHECK_EQ(osc2_hap_stream_format(&hap, OSC2_HAP_CH_RIGHT_GRIP), 0);   /* stays set */
	}
}

OPENSC2_TEST(haptics_stream_interpolation)
{
	/* s8 at 1 kHz on a grip: each sample and seven steps towards the next */
	uint8_t bytes[40];

	hap_fresh();
	hap.master_db = 0;
	for (int i = 0; i < 40; i++) {
		bytes[i] = (uint8_t)(int8_t)(i % 2 ? -64 : 64);
	}
	hap_format(OSC2_HAP_CH_LEFT_GRIP, 7);         /* s8, 1 kHz */
	(void)hap_status_flags(OSC2_HAP_GRIPS, OSC2_HAP_CH_LEFT_GRIP);
	(void)osc2_hap_stream_write(&hap, OSC2_HAP_CH_LEFT_GRIP, bytes, 20);
	CHECK(!osc2_hap_pending(&hap, OSC2_HAP_GRIPS));   /* nothing held before it */
	(void)osc2_hap_stream_write(&hap, OSC2_HAP_CH_LEFT_GRIP, bytes + 20, 20);
	CHECK(osc2_hap_pending(&hap, OSC2_HAP_GRIPS));    /* 20 > 16 before this write */
	hap_run(OSC2_HAP_GRIPS, 8, true);
	/* 64 * 32767 / 127 = 16512 then the steps down to -16512: eight values per sample */
	CHECK_EQ(cap.s[0][32], 16513);
	CHECK_EQ(cap.s[0][33], 16513 + (-16513 - 16513) * 1 / 8);
	CHECK_EQ(cap.s[0][36], 0);
	CHECK_EQ(cap.s[0][40], -16513);
	/* the 40th sample has nothing to interpolate towards: the stream stops before it */
	CHECK(!osc2_hap_busy(&hap, OSC2_HAP_GRIPS));
	CHECK_EQ(osc2_hap_stream_fill(&hap, OSC2_HAP_CH_LEFT_GRIP), 1);   /* the one looked at */
	CHECK_EQ(hap_status_flags(OSC2_HAP_GRIPS, OSC2_HAP_CH_LEFT_GRIP),
		 OSC2_HAP_FLAG_STOPPED);

	/* mu-law at 8 kHz: one sample per output sample */
	hap_fresh();
	hap.master_db = 0;
	hap_format(OSC2_HAP_CH_LEFT_GRIP, 8);
	memset(bytes, 0x80, sizeof(bytes));           /* +32124 */
	for (int i = 0; i < 5; i++) {                 /* 160 > 128 bytes held before the last */
		(void)osc2_hap_stream_write(&hap, OSC2_HAP_CH_LEFT_GRIP, bytes, 40);
	}
	hap_run(OSC2_HAP_GRIPS, 1, true);
	CHECK_EQ(cap.s[0][32], 32124);
	CHECK_EQ(cap.s[0][63], 32124);
}

OPENSC2_TEST(haptics_stream_high_low)
{
	/* s8 at 4 kHz on the left pad: water mark 64 bytes, start above 32 held */
	uint8_t bytes[64];
	struct osc2_hap_stream *st;

	hap_fresh();
	memset(bytes, 0, sizeof(bytes));
	hap_format(OSC2_HAP_CH_LEFT_PAD, 5);
	CHECK_EQ(hap_status_flags(OSC2_HAP_PADS, OSC2_HAP_CH_LEFT_PAD), OSC2_HAP_FLAG_FORMAT);
	for (int i = 0; i < 7; i++) {
		CHECK_EQ(osc2_hap_stream_write(&hap, OSC2_HAP_CH_LEFT_PAD, bytes, 64), 0);
	}
	CHECK(osc2_hap_pending(&hap, OSC2_HAP_PADS));         /* queued by the second write */
	/* 448 held: the next 40 leave 24 free, less than 64: pause (0x08), and 0x04 is armed */
	CHECK_EQ(osc2_hap_stream_write(&hap, OSC2_HAP_CH_LEFT_PAD, bytes, 40), OSC2_HAP_FLAG_HIGH);
	/* 0x01 and 0x08 never together: a write that does not fit says only 0x01 */
	CHECK_EQ(osc2_hap_stream_write(&hap, OSC2_HAP_CH_LEFT_PAD, bytes, 30), OSC2_HAP_FLAG_DROPPED);
	CHECK_EQ(osc2_hap_stream_fill(&hap, OSC2_HAP_CH_LEFT_PAD), 512);
	/* 512 bytes at 4 kHz: 128 ms. After 14 blocks (16 + 13 * 32 samples) 80 bytes are left;
	 * the 15th block takes it below 64: 0x04, once; the ring runs dry in the 17th: 0x02 */
	hap_run(OSC2_HAP_PADS, 14, true);
	CHECK_EQ(osc2_hap_stream_fill(&hap, OSC2_HAP_CH_LEFT_PAD), 80);
	CHECK_EQ(hap_status_flags(OSC2_HAP_PADS, OSC2_HAP_CH_LEFT_PAD), 0);
	hap_run(OSC2_HAP_PADS, 1, false);
	CHECK_EQ(hap_status_flags(OSC2_HAP_PADS, OSC2_HAP_CH_LEFT_PAD), OSC2_HAP_FLAG_LOW);
	hap_run(OSC2_HAP_PADS, 1, false);
	CHECK_EQ(hap_status_flags(OSC2_HAP_PADS, OSC2_HAP_CH_LEFT_PAD), 0);
	hap_run(OSC2_HAP_PADS, 1, false);
	CHECK_EQ(hap_status_flags(OSC2_HAP_PADS, OSC2_HAP_CH_LEFT_PAD), OSC2_HAP_FLAG_STOPPED);
	CHECK(!osc2_hap_busy(&hap, OSC2_HAP_PADS));
	/* 0x06: the stream runs dry while 0x04 is still due (set up directly: one byte of a 16-bit
	 * format left, the low flag armed) */
	hap_fresh();
	hap_format(OSC2_HAP_CH_RIGHT_GRIP, 0);
	for (int i = 0; i < 6; i++) {
		(void)osc2_hap_stream_write(&hap, OSC2_HAP_CH_RIGHT_GRIP, bytes, 64);
	}
	hap_run(OSC2_HAP_GRIPS, 1, true);
	(void)hap_status_flags(OSC2_HAP_GRIPS, OSC2_HAP_CH_RIGHT_GRIP);
	st = &hap.seq[OSC2_HAP_GRIPS].ch[1].stream;
	st->count = 1;
	st->low_armed = true;
	hap_run(OSC2_HAP_GRIPS, 1, false);
	CHECK_EQ(hap_status_flags(OSC2_HAP_GRIPS, OSC2_HAP_CH_RIGHT_GRIP),
		 OSC2_HAP_FLAG_STOPPED | OSC2_HAP_FLAG_LOW);
	CHECK_EQ(osc2_hap_stream_fill(&hap, OSC2_HAP_CH_RIGHT_GRIP), 1);   /* half a sample stays */
}

/* ---- scripts, cues, drop sounds --------------------------------------------------------------- */

OPENSC2_TEST(haptics_scripts)
{
	struct osc2_hap_player p;
	struct osc2_hap_cmd c;
	uint32_t t = 1000;
	int steps = 0;

	/* every id 1..17 has steps and an end; nothing outside */
	for (uint8_t id = 0; id <= 20; id++) {
		uint8_t count;
		const struct osc2_hap_step *s = osc2_hap_script(id, &count);

		if (id >= 1 && id <= 17) {
			CHECK(s != NULL && count >= 2);
			if (s) {
				CHECK_EQ(s[count - 1].kind, OSC2_HAP_STEP_END);
				for (uint8_t i = 0; i + 1 < count; i++) {
					CHECK(s[i].kind != OSC2_HAP_STEP_END);
				}
			}
		} else {
			CHECK(s == NULL);
		}
	}
	/* the player: one script at a time, steps at their delays */
	osc2_hap_player_init(&p, OSC2_HAP_CH_LEFT_GRIP);
	CHECK_EQ(osc2_hap_player_wait(&p, t), -1);
	CHECK(!osc2_hap_player_start(&p, 0, 0, t));
	CHECK(!osc2_hap_player_start(&p, 18, 0, t));
	CHECK(osc2_hap_player_start(&p, 1, -12, t));
	CHECK(!osc2_hap_player_start(&p, 2, -12, t));        /* already active */
	CHECK_EQ(osc2_hap_player_poll(&p, t, &c), OSC2_HAP_POLL_NONE);
	CHECK_EQ(osc2_hap_player_wait(&p, t), 1);
	CHECK_EQ(osc2_hap_player_poll(&p, t + 1, &c), OSC2_HAP_POLL_STEP);   /* the sweep */
	CHECK_EQ(c.type, OSC2_HAP_GEN_SWEEP);
	CHECK_EQ(c.channel, OSC2_HAP_CH_LEFT_GRIP);
	CHECK_EQ(c.gain_db, -12);
	CHECK_EQ(osc2_hap_player_wait(&p, t + 1), 120);
	CHECK_EQ(osc2_hap_player_poll(&p, t + 120, &c), OSC2_HAP_POLL_NONE);
	CHECK_EQ(osc2_hap_player_poll(&p, t + 121, &c), OSC2_HAP_POLL_STEP);   /* the blip */
	CHECK_EQ(c.type, OSC2_HAP_GEN_TONE);
	CHECK_EQ(c.tone.freq_mhz, 700000u);
	CHECK_EQ(osc2_hap_player_poll(&p, t + 211, &c), OSC2_HAP_POLL_DONE);   /* the end, 90 ms on */
	CHECK(!p.busy);
	CHECK(osc2_hap_player_start(&p, 2, -12, t + 300));
	/* gains add without wrapping round: -128 + a positive offset stays very quiet */
	osc2_hap_player_init(&p, OSC2_HAP_CH_RIGHT_GRIP);
	CHECK(osc2_hap_player_start(&p, 11, -128, t));    /* the thump: +2 dB in the script */
	CHECK_EQ(osc2_hap_player_poll(&p, t + 1, &c), OSC2_HAP_POLL_STEP);
	CHECK_EQ(c.gain_db, -126);
	/* script 13's pulses play at full scale whatever the gain */
	osc2_hap_player_init(&p, OSC2_HAP_CH_LEFT_PAD);
	CHECK(osc2_hap_player_start(&p, 13, -20, t));
	while (osc2_hap_player_poll(&p, t, &c) != OSC2_HAP_POLL_DONE && steps < 10) {
		if (p.busy && c.type == OSC2_HAP_GEN_PULSE) {
			CHECK_EQ(c.gain_db, 0);
		}
		t += 1;
		steps++;
	}
	/* the drop sounds are a second or so long */
	{
		uint8_t count;
		const struct osc2_hap_step *s16 = osc2_hap_script(16, &count);
		uint32_t total = 0;

		for (uint8_t i = 0; i < count; i++) {
			total += s16[i].delay_ms;
		}
		CHECK(total >= 1000 && total <= 1300);
	}
}

OPENSC2_TEST(haptics_cues)
{
	uint8_t script = 0;
	int8_t gain = 0;
	static const uint8_t want_script[11] = { 1, 1, 2, 1, 2, 5, 8, 9, 10, 3, 4 };

	for (uint8_t cue = 0; cue < 11; cue++) {
		CHECK(osc2_hap_cue(cue, 2, &script, &gain));
		CHECK_EQ(script, want_script[cue]);
		CHECK_EQ(gain, cue == 8 ? 2 : cue == 6 || cue == 7 ? -13 : -12);
		CHECK(osc2_hap_cue(cue, 1, &script, &gain));
		CHECK_EQ(gain, cue == 8 ? 2 : cue == 6 || cue == 7 ? -19 : -18);
		CHECK_EQ(osc2_hap_cue(cue, 0, &script, &gain), cue == 8);   /* silent but cue 8 */
		CHECK_EQ(osc2_hap_cue(cue, 3, &script, &gain), cue == 8);
	}
	CHECK(!osc2_hap_cue(11, 2, &script, &gain));
	CHECK(!osc2_hap_cue(255, 2, &script, &gain));
}

OPENSC2_TEST(haptics_drop)
{
	struct osc2_hap_drop d;
	int n16 = 0, n17 = 0;

	osc2_hap_drop_init(&d);
	CHECK_EQ(osc2_hap_drop_wait(&d, 0), -1);
	CHECK(osc2_hap_drop_fall(&d, 1000));
	CHECK(!osc2_hap_drop_fall(&d, 1100));                 /* not again until re-armed */
	CHECK_EQ(osc2_hap_drop_wait(&d, 1000), 250);
	CHECK_EQ(osc2_hap_drop_poll(&d, 1249), OSC2_HAP_DROP_NOTHING);
	CHECK_EQ(osc2_hap_drop_poll(&d, 1250), OSC2_HAP_DROP_PLAY);
	CHECK_EQ(osc2_hap_drop_wait(&d, 1250), 4750);
	CHECK(!osc2_hap_drop_fall(&d, 3000));
	CHECK_EQ(osc2_hap_drop_poll(&d, 5999), OSC2_HAP_DROP_NOTHING);
	CHECK_EQ(osc2_hap_drop_poll(&d, 6000), OSC2_HAP_DROP_REARMED);
	CHECK(osc2_hap_drop_fall(&d, 6001));
	/* the pick: 48 of 256 values script 16, 80 script 17, the rest nothing */
	for (int r = 0; r < 256; r++) {
		uint8_t s = osc2_hap_drop_pick((uint8_t)r);

		n16 += s == 16;
		n17 += s == 17;
		CHECK(s == 0 || s == 16 || s == 17);
	}
	CHECK_EQ(n16, 48);
	CHECK_EQ(n17, 80);
	CHECK_EQ(osc2_hap_drop_pick(47), 16);
	CHECK_EQ(osc2_hap_drop_pick(48), 17);
	CHECK_EQ(osc2_hap_drop_pick(127), 17);
	CHECK_EQ(osc2_hap_drop_pick(128), 0);
	/* the clock may wrap */
	osc2_hap_drop_init(&d);
	CHECK(osc2_hap_drop_fall(&d, 0xFFFFFF00u));
	CHECK_EQ(osc2_hap_drop_poll(&d, 0xFFFFFF00u + 250u), OSC2_HAP_DROP_PLAY);
}

/* ---- haptics.txt: rendered blocks the board must reproduce ------------------------------------
 *   RENDER <name> PATH <0|1> SEED <hex> MASTER <dB> S27 <dB> BLOCKS <n> OUT <report hex> CRC <hex>...
 * From a fresh engine: the rumble seed, the master gain (setting 76), setting 27, the report
 * decoded and its commands queued, `n` blocks of PATH rendered (the first with the lead-in), each
 * block's CRC-32. Something must still play before each block: the board's sequencer stops
 * rendering after the block in which the last sound ended. */

static const char *const builtin_haptics[] = {
	"RENDER tone200 PATH 1 SEED 14cb MASTER -3 S27 -10 BLOCKS 3 OUT 8303fac8006400000000 "
	"CRC ab9de1fd 6bbe0906 dfc8e7cc",
};

static void haptics_line(char *tok[], int n, void *ctx)
{
	uint8_t report[64], block[OSC2_HAP_BLOCK_MAX_BYTES];
	struct opensc2_output o[2];
	struct osc2_hap_actions a;
	int i_path = vec_find(tok, n, "PATH"), i_seed = vec_find(tok, n, "SEED");
	int i_master = vec_find(tok, n, "MASTER"), i_s27 = vec_find(tok, n, "S27");
	int i_blocks = vec_find(tok, n, "BLOCKS"), i_out = vec_find(tok, n, "OUT");
	int i_crc = vec_find(tok, n, "CRC");
	int len, outs, blocks;
	uint8_t path;
	char got[160];
	size_t at = 0;
	bool same = true;

	(void)ctx;
	if (strcmp(tok[0], "RENDER") != 0) {
		CHECK(false);
		return;
	}
	CHECK(i_path > 0 && i_seed > 0 && i_master > 0 && i_s27 > 0 && i_blocks > 0 && i_out > 0 &&
	      i_crc > i_out);
	if (i_crc <= i_out || i_path < 0 || i_blocks < 0 || i_seed < 0 || i_master < 0 ||
	    i_s27 < 0) {
		return;
	}
	path = (uint8_t)atoi(tok[i_path + 1]);
	blocks = atoi(tok[i_blocks + 1]);
	hap_fresh();
	osc2_hap_seed(&hap, (uint32_t)strtoul(tok[i_seed + 1], NULL, 16));
	hap.master_db = (int16_t)atoi(tok[i_master + 1]);
	i_out++;
	len = vec_hex(tok, n, &i_out, report, sizeof(report));
	CHECK(len > 0);
	outs = opensc2_decode_output(report, (size_t)(len > 0 ? len : 0), o);
	CHECK(outs >= 1);
	for (int k = outs - 1; k >= 0; k--) {
		/* 0x88's right grip first, as the synthesiser feeds it */
		int idx = report[0] == 0x88 ? k : outs - 1 - k;

		osc2_hap_actions(&o[idx], (int16_t)atoi(tok[i_s27 + 1]), &a);
		for (uint8_t c = 0; c < a.n_cmd; c++) {
			(void)osc2_hap_queue(&hap, &a.cmd[c]);
		}
		for (uint8_t w = 0; w < a.n_write; w++) {
			(void)osc2_hap_stream_write(&hap, a.write[w].channel, a.write[w].data,
						    a.write[w].len);
		}
	}
	CHECK(blocks >= 1 && blocks <= 13 && i_crc + blocks < n + 1);
	for (int b = 0; b < blocks && i_crc + 1 + b < n; b++) {
		size_t bytes;
		uint32_t crc;

		osc2_hap_apply(&hap, path);
		/* The board's sequencer renders only while something plays: no block past the end. */
		CHECK(osc2_hap_busy(&hap, path));
		bytes = osc2_hap_render(&hap, path, b == 0, block);
		crc = osc2_crc32(0, block, bytes);
		at += (size_t)snprintf(&got[at], sizeof(got) - at, " %08lx", (unsigned long)crc);
		if (crc != (uint32_t)strtoul(tok[i_crc + 1 + b], NULL, 16)) {
			same = false;
		}
	}
	CHECK(same);
	if (!same) {
		printf("  rendered: CRC%s\n", got);
	}
}

OPENSC2_TEST(haptics_vectors)
{
	vec_run_builtin(builtin_haptics, sizeof(builtin_haptics) / sizeof(builtin_haptics[0]),
			haptics_line, NULL);
	(void)vec_run_file("haptics.txt", haptics_line, NULL);
}

/* ---- haptics_run.c: the synthesiser around its engine ------------------------------------------ */

static struct osc2_hap_rt hrt;

static int hrt_items(uint8_t what, uint8_t kind, struct osc2_hap_out_item *out, int max)
{
	struct osc2_hap_out_item it[OSC2_HAP_OUTBOX];
	uint8_t n = osc2_hap_rt_take(&hrt, it, OSC2_HAP_OUTBOX);
	int found = 0;

	for (uint8_t i = 0; i < n; i++) {
		if (it[i].what == what && (kind == 0 || it[i].kind == kind)) {
			if (found < max) {
				out[found] = it[i];
			}
			found++;
		}
	}
	return found;
}

static uint8_t hrt_feed(const uint8_t *report, size_t len, uint32_t now)
{
	struct opensc2_output o[2];
	int n = opensc2_decode_output(report, len, o);

	CHECK(n >= 1);
	return osc2_hap_rt_feed(&hrt, o, n, -10, now);
}

OPENSC2_TEST(haptics_run_feed_and_capture)
{
	static const uint8_t tone[10] = { 0x83, 3, 0xFA, 200, 0, 100, 0, 0, 0, 0 };
	uint8_t block[OSC2_HAP_BLOCK_MAX_BYTES], args[2] = { OSC2_HAP_GRIPS, 2 };
	uint8_t result[OSC2_HAP_STATUS_LEN], rlen, wake;
	struct osc2_hap_rt_glue glue = { 0 };
	struct osc2_hap_out_item it[8];
	bool busy, schedule;
	size_t bytes;

	osc2_hap_rt_init(&hrt);
	hrt.notes = true;
	CHECK_EQ(osc2_hap_rt_test_op(&hrt, OSC2_HAP_OP_CAPTURE, args, 2, 0, &glue, result, &rlen,
				     &wake, &schedule), 0);
	CHECK_EQ(hrt_feed(tone, sizeof(tone), 0), 2);              /* the grips have work */
	CHECK(osc2_hap_rt_idle(&hrt, OSC2_HAP_GRIPS));            /* applied: it plays */
	CHECK_EQ(hrt_items(OSC2_HAP_OUT_NOTE, OSC2_HAP_RING_GEN, it, 8), 1);
	CHECK_EQ(it[0].data[0], OSC2_HAP_CH_LEFT_GRIP);
	CHECK_EQ(it[0].data[1], OSC2_HAP_GEN_TONE);
	CHECK_EQ(it[0].data[2], OSC2_HAP_GEN_EV_START);
	CHECK_EQ((int8_t)it[0].data[3], -6);
	CHECK_EQ(opensc2_get_le16(&it[0].data[4]), 200);
	bytes = osc2_hap_rt_block(&hrt, OSC2_HAP_GRIPS, true, block, &busy);
	CHECK_EQ(bytes, 256);
	CHECK(busy);
	CHECK_EQ(hrt_items(OSC2_HAP_OUT_NOTE, OSC2_HAP_RING_BLOCK, it, 8), 1);
	CHECK_EQ(it[0].len, 12);
	CHECK_EQ(it[0].data[0], OSC2_HAP_GRIPS);
	CHECK_EQ(opensc2_get_le32(&it[0].data[1]), 0);                          /* index */
	CHECK_EQ(opensc2_get_le32(&it[0].data[5]), osc2_crc32(0, block, bytes));
	CHECK_EQ(opensc2_get_le16(&it[0].data[9]), osc2_hap_block_peak(OSC2_HAP_GRIPS, block));
	CHECK_EQ(it[0].data[11], OSC2_HAP_BLOCK_FIRST);
	(void)osc2_hap_rt_block(&hrt, OSC2_HAP_GRIPS, false, block, &busy);
	CHECK_EQ(hrt_items(OSC2_HAP_OUT_NOTE, OSC2_HAP_RING_BLOCK, it, 8), 1);
	CHECK_EQ(opensc2_get_le32(&it[0].data[1]), 1);
	(void)osc2_hap_rt_block(&hrt, OSC2_HAP_GRIPS, false, block, &busy);
	CHECK_EQ(hrt_items(OSC2_HAP_OUT_NOTE, OSC2_HAP_RING_BLOCK, it, 8), 0);   /* two captured */
	/* without the hooks armed nothing is noted; the statuses still go out */
	hrt.notes = false;
	{
		static const uint8_t fmt[4] = { 0x86, 2, 0, 1 };   /* s16le at 4 kHz, the left grip */

		CHECK_EQ(hrt_feed(fmt, sizeof(fmt), 0), 2);
		CHECK(osc2_hap_rt_idle(&hrt, OSC2_HAP_GRIPS));
		CHECK_EQ(hrt_items(OSC2_HAP_OUT_STATUS, 0, it, 8), 1);
		CHECK_EQ(it[0].data[0], 0);                  /* the left grip's id */
		CHECK_EQ(opensc2_get_le32(&it[0].data[1]), OSC2_HAP_FLAG_FORMAT);
	}
}

OPENSC2_TEST(haptics_run_stream_writes)
{
	uint8_t report[64] = { 0x87, 0 };   /* the left grip */
	struct osc2_hap_out_item it[8];
	static const uint8_t fmt[4] = { 0x86, 2, 0, 0 };   /* s16le at 8 kHz */
	uint8_t wake = 0;

	osc2_hap_rt_init(&hrt);
	CHECK_EQ(hrt_feed(fmt, sizeof(fmt), 0), 2);
	(void)osc2_hap_rt_idle(&hrt, OSC2_HAP_GRIPS);
	(void)hrt_items(OSC2_HAP_OUT_STATUS, 0, it, 8);
	/* 62 bytes a report: the fifth leaves 202 free (< 256), 0x08 at once, and with 248 held
	 * before it starts nothing; the sixth, with 310 held before it, queues the start */
	for (int i = 0; i < 4; i++) {
		wake |= hrt_feed(report, sizeof(report), 0);
	}
	CHECK_EQ(wake, 0);
	CHECK_EQ(hrt_items(OSC2_HAP_OUT_STATUS, 0, it, 8), 0);
	CHECK_EQ(hrt_feed(report, sizeof(report), 0), 0);         /* 248 held before: not > 256 */
	CHECK_EQ(hrt_items(OSC2_HAP_OUT_STATUS, 0, it, 8), 1);
	CHECK_EQ(opensc2_get_le32(&it[0].data[1]), OSC2_HAP_FLAG_HIGH);
	CHECK_EQ(hrt_feed(report, sizeof(report), 0), 2);         /* 310 held: the start, queued */
	CHECK(osc2_hap_rt_idle(&hrt, OSC2_HAP_GRIPS));
}

OPENSC2_TEST(haptics_run_write_order)
{
	/* 0x88 writes the right grip's bytes before the left grip's, and two-channel targets of
	 * 0x87 take the right channel first: their 0x44 flags go out in that order */
	uint8_t r88[64] = { 0x88, 31 }, r87[64] = { 0x87, 5 };   /* 0x87 target 5: both pads */
	struct osc2_hap_out_item it[8];
	const struct osc2_hap_stream *lg, *rg;

	memset(&r88[2], 0x11, 31);               /* the left grip's bytes, [1..31] of the payload */
	memset(&r88[33], 0x22, 31);              /* the right grip's, [32..62] */
	osc2_hap_rt_init(&hrt);
	/* no format: the rings only fill; less than 256 bytes free (format 0's water mark): 0x08 */
	for (int i = 0; i < 8; i++) {
		(void)hrt_feed(r88, sizeof(r88), 0);
	}
	CHECK_EQ(hrt_items(OSC2_HAP_OUT_STATUS, 0, it, 8), 0);   /* 248 bytes each */
	(void)hrt_feed(r88, sizeof(r88), 0);
	CHECK_EQ(hrt_items(OSC2_HAP_OUT_STATUS, 0, it, 8), 2);
	CHECK_EQ(it[0].data[0], 1);                             /* the right grip's id first */
	CHECK_EQ(opensc2_get_le32(&it[0].data[1]), OSC2_HAP_FLAG_HIGH);
	CHECK_EQ(it[1].data[0], 0);
	CHECK_EQ(opensc2_get_le32(&it[1].data[1]), OSC2_HAP_FLAG_HIGH);
	lg = &hrt.engine.seq[OSC2_HAP_GRIPS].ch[0].stream;
	rg = &hrt.engine.seq[OSC2_HAP_GRIPS].ch[1].stream;
	CHECK_EQ(lg->count, 279);
	CHECK_EQ(rg->count, 279);
	CHECK_EQ(lg->buf[0], 0x11);
	CHECK_EQ(rg->buf[0], 0x22);
	/* both pads: the right pad's id (4) before the left pad's (3) */
	for (int i = 0; i < 4; i++) {
		(void)hrt_feed(r87, sizeof(r87), 0);
	}
	CHECK_EQ(hrt_items(OSC2_HAP_OUT_STATUS, 0, it, 8), 0);   /* 248 bytes each */
	(void)hrt_feed(r87, sizeof(r87), 0);
	CHECK_EQ(hrt_items(OSC2_HAP_OUT_STATUS, 0, it, 8), 2);
	CHECK_EQ(it[0].data[0], 4);
	CHECK_EQ(it[1].data[0], 3);
}

OPENSC2_TEST(haptics_run_scripts_cues_drops)
{
	struct osc2_hap_out_item it[16];
	struct osc2_hap_rt_glue glue = { .features = 1, .amp_mode = 1, .outputs = 2 };
	uint8_t result[OSC2_HAP_STATUS_LEN], rlen, wake;
	uint8_t play_cue[3] = { OSC2_HAP_PLAY_CUE, 0, 2 }, fall[2] = { OSC2_HAP_PLAY_FALL, 40 };
	uint8_t seed[4] = { 0x78, 0x56, 0x34, 0x12 };
	bool schedule;
	uint32_t t = 5000;

	osc2_hap_rt_init(&hrt);
	hrt.notes = true;
	/* cue 0 at level 2: script 1 on both grips at -12 dB */
	CHECK_EQ(osc2_hap_rt_test_op(&hrt, OSC2_HAP_OP_PLAY, play_cue, 3, t, &glue, result, &rlen,
				     &wake, &schedule), 0);
	CHECK_EQ(rlen, 4);
	CHECK_EQ((int32_t)opensc2_get_le32(result), 2);
	CHECK(schedule);
	CHECK_EQ(osc2_hap_rt_next(&hrt, t), 1);
	CHECK_EQ(hrt_items(OSC2_HAP_OUT_NOTE, OSC2_HAP_RING_SCRIPT, it, 16), 3);   /* cue, 2 starts */
	CHECK_EQ(it[0].data[0], OSC2_HAP_SCRIPT_EV_CUE);
	CHECK_EQ(it[0].data[2], 0);
	CHECK_EQ((int8_t)it[0].data[3], -12);
	CHECK_EQ(opensc2_get_le16(&it[0].data[4]), 1);
	CHECK_EQ(it[1].data[0], OSC2_HAP_SCRIPT_EV_START);
	CHECK_EQ(it[1].data[1], OSC2_HAP_CH_LEFT_GRIP);
	CHECK_EQ(it[2].data[1], OSC2_HAP_CH_RIGHT_GRIP);
	/* again while it runs: both grips refuse */
	CHECK_EQ(osc2_hap_rt_cue(&hrt, 3, 2, t), 0);
	CHECK_EQ(osc2_hap_rt_poll(&hrt, t + 1, 0), 2);            /* the first steps, the grips */
	CHECK(osc2_hap_rt_idle(&hrt, OSC2_HAP_GRIPS));
	/* a silent level plays nothing */
	CHECK_EQ(osc2_hap_rt_cue(&hrt, 0, 0, t), -1);
	/* the status */
	CHECK_EQ(osc2_hap_rt_test_op(&hrt, OSC2_HAP_OP_STATUS, NULL, 0, t, &glue, result, &rlen,
				     &wake, &schedule), 0);
	CHECK_EQ(rlen, OSC2_HAP_STATUS_LEN);
	CHECK_EQ(result[0], 1);
	CHECK_EQ(result[1], 2);                                    /* the grips play */
	CHECK_EQ(result[2], 2);
	CHECK_EQ(result[3], 1);
	CHECK_EQ((int8_t)result[4], -3);
	CHECK_EQ(result[5], 0x0C);                                 /* both grips' players */
	CHECK_EQ(result[22 + OSC2_HAP_CH_LEFT_GRIP], 1u << OSC2_HAP_GEN_SWEEP);
	CHECK_EQ(result[34], 0xFF);                                /* no format */
	/* the reset stops everything, reseeds, frees the players */
	CHECK_EQ(osc2_hap_rt_test_op(&hrt, OSC2_HAP_OP_RESET, seed, 4, t, &glue, result, &rlen,
				     &wake, &schedule), 0);
	CHECK_EQ(hrt.engine.lcg, 0x12345678u);
	CHECK(!osc2_hap_busy(&hrt.engine, OSC2_HAP_GRIPS));
	CHECK(!hrt.player[OSC2_HAP_CH_LEFT_GRIP].busy);
	CHECK_EQ(osc2_hap_rt_next(&hrt, t), -1);
	/* a free fall with a forced byte (40: drop sound 1), 250 ms later on the left grip */
	CHECK_EQ(osc2_hap_rt_test_op(&hrt, OSC2_HAP_OP_PLAY, fall, 2, t, &glue, result, &rlen,
				     &wake, &schedule), 0);
	CHECK_EQ((int32_t)opensc2_get_le32(result), 1);
	CHECK_EQ(osc2_hap_rt_next(&hrt, t), 250);
	(void)hrt_items(OSC2_HAP_OUT_NOTE, OSC2_HAP_RING_SCRIPT, it, 16);
	CHECK_EQ(osc2_hap_rt_poll(&hrt, t + 250, 0xFF), 0);       /* the forced byte, not 0xFF */
	CHECK_EQ(hrt_items(OSC2_HAP_OUT_NOTE, OSC2_HAP_RING_SCRIPT, it, 16), 2);
	CHECK_EQ(it[0].data[0], OSC2_HAP_SCRIPT_EV_DROP);
	CHECK_EQ(it[0].data[2], 16);
	CHECK_EQ(opensc2_get_le16(&it[0].data[4]), 40);
	CHECK_EQ(it[1].data[0], OSC2_HAP_SCRIPT_EV_START);
	CHECK_EQ(it[1].data[1], OSC2_HAP_CH_LEFT_GRIP);
	CHECK_EQ(it[1].data[2], 16);
	/* a second fall before the re-arm is ignored; at 5 s falls count again */
	CHECK(!osc2_hap_rt_fall(&hrt, t + 1000));
	CHECK_EQ(osc2_hap_rt_poll(&hrt, t + 1001, 0) & 2, 2);     /* the drop sound's steps */
	for (uint32_t ms = t + 1002; ms <= t + 5000; ms++) {
		(void)osc2_hap_rt_poll(&hrt, ms, 0);
	}
	CHECK(osc2_hap_rt_fall(&hrt, t + 5001));
	/* the system's byte when none is forced: 200 plays nothing */
	(void)hrt_items(OSC2_HAP_OUT_NOTE, OSC2_HAP_RING_SCRIPT, it, 16);
	(void)osc2_hap_rt_poll(&hrt, t + 5251, 200);
	CHECK_EQ(hrt_items(OSC2_HAP_OUT_NOTE, OSC2_HAP_RING_SCRIPT, it, 16), 1);
	CHECK_EQ(it[0].data[2], 0);
	/* bad requests */
	CHECK_EQ(osc2_hap_rt_test_op(&hrt, OSC2_HAP_OP_PLAY, play_cue, 2, t, &glue, result, &rlen,
				     &wake, &schedule), 2);
	play_cue[0] = 9;
	CHECK_EQ(osc2_hap_rt_test_op(&hrt, OSC2_HAP_OP_PLAY, play_cue, 3, t, &glue, result, &rlen,
				     &wake, &schedule), 4);
	CHECK_EQ(osc2_hap_rt_test_op(&hrt, 0x84, NULL, 0, t, &glue, result, &rlen, &wake,
				     &schedule), 1);
}

OPENSC2_TEST(haptics_helpers)
{
	struct opensc2_freefall ff;
	int16_t still[3] = { 0, 0, 16384 }, falling[3] = { 300, -200, 500 };
	uint8_t pads[64], grips[256];
	bool fell = false;

	/* a block's level: the louder channel's peak, 0..255 */
	memset(pads, 128, sizeof(pads));
	CHECK_EQ(opensc2_haptics_block_level(OPENSC2_HAPTICS_PADS, pads, 32), 0);
	pads[5] = 255;
	CHECK_EQ(opensc2_haptics_block_level(OPENSC2_HAPTICS_PADS, pads, 32), 255);
	memset(grips, 0, sizeof(grips));
	grips[10] = 0x00;
	grips[11] = 0xC0;                       /* -16384 */
	CHECK_EQ(opensc2_haptics_block_level(OPENSC2_HAPTICS_GRIPS, grips, 64), 127);
	/* the free-fall detector: 150 ms below 0.35 g, once, then above 0.6 g again */
	opensc2_freefall_init(&ff);
	CHECK(!opensc2_freefall_update(&ff, still, 0));
	for (uint32_t t = 10; t <= 300; t += 10) {
		bool f = opensc2_freefall_update(&ff, falling, t);

		CHECK(!(f && fell));                 /* only once */
		if (f) {
			CHECK_EQ(t, 160);               /* the first low sample at 10 ms, + 150 ms */
		}
		fell |= f;
	}
	CHECK(fell);
	CHECK(!opensc2_freefall_update(&ff, falling, 400));
	CHECK(!opensc2_freefall_update(&ff, still, 410));    /* landed: armed again */
	CHECK(!opensc2_freefall_update(&ff, falling, 420));
	CHECK(opensc2_freefall_update(&ff, falling, 570));
}

/* The system state machine's cue events and the synthesiser that plays them (system.c's
 * play_cue() hands each cue to opensc2_haptics_play_cue()) map every cue and chime level alike. */
OPENSC2_TEST(haptics_cue_map_is_the_machines)
{
	CHECK_EQ(OSC2_HAP_CUE_COUNT, OSC2_CUE_COUNT);
	for (uint8_t cue = 0; cue <= OSC2_CUE_COUNT; cue++) {
		for (uint8_t level = 0; level <= 3; level++) {
			uint8_t s1 = 0, s2 = 0;
			int8_t g1 = 0, g2 = 0;
			bool a = osc2_cue_map(cue, level, &s1, &g1);
			bool b = osc2_hap_cue(cue, level, &s2, &g2);

			CHECK_EQ(a, b);
			if (a && b) {
				CHECK_EQ(s1, s2);
				CHECK_EQ(g1, g2);
			}
		}
	}
}
