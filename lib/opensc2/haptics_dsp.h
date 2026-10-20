/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * haptics_dsp.h: the arithmetic of the haptics synthesiser (CONFIG_OPENSC2_HAPTICS_SYNTH), in
 * integers only, so a rendered block is bit for bit the same on the board and on a PC (the host
 * tests and the hardware test suite compare CRCs of blocks). Pure C, no kernel.
 *
 * Units: a sample is Q15 in an int32_t (32768 = 1.0; sums and boosted values may go past it until
 * the mix clips them); a gain is Q16 (65536 = 1.0); a phase is a uint32_t, one full cycle = 2^32.
 * The tables are in haptics_tables.c, written by tools/gen_haptics_tables.py.
 */
#ifndef OPENSC2_HAPTICS_DSP_H_
#define OPENSC2_HAPTICS_DSP_H_

#include <stdbool.h>
#include <stdint.h>

#define OSC2_HAP_ONE_Q15    32768
#define OSC2_HAP_FULL_Q15   32767        /* the largest sample a block holds */
#define OSC2_HAP_ONE_Q16    65536
#define OSC2_HAP_GAIN_MIN_DB (-30)
#define OSC2_HAP_GAIN_MAX_DB 30
#define OSC2_HAP_CLICK_A_LEN 70
#define OSC2_HAP_CLICK_B_LEN 93

extern const int16_t osc2_hap_sine_q15[257];
extern const uint32_t osc2_hap_gain_q16[61];
extern const uint32_t osc2_hap_exp2_q16[65];
extern const uint32_t osc2_hap_log2_q16[65];
extern const int8_t osc2_hap_click_a[OSC2_HAP_CLICK_A_LEN];
extern const int8_t osc2_hap_click_b[OSC2_HAP_CLICK_B_LEN];

/* sin(2 pi phase / 2^32), Q15 (-32767..32767): a quarter-wave table with linear interpolation. */
int32_t osc2_hap_sin(uint32_t phase);

/* A gain in dB as a factor, Q16: 10^(dB / 20) in 1 dB steps, from -30 to +30 dB; values beyond
 * play at the nearest end, so a gain never mutes (the real controller's rule, page 8). */
uint32_t osc2_hap_gain_q16_db(int32_t db);

/* A signed 8-bit dB sum without wrapping round (the real controller adds in 8 bits and wraps). */
int8_t osc2_hap_db_add(int32_t a, int32_t b);

/* log2(v) of an integer v >= 1, Q16 (0 for v = 0). */
int32_t osc2_hap_log2(uint32_t v);

/* v * 2^(e / 65536), e signed Q16; the result saturates at 2^63 - 1. */
uint64_t osc2_hap_scale_exp2(uint64_t v, int32_t e);

/* The phase step of a frequency in milli-hertz at `rate` samples a second (above the Nyquist
 * frequency it aliases, as the sine of the phase would). */
uint32_t osc2_hap_phase_step(uint32_t freq_mhz, uint32_t rate);

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

/* One sample at `p` as Q15: s16le as it is (-32768 reads as -32767), s8 times 32767 / 127, mu-law
 * through its 16-bit value (page 8's scalings: 1/32767, 1/127 and the 16-bit value / 32767). */
int32_t osc2_hap_pcm_sample(const uint8_t *p, uint8_t enc);

/* Q15 product helpers. */
static inline int32_t osc2_hap_mul_q16(int32_t v, int32_t gain_q16)
{
	return (int32_t)(((int64_t)v * gain_q16) >> 16);
}

static inline int32_t osc2_hap_mul_q15(int32_t v, int32_t f_q15)
{
	return (int32_t)(((int64_t)v * f_q15) >> 15);
}

/* A mixed sample clipped to the block's range: -32767..32767. */
static inline int32_t osc2_hap_clip(int64_t v)
{
	return v > OSC2_HAP_FULL_Q15 ? OSC2_HAP_FULL_Q15 :
	       v < -OSC2_HAP_FULL_Q15 ? -OSC2_HAP_FULL_Q15 : (int32_t)v;
}

/* A clipped sample as the pads' unsigned 8-bit value: 127.5 + 127.5 * v, rounded (silence 128). */
static inline uint8_t osc2_hap_to_u8(int32_t v)
{
	return (uint8_t)(((v + OSC2_HAP_FULL_Q15) * 255 + OSC2_HAP_FULL_Q15) / (2 * OSC2_HAP_FULL_Q15));
}

#endif /* OPENSC2_HAPTICS_DSP_H_ */
