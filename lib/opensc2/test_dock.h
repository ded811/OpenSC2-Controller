/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * test_dock.h: the hardware test hooks' part of the dock (CONFIG_OPENSC2_TEST_HOOKS with
 * CONFIG_OPENSC2_DOCK_LINK), in test_dock.c:
 *   - the dock ops 0x90 to 0x93 of the test channel: load a frame in two parts, hand it to the dock
 *     link as if it had arrived from the puck, read the answer back (serials, bonds and the chip
 *     ID masked), and feed the pilot check, the supply rule and the docked states injected values;
 *   - the dock's ring entries (kinds 0x70 to 0x73, link 5).
 * The bytes are in docs/test-protocol.md. Not a public interface. Without the hooks every note is
 * an empty inline function.
 */
#ifndef OPENSC2_TEST_DOCK_H_
#define OPENSC2_TEST_DOCK_H_

#include <stdbool.h>
#include <stdint.h>
#include "dock_rules.h"

/* The dock's number in the ring (the library's OSC2_LINK_DOCK). */
#define OSC2_TEST_LINK_DOCK 5

/* DOCK_SESSION's `what`. */
#define OSC2_TEST_DOCK_START 0
#define OSC2_TEST_DOCK_END   1

/* DOCK_STATE's `what`: a supply event, an entry into the dock states, another event. */
#define OSC2_TEST_DOCK_ST_SUPPLY 0
#define OSC2_TEST_DOCK_ST_ENTER  1
#define OSC2_TEST_DOCK_ST_EVENT  2

#if defined(CONFIG_OPENSC2_TEST_HOOKS) && defined(CONFIG_OPENSC2_DOCK_LINK)

void osc2_test_note_dock_session(uint8_t what, uint8_t reason, uint32_t frames);
void osc2_test_note_dock_frame(uint8_t type, uint8_t outcome, uint8_t len, int32_t result,
			       uint32_t handling_us);
void osc2_test_note_dock_pilot(const int32_t mv[OSC2_DOCK_PILOT_READS], uint8_t inside,
			       bool present);
void osc2_test_note_dock_state(uint8_t what, uint8_t event, uint8_t chord,
			       const struct osc2_dock_step *step);
/* Is the arming that started the test hooks' dock session still on? Such a session lasts only
 * that long. */
bool osc2_test_dock_armed(void);
/* The ops' state back to that of a boot (the host tests and the firmware emulator). */
void osc2_test_dock_reset(void);
/* The environment's dock_op: one of the ops 0x90 to 0x93 (test_core.c has checked the argument
 * lengths). Returns the status; the result goes to `result` (at most OSC2_TEST_RESULT_MAX). */
uint8_t osc2_test_dock_op(uint8_t op, const uint8_t *args, uint8_t alen, uint8_t *result,
			  uint8_t *rlen);

#else

static inline void osc2_test_note_dock_session(uint8_t what, uint8_t reason, uint32_t frames)
{
	(void)what;
	(void)reason;
	(void)frames;
}

static inline void osc2_test_note_dock_frame(uint8_t type, uint8_t outcome, uint8_t len,
					     int32_t result, uint32_t handling_us)
{
	(void)type;
	(void)outcome;
	(void)len;
	(void)result;
	(void)handling_us;
}

static inline void osc2_test_note_dock_pilot(const int32_t mv[OSC2_DOCK_PILOT_READS],
					     uint8_t inside, bool present)
{
	(void)mv;
	(void)inside;
	(void)present;
}

static inline void osc2_test_note_dock_state(uint8_t what, uint8_t event, uint8_t chord,
					     const struct osc2_dock_step *step)
{
	(void)what;
	(void)event;
	(void)chord;
	(void)step;
}

static inline bool osc2_test_dock_armed(void)
{
	return false;
}

#endif

#endif /* OPENSC2_TEST_DOCK_H_ */
