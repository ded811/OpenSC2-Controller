/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * test_dock.c: the hardware test hooks' part of the dock (test_dock.h): the dock ops 0x90 to 0x93
 * and the dock's ring entries. Pure C on the hooks' core and the dock link: the same file runs in
 * the firmware, the host tests and the firmware emulator. Every op runs under the hooks' request
 * lock, one at a time, so the state here needs no lock of its own.
 *
 * What the PC can do with them, on a board with nothing wired: load a 73-byte frame in two parts
 * (a request carries at most 54 argument bytes), hand it to the dock link as if it had come from
 * the puck (a session starts first if asked), read the answer back in two parts, and feed the
 * pilot check, the supply rule and the docked states values of its own. Safety, as for HOST_SET:
 * a SET frame runs only an opcode of HOST_SET's allowlist, and the answer's serials, bonds and
 * chip ID never come back (zeros instead, with a CRC-32 to compare).
 */
#include <string.h>
#include "internal.h"
#include "dock_link.h"
#include "retained.h"   /* osc2_crc32() */
#include "test_dock.h"
#include "test_hooks.h"

#define CORE (&osc2_test_core_obj)
#define ENV  (&osc2_test_env_obj)

/* The identity record's two serials in a probe's answer: payload bytes 16 to 43. */
#define SERIALS_OFF  (OPENSC2_DOCK_PAYLOAD_OFF + 16)
#define SERIALS_LEN  28
/* A GET answer's data: after the opcode and the length. */
#define GET_DATA_OFF (OPENSC2_DOCK_PAYLOAD_OFF + 2)

/* DOCK_RUN: flags, outcome, the answer's flags. */
#define RUN_START_SESSION   0x01
#define RUN_ANSWERED        0
#define RUN_SHORT           1     /* fewer than 73 bytes: the gap timeout ends the session */
#define RUN_BAD_CRC         2     /* the session ends */
#define RUN_NOTHING         3     /* no byte at all: the session goes on waiting */
#define ANS_CRC_OK          0x01
#define ANS_MASKED          0x02
#define RUN_RESULT_LEN      21
#define READ_MAX            55    /* answer bytes in one DOCK_READ */

/* DOCK_CTRL. */
#define CTRL_STATUS  0
#define CTRL_END     1
#define CTRL_PILOT   2
#define CTRL_SUPPLY  3
#define CTRL_EVENT   4
#define CTRL_RESET   5
#define ST_ACTIVE    0x01
#define ST_ALONE     0x02
#define ST_TEST      0x04
#define ST_UART      0x08         /* built with CONFIG_OPENSC2_DOCK */
#if defined(CONFIG_OPENSC2_DOCK)
#define ST_BUILT     ST_UART
#else
#define ST_BUILT     0
#endif

static struct {
	uint8_t frame[OPENSC2_DOCK_FRAME_LEN];    /* the request being loaded */
	uint8_t answer[OPENSC2_DOCK_FRAME_LEN];   /* the last answer, as it went out */
	uint8_t masked[OPENSC2_DOCK_FRAME_LEN];   /* 1: never read back */
	bool have_answer;
	uint32_t session_arm_us;                  /* the arming the test session belongs to */
	/* the docked states and the supply rule, on injected values only: nothing they decide is
	 * done (the radio, reboots and the LED stay as they are), it is recorded in the ring */
	struct osc2_dock_sm sm;
	struct osc2_dock_power_rule power;
} t;

/* ---- the ring -------------------------------------------------------------------------------- */

static void note(uint8_t kind, const uint8_t *d, uint8_t len)
{
	osc2_test_core_note(CORE, ENV, kind, OSC2_TEST_LINK_DOCK, d, len);
}

void osc2_test_note_dock_session(uint8_t what, uint8_t reason, uint32_t frames)
{
	uint8_t d[6] = { what, reason };

	if (osc2_test_core_armed(CORE)) {
		opensc2_put_le32(&d[2], frames);
		note(OSC2_TEST_KIND_DOCK_SESSION, d, sizeof(d));
	}
}

void osc2_test_note_dock_frame(uint8_t type, uint8_t outcome, uint8_t len, int32_t result,
			       uint32_t handling_us)
{
	uint8_t d[11] = { type, outcome, len };

	if (osc2_test_core_armed(CORE)) {
		opensc2_put_le32(&d[3], (uint32_t)result);
		opensc2_put_le32(&d[7], handling_us);
		note(OSC2_TEST_KIND_DOCK_FRAME, d, sizeof(d));
	}
}

void osc2_test_note_dock_pilot(const int32_t mv[OSC2_DOCK_PILOT_READS], uint8_t inside,
			       bool present)
{
	uint8_t d[8];

	if (osc2_test_core_armed(CORE)) {
		for (int i = 0; i < OSC2_DOCK_PILOT_READS; i++) {
			opensc2_put_le16(&d[2 * i], (uint16_t)(int16_t)(mv[i] < 0 ? -1 : mv[i]));
		}
		d[6] = inside;
		d[7] = present ? 1 : 0;
		note(OSC2_TEST_KIND_DOCK_PILOT, d, sizeof(d));
	}
}

void osc2_test_note_dock_state(uint8_t what, uint8_t event, uint8_t chord,
			       const struct osc2_dock_step *step)
{
	uint8_t d[10] = { what, event, chord, (uint8_t)step->from, (uint8_t)step->to,
			  step->handled ? 1 : 0 };

	if (osc2_test_core_armed(CORE)) {
		opensc2_put_le32(&d[6], step->actions);
		note(OSC2_TEST_KIND_DOCK_STATE, d, sizeof(d));
	}
}

bool osc2_test_dock_armed(void)
{
	/* The arming that started the session: ARM sets a new time base, so a session does not
	 * outlive its arming into the next one either. */
	return osc2_test_core_armed(CORE) && CORE->arm_us == t.session_arm_us;
}

/* ---- the ops --------------------------------------------------------------------------------- */

void osc2_test_dock_reset(void)
{
	memset(&t, 0, sizeof(t));
	osc2_dock_sm_init(&t.sm);
}

static void start_test_session(void)
{
	t.session_arm_us = CORE->arm_us;
	osc2_dock_session_start(OSC2_DOCK_BY_TEST);
}

static uint8_t op_load(const uint8_t *args, uint8_t alen)
{
	uint8_t off = args[0], n = (uint8_t)(alen - 1);

	if ((size_t)off + n > OPENSC2_DOCK_FRAME_LEN) {
		return OSC2_TEST_BAD_ARG;
	}
	memcpy(&t.frame[off], &args[1], n);
	return OSC2_TEST_OK;
}

/* Which bytes of the answer just made must never go back to the PC. */
static void mask_answer(int32_t result)
{
	memset(t.masked, 0, sizeof(t.masked));
	if (t.answer[OPENSC2_DOCK_TYPE_OFF] == OPENSC2_DOCK_PROBE) {
		memset(&t.masked[SERIALS_OFF], 1, SERIALS_LEN);
	} else if (t.answer[OPENSC2_DOCK_TYPE_OFF] == OPENSC2_DOCK_GET &&
		   result == OSC2_DOCK_RESULT_GET_OK) {
		/* As HOST_GET masks them: the bonds, the serials, the chip ID. */
		uint8_t stored[2 + OPENSC2_ANSWER_MAX];
		uint8_t stored_len = osc2_command_stored(OSC2_LINK_DOCK, stored);
		uint8_t n = MIN(t.answer[OPENSC2_DOCK_PAYLOAD_OFF + 1], (uint8_t)OPENSC2_ANSWER_MAX);

		if (osc2_test_host_get_masked(stored, stored_len, n)) {
			memset(&t.masked[GET_DATA_OFF], 1, n);
		}
	}
}

static uint8_t op_run(const uint8_t *args, uint8_t *result, uint8_t *rlen)
{
	uint8_t len = args[0], flags = args[1];
	uint8_t outcome = RUN_NOTHING;

	if (len > OPENSC2_DOCK_FRAME_LEN || (flags & ~RUN_START_SESSION) != 0) {
		return OSC2_TEST_BAD_ARG;
	}
	/* A SET runs a vendor command on the dock link: only what HOST_SET may run (nothing that
	 * writes flash, reboots into a bootloader, erases or resets). */
	if (len == OPENSC2_DOCK_FRAME_LEN && t.frame[OPENSC2_DOCK_TYPE_OFF] == OPENSC2_DOCK_SET &&
	    !osc2_test_host_set_allowed(t.frame[OPENSC2_DOCK_PAYLOAD_OFF],
					&t.frame[OPENSC2_DOCK_PAYLOAD_OFF + 2],
					t.frame[OPENSC2_DOCK_PAYLOAD_OFF + 1])) {
		return OSC2_TEST_DENIED;
	}
	if (!osc2_dock_active()) {
		if (!(flags & RUN_START_SESSION)) {
			return OSC2_TEST_NO_LINK;
		}
		start_test_session();
	}
	memset(result, 0, RUN_RESULT_LEN);
	if (len > 0) {
		uint8_t ans[OPENSC2_DOCK_FRAME_LEN];
		uint32_t us = 0;
		enum osc2_dock_outcome o = osc2_dock_receive(t.frame, len, ans, &us);

		if (o == OSC2_DOCK_ANSWERED) {
			int32_t rc = (int32_t)opensc2_get_le32(&ans[OPENSC2_DOCK_RESULT_OFF]);
			uint8_t hidden[OPENSC2_DOCK_FRAME_LEN];
			uint8_t nh = 0;

			memcpy(t.answer, ans, sizeof(ans));
			t.have_answer = true;
			mask_answer(rc);
			for (int i = 0; i < OPENSC2_DOCK_FRAME_LEN; i++) {
				if (t.masked[i]) {
					hidden[nh++] = ans[i];
				}
			}
			outcome = RUN_ANSWERED;
			result[2] = ans[0];
			result[3] = ans[OPENSC2_DOCK_TYPE_OFF];
			opensc2_put_le32(&result[4], (uint32_t)rc);
			result[8] = (opensc2_dock_frame_check(ans, sizeof(ans)) == OPENSC2_DOCK_FRAME_OK ?
				     ANS_CRC_OK : 0) | (nh ? ANS_MASKED : 0);
			opensc2_put_le32(&result[9], osc2_crc32(0, ans, sizeof(ans)));
			opensc2_put_le32(&result[13], nh ? osc2_crc32(0, hidden, nh) : 0);
		} else {
			outcome = o == OSC2_DOCK_BAD_SIZE ? RUN_SHORT : RUN_BAD_CRC;
		}
		opensc2_put_le32(&result[17], us);
	}
	result[0] = outcome;
	result[1] = osc2_dock_active() ? 1 : 0;
	*rlen = RUN_RESULT_LEN;
	return OSC2_TEST_OK;
}

static uint8_t op_read(const uint8_t *args, uint8_t *result, uint8_t *rlen)
{
	uint8_t off = args[0], n = args[1];

	if (n == 0 || n > READ_MAX || (size_t)off + n > OPENSC2_DOCK_FRAME_LEN) {
		return OSC2_TEST_BAD_ARG;
	}
	if (!t.have_answer) {
		return OSC2_TEST_BUSY;   /* nothing answered yet */
	}
	result[0] = off;
	result[1] = n;
	for (uint8_t i = 0; i < n; i++) {
		result[2 + i] = t.masked[off + i] ? 0 : t.answer[off + i];
	}
	*rlen = (uint8_t)(2 + n);
	return OSC2_TEST_OK;
}

static void result_step(uint8_t *result, uint8_t *rlen, int event, const struct osc2_dock_step *s)
{
	result[0] = event < 0 ? 0xFF : (uint8_t)event;
	result[1] = (uint8_t)s->from;
	result[2] = (uint8_t)s->to;
	result[3] = s->handled ? 1 : 0;
	opensc2_put_le32(&result[4], s->actions);
	*rlen = 8;
}

static uint8_t op_ctrl(const uint8_t *args, uint8_t *result, uint8_t *rlen)
{
	switch (args[0]) {
	case CTRL_STATUS: {
		struct osc2_dock_info info;
		uint8_t stored[2 + OPENSC2_ANSWER_MAX];

		osc2_dock_info(&info);
		(void)osc2_command_stored(OSC2_LINK_DOCK, stored);
		result[0] = (info.active ? ST_ACTIVE : 0) | (info.alone ? ST_ALONE : 0) |
			    (info.active && info.starter == OSC2_DOCK_BY_TEST ? ST_TEST : 0) | ST_BUILT;
		result[1] = info.last_end;
		opensc2_put_le32(&result[2], info.sessions);
		opensc2_put_le32(&result[6], info.frames);
		opensc2_put_le32(&result[10], info.errors);
		opensc2_put_le32(&result[14], info.slowest_us);
		result[18] = (uint8_t)t.sm.state;
		result[19] = (uint8_t)t.power.kind;
		result[20] = stored[0];   /* the dock's stored request */
		result[21] = info.cache_valid ? 1 : 0;
		*rlen = 22;
		return OSC2_TEST_OK;
	}
	case CTRL_END:
		osc2_dock_session_end(OSC2_DOCK_END_STOPPED);
		return OSC2_TEST_OK;
	case CTRL_PILOT: {
		/* Three readings in mV (0xFFFF: a conversion that failed), the check as between
		 * sessions: one that passes starts a session; a session that runs does not look at
		 * the pilot. */
		int32_t mv[OSC2_DOCK_PILOT_READS];
		uint8_t inside = 0;
		bool present;

		for (int i = 0; i < OSC2_DOCK_PILOT_READS; i++) {
			uint16_t v = opensc2_get_le16(&args[1 + 2 * i]);

			mv[i] = v == 0xFFFFu ? -1 : (int32_t)v;
		}
		present = osc2_dock_pilot_present(mv, &inside);
		osc2_test_note_dock_pilot(mv, inside, present);
		if (present && !osc2_dock_active()) {
			start_test_session();
		}
		result[0] = present ? 1 : 0;
		result[1] = inside;
		result[2] = osc2_dock_active() ? 1 : 0;
		*rlen = 3;
		return OSC2_TEST_OK;
	}
	case CTRL_SUPPLY: {
		/* usb, plus, pilot (0 or 1 each), then how the controller comes onto the dock if the
		 * supply turns out to be the dock while outside the dock states (enum osc2_dock_entry),
		 * and whether this is a warm power-on. */
		struct osc2_dock_power_in in = { .usb = args[1] != 0, .plus = args[2] != 0,
						 .pilot = args[3] != 0 };
		struct osc2_dock_step step = { .from = t.sm.state, .to = t.sm.state };
		int event;

		if (args[1] > 1 || args[2] > 1 || args[3] > 1 || args[4] > OSC2_DOCK_ON_BATTERY ||
		    args[5] > 1) {
			return OSC2_TEST_BAD_ARG;
		}
		event = osc2_dock_power_step(&t.power, &in);
		if (event == OSC2_DOCK_SUPPLY_DOCK && t.sm.state == OSC2_DOCK_AWAY) {
			osc2_dock_sm_enter(&t.sm, (enum osc2_dock_entry)args[4], args[5] != 0, &step);
			osc2_test_note_dock_state(OSC2_TEST_DOCK_ST_ENTER, args[4], 0, &step);
		} else if (event >= 0) {
			osc2_dock_sm_step(&t.sm, (enum osc2_dock_event)event, 0, &step);
			osc2_test_note_dock_state(OSC2_TEST_DOCK_ST_SUPPLY, (uint8_t)event, 0, &step);
		}
		result_step(result, rlen, event, &step);
		return OSC2_TEST_OK;
	}
	case CTRL_EVENT: {
		struct osc2_dock_step step;

		if (args[1] >= OSC2_DOCK_EV_COUNT) {
			return OSC2_TEST_BAD_ARG;
		}
		osc2_dock_sm_step(&t.sm, (enum osc2_dock_event)args[1], args[2], &step);
		osc2_test_note_dock_state(OSC2_TEST_DOCK_ST_EVENT, args[1], args[2], &step);
		result_step(result, rlen, args[1], &step);
		return OSC2_TEST_OK;
	}
	case CTRL_RESET:
		osc2_dock_sm_init(&t.sm);
		memset(&t.power, 0, sizeof(t.power));
		return OSC2_TEST_OK;
	default:
		return OSC2_TEST_BAD_ARG;
	}
}

uint8_t osc2_test_dock_op(uint8_t op, const uint8_t *args, uint8_t alen, uint8_t *result,
			  uint8_t *rlen)
{
	switch (op) {
	case OSC2_TEST_OP_DOCK_LOAD:
		return op_load(args, alen);
	case OSC2_TEST_OP_DOCK_RUN:
		return op_run(args, result, rlen);
	case OSC2_TEST_OP_DOCK_READ:
		return op_read(args, result, rlen);
	case OSC2_TEST_OP_DOCK_CTRL:
		return op_ctrl(args, result, rlen);
	default:
		return OSC2_TEST_BAD_OP;
	}
}
