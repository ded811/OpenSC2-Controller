/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * haptics_dsp.c: the synthesiser's integer arithmetic (haptics_dsp.h). Pure C.
 */
#include "haptics_dsp.h"

#define HALF_CYCLE    (OSC2_HAP_CYCLE / 2)
#define QUARTER_CYCLE (OSC2_HAP_CYCLE / 4)

/* The polynomial's coefficients as the real controller rounds them, -0.166666 and 0.008333 (in
 * single precision), in Q30 and in Q16. */
#define C3_Q30 (-178956256LL)
#define C5_Q30 8947491LL
#define C3_Q16 (-10923LL)
#define C5_Q16 546LL
#define HALF_PI_Q30 1686629713LL      /* pi / 2: Q32 cycles to Q30 radians */
#define TWO_PI_Q16 411775LL           /* 2 pi: Q32 cycles to Q16 radians (with >> 32) */

/* x (1 + x^2 (c3 + x^2 c5)) for x = t * 2 pi, t in Q32 cycles of either sign: Q15. Within the
 * first quarter (and a little beyond) in Q30; up to 10 cycles in Q16; past that the fifth power
 * has left every gain behind. */
static int64_t poly(int64_t t)
{
	int64_t a = t < 0 ? -t : t;

	if (a <= OSC2_HAP_CYCLE / 3) {
		int64_t x = (t * HALF_PI_Q30) >> 30;               /* radians, Q30, |x| < 2.1 */
		int64_t x2 = (x * x) >> 30;
		int64_t inner = C3_Q30 + ((x2 * C5_Q30) >> 30);
		int64_t p = (x * ((1LL << 30) + ((x2 * inner) >> 30))) >> 30;

		return p >= 0 ? (p + (1 << 14)) >> 15 : -((-p + (1 << 14)) >> 15);
	}
	if (a <= 10 * OSC2_HAP_CYCLE) {
		int64_t x = (t * TWO_PI_Q16) >> 32;                /* radians, Q16, |x| < 63 */
		int64_t x2 = (x * x) >> 16;
		int64_t inner = C3_Q16 + ((x2 * C5_Q16) >> 16);
		int64_t p = (x * ((1LL << 16) + ((x2 * inner) >> 16))) >> 16;

		return osc2_hap_sat(p / 2);
	}
	return t < 0 ? -OSC2_HAP_HUGE : OSC2_HAP_HUGE;
}

int64_t osc2_hap_sinp(int64_t x)
{
	/* One cycle at most either way, then the quarter by symmetry (the real one's comparisons:
	 * strictly above 2 pi, below 0, above pi, below pi / 2). */
	if (x > OSC2_HAP_CYCLE) {
		x -= OSC2_HAP_CYCLE;
	}
	if (x < 0) {
		x += OSC2_HAP_CYCLE;
	}
	if (x > HALF_CYCLE) {
		x -= HALF_CYCLE;
		return x < QUARTER_CYCLE ? -poly(x) : -poly(HALF_CYCLE - x);
	}
	return x < QUARTER_CYCLE ? poly(x) : poly(HALF_CYCLE - x);
}

uint32_t osc2_hap_gain_q16_db(int32_t db)
{
	if (db < OSC2_HAP_GAIN_MIN_DB) {
		db = OSC2_HAP_GAIN_MIN_DB;
	} else if (db > OSC2_HAP_GAIN_MAX_DB) {
		db = OSC2_HAP_GAIN_MAX_DB;
	}
	return osc2_hap_gain_q16[db - OSC2_HAP_GAIN_MIN_DB];
}

int8_t osc2_hap_db_add(int32_t a, int32_t b)
{
	return (int8_t)(uint8_t)(uint32_t)(a + b);
}

int32_t osc2_hap_log2(uint32_t v)
{
	int32_t msb = 31;
	uint32_t m, index, frac;
	int32_t a, b;

	if (v == 0) {
		return 0;
	}
	while (!(v & (1u << msb))) {
		msb--;
	}
	/* The mantissa 1.xxx as 16 bits of fraction: 6 for the table, 10 to interpolate. */
	m = msb >= 16 ? (v >> (msb - 16)) : (v << (16 - msb));
	m &= 0xFFFFu;
	index = m >> 10;
	frac = m & 0x3FFu;
	a = (int32_t)osc2_hap_log2_q16[index];
	b = (int32_t)osc2_hap_log2_q16[index + 1];
	return msb * 65536 + a + (int32_t)(((int64_t)(b - a) * (int32_t)frac) >> 10);
}

uint64_t osc2_hap_scale_exp2(uint64_t v, int32_t e)
{
	/* e = whole * 65536 + f with 0 <= f < 65536 (whole rounded down, also for negative e) */
	int32_t whole = e >= 0 ? e / 65536 : -((65535 - e) / 65536);
	uint32_t f = (uint32_t)(e - whole * 65536);
	uint32_t index = f >> 10;
	uint32_t frac = f & 0x3FFu;
	uint32_t a = osc2_hap_exp2_q16[index];
	uint32_t b = osc2_hap_exp2_q16[index + 1];
	uint64_t m = a + (((uint64_t)(b - a) * frac) >> 10);   /* 2^(f / 65536), Q16 */
	uint64_t r;

	if (v > UINT64_MAX / m) {
		return INT64_MAX;
	}
	r = (v * m) >> 16;
	if (whole >= 0) {
		if (whole >= 63 || r > ((uint64_t)INT64_MAX >> whole)) {
			return INT64_MAX;
		}
		return r << whole;
	}
	return whole <= -64 ? 0 : r >> -whole;
}

int64_t osc2_hap_phase_step(int64_t freq_mhz, uint32_t rate)
{
	uint64_t f = (uint64_t)(freq_mhz < 0 ? -freq_mhz : freq_mhz);
	uint64_t d = 1000u * (uint64_t)rate;
	int64_t step;

	if (rate == 0) {
		return 0;
	}
	/* 2^32 * f / (1000 * rate), rounded; the frequency's whole hertz and its fraction apart, so
	 * any 32-bit frequency fits. */
	step = (int64_t)((f / d) << 32) + (int64_t)((((f % d) << 32) + d / 2) / d);
	return freq_mhz < 0 ? -step : step;
}

int16_t osc2_hap_mulaw(uint8_t u)
{
	uint8_t mu = (uint8_t)~u;
	int32_t exponent = (mu >> 4) & 0x07;
	int32_t mantissa = mu & 0x0F;
	int32_t magnitude = (((mantissa << 3) + 0x84) << exponent) - 0x84;

	return (int16_t)(mu & 0x80 ? -magnitude : magnitude);
}

/* v * 32768 / d, rounded half away from zero. */
static int32_t scale(int32_t v, int32_t d)
{
	int32_t m = v < 0 ? -v : v;
	int32_t r = (m * OSC2_HAP_ONE_Q15 + d / 2) / d;

	return v < 0 ? -r : r;
}

int32_t osc2_hap_pcm_sample(const uint8_t *p, uint8_t enc)
{
	switch (enc) {
	case OSC2_HAP_ENC_S16LE:
		return scale((int16_t)(uint16_t)(p[0] | (p[1] << 8)), 32767);
	case OSC2_HAP_ENC_S8:
		return scale((int8_t)p[0], 127);
	default:
		return scale(osc2_hap_mulaw(p[0]), 32767);
	}
}
