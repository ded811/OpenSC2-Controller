/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * crash.h: crash records (crash.c; the host tests have a stand-in). Not a public interface.
 */
#ifndef OPENSC2_CRASH_H_
#define OPENSC2_CRASH_H_

#include <stdint.h>
#include "crash_rules.h"

/* The bytes of the retained RAM region the crash record's slot takes (usblog.c leaves them). */
#if defined(CONFIG_OPENSC2_CRASH_RECORD)
#define OSC2_CRASH_SLOT_SIZE 44
#else
#define OSC2_CRASH_SLOT_SIZE 0
#endif

/* opensc2_init(), after the settings loaded: a record a fatal error left in RAM is stored as
 * debug/resetreason (once per new crash: the same unread crash again writes no flash), and the RAM
 * copy cleared. */
void osc2_crash_boot(void);
/* 0xF2 selector 1: {1, the stored record} into `out` (zeros without one); a new record is then
 * marked seen and saved again. Returns the answer's length, 34. */
uint8_t osc2_crash_read(uint8_t out[OSC2_CRASH_ANSWER_LEN]);

#endif /* OPENSC2_CRASH_H_ */
