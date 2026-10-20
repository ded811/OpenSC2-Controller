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
	return percent == 100 ? 100 : 0;
}
