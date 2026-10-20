/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * settings_defaults.c: defaults and ranges of the controller settings. The defaults and the ranges
 * of 36 and 68 are the protocol documentation's (not public yet; docs/protocol.md lists the
 * pages); the other ranges come from the same authors' study of the controller's firmware (not in
 * that documentation yet). Numbers with no known default start at 0; numbers with no known range
 * are not clamped.
 */
#include <opensc2/settings.h>

struct setting_default {
	uint8_t number;
	int16_t value;
};

static const struct setting_default defaults[] = {
	{ OPENSC2_SETTING_PAD_ROTATION_ANGLE, 0 },
	{ OPENSC2_SETTING_LIZARD_MODE, 1 },
	{ OPENSC2_SETTING_MOMENTUM_MIN_VEL, 200 },
	{ OPENSC2_SETTING_MOMENTUM_DECAY, 100 },
	{ OPENSC2_SETTING_HAPTIC_INCREMENT, 5500 },
	{ OPENSC2_SETTING_PAD_ROTATION, 20 },
	{ OPENSC2_SETTING_RUMBLE_GAIN_DB, -10 },
	{ OPENSC2_SETTING_STICK_TOUCH_ON, 500 },
	{ OPENSC2_SETTING_STICK_TOUCH_OFF, 400 },
	{ OPENSC2_SETTING_GRIP_SENSITIVITY, 100 },
	{ OPENSC2_SETTING_GRIP_RELEASE, 80 },
	{ OPENSC2_SETTING_LED_CURVE, 1 },
	{ OPENSC2_SETTING_LED_BRIGHTNESS, 50 },
	{ OPENSC2_SETTING_SLEEP_TIMEOUT_S, 900 },
	{ OPENSC2_SETTING_LEFT_CLICK_PRESSURE, 40 },
	{ OPENSC2_SETTING_RIGHT_CLICK_PRESSURE, 40 },
	{ OPENSC2_SETTING_PAD_FILTER, 1 },
	{ OPENSC2_SETTING_PAD_CLIP, 1 },
	{ OPENSC2_SETTING_TRIGGER_THRESHOLD, 90 },
	{ OPENSC2_SETTING_HAPTICS_ENABLED, 1 },
	{ OPENSC2_SETTING_STEAM_WATCHDOG, 1 },
	{ OPENSC2_SETTING_PAD_TOUCH_ON, 1300 },
	{ OPENSC2_SETTING_PAD_TOUCH_OFF, 1000 },
	{ OPENSC2_SETTING_HAPTIC_GAIN_DB, -3 },
	{ OPENSC2_SETTING_PAD_STABILIZER, 1 },
	{ OPENSC2_SETTING_PAD_IGNORE_ON_STICK, 3 },
	{ OPENSC2_SETTING_IMU_BIAS, 1 },
	{ OPENSC2_SETTING_IMU_THRESHOLD, 100 },
};

/* The known ranges (36 and 68 from the protocol documentation, the others from the same authors'
 * study of the controller's firmware, not in that documentation yet); vendor command 0x87 clamps
 * to them. */
static const struct {
	uint8_t number;
	int16_t min, max;
} ranges[] = {
	{ OPENSC2_SETTING_LIZARD_MODE, 0, 1 },
	{ OPENSC2_SETTING_MOMENTUM_MIN_VEL, 0, 1000 },
	{ OPENSC2_SETTING_MOMENTUM_DECAY, 1, 1000 },
	{ OPENSC2_SETTING_HAPTIC_INCREMENT, 0, 32767 },
	{ OPENSC2_SETTING_IR_LED_BRIGHTNESS, 0, 100 },
	{ OPENSC2_SETTING_LED_BRIGHTNESS, 0, 100 },
	{ OPENSC2_SETTING_TRIGGER_THRESHOLD, 40, 99 },
	{ OPENSC2_SETTING_HAPTICS_ENABLED, 0, 2 },
	{ OPENSC2_SETTING_STEAM_WATCHDOG, 0, 1 },
};

int16_t opensc2_setting_default(uint8_t number)
{
	for (unsigned int i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
		if (defaults[i].number == number) {
			return defaults[i].value;
		}
	}
	return 0;
}

int16_t opensc2_setting_clamp(uint8_t number, int16_t value)
{
	for (unsigned int i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
		if (ranges[i].number == number) {
			return value < ranges[i].min ? ranges[i].min :
			       value > ranges[i].max ? ranges[i].max : value;
		}
	}
	return value;
}
