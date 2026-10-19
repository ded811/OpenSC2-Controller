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
	int32_t percent = setting68 < 40 ? 40 : setting68 > 99 ? 99 : setting68;
	int32_t on = OPENSC2_TRIGGER_MAX * (percent + 5) / 100;
	int32_t off = OPENSC2_TRIGGER_MAX * (percent - 5) / 100;

	if (on > OPENSC2_TRIGGER_MAX - 1) {
		on = OPENSC2_TRIGGER_MAX - 1;
	}
	if ((int32_t)value > on) {
		return true;
	}
	if ((int32_t)value < off) {
		return false;
	}
	return previous;
}
