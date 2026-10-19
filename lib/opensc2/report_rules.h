/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * report_rules.h: the rules of the state report that are pure logic, so the host tests run them as
 * the library does: the triggers' digital bits.
 */
#ifndef OPENSC2_REPORT_RULES_H_
#define OPENSC2_REPORT_RULES_H_

#include <stdbool.h>
#include <stdint.h>

/* A trigger's digital bit for its value, with controller setting 68 (the threshold in percent,
 * clamped to 40..99) and the bit the previous report had: on above the threshold plus 5 points
 * (at most 32766, so a fully pulled trigger always counts), off below it minus 5, unchanged in
 * between. Integer division truncates. */
bool osc2_trigger_click(uint16_t value, int16_t setting68, bool previous);

#endif /* OPENSC2_REPORT_RULES_H_ */
