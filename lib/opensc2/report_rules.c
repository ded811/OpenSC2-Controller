/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * report_rules.c: the triggers' digital bits (report_rules.h). Pure logic.
 */
#include <opensc2/types.h>
#include "report_rules.h"

bool osc2_trigger_click(uint16_t value, int16_t setting68, bool previous)
{
	/* the level as the stage takes it: the setting as an unsigned number, in percent */
	float t = (float)(uint32_t)(int32_t)setting68 / 100.0f;
	float h = 5.0f / 100.0f;
	float v = (float)(value > OPENSC2_TRIGGER_MAX ? OPENSC2_TRIGGER_MAX : value) /
		  (float)OPENSC2_TRIGGER_MAX;

	if (previous) {
		return !(t - h > v);
	}
	return t + h < v;
}
