/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * test_sys_notes.c: what system.c asks the test hooks (test_sys.h): the inputs injected into the
 * system rules, whether a power-off must become a reboot, and the ring entries of the machine. On
 * the glue's core and environment, as test_notes.c does it. Pure C: the same file runs in the
 * firmware and in the host tests' hooks binary.
 */
#include "test_hooks.h"
#include "test_sys.h"

#define CORE (&osc2_test_core_obj)
#define ENV  (&osc2_test_env_obj)

bool osc2_test_sys_buttons(uint32_t *buttons)
{
	return osc2_test_sys_get(ENV, OSC2_TEST_SYS_BUTTONS, buttons);
}

bool osc2_test_sys_level(uint8_t *level)
{
	uint32_t v;

	if (!osc2_test_sys_get(ENV, OSC2_TEST_SYS_LEVEL, &v)) {
		return false;
	}
	*level = (uint8_t)v;
	return true;
}

bool osc2_test_sys_source(uint8_t *source)
{
	uint32_t v;

	if (!osc2_test_sys_get(ENV, OSC2_TEST_SYS_SOURCE, &v)) {
		return false;
	}
	*source = (uint8_t)v;
	return true;
}

bool osc2_test_sys_keep_on(void)
{
	return osc2_test_core_armed(CORE) || osc2_test_sys_booted();
}

void osc2_test_sys_boot_apply(const uint32_t words[OSC2_TEST_SYS_BOOT_WORDS], uint32_t now_ms)
{
	osc2_test_sys_apply_boot(ENV, words, now_ms);
}

int32_t osc2_test_sys_next(uint32_t now_ms)
{
	return osc2_test_sys_hold_next(ENV, now_ms);
}

bool osc2_test_sys_poll(uint32_t now_ms)
{
	return osc2_test_sys_hold_poll(ENV, now_ms);
}

/* ---- notes (only while armed) ---------------------------------------------------------------- */

static void note(uint8_t kind, const uint8_t *d, uint8_t n)
{
	osc2_test_core_note(CORE, ENV, kind, OSC2_TEST_LINK_NONE, d, n);
}

void osc2_test_sys_note_state(uint8_t from, uint8_t to, uint8_t cause, uint8_t transport,
			      uint8_t source)
{
	uint8_t d[5] = { from, to, cause, transport, source };

	if (osc2_test_core_armed(CORE)) {
		note(OSC2_TEST_KIND_SYS_STATE, d, sizeof(d));
	}
}

void osc2_test_sys_note_led(uint8_t mode, uint8_t flags, uint8_t level, uint8_t full_level)
{
	uint8_t d[4] = { mode, flags, level, full_level };

	if (osc2_test_core_armed(CORE)) {
		note(OSC2_TEST_KIND_LED_MODE, d, sizeof(d));
	}
}

void osc2_test_sys_note_cue(uint8_t cue, uint8_t script, int8_t gain_db, bool played)
{
	uint8_t d[4] = { cue, script, (uint8_t)gain_db, played ? 1 : 0 };

	if (osc2_test_core_armed(CORE)) {
		note(OSC2_TEST_KIND_CUE, d, sizeof(d));
	}
}

void osc2_test_sys_note_end(uint8_t state, uint8_t cause, uint8_t end, uint8_t how)
{
	uint8_t d[4] = { state, cause, end, how };

	if (osc2_test_core_armed(CORE)) {
		note(OSC2_TEST_KIND_SYS_END, d, sizeof(d));
	}
}

void osc2_test_sys_note_chord(uint8_t what, uint8_t chord)
{
	uint8_t d[2] = { what, chord };

	if (osc2_test_core_armed(CORE)) {
		note(OSC2_TEST_KIND_CHORD, d, sizeof(d));
	}
}

void osc2_test_sys_note_source(uint8_t from, uint8_t to, int event)
{
	uint8_t d[3] = { from, to, event < 0 ? 0xFF : (uint8_t)event };

	if (osc2_test_core_armed(CORE)) {
		note(OSC2_TEST_KIND_SOURCE, d, sizeof(d));
	}
}

void osc2_test_sys_note_idle(uint16_t history)
{
	uint32_t v;
	uint8_t d[2];

	/* Only while a test injects into the system rules: a long run on a board with nothing wired
	 * would otherwise meet the idle monitor every period after the timeout, in the middle of
	 * other groups' rings. */
	if (osc2_test_core_armed(CORE) &&
	    (osc2_test_sys_get(ENV, OSC2_TEST_SYS_BUTTONS, &v) ||
	     osc2_test_sys_get(ENV, OSC2_TEST_SYS_LEVEL, &v) ||
	     osc2_test_sys_get(ENV, OSC2_TEST_SYS_SOURCE, &v))) {
		opensc2_put_le16(d, history);
		note(OSC2_TEST_KIND_IDLE, d, sizeof(d));
	}
}
