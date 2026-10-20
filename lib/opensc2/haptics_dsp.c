/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * haptics_dsp.c: the synthesiser's integer arithmetic (haptics_dsp.h). Pure C.
 */
#include "haptics_dsp.h"

int32_t osc2_hap_sin(uint32_t phase)
{
	uint32_t quadrant = phase >> 30;
	uint32_t index = (phase >> 22) & 0xFFu;
	int32_t frac = (int32_t)((phase >> 6) & 0xFFFFu);
	int32_t a, b, v;

	if (quadrant & 1u) {
		/* The second and fourth quarters run the table backwards. */
		a = osc2_hap_sine_q15[256 - index];
		b = osc2_hap_sine_q15[255 - index];
	} else {
		a = osc2_hap_sine_q15[index];
		b = osc2_hap_sine_q15[index + 1];
	}
	v = a + (int32_t)(((int64_t)(b - a) * frac) >> 16);
	return quadrant & 2u ? -v : v;
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
	int32_t s = a + b;

	return (int8_t)(s > 127 ? 127 : s < -128 ? -128 : s);
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

uint32_t osc2_hap_phase_step(uint32_t freq_mhz, uint32_t rate)
{
	if (rate == 0) {
		return 0;
	}
	/* 2^32 * f / (1000 * rate), rounded; the cast keeps it modulo one cycle. */
	return (uint32_t)((((uint64_t)freq_mhz << 32) + 500u * rate) / (1000u * (uint64_t)rate));
}

int16_t osc2_hap_mulaw(uint8_t u)
{
	uint8_t mu = (uint8_t)~u;
	int32_t exponent = (mu >> 4) & 0x07;
	int32_t mantissa = mu & 0x0F;
	int32_t magnitude = (((mantissa << 3) + 0x84) << exponent) - 0x84;

	return (int16_t)(mu & 0x80 ? -magnitude : magnitude);
}

int32_t osc2_hap_pcm_sample(const uint8_t *p, uint8_t enc)
{
	int32_t v;

	switch (enc) {
	case OSC2_HAP_ENC_S16LE:
		v = (int16_t)(uint16_t)(p[0] | (p[1] << 8));
		return v < -OSC2_HAP_FULL_Q15 ? -OSC2_HAP_FULL_Q15 : v;
	case OSC2_HAP_ENC_S8:
		v = (int8_t)p[0];
		if (v < -127) {
			v = -127;
		}
		/* times 32767 / 127, rounded half away from zero */
		return v >= 0 ? (v * OSC2_HAP_FULL_Q15 + 63) / 127 : -((-v * OSC2_HAP_FULL_Q15 + 63) / 127);
	default:
		return osc2_hap_mulaw(p[0]);
	}
}
