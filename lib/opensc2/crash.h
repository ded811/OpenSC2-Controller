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

/* The fatal errors osc2_crash_provoke() can cause. */
#define OSC2_CRASH_OOPS       0   /* k_oops(): reason 3, as the watchdog warning crashes */
#define OSC2_CRASH_PANIC      1   /* k_panic(): reason 4 */
#define OSC2_CRASH_EXCEPTION  2   /* an undefined instruction: reason 0, a CPU exception */

/* opensc2_init(), after the settings loaded: a record a fatal error left in RAM is stored as
 * debug/resetreason (once per new crash: the same unread crash again writes no flash), and the RAM
 * copy cleared. */
void osc2_crash_boot(void);
/* 0xF2 selector 1: {1, the stored record} into `out` (zeros without one); a new record is then
 * marked seen and saved again. Returns the answer's length, 34. */
uint8_t osc2_crash_read(uint8_t out[OSC2_CRASH_ANSWER_LEN]);
/* The test hooks: a fatal error of `kind` `delay_ms` from now, from the system work queue. -ENOTSUP
 * without CONFIG_OPENSC2_CRASH_RECORD (the board would halt instead of resetting), -EINVAL for an
 * unknown kind. */
int osc2_crash_provoke(uint8_t kind, uint16_t delay_ms);

#endif /* OPENSC2_CRASH_H_ */
