/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: <opensc2/docking.h>, the dock as an application sees it: the docked stick
 * offsets (dock_api.c) as the system state machine turns them on and off and as the library's
 * stick processing applies them (cal_store.c), and the calls for a board that carries the dock's
 * frames on a wire of its own (dock_link.c). After cal_store_cases.h, whose helpers it uses.
 */
#include <errno.h>
#include <string.h>
#include <opensc2/docking.h>
#include "dock_link.h"
#include "sys_rules.h"

static const struct opensc2_dock_stick_offsets dk_offsets = {
	.x = { .min = -15, .max = -15, .rest_low = -15, .rest_high = -15 },
	.y = { .min = -80, .max = -28, .rest_low = -65, .rest_high = -65 },
};

OPENSC2_TEST(dock_offsets_apply)
{
	struct opensc2_cal_stick c = { .format = OPENSC2_CAL_FORMAT_ANALOG,
				       .x = { 10, 65530, 1600, 1700 },
				       .y = { 300, 3000, 1550, 1650 } };
	struct opensc2_dock_stick_offsets big = {
		.x = { .min = -11, .max = 20, .rest_low = 0, .rest_high = 0 },
	};

	/* each value moved and clamped to 0..65535 */
	opensc2_dock_stick_offsets_apply(&big, &c);
	CHECK_EQ(c.x.min, 0);
	CHECK_EQ(c.x.max, 65535);
	CHECK_EQ(c.x.rest_low, 1600);
	opensc2_dock_stick_offsets_apply(&dk_offsets, &c);
	CHECK_EQ(c.x.rest_low, 1585);
	CHECK_EQ(c.x.rest_high, 1685);
	CHECK_EQ(c.y.min, 220);
	CHECK_EQ(c.y.max, 2972);
	CHECK_EQ(c.y.rest_low, 1485);
	CHECK_EQ(c.y.rest_high, 1585);
}

OPENSC2_TEST(dock_offsets_follow_the_states)
{
	struct opensc2_cal_stick c = { .format = OPENSC2_CAL_FORMAT_ANALOG,
				       .x = { 300, 3000, 1600, 1700 },
				       .y = { 200, 3100, 1550, 1650 } };
	struct opensc2_cal_stick moved;
	uint32_t gen;

	stub_reset();
	CHECK_EQ(opensc2_dock_set_stick_offsets((enum opensc2_side)2, &dk_offsets), -EINVAL);
	CHECK_EQ(opensc2_dock_set_stick_offsets(OPENSC2_LEFT, &dk_offsets), 0);
	/* not docked: nothing moves */
	CHECK(!opensc2_dock_stick_offsets_active());
	moved = c;
	opensc2_dock_stick_adjust(OPENSC2_LEFT, &moved);
	CHECK(memcmp(&moved, &c, sizeof(c)) == 0);
	/* a power-on on the dock (radio off): on, and the sticks set up again */
	gen = opensc2_cal_generation();
	osc2_dock_offsets_follow(OSC2_SYS_START, OSC2_SYS_DOCK_OFF);
	CHECK(opensc2_dock_stick_offsets_active());
	CHECK(opensc2_cal_generation() != gen);
	moved = c;
	opensc2_dock_stick_adjust(OPENSC2_LEFT, &moved);
	CHECK_EQ(moved.x.min, 285);
	CHECK_EQ(moved.y.rest_high, 1585);
	/* the right stick has none */
	moved = c;
	opensc2_dock_stick_adjust(OPENSC2_RIGHT, &moved);
	CHECK(memcmp(&moved, &c, sizeof(c)) == 0);
	/* an unusable format is left alone */
	moved = c;
	moved.format = 0;
	opensc2_dock_stick_adjust(OPENSC2_LEFT, &moved);
	CHECK_EQ(moved.x.min, 300);
	/* set up again only on a change: a chord on the dock, then the link up on the dock */
	gen = opensc2_cal_generation();
	osc2_dock_offsets_follow(OSC2_SYS_DOCK_OFF, OSC2_SYS_DOCK_CHORD);
	osc2_dock_offsets_follow(OSC2_SYS_DOCK_CHORD, OSC2_SYS_DOCK_ON);
	CHECK(opensc2_dock_stick_offsets_active());
	CHECK_EQ(opensc2_cal_generation(), gen);
	/* new values while docked: set up again at once */
	CHECK_EQ(opensc2_dock_set_stick_offsets(OPENSC2_RIGHT, &dk_offsets), 0);
	CHECK(opensc2_cal_generation() != gen);
	/* lifted off with the link up: off on battery */
	osc2_dock_offsets_follow(OSC2_SYS_DOCK_ON, OSC2_SYS_BATTERY);
	CHECK(!opensc2_dock_stick_offsets_active());
	/* on battery, then onto the dock with the link up: on; lifted off with the radio off: off */
	osc2_dock_offsets_follow(OSC2_SYS_BATTERY, OSC2_SYS_DOCK_ON);
	CHECK(opensc2_dock_stick_offsets_active());
	osc2_dock_offsets_follow(OSC2_SYS_DOCK_ON, OSC2_SYS_DOCK_OFF);
	CHECK(opensc2_dock_stick_offsets_active());
	osc2_dock_offsets_follow(OSC2_SYS_DOCK_OFF, OSC2_SYS_LIFTED);
	CHECK(!opensc2_dock_stick_offsets_active());
	/* a chord while lifted off (battery operation) leaves them off; states away from the dock
	 * change nothing */
	osc2_dock_offsets_follow(OSC2_SYS_LIFTED, OSC2_SYS_CHORD);
	osc2_dock_offsets_follow(OSC2_SYS_CHORD, OSC2_SYS_BATTERY);
	osc2_dock_offsets_follow(OSC2_SYS_BATTERY, OSC2_SYS_USB_LINK_ON);
	CHECK(!opensc2_dock_stick_offsets_active());
	osc2_dock_offsets_follow(OSC2_SYS_LIFTED, OSC2_SYS_DOCK_OFF);
	osc2_dock_offsets_follow(OSC2_SYS_DOCK_OFF, OSC2_SYS_REBOOT);
	CHECK(opensc2_dock_stick_offsets_active());   /* a reboot ends it all anyway */
	/* the application's own say: only without the system state machine */
#if defined(CONFIG_OPENSC2_SYSTEM)
	CHECK_EQ(opensc2_dock_set_resting(false), -EPERM);
	CHECK(opensc2_dock_stick_offsets_active());
#else
	CHECK_EQ(opensc2_dock_set_resting(false), 0);
	CHECK(!opensc2_dock_stick_offsets_active());
	CHECK_EQ(opensc2_dock_set_resting(true), 0);
	CHECK(opensc2_dock_stick_offsets_active());
#endif
	CHECK_EQ(opensc2_dock_set_stick_offsets(OPENSC2_LEFT, NULL), 0);
	moved = c;
	opensc2_dock_stick_adjust(OPENSC2_LEFT, &moved);
	CHECK(memcmp(&moved, &c, sizeof(c)) == 0);
}

#if defined(CONFIG_OPENSC2_CAL)
/* The library's own stick processing takes the offsets while docked. */
OPENSC2_TEST(dock_offsets_in_processing)
{
	static const struct opensc2_dock_stick_offsets rest_up = {
		.x = { .min = 0, .max = 0, .rest_low = 200, .rest_high = 200 },
	};
	struct opensc2_state s;
	int16_t away;

	stub_reset();
	CHECK_EQ(cal_named(0xEE, "cal/joy_l", cal_joy_bytes, 18), 0);
	CHECK_EQ(opensc2_dock_set_stick_offsets(OPENSC2_LEFT, &rest_up), 0);
	/* the stick at its stored rest (1650 mV, 2048 counts) */
	opensc2_set_stick_raw(OPENSC2_LEFT, 2048, 2048);
	cal_frame_state(&s);
	away = s.stick[OPENSC2_LEFT].x;
	CHECK(away > -400 && away < 400);
	/* docked: the rest band 200 mV higher, so the same reading is below the centre */
	osc2_dock_offsets_follow(OSC2_SYS_START, OSC2_SYS_DOCK_OFF);
	cal_frame_state(&s);
	CHECK(s.stick[OPENSC2_LEFT].x < away - 3000);
	/* lifted off: as before */
	osc2_dock_offsets_follow(OSC2_SYS_DOCK_OFF, OSC2_SYS_LIFTED);
	cal_frame_state(&s);
	CHECK_EQ(s.stick[OPENSC2_LEFT].x, away);
}
#endif

/* A board that carries the frames itself: the public calls. */
OPENSC2_TEST(dock_public_wire)
{
	uint8_t req[OPENSC2_DOCK_FRAME_LEN], ans[OPENSC2_DOCK_FRAME_LEN];
	struct opensc2_dock_status st;

	stub_reset();
	(void)opensc2_dock_build_request(OPENSC2_DOCK_PROBE, NULL, 0, req);
	CHECK_EQ(opensc2_dock_frame(req, sizeof(req), ans), -ENOTCONN);
	CHECK_EQ(opensc2_dock_session_end(), -EALREADY);
	CHECK_EQ(opensc2_dock_session_start(), 0);
	CHECK_EQ(opensc2_dock_session_start(), -EALREADY);
	CHECK_EQ(opensc2_dock_frame(req, sizeof(req), ans), 0);
	CHECK_EQ(ans[0], 0xAC);
	CHECK_EQ(opensc2_get_le32(&ans[8]), 0x1302);
	opensc2_dock_answer_sent();
	CHECK_EQ(opensc2_dock_status(&st), 0);
	CHECK(st.session);
	CHECK(!st.alone);   /* USB is up (the stand-ins' boot) */
	CHECK_EQ(st.frames, 1);
	CHECK_EQ(st.sessions, 1);
	CHECK(!st.pilot);
	CHECK_EQ(st.supply, OPENSC2_DOCK_SUPPLY_NONE);
	/* the dock alone: GET input builds its report once the answer is sent */
	stub.link_active[OSC2_LINK_USB] = false;
	(void)opensc2_dock_build_request(OPENSC2_DOCK_INPUT, NULL, 0, req);
	CHECK_EQ(opensc2_dock_frame(req, sizeof(req), ans), 0);
	CHECK_EQ(dock_stub.samples, 0);
	opensc2_dock_answer_sent();
	CHECK_EQ(dock_stub.samples, 1);
	CHECK_EQ(opensc2_dock_status(&st), 0);
	CHECK(st.alone);
	stub.link_active[OSC2_LINK_USB] = true;
	/* a short frame ends the session */
	CHECK_EQ(opensc2_dock_frame(req, 40, ans), -EMSGSIZE);
	CHECK_EQ(opensc2_dock_status(&st), 0);
	CHECK(!st.session);
	CHECK_EQ(st.errors, 1);
	/* a bad CRC too */
	CHECK_EQ(opensc2_dock_session_start(), 0);
	req[20] ^= 1;
	CHECK_EQ(opensc2_dock_frame(req, sizeof(req), ans), -EBADMSG);
	CHECK_EQ(opensc2_dock_session_end(), -EALREADY);
	/* the pilot and the supply as the board's glue reports them */
	osc2_dock_pilot_seen(true);
	CHECK_EQ(osc2_dock_supply_step(true, false, NULL), OSC2_DOCK_SUPPLY_USB);
	CHECK_EQ(opensc2_dock_status(&st), 0);
	CHECK(st.pilot);
	CHECK_EQ(st.supply, OPENSC2_DOCK_SUPPLY_USB);
	CHECK(!st.stick_offsets);
	osc2_dock_offsets_docked(true);
	CHECK_EQ(opensc2_dock_status(&st), 0);
	CHECK(st.stick_offsets);
	osc2_dock_offsets_docked(false);
}
