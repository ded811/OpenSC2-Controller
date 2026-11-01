/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * proto_rules.c: the vendor commands' pure rules (proto_rules.h). Pure logic.
 */
#include <opensc2/settings.h>
#include "proto_rules.h"

int16_t osc2_setting_answer(uint8_t number, int16_t value)
{
	switch (number) {
	case OPENSC2_SETTING_IMU_MODE:
	case OPENSC2_SETTING_IMU_BIAS:
	case OPENSC2_SETTING_IMU_THRESHOLD:
		return opensc2_setting_default(number);
	default:
		return value;
	}
}

uint8_t osc2_led_readback(uint8_t percent)
{
	return (uint8_t)(percent / 100u * 100u);
}

/* Zephyr's RESET_* bits (include/zephyr/drivers/hwinfo.h), restated: this file is pure C. */
#define ZEPHYR_RESET_PIN       (1u << 0)
#define ZEPHYR_RESET_SOFTWARE  (1u << 1)
#define ZEPHYR_RESET_BROWNOUT  (1u << 2)
#define ZEPHYR_RESET_POR       (1u << 3)
#define ZEPHYR_RESET_WATCHDOG  (1u << 4)
/* Debug (5) up to temperature (14): the causes the real controller reports as "other". The later
 * bits (the bootloader, a flash error) it does not count. */
#define ZEPHYR_RESET_OTHERS    (0x3FFu << 5)

uint8_t osc2_reset_flags(uint32_t causes)
{
	uint8_t flags = 0;

	flags |= (causes & ZEPHYR_RESET_POR) ? OSC2_RESET_POWER_ON : 0;
	flags |= (causes & ZEPHYR_RESET_BROWNOUT) ? OSC2_RESET_BROWNOUT : 0;
	flags |= (causes & ZEPHYR_RESET_OTHERS) ? OSC2_RESET_OTHER : 0;   /* debug, wake, lock-up */
	flags |= (causes & ZEPHYR_RESET_PIN) ? OSC2_RESET_PIN : 0;
	flags |= (causes & ZEPHYR_RESET_WATCHDOG) ? OSC2_RESET_WATCHDOG : 0;
	flags |= (causes & ZEPHYR_RESET_SOFTWARE) ? OSC2_RESET_SOFTWARE : 0;
	return flags;
}

/* The channels of a target, as 0x44 IDs, left first or right first. */
static uint8_t channel_ids(uint8_t channels, bool right_first, uint8_t ids[4])
{
	static const struct {
		uint8_t mask, id;
	} left_first[4] = {
		{ OPENSC2_HAPTIC_LEFT_PAD, OSC2_STATUS_LEFT_PAD },
		{ OPENSC2_HAPTIC_RIGHT_PAD, OSC2_STATUS_RIGHT_PAD },
		{ OPENSC2_HAPTIC_LEFT_GRIP, OSC2_STATUS_LEFT_GRIP },
		{ OPENSC2_HAPTIC_RIGHT_GRIP, OSC2_STATUS_RIGHT_GRIP },
	};
	/* Swapping each pair gives the right-first order. */
	static const uint8_t right_order[4] = { 1, 0, 3, 2 };
	uint8_t n = 0;

	for (int i = 0; i < 4; i++) {
		int k = right_first ? right_order[i] : i;

		if (channels & left_first[k].mask) {
			ids[n++] = left_first[k].id;
		}
	}
	return n;
}

uint8_t osc2_stop_status(const struct opensc2_output *out, uint8_t ids[4])
{
	uint8_t n = 0;

	switch (out->type) {
	case OPENSC2_OUTPUT_RUMBLE:
		/* A speed of 0 is a stop-all on that grip; the right one is queued first. */
		if (out->rumble.right_speed == 0) {
			ids[n++] = OSC2_STATUS_RIGHT_GRIP;
		}
		if (out->rumble.left_speed == 0) {
			ids[n++] = OSC2_STATUS_LEFT_GRIP;
		}
		return n;
	case OPENSC2_OUTPUT_HAPTIC:
		break;
	default:
		return 0;
	}
	switch (out->haptic.kind) {
	case OPENSC2_HAPTIC_PULSE:
		return out->haptic.pulse.on_us == 0 ? channel_ids(out->haptic.channels, false, ids) : 0;
	case OPENSC2_HAPTIC_COMMAND:
		return out->haptic.command.command == OPENSC2_HAPTIC_CMD_STOP_ALL ?
		       channel_ids(out->haptic.channels, false, ids) : 0;
	case OPENSC2_HAPTIC_PCM_CONTROL:
		return out->haptic.pcm_control.operation == OPENSC2_PCM_STOP ?
		       channel_ids(out->haptic.channels, true, ids) : 0;
	default:
		return 0;
	}
}
