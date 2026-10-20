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

/* The test hooks' ops 0x80..0x83: the test channel's status, the result in `result` / `*rlen`. */
uint8_t osc2_haptics_test_op(uint8_t op, const uint8_t *args, uint8_t alen, uint8_t *result,
			     uint8_t *rlen);

#else

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
