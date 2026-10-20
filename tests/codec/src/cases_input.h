/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Test cases for the input rules after the injection point (input_rules.c), the touch stabiliser,
 * the parts of lizard mode's processing the shared vectors do not reach (its haptic events, what
 * ends a glide), and the radio's queue of extra reports (report_queue.c). Written once, run two
 * ways, like cases.h: as a Zephyr ztest suite (tests/codec) and as a plain C program on a PC
 * (tests/host). The including file defines OPENSC2_TEST(name), CHECK(cond) and CHECK_EQ(a, b).
 *
 * The expected values follow the real controller's behaviour as the protocol documentation (not
 * public yet; docs/protocol.md lists its pages) describes it, worked out by hand.
 */
#include <errno.h>
#include <string.h>
#include <opensc2/codec.h>
#include <opensc2/settings.h>
#include "input_rules.h"
#include "lizard.h"
#include "report_queue.h"

static void in_defaults(int16_t settings[OPENSC2_SETTINGS_COUNT])
{
	for (int i = 0; i < OPENSC2_SETTINGS_COUNT; i++) {
		settings[i] = opensc2_setting_default((uint8_t)i);
	}
}

/* ---- the distance approximation and the stick-touch fallback -------------------------------- */

OPENSC2_TEST(input_approx_len)
{
	CHECK_EQ(osc2_approx_len(0, 0), 0);
	/* Along an axis 1007/1024 of the length: 12203 is 12000, 12204 is 12001. */
	CHECK_EQ(osc2_approx_len(12203, 0), 12000);
	CHECK_EQ(osc2_approx_len(0, -12204), 12001);
	/* On a diagonal (M < 16 m) 1408/1024: 8727 is 12000, 8728 is 12001. */
	CHECK_EQ(osc2_approx_len(8727, -8727), 12000);
	CHECK_EQ(osc2_approx_len(-8728, 8728), 12001);
	/* M = 16 m: no correction; M just below: the correction. */
	CHECK_EQ(osc2_approx_len(1600, 100), (1007 * 1600 + 441 * 100 + 512) / 1024);
	CHECK_EQ(osc2_approx_len(1599, 100), (1007 * 1599 + 441 * 100 - 40 * 1599 + 512) / 1024);
	/* The largest differences of two pad positions do not overflow. */
	CHECK_EQ(osc2_approx_len(65534, -65534), (1408 * 65534 + 512) / 1024);
	CHECK_EQ(osc2_approx_len(-32768, 0), (1007 * 32768 + 512) / 1024);
}

OPENSC2_TEST(input_stick_fallback)
{
	struct osc2_input_rules r;
	struct opensc2_state s;
	int16_t settings[OPENSC2_SETTINGS_COUNT];

	in_defaults(settings);
	osc2_input_rules_reset(&r);
	memset(&s, 0, sizeof(s));
	s.stick[OPENSC2_LEFT].x = 12204;
	s.stick[OPENSC2_RIGHT].y = -12203;
	osc2_input_rules_apply(&r, OSC2_RULE_STICK_FALLBACK, settings, &s);
	CHECK_EQ(s.buttons, OPENSC2_TOUCH_LEFT_STICK);
	/* Without the rule, or with raw sticks (46 = 1 or 2): nothing. */
	memset(&s, 0, sizeof(s));
	s.stick[OPENSC2_LEFT].x = 30000;
	osc2_input_rules_apply(&r, 0, settings, &s);
	CHECK_EQ(s.buttons, 0);
	settings[OPENSC2_SETTING_RAW_STICKS] = 1;
	osc2_input_rules_apply(&r, OSC2_RULE_STICK_FALLBACK, settings, &s);
	CHECK_EQ(s.buttons, 0);
	/* No hysteresis: back under 12000, the bit goes (the application's own bit stays). */
	settings[OPENSC2_SETTING_RAW_STICKS] = 0;
	s.stick[OPENSC2_LEFT].x = 12000;
	s.buttons = OPENSC2_TOUCH_RIGHT_STICK;
	osc2_input_rules_apply(&r, OSC2_RULE_STICK_FALLBACK, settings, &s);
	CHECK_EQ(s.buttons, OPENSC2_TOUCH_RIGHT_STICK);
}

/* ---- the pads' clicks by pressure ----------------------------------------------------------- */

OPENSC2_TEST(input_pad_click)
{
	/* 40: on above 48 % (15728.16 of 32767), off below 32 % (10485.44). */
	CHECK(!osc2_pad_click(15728, 40, false));
	CHECK(osc2_pad_click(15729, 40, false));
	CHECK(osc2_pad_click(10486, 40, true));
	CHECK(!osc2_pad_click(10485, 40, true));
	CHECK(!osc2_pad_click(12000, 40, false));   /* in between: kept */
	CHECK(osc2_pad_click(12000, 40, true));
	/* -1 counts as 100, and anything below it as -1: never on, off below 92 %. */
	CHECK(!osc2_pad_click(32767, -1, false));
	CHECK(!osc2_pad_click(65535, -7, false));
	CHECK(osc2_pad_click(30146, -1, true));
	CHECK(!osc2_pad_click(30145, -1, true));
	/* Above 100: 100. */
	CHECK(!osc2_pad_click(32767, 300, false));
	CHECK(!osc2_pad_click(30145, 300, true));
	/* Below 9 the click can only end with a new threshold: 0 is on above 8 %, never off. */
	CHECK(osc2_pad_click(2622, 0, false));
	CHECK(osc2_pad_click(0, 0, true));
	CHECK(osc2_pad_click(0, 8, true));
	CHECK(!osc2_pad_click(0, 9, true));
}

OPENSC2_TEST(input_pad_click_rule)
{
	struct osc2_input_rules r;
	struct opensc2_state s;
	int16_t settings[OPENSC2_SETTINGS_COUNT];
	const uint32_t rules = OSC2_RULE_PAD_CLICK | OSC2_RULE_PAD_BLANK;

	in_defaults(settings);
	osc2_input_rules_reset(&r);
	memset(&s, 0, sizeof(s));
	/* Pressed while not touched: the click follows the pressure but is not reported. */
	s.pad[OPENSC2_RIGHT].pressure = 20000;
	osc2_input_rules_apply(&r, rules, settings, &s);
	CHECK_EQ(s.buttons & OPENSC2_CLICK_RIGHT_PAD, 0);
	CHECK(!s.pad[OPENSC2_RIGHT].clicked);
	CHECK(r.pad_click[OPENSC2_RIGHT]);
	/* Touched: reported, also with the pad's flag (no button bit) as the touch. */
	s.pad[OPENSC2_RIGHT].touched = true;
	osc2_input_rules_apply(&r, rules, settings, &s);
	CHECK(s.pad[OPENSC2_RIGHT].clicked);
	CHECK(s.buttons & OPENSC2_CLICK_RIGHT_PAD);
	/* A new threshold clears it: at 60 % the same press is in between, so it stays off. */
	settings[OPENSC2_SETTING_RIGHT_CLICK_PRESSURE] = 60;
	s.pad[OPENSC2_RIGHT].pressure = 18000;
	s.buttons = 0;
	osc2_input_rules_apply(&r, rules, settings, &s);
	CHECK(!s.pad[OPENSC2_RIGHT].clicked);
	CHECK_EQ(s.buttons & OPENSC2_CLICK_RIGHT_PAD, 0);
	/* The application's click is replaced, on the left pad too. */
	s.buttons = OPENSC2_TOUCH_LEFT_PAD | OPENSC2_CLICK_LEFT_PAD;
	s.pad[OPENSC2_LEFT].clicked = true;
	osc2_input_rules_apply(&r, rules, settings, &s);
	CHECK_EQ(s.buttons & OPENSC2_CLICK_LEFT_PAD, 0);
	CHECK(!s.pad[OPENSC2_LEFT].clicked);
	/* A blanked pad (setting 82, the stick touched) reports no click, while the click follows
	 * the pressure. */
	settings[OPENSC2_SETTING_RIGHT_CLICK_PRESSURE] = 40;
	s.buttons = OPENSC2_TOUCH_RIGHT_STICK | OPENSC2_TOUCH_RIGHT_PAD;
	s.pad[OPENSC2_RIGHT].pressure = 32767;
	osc2_input_rules_apply(&r, rules, settings, &s);
	CHECK(r.pad_click[OPENSC2_RIGHT]);
	CHECK_EQ(s.buttons, OPENSC2_TOUCH_RIGHT_STICK);
	CHECK(!s.pad[OPENSC2_RIGHT].clicked);
	CHECK_EQ(s.pad[OPENSC2_RIGHT].pressure, 0);
	/* The rule off: the application's click passes and the memory is cleared. */
	s.buttons = OPENSC2_TOUCH_RIGHT_PAD | OPENSC2_CLICK_RIGHT_PAD;
	osc2_input_rules_apply(&r, 0, settings, &s);
	CHECK(s.buttons & OPENSC2_CLICK_RIGHT_PAD);
	CHECK(!r.pad_click[OPENSC2_RIGHT]);
}

/* ---- setting 82 and the triggers ------------------------------------------------------------ */

OPENSC2_TEST(input_blank_and_triggers)
{
	struct osc2_input_rules r;
	struct opensc2_state s;
	int16_t settings[OPENSC2_SETTINGS_COUNT];

	in_defaults(settings);
	osc2_input_rules_reset(&r);
	memset(&s, 0, sizeof(s));
	/* The left stick by the fallback blanks the left pad; the grip bit stays. */
	s.stick[OPENSC2_LEFT].x = -20000;
	s.buttons = OPENSC2_TOUCH_LEFT_PAD | OPENSC2_CLICK_LEFT_PAD | OPENSC2_TOUCH_LEFT_GRIP;
	s.pad[OPENSC2_LEFT] = (struct opensc2_trackpad){ .x = 100, .y = -100, .pressure = 900,
							 .touched = true, .clicked = true };
	osc2_input_rules_apply(&r, OSC2_RULES_DEFAULT, settings, &s);
	CHECK_EQ(s.buttons, OPENSC2_TOUCH_LEFT_STICK | OPENSC2_TOUCH_LEFT_GRIP);
	CHECK(!s.pad[OPENSC2_LEFT].touched);
	CHECK(!s.pad[OPENSC2_LEFT].clicked);
	CHECK_EQ(s.pad[OPENSC2_LEFT].x, 0);
	CHECK_EQ(s.pad[OPENSC2_LEFT].pressure, 0);
	/* Raw pressure (60 = 1) passes. */
	settings[OPENSC2_SETTING_RAW_PRESSURE] = 1;
	s.pad[OPENSC2_LEFT].pressure = 900;
	osc2_input_rules_apply(&r, OSC2_RULES_DEFAULT, settings, &s);
	CHECK_EQ(s.pad[OPENSC2_LEFT].pressure, 900);
	settings[OPENSC2_SETTING_RAW_PRESSURE] = 0;

	/* Raw triggers (62): no bit, even with the trigger bits' rule off (the application's
	 * bits are cleared), and the hysteresis starts again from off. */
	memset(&s, 0, sizeof(s));
	s.trigger[OPENSC2_LEFT] = 32767;
	osc2_input_rules_apply(&r, OSC2_RULES_DEFAULT, settings, &s);
	CHECK(s.buttons & OPENSC2_CLICK_LEFT_TRIGGER);
	CHECK(r.trigger_bit[OPENSC2_LEFT]);
	settings[OPENSC2_SETTING_RAW_TRIGGERS] = 2;
	osc2_input_rules_apply(&r, OSC2_RULES_DEFAULT, settings, &s);
	CHECK_EQ(s.buttons & OPENSC2_CLICK_LEFT_TRIGGER, 0);
	CHECK(!r.trigger_bit[OPENSC2_LEFT]);
	s.buttons = OPENSC2_CLICK_LEFT_TRIGGER | OPENSC2_CLICK_RIGHT_TRIGGER;
	osc2_input_rules_apply(&r, OSC2_RULE_RAW_TRIGGERS, settings, &s);
	CHECK_EQ(s.buttons, 0);
	/* 62 back to 0: from off, at 30000 (between 85 % and 95 %) the bit stays off. */
	settings[OPENSC2_SETTING_RAW_TRIGGERS] = 0;
	s.trigger[OPENSC2_LEFT] = 30000;
	osc2_input_rules_apply(&r, OSC2_RULES_DEFAULT, settings, &s);
	CHECK_EQ(s.buttons & OPENSC2_CLICK_LEFT_TRIGGER, 0);
}

/* ---- the touch stabiliser ------------------------------------------------------------------- */

OPENSC2_TEST(input_stabiliser)
{
	struct osc2_stabiliser st;
	int32_t x, y;

	osc2_stabiliser_reset(&st);
	/* The touch-down point is the output, and held within the radius. */
	x = 100;
	y = 200;
	osc2_stabiliser_step(&st, OSC2_STABILISER_FINE, true, &x, &y);
	CHECK_EQ(x, 100);
	CHECK_EQ(y, 200);
	x = 4000;
	y = 1000;   /* approx (3900, 800) = 4027: inside */
	osc2_stabiliser_step(&st, OSC2_STABILISER_FINE, true, &x, &y);
	CHECK_EQ(x, 100);
	CHECK_EQ(y, 200);
	/* Out (approx (4900, 800) = 4972): the thumb's movement since the last sample, then an
	 * eighth of the gap plus one on each axis. */
	x = 5000;
	y = 1000;
	osc2_stabiliser_step(&st, OSC2_STABILISER_FINE, true, &x, &y);
	CHECK_EQ(x, 1100 + 3900 / 8 + 1);   /* 100 + 1000, gap 3900 */
	CHECK_EQ(y, 200 + 800 / 8 + 1);     /* no movement on y, gap 800 */
	/* The thumb stops: the gap stays. */
	x = 5000;
	y = 1000;
	osc2_stabiliser_step(&st, OSC2_STABILISER_FINE, true, &x, &y);
	CHECK_EQ(x, 1588);
	CHECK_EQ(y, 301);
	/* Rounded down: a gap of -1 is closed by 2 (one unit too far), +1 by 1. */
	st.ox = 1;
	st.px = 0;
	st.oy = -1;
	st.py = 0;
	x = -1;
	y = 1;
	osc2_stabiliser_step(&st, OSC2_STABILISER_FINE, true, &x, &y);
	CHECK_EQ(x, -2);
	CHECK_EQ(y, 1);
	/* No touch: it starts over. */
	osc2_stabiliser_step(&st, OSC2_STABILISER_FINE, false, &x, &y);
	CHECK(!st.on);
	x = -9000;
	y = 0;
	osc2_stabiliser_step(&st, OSC2_STABILISER_COARSE, true, &x, &y);
	CHECK_EQ(x, -9000);
	x = -1000;   /* 8000 away: within the coarse radius */
	osc2_stabiliser_step(&st, OSC2_STABILISER_COARSE, true, &x, &y);
	CHECK_EQ(x, -9000);

	/* The fine radius along an axis: 4443 away is still held (4369 by the approximation), 4444
	 * is out. Out once, the output follows for the rest of the touch, back inside the radius
	 * too: no second hold. */
	osc2_stabiliser_reset(&st);
	x = 0;
	y = 0;
	osc2_stabiliser_step(&st, OSC2_STABILISER_FINE, true, &x, &y);
	x = 4443;
	osc2_stabiliser_step(&st, OSC2_STABILISER_FINE, true, &x, &y);
	CHECK_EQ(x, 0);
	CHECK(st.holding);
	x = 4444;
	osc2_stabiliser_step(&st, OSC2_STABILISER_FINE, true, &x, &y);
	CHECK(!st.holding);
	CHECK_EQ(x, 1 + 4443 / 8 + 1);   /* the thumb's 1, then an eighth of the gap plus one */
	x = 100;
	osc2_stabiliser_step(&st, OSC2_STABILISER_FINE, true, &x, &y);
	CHECK_EQ(x, 557 - 4344 + 3887 / 8 + 1);   /* moved with the thumb, the gap closing */
	CHECK_EQ(y, 0);
}

/* ---- lizard mode's haptic events and what ends a glide --------------------------------------- */

OPENSC2_TEST(lizard_haptic_events)
{
	struct opensc2_haptic h[4];

	CHECK_EQ(opensc2_lizard_haptics(OPENSC2_LIZARD_KEYBOARD | OPENSC2_LIZARD_MOUSE, h), 0);
	CHECK_EQ(opensc2_lizard_haptics(OPENSC2_LIZARD_HAPTICS, h), 4);
	CHECK_EQ(h[0].target, 0);
	CHECK_EQ(h[0].command.command, OPENSC2_HAPTIC_CMD_CLICK_A);
	CHECK_EQ(h[1].target, 0);
	CHECK_EQ(h[1].command.command, OPENSC2_HAPTIC_CMD_CLICK_B);
	CHECK_EQ(h[2].target, 1);
	CHECK_EQ(h[2].command.command, OPENSC2_HAPTIC_CMD_CLICK_A);
	CHECK_EQ(h[3].target, 1);
	CHECK_EQ(h[3].command.command, OPENSC2_HAPTIC_CMD_CLICK_B);
	for (int i = 0; i < 4; i++) {
		CHECK_EQ(h[i].kind, OPENSC2_HAPTIC_COMMAND);
		CHECK_EQ(h[i].report_id, 0);   /* made by the library */
		CHECK_EQ(h[i].channels, h[i].target ? OPENSC2_HAPTIC_RIGHT_PAD :
						      OPENSC2_HAPTIC_LEFT_PAD);
		CHECK_EQ(h[i].command.gain_db,
			 h[i].command.command == OPENSC2_HAPTIC_CMD_CLICK_A ? 8 : 6);
	}
	CHECK_EQ(opensc2_lizard_haptics(OPENSC2_LIZARD_CLICK_RIGHT, h), 1);
	CHECK_EQ(h[0].channels, OPENSC2_HAPTIC_RIGHT_PAD);
}

/* A touch of 21 frames moving 2000 per frame on the right pad: the flick that glides. */
static void lz_flick(struct opensc2_lizard *lz, const int16_t *settings, uint32_t rules)
{
	struct opensc2_state s;
	uint8_t kb[9], mouse[6];

	memset(&s, 0, sizeof(s));
	s.pad[OPENSC2_RIGHT].touched = true;
	for (int i = 0; i < 21; i++) {
		s.pad[OPENSC2_RIGHT].x = (int16_t)(-21000 + 2000 * i);
		(void)opensc2_lizard_step(lz, &s, settings, rules, kb, mouse);
	}
}

static uint32_t lz_idle(struct opensc2_lizard *lz, const int16_t *settings, uint32_t rules,
			uint8_t mouse[6])
{
	struct opensc2_state s;
	uint8_t kb[9];

	memset(&s, 0, sizeof(s));
	return opensc2_lizard_step(lz, &s, settings, rules, kb, mouse);
}

OPENSC2_TEST(lizard_glide_ends)
{
	struct opensc2_lizard lz;
	int16_t settings[OPENSC2_SETTINGS_COUNT];
	uint8_t kb[9], mouse[6];
	const uint32_t rules = OSC2_RULE_LIZARD_GLIDE;

	in_defaults(settings);
	/* The flick glides: the lift-off frame moves by the flick speed (2000 / 128 = 15). */
	opensc2_lizard_reset(&lz);
	lz_flick(&lz, settings, rules);
	CHECK_EQ(lz_idle(&lz, settings, rules, mouse), OPENSC2_LIZARD_MOUSE);
	CHECK(lz.pad[OPENSC2_RIGHT].gliding);
	/* The resync of the test hooks zeroes the momentum: nothing after it. */
	opensc2_lizard_resync(&lz);
	CHECK(!lz.pad[OPENSC2_RIGHT].gliding);
	CHECK_EQ(lz_idle(&lz, settings, rules, mouse), 0);
	CHECK_EQ(lz_idle(&lz, settings, rules, mouse), 0);
	/* Lizard mode turning off ends it, and turning on starts over. */
	lz_flick(&lz, settings, rules);
	(void)lz_idle(&lz, settings, rules, mouse);
	CHECK(lz.pad[OPENSC2_RIGHT].gliding);
	CHECK_EQ(opensc2_lizard_leave(&lz, kb, mouse), OPENSC2_LIZARD_KEYBOARD | OPENSC2_LIZARD_MOUSE);
	CHECK(!lz.pad[OPENSC2_RIGHT].gliding);
	CHECK_EQ(lz_idle(&lz, settings, rules, mouse), 0);
	(void)opensc2_lizard_enter(&lz, kb, mouse);
	CHECK_EQ(lz_idle(&lz, settings, rules, mouse), 0);
	/* The glide switched off in the middle of one ends it. */
	lz_flick(&lz, settings, rules);
	(void)lz_idle(&lz, settings, rules, mouse);
	(void)lz_idle(&lz, settings, 0, mouse);
	CHECK(!lz.pad[OPENSC2_RIGHT].gliding);
	CHECK_EQ(lz_idle(&lz, settings, rules, mouse), 0);
	/* A dropped link does not: the trackers keep what they had (no frames run meanwhile). */
	lz_flick(&lz, settings, rules);
	(void)lz_idle(&lz, settings, rules, mouse);
	CHECK(lz.pad[OPENSC2_RIGHT].gliding);
	CHECK_EQ(lz.pad[OPENSC2_RIGHT].glide_frame, 1);
}

OPENSC2_TEST(lizard_ticks_need_lizard_mode)
{
	struct opensc2_lizard lz;
	struct opensc2_state s;
	int16_t settings[OPENSC2_SETTINGS_COUNT];
	uint8_t kb[9], mouse[6];
	uint32_t made = 0;

	in_defaults(settings);
	settings[OPENSC2_SETTING_HAPTIC_INCREMENT] = 0;   /* a tick every step */
	opensc2_lizard_reset(&lz);
	memset(&s, 0, sizeof(s));
	s.pad[OPENSC2_LEFT].touched = true;
	(void)opensc2_lizard_step(&lz, &s, settings, OSC2_RULE_LIZARD_TICKS, kb, mouse);
	s.pad[OPENSC2_LEFT].x = 3000;
	CHECK(opensc2_lizard_step(&lz, &s, settings, OSC2_RULE_LIZARD_TICKS, kb, mouse) &
	      OPENSC2_LIZARD_TICK_LEFT);
	/* Off: no step at all, and back on the count starts at the next touch. */
	(void)opensc2_lizard_leave(&lz, kb, mouse);
	s.pad[OPENSC2_LEFT].x = 6000;
	CHECK_EQ(opensc2_lizard_step(&lz, &s, settings, OSC2_RULE_LIZARD_TICKS, kb, mouse), 0);
	(void)opensc2_lizard_enter(&lz, kb, mouse);
	for (int i = 0; i < 4; i++) {
		s.pad[OPENSC2_LEFT].x = (int16_t)(9000 + 3000 * i);
		made |= opensc2_lizard_step(&lz, &s, settings, OSC2_RULE_LIZARD_TICKS, kb, mouse);
	}
	CHECK(made & OPENSC2_LIZARD_TICK_LEFT);   /* the touch after enter counts again */
}

/* ---- the radio's queue of extra reports ------------------------------------------------------ */

OPENSC2_TEST(report_queue)
{
	struct osc2_rq_item items[5];
	struct osc2_rq q;
	uint8_t r[64] = { 0x40 };

	osc2_rq_init(&q, items, 5, 3);
	CHECK(!osc2_rq_backlog(&q));
	CHECK_EQ(osc2_rq_put(&q, r, 0, false), -EINVAL);
	CHECK_EQ(osc2_rq_put(&q, r, 65, false), -EINVAL);
	/* Up to the depth everything queues. */
	for (uint8_t i = 1; i <= 3; i++) {
		r[1] = i;
		CHECK_EQ(osc2_rq_put(&q, r, 6, i == 2), 0);
	}
	CHECK(osc2_rq_backlog(&q));
	/* Past it a droppable report is dropped; the others wait behind, up to the capacity. */
	r[1] = 9;
	CHECK_EQ(osc2_rq_put(&q, r, 15, true), -ENOMEM);
	r[1] = 4;
	CHECK_EQ(osc2_rq_put(&q, r, 9, false), 0);
	r[1] = 5;
	CHECK_EQ(osc2_rq_put(&q, r, 6, false), 0);
	r[1] = 6;
	CHECK_EQ(osc2_rq_put(&q, r, 6, false), -ENOMEM);
	CHECK_EQ(q.count, 5);
	/* In order, oldest first. */
	for (uint8_t i = 0; i < 5; i++) {
		CHECK_EQ(osc2_rq_peek(&q, i)->data[1], i + 1);
	}
	CHECK_EQ(osc2_rq_peek(&q, 3)->len, 9);
	CHECK(osc2_rq_peek(&q, 5) == NULL);
	/* Two went out: the rest moves up, still in order; three left is still a backlog. */
	osc2_rq_take(&q, 2);
	CHECK_EQ(osc2_rq_peek(&q, 0)->data[1], 3);
	CHECK(osc2_rq_backlog(&q));
	osc2_rq_take(&q, 1);
	CHECK(!osc2_rq_backlog(&q));
	/* Around the end of the storage. */
	for (uint8_t i = 6; i <= 8; i++) {
		r[1] = i;
		CHECK_EQ(osc2_rq_put(&q, r, 6, false), 0);
	}
	CHECK_EQ(osc2_rq_peek(&q, 0)->data[1], 4);
	CHECK_EQ(osc2_rq_peek(&q, 4)->data[1], 8);
	osc2_rq_take(&q, 9);   /* more than there are: all */
	CHECK_EQ(q.count, 0);
	CHECK(!osc2_rq_backlog(&q));
	CHECK_EQ(osc2_rq_put(&q, r, 6, true), 0);
	osc2_rq_purge(&q);
	CHECK(osc2_rq_peek(&q, 0) == NULL);
	/* A depth of 0: lossless reports only, and any of them is a backlog. */
	osc2_rq_init(&q, items, 2, 0);
	CHECK_EQ(osc2_rq_put(&q, r, 6, true), -ENOMEM);
	CHECK_EQ(osc2_rq_put(&q, r, 6, false), 0);
	CHECK(osc2_rq_backlog(&q));
}
