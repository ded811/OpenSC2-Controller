/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * haptics_help.c: the pure helpers of <opensc2/haptics.h>, for outputs and IMUs of the
 * application's own: a block's loudness and a free-fall detector. No kernel; built with or without
 * the synthesiser.
 */
#include <opensc2/haptics.h>
#include <opensc2/types.h>

/* 0.35 g and 0.6 g squared, in report 0x42's units (16384 per g). */
#define FALL_BELOW_SQ ((int64_t)5734 * 5734)
#define FALL_ABOVE_SQ ((int64_t)9830 * 9830)
#define FALL_MS 150u

uint8_t opensc2_haptics_block_level(enum opensc2_haptics_path path, const void *block,
				    size_t frames)
{
	const uint8_t *b = block;
	uint32_t peak = 0;

	if (path == OPENSC2_HAPTICS_PADS) {
		for (size_t i = 0; i < frames * 2; i++) {
			uint32_t d = b[i] >= 128u ? b[i] - 128u : 128u - b[i];

			peak = d > peak ? d : peak;
		}
		peak = peak * 255u / 127u;
	} else {
		for (size_t i = 0; i < frames * 2; i++) {
			int32_t v = (int16_t)(uint16_t)(b[i * 2] | (b[i * 2 + 1] << 8));
			uint32_t d = (uint32_t)(v < 0 ? -v : v);

			peak = d > peak ? d : peak;
		}
		peak = peak * 255u / 32767u;
	}
	return (uint8_t)(peak > 255u ? 255u : peak);
}

void opensc2_freefall_init(struct opensc2_freefall *ff)
{
	ff->falling = false;
	ff->reported = false;
	ff->since_ms = 0;
}

bool opensc2_freefall_update(struct opensc2_freefall *ff, const int16_t accel[3], uint32_t now_ms)
{
	int64_t m2 = (int64_t)accel[0] * accel[0] + (int64_t)accel[1] * accel[1] +
		     (int64_t)accel[2] * accel[2];

	if (ff->reported) {
		/* Wait for the landing (or a hand) before the next fall counts. */
		if (m2 > FALL_ABOVE_SQ) {
			ff->reported = false;
			ff->falling = false;
		}
		return false;
	}
	if (m2 >= FALL_BELOW_SQ) {
		ff->falling = false;
		return false;
	}
	if (!ff->falling) {
		ff->falling = true;
		ff->since_ms = now_ms;
		return false;
	}
	if (now_ms - ff->since_ms >= FALL_MS) {
		ff->reported = true;
		return true;
	}
	return false;
}
