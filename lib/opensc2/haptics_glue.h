/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * haptics_glue.h: where the rest of the library reaches the haptics synthesiser
 * (CONFIG_OPENSC2_HAPTICS_SYNTH, haptics.c). Without the option every call is an empty inline
 * function, so the call sites need no #if. Not a public interface.
 */
#ifndef OPENSC2_HAPTICS_GLUE_H_
#define OPENSC2_HAPTICS_GLUE_H_

#include <stdint.h>
#include <opensc2/types.h>

#if defined(CONFIG_OPENSC2_HAPTICS_SYNTH)

/* The outputs one output report decoded into (report.c's dispatcher, after the application's
 * callbacks), on `link` (enum osc2_link). */
void osc2_haptics_outputs(uint8_t link, const struct opensc2_output *out, int n);

/* Haptic events the library makes itself, as the haptic callback gets them (report ID 0):
 * lizard mode's pad ticks (click A) and pad presses (click B), each at its gain as given. */
void osc2_haptics_library(const struct opensc2_haptic *h, uint8_t n);

/* The test hooks' ops 0x80..0x83: the test channel's status, the result in `result` / `*rlen`. */
uint8_t osc2_haptics_test_op(uint8_t op, const uint8_t *args, uint8_t alen, uint8_t *result,
			     uint8_t *rlen);

/* Controller setting `number` changed (OPENSC2_SETTINGS_ALL: any of them, the defaults restored,
 * and once at start-up): settings 75 and 76 go to the four channels as configure commands, as the
 * real controller's settings handler hands them on. */
void osc2_haptics_setting(uint8_t number);

/* settings/haptics/haptic_master_gain_db (key 0) or .../amplifier_mode (key 1) set by name
 * (0xEE): to the four channels at once, bypassing the numbered settings. 0 or -ENOMEM. */
int osc2_haptics_key(uint8_t key, int16_t value);

#else

static inline void osc2_haptics_setting(uint8_t number)
{
	(void)number;
}

static inline int osc2_haptics_key(uint8_t key, int16_t value)
{
	(void)key;
	(void)value;
	return 0;
}

static inline void osc2_haptics_library(const struct opensc2_haptic *h, uint8_t n)
{
	(void)h;
	(void)n;
}

static inline void osc2_haptics_outputs(uint8_t link, const struct opensc2_output *out, int n)
{
	(void)link;
	(void)out;
	(void)n;
}

static inline uint8_t osc2_haptics_test_op(uint8_t op, const uint8_t *args, uint8_t alen,
					   uint8_t *result, uint8_t *rlen)
{
	(void)op;
	(void)args;
	(void)alen;
	(void)result;
	*rlen = 0;
	return 1;   /* BAD_OP: the synthesiser is not built */
}

#endif /* CONFIG_OPENSC2_HAPTICS_SYNTH */

#endif /* OPENSC2_HAPTICS_GLUE_H_ */
