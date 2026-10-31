/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * haptics_dsp.h: the arithmetic of the haptics synthesiser (CONFIG_OPENSC2_HAPTICS_SYNTH), in
 * integers only, so a rendered block is bit for bit the same on the board and on a PC (the host
 * tests and the hardware test suite compare CRCs of blocks). Pure C, no kernel.
 *
 * It reproduces the real controller's arithmetic, quirks included (the protocol documentation's
 * page on the haptics): a fifth-order polynomial sine whose peak is about 1.0045, phases that are
 * wrapped by one cycle per step only, gains in dB added in signed 8 bits, and samples scaled by
 * 1/32767 and 1/127. Where the real arithmetic produces a value that is not a number, the engine
 * carries OSC2_HAP_NAN; where it produces a huge one, OSC2_HAP_HUGE stands in for it (it clips
 * to full scale through any gain).
 *
 * Units: a sample is Q15 in an int64_t (32768 = 1.0; sums and boosted values may go past it until
 * the mix clips them); a gain is Q16 (65536 = 1.0); a phase is Q32 cycles in an int64_t (2^32 = one
 * cycle, 2 pi). The tables are in haptics_tables.c, written by tools/gen_haptics_tables.py.
 */
#ifndef OPENSC2_HAPTICS_DSP_H_
#define OPENSC2_HAPTICS_DSP_H_

#include <stdbool.h>
#include <stdint.h>

#define OSC2_HAP_ONE_Q15    32768
#define OSC2_HAP_FULL_Q15   32767        /* the largest grip sample a block holds */
#define OSC2_HAP_ONE_Q16    65536
#define OSC2_HAP_CYCLE      ((int64_t)1 << 32)
#define OSC2_HAP_GAIN_MIN_DB (-30)
#define OSC2_HAP_GAIN_MAX_DB 30
#define OSC2_HAP_CLICK_A_LEN 70
#define OSC2_HAP_CLICK_B_LEN 93

/* A sample that is not a number (the real arithmetic's NaN): it spoils the channel's whole mix
 * for that sample, and the mix plays it as +1, full scale, whatever the gains. */
#define OSC2_HAP_NAN        INT64_MIN
/* The size of a huge sample (a sine evaluated far outside its range): 65536.0, which stays above
 * full scale through any product of gains the synthesiser has. */
#define OSC2_HAP_HUGE       ((int64_t)1 << 31)

extern const uint32_t osc2_hap_gain_q16[61];
extern const uint32_t osc2_hap_exp2_q16[65];
extern const uint32_t osc2_hap_log2_q16[65];
extern const int8_t osc2_hap_click_a[OSC2_HAP_CLICK_A_LEN];
extern const int8_t osc2_hap_click_b[OSC2_HAP_CLICK_B_LEN];

/* A value limited to +-OSC2_HAP_HUGE (OSC2_HAP_NAN passes unchanged). */
static inline int64_t osc2_hap_sat(int64_t v)
{
	if (v == OSC2_HAP_NAN) {
		return v;
	}
	return v > OSC2_HAP_HUGE ? OSC2_HAP_HUGE : v < -OSC2_HAP_HUGE ? -OSC2_HAP_HUGE : v;
}

/* The sine of a phase in Q32 cycles, Q15, as the real controller computes it: the phase is
 * brought into 0..2 pi by adding or taking away one cycle at most (once each way), reduced to the
 * first quarter by symmetry and put through x (1 - x^2 / 6 + x^4 / 120) with the real one's
 * rounded coefficients, so the peak is about 1.0045 (32915). A phase still outside 0..2 pi after
 * that one step (a tone above the channel's rate) leaves the polynomial's range: the value grows
 * with the fifth power of the phase, limited to +-OSC2_HAP_HUGE. */
int64_t osc2_hap_sinp(int64_t phase);

/* The same for a phase of one cycle at most (0 .. 2^32 - 1). */
static inline int32_t osc2_hap_sin(uint32_t phase)
{
	return (int32_t)osc2_hap_sinp((int64_t)phase);
}

/* A gain in dB as a factor, Q16: 10^(dB / 20) in 1 dB steps, from -30 to +30 dB; values beyond
 * play at the nearest end, so a gain never mutes (the real controller's rule). */
uint32_t osc2_hap_gain_q16_db(int32_t db);

/* A signed 8-bit dB sum, wrapping round as the real controller's does (127 + 1 is -128). */
int8_t osc2_hap_db_add(int32_t a, int32_t b);

/* log2(v) of an integer v >= 1, Q16 (0 for v = 0). */
int32_t osc2_hap_log2(uint32_t v);

/* v * 2^(e / 65536), e signed Q16; the result saturates at 2^63 - 1. */
uint64_t osc2_hap_scale_exp2(uint64_t v, int32_t e);

/* The phase step of a frequency in milli-hertz (negative: the sine runs backwards) at `rate`
 * samples a second, in Q32 cycles, not wrapped: above the channel's rate it is more than a
 * cycle, which the real controller's one-cycle wrap does not bring back. */
int64_t osc2_hap_phase_step(int64_t freq_mhz, uint32_t rate);

/* Standard G.711 mu-law to a 16-bit value (-32124..32124). */
int16_t osc2_hap_mulaw(uint8_t u);

/* PCM encodings of the stream formats (enum opensc2_pcm_encoding): s16le, s8, mu-law. */
#define OSC2_HAP_ENC_S16LE 0
#define OSC2_HAP_ENC_S8    1
#define OSC2_HAP_ENC_MULAW 2

/* Bytes per sample of an encoding (2 for s16le, else 1). */
static inline uint8_t osc2_hap_enc_bytes(uint8_t enc)
{
	return enc == OSC2_HAP_ENC_S16LE ? 2 : 1;
}

/* One sample at `p` as Q15, scaled as the real controller scales it: s16le / 32767 (so -32768 is
 * a hair beyond -1), s8 / 127 (-128 likewise), mu-law through its 16-bit value / 32767. */
int32_t osc2_hap_pcm_sample(const uint8_t *p, uint8_t enc);

/* v * gain (Q16), limited to +-OSC2_HAP_HUGE; OSC2_HAP_NAN stays OSC2_HAP_NAN. */
static inline int64_t osc2_hap_mul_q16(int64_t v, int32_t gain_q16)
{
	if (v == OSC2_HAP_NAN) {
		return v;
	}
	return osc2_hap_sat((osc2_hap_sat(v) * gain_q16) / OSC2_HAP_ONE_Q16);
}

/* v * f (Q15), limited to +-OSC2_HAP_HUGE; OSC2_HAP_NAN stays OSC2_HAP_NAN. */
static inline int64_t osc2_hap_mul_q15(int64_t v, int64_t f_q15)
{
	if (v == OSC2_HAP_NAN || f_q15 == OSC2_HAP_NAN) {
		return OSC2_HAP_NAN;
	}
	return osc2_hap_sat((osc2_hap_sat(v) * osc2_hap_sat(f_q15)) / OSC2_HAP_ONE_Q15);
}

/* A mixed sample clipped to +-1 (+-32768); a NaN plays as +1 (it fails both of the clip's
 * comparisons on the real controller). */
static inline int32_t osc2_hap_clip(int64_t v)
{
	if (v == OSC2_HAP_NAN) {
		return OSC2_HAP_ONE_Q15;
	}
	return v > OSC2_HAP_ONE_Q15 ? OSC2_HAP_ONE_Q15 :
	       v < -OSC2_HAP_ONE_Q15 ? -OSC2_HAP_ONE_Q15 : (int32_t)v;
}

/* A clipped sample (+-32768) as the pads' unsigned 8-bit value: 127.5 + 127.5 * v, rounded
 * (silence 128, +1 is 255, -1 is 0). */
static inline uint8_t osc2_hap_to_u8(int32_t v)
{
	return (uint8_t)((255 * (int64_t)(v + OSC2_HAP_ONE_Q15) + OSC2_HAP_ONE_Q15) /
			 (2 * OSC2_HAP_ONE_Q15));
}

/* A clipped sample (+-32768) as the grips' signed 16-bit value: v * 32767, rounded half away from
 * zero (+-32767 at full scale). */
static inline int16_t osc2_hap_to_s16(int32_t v)
{
	int32_t m = v < 0 ? -v : v;
	int32_t r = (int32_t)(((int64_t)m * OSC2_HAP_FULL_Q15 + OSC2_HAP_ONE_Q15 / 2) /
			      OSC2_HAP_ONE_Q15);

	return (int16_t)(v < 0 ? -r : r);
}

#endif /* OPENSC2_HAPTICS_DSP_H_ */
