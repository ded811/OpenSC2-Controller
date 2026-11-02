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

/* A trigger's digital bit for its value, with controller setting 68 (the threshold in percent) and
 * the bit the previous report had, as the real controller's trigger stage works it out: with
 * v = value / 32767 and t = setting / 100 in single-precision floating point, on when v passes
 * t + 0.05, off when it falls below t - 0.05, unchanged in between. So from 95 on (0.95 + 0.05
 * is 1.0, which v reaches but never passes) the bit never turns on. */
bool osc2_trigger_click(uint16_t value, int16_t setting68, bool previous);

#endif /* OPENSC2_REPORT_RULES_H_ */
