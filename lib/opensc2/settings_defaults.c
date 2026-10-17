/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * settings_defaults.c: defaults of the controller settings, from the published documentation of
 * the controller's settings table. Numbers it gives no default for start at 0.
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

int16_t opensc2_setting_default(uint8_t number)
{
	for (unsigned int i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
		if (defaults[i].number == number) {
			return defaults[i].value;
		}
	}
	return 0;
}
