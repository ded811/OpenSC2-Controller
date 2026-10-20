/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests of the system state machine's pure rules: the chords (chord_rules.c), the idle monitor
 * (idle_rules.c), the machine itself (sys_rules.c: the power-on decision, the states on USB power,
 * on the dock and on battery, the chord states, the shutdowns, the cues and their levels) and the
 * status LED (led_rules.c: the brightness curve, the duty, the pattern player). The expected
 * values are the protocol documentation's (not public yet; docs/protocol.md lists the pages),
 * written out by hand.
 */
#include <string.h>
#include <opensc2/led.h>
#include "chord_rules.h"
#include "idle_rules.h"
#include "sys_rules.h"

/* ---- chords ---------------------------------------------------------------------------------- */

OPENSC2_TEST(chord_match)
{
	const uint32_t steam = OPENSC2_BTN_STEAM;

	CHECK_EQ(osc2_chord_match(0x00010000u, false), OSC2_CHORD_STEAM);
	CHECK_EQ(osc2_chord_match(0x00000201u, false), OSC2_CHORD_PUCK1);   /* A + RB */
	CHECK_EQ(osc2_chord_match(0x00010201u, false), OSC2_CHORD_PUCK1);   /* with STEAM */
	CHECK_EQ(osc2_chord_match(0x00080001u, false), OSC2_CHORD_PUCK2);   /* A + LB */
	CHECK_EQ(osc2_chord_match(0x00090001u, false), OSC2_CHORD_PUCK2);
	CHECK_EQ(osc2_chord_match(0x00070180u, false), OSC2_CHORD_KEYLOCK);
	/* The Bluetooth chords only exist with Bluetooth. */
	CHECK_EQ(osc2_chord_match(0x00000202u, false), OSC2_CHORD_NONE);
	CHECK_EQ(osc2_chord_match(0x00000202u, true), OSC2_CHORD_BT1);    /* B + RB */
	CHECK_EQ(osc2_chord_match(0x00010202u, true), OSC2_CHORD_BT1);
	CHECK_EQ(osc2_chord_match(0x00080002u, true), OSC2_CHORD_BT2);    /* B + LB */
	CHECK_EQ(osc2_chord_match(0x00090002u, true), OSC2_CHORD_BT2);
	/* Exact words: one more button spoils a chord; the keylock needs STEAM. */
	CHECK_EQ(osc2_chord_match(steam | OPENSC2_BTN_X, false), OSC2_CHORD_NONE);
	CHECK_EQ(osc2_chord_match(0x00000201u | OPENSC2_BTN_X, false), OSC2_CHORD_NONE);
	CHECK_EQ(osc2_chord_match(0x00060180u, false), OSC2_CHORD_NONE);
	/* The stick-touch, trigger-digital and grip bits are masked out ... */
	CHECK_EQ(osc2_chord_match(steam | OPENSC2_TOUCH_LEFT_STICK | OPENSC2_TOUCH_RIGHT_STICK |
				  OPENSC2_CLICK_LEFT_TRIGGER | OPENSC2_CLICK_RIGHT_TRIGGER |
				  OPENSC2_TOUCH_LEFT_GRIP | OPENSC2_TOUCH_RIGHT_GRIP, false),
		 OSC2_CHORD_STEAM);
	/* ... the pad touch and click bits are not: a thumb on a pad spoils a chord. */
	CHECK_EQ(osc2_chord_match(steam | OPENSC2_TOUCH_RIGHT_PAD, false), OSC2_CHORD_NONE);
	CHECK_EQ(osc2_chord_match(steam | OPENSC2_CLICK_LEFT_PAD, false), OSC2_CHORD_NONE);
	CHECK_EQ(osc2_chord_match(0, false), OSC2_CHORD_NONE);
	CHECK(osc2_chord_is_transport(OSC2_CHORD_STEAM));
	CHECK(osc2_chord_is_transport(OSC2_CHORD_PUCK2));
	CHECK(!osc2_chord_is_transport(OSC2_CHORD_KEYLOCK));
	CHECK(!osc2_chord_is_transport(OSC2_CHORD_NONE));
}

OPENSC2_TEST(chord_timers)
{
	struct osc2_chords c;
	struct osc2_chord_out o;

	/* STEAM alone: down at once, "pressed" at 20 ms, "held" at 7 s, up at the release. */
	osc2_chords_init(&c, false, 0, 1000, &o);
	CHECK_EQ(o.events, 0);
	CHECK_EQ(osc2_chords_next(&c, 1000), -1);
	osc2_chords_update(&c, OPENSC2_BTN_STEAM, 1000, &o);
	CHECK_EQ(o.events, OSC2_CHORD_EV_DOWN);
	CHECK_EQ(o.down, OSC2_CHORD_STEAM);
	CHECK_EQ(osc2_chords_next(&c, 1000), 20);
	osc2_chords_poll(&c, 1019, &o);
	CHECK_EQ(o.events, 0);
	osc2_chords_poll(&c, 1020, &o);
	CHECK_EQ(o.events, OSC2_CHORD_EV_STEAM_PRESS);
	CHECK_EQ(osc2_chords_next(&c, 1020), 7000 - 20);
	osc2_chords_poll(&c, 8000, &o);
	CHECK_EQ(o.events, OSC2_CHORD_EV_STEAM_HOLD);
	CHECK_EQ(osc2_chords_next(&c, 8000), -1);
	osc2_chords_poll(&c, 9000, &o);
	CHECK_EQ(o.events, 0);                         /* each fires once */
	osc2_chords_update(&c, 0, 9000, &o);
	CHECK_EQ(o.events, OSC2_CHORD_EV_UP | OSC2_CHORD_EV_STEAM_UP);
	CHECK_EQ(o.up, OSC2_CHORD_STEAM);

	/* A release before 20 ms cancels both timers. */
	osc2_chords_update(&c, OPENSC2_BTN_STEAM, 10000, &o);
	osc2_chords_update(&c, 0, 10010, &o);
	CHECK_EQ(osc2_chords_next(&c, 10010), -1);
	osc2_chords_poll(&c, 20000, &o);
	CHECK_EQ(o.events, 0);

	/* STEAM, then A + RB with STEAM: STEAM alone releases, the transport chord matches. */
	osc2_chords_update(&c, OPENSC2_BTN_STEAM, 30000, &o);
	osc2_chords_update(&c, OPENSC2_BTN_STEAM | OPENSC2_BTN_A, 30005, &o);
	CHECK_EQ(o.events, OSC2_CHORD_EV_UP);
	CHECK_EQ(o.up, OSC2_CHORD_STEAM);
	osc2_chords_update(&c, OPENSC2_BTN_STEAM | OPENSC2_BTN_A | OPENSC2_BTN_R, 30010, &o);
	CHECK_EQ(o.events, OSC2_CHORD_EV_DOWN);
	CHECK_EQ(o.down, OSC2_CHORD_PUCK1);
	CHECK_EQ(osc2_chords_next(&c, 30010), -1);     /* no STEAM timers with another button */
	/* A bit the chords ignore changes nothing. */
	osc2_chords_update(&c, OPENSC2_BTN_STEAM | OPENSC2_BTN_A | OPENSC2_BTN_R |
			   OPENSC2_TOUCH_LEFT_GRIP, 30020, &o);
	CHECK_EQ(o.events, 0);
	CHECK_EQ(c.current, OSC2_CHORD_PUCK1);

	/* The keylock: the 5 ms tap, 3 s, and no DOWN or UP (it is no transport chord). */
	osc2_chords_update(&c, 0x00070180u, 40000, &o);
	CHECK_EQ(o.events, OSC2_CHORD_EV_UP);
	CHECK_EQ(osc2_chords_next(&c, 40000), 5);
	osc2_chords_poll(&c, 40005, &o);
	CHECK_EQ(o.events, OSC2_CHORD_EV_KEYLOCK_TAP);
	osc2_chords_poll(&c, 43005, &o);
	CHECK_EQ(o.events, OSC2_CHORD_EV_KEYLOCK_HOLD);
	osc2_chords_update(&c, 0, 43100, &o);
	CHECK_EQ(o.events, OSC2_CHORD_EV_STEAM_UP);

	/* At boot a held chord matches at once; STEAM alone starts its timers then. */
	osc2_chords_init(&c, false, OPENSC2_BTN_STEAM, 500, &o);
	CHECK_EQ(o.events, OSC2_CHORD_EV_DOWN);
	CHECK_EQ(o.down, OSC2_CHORD_STEAM);
	CHECK_EQ(osc2_chords_next(&c, 500), 20);
	osc2_chords_init(&c, true, 0x00010202u, 500, &o);
	CHECK_EQ(o.down, OSC2_CHORD_BT1);
	/* The same word again does nothing. */
	osc2_chords_update(&c, 0x00010202u, 600, &o);
	CHECK_EQ(o.events, 0);
}

/* ---- the idle monitor ------------------------------------------------------------------------ */

OPENSC2_TEST(idle_monitor)
{
	struct osc2_idle m;
	uint32_t t, fired_at = 0;

	CHECK(osc2_idle_enabled(900));
	CHECK(osc2_idle_enabled(9998));
	CHECK(!osc2_idle_enabled(9999));
	CHECK(!osc2_idle_enabled(0));
	CHECK(!osc2_idle_enabled(-5));

	/* 900 s: 16 periods of 56.25 s, cut to 56250 ms; the history starts full. */
	osc2_idle_start(&m, 900, 0);
	CHECK_EQ(m.period_ms, 56250);
	CHECK_EQ(m.history, 0xFFFF);
	CHECK(osc2_idle_recent(&m));
	CHECK_EQ(osc2_idle_next(&m, 0), 56250);
	/* No input at all: the first idle event at the end of the 16th period. */
	for (t = 0; t <= 17 * 56250u; t += 250) {
		if (osc2_idle_poll(&m, t) && !fired_at) {
			fired_at = t;
		}
	}
	CHECK_EQ(fired_at, 16 * 56250u);
	CHECK(!osc2_idle_recent(&m));
	/* ... and again at the end of every further idle period. */
	CHECK(osc2_idle_poll(&m, 18 * 56250u));

	/* Activity in a period keeps it alive: 16 to 17 periods after the last one. */
	osc2_idle_start(&m, 16, 0);              /* 16 s: 1000 ms periods */
	CHECK_EQ(m.period_ms, 1000);
	osc2_idle_activity(&m);                  /* in the first period */
	fired_at = 0;
	for (t = 0; t <= 30000; t += 50) {
		if (osc2_idle_poll(&m, t) && !fired_at) {
			fired_at = t;
		}
	}
	CHECK_EQ(fired_at, 17000);

	/* A change of setting 50 refills the history and starts a new period; the same value
	 * changes nothing. */
	osc2_idle_start(&m, 16, 0);
	(void)osc2_idle_poll(&m, 5000);
	CHECK_EQ(m.history, 0xFFE0);
	osc2_idle_setting(&m, 16, 5500);
	CHECK_EQ(m.history, 0xFFE0);
	osc2_idle_setting(&m, 32, 5500);
	CHECK_EQ(m.history, 0xFFFF);
	CHECK_EQ(m.period_ms, 2000);
	CHECK_EQ(osc2_idle_next(&m, 5500), 2000);
	/* Off: never idle, always "recent". */
	osc2_idle_setting(&m, 0, 6000);
	CHECK_EQ(osc2_idle_next(&m, 6000), -1);
	CHECK(!osc2_idle_poll(&m, 1000000));
	CHECK(osc2_idle_recent(&m));

	/* The activity rules. */
	CHECK(osc2_idle_buttons_active(0, OPENSC2_BTN_A));                 /* a press */
	CHECK(!osc2_idle_buttons_active(OPENSC2_BTN_A, OPENSC2_BTN_A));    /* held: no change */
	CHECK(!osc2_idle_buttons_active(OPENSC2_BTN_A, 0));                /* a release alone */
	CHECK(osc2_idle_buttons_active(OPENSC2_BTN_A | OPENSC2_BTN_B, OPENSC2_BTN_A));
	CHECK(osc2_idle_buttons_active(0, OPENSC2_TOUCH_LEFT_PAD));        /* a pad touched */
	CHECK(osc2_idle_buttons_active(0, OPENSC2_TOUCH_RIGHT_STICK));     /* a stick touched */
	CHECK(!osc2_idle_buttons_active(0, OPENSC2_TOUCH_LEFT_GRIP));      /* grips do not count */
	{
		struct opensc2_state s;

		memset(&s, 0, sizeof(s));
		CHECK(!osc2_idle_state_active(&s));
		s.stick[OPENSC2_LEFT].x = 4000;
		CHECK(!osc2_idle_state_active(&s));
		s.stick[OPENSC2_LEFT].x = -4001;
		CHECK(osc2_idle_state_active(&s));
		s.stick[OPENSC2_LEFT].x = 0;
		s.stick[OPENSC2_RIGHT].y = 4001;
		CHECK(osc2_idle_state_active(&s));
		s.stick[OPENSC2_RIGHT].y = 0;
		s.trigger[OPENSC2_RIGHT] = 1;
		CHECK(osc2_idle_state_active(&s));
		s.trigger[OPENSC2_RIGHT] = 0;
		s.pad[OPENSC2_LEFT].touched = true;
		CHECK(osc2_idle_state_active(&s));
		s.pad[OPENSC2_LEFT].touched = false;
		s.imu.gyro[0] = 30000;                 /* motion does not count */
		s.buttons = OPENSC2_TOUCH_RIGHT_GRIP;
		CHECK(!osc2_idle_state_active(&s));
	}
}

/* ---- the machine ----------------------------------------------------------------------------- */

static struct osc2_sys sm;
static struct osc2_sys_in sysin;
static struct osc2_sys_out sysout;

static void sys_in_reset(void)
{
	memset(&sysin, 0, sizeof(sysin));
	sysin.stored = OSC2_TRANSPORT_PUCK1;
	sysin.level = 255;
	sysin.link_up = true;
	sysin.plug_in_reboot = true;
}

static void sys_boot(bool wired, bool warm, uint8_t source, uint32_t now)
{
	struct osc2_sys_boot_in b = { .wired = wired, .warm = warm, .source = source };

	osc2_sys_boot(&sm, &sysin, &b, now, &sysout);
}

static void ev(uint8_t e, uint8_t arg, uint32_t now)
{
	osc2_sys_event(&sm, &sysin, e, arg, now, &sysout);
}

#define HAS(action) ((sysout.actions & (action)) != 0)

OPENSC2_TEST(sys_power_on)
{
	/* Wired: the power-on chime, the LED left as it is, a USB power-on deletes the keylock. */
	sys_in_reset();
	sysin.keylock = true;
	sys_boot(true, false, OSC2_SRC_USB, 0);
	CHECK_EQ(sm.state, OSC2_SYS_WIRED_WAIT);
	CHECK(HAS(OSC2_SYS_DO_CUE));
	CHECK_EQ(sysout.cue, OSC2_CUE_POWER_ON);
	CHECK(!HAS(OSC2_SYS_DO_LED));
	CHECK(HAS(OSC2_SYS_DO_KEYLOCK_DEL));
	CHECK(!HAS(OSC2_SYS_DO_START));
	CHECK_EQ(osc2_sys_next(&sm, 0), 500);
	CHECK_EQ(sm.boot_state, OSC2_SYS_WIRED_WAIT);
	CHECK_EQ(sm.boot_cue, OSC2_CUE_POWER_ON);

	/* Wireless on USB power (the warm restart): the stored transport's cue, its link started. */
	sys_in_reset();
	sys_boot(false, true, OSC2_SRC_USB, 0);
	CHECK_EQ(sm.state, OSC2_SYS_USB_LINK_ON);
	CHECK_EQ(sysout.cue, 3);
	CHECK(HAS(OSC2_SYS_DO_START));
	CHECK_EQ(sysout.transport, OSC2_TRANSPORT_PUCK1);
	CHECK(!sysout.pairing);
	CHECK_EQ(sysout.led_mode, OSC2_LED_PUCK);
	CHECK_EQ(sysout.led_flags, OSC2_LED_EXTERNAL);
	sysin.stored = OSC2_TRANSPORT_PUCK2;
	sys_boot(false, true, OSC2_SRC_USB, 0);
	CHECK_EQ(sysout.cue, 4);
	CHECK_EQ(sysout.transport, OSC2_TRANSPORT_PUCK2);
	sysin.stored = OSC2_TRANSPORT_BT2;
	sys_boot(false, true, OSC2_SRC_USB, 0);
	CHECK_EQ(sysout.cue, 2);
	CHECK_EQ(sysout.led_mode, OSC2_LED_BLUETOOTH);
	/* A stored value above 3: the main puck, without a cue. */
	sysin.stored = 9;
	sys_boot(false, true, OSC2_SRC_USB, 0);
	CHECK(!HAS(OSC2_SYS_DO_CUE));
	CHECK_EQ(sysout.transport, OSC2_TRANSPORT_PUCK1);

	/* The dock: after a warm reboot the stored link without a cue; otherwise radio off. */
	sys_in_reset();
	sysin.keylock = true;
	sys_boot(false, true, OSC2_SRC_DOCK, 0);
	CHECK_EQ(sm.state, OSC2_SYS_DOCK_ON);
	CHECK(HAS(OSC2_SYS_DO_START));
	CHECK(!HAS(OSC2_SYS_DO_CUE));
	CHECK(HAS(OSC2_SYS_DO_KEYLOCK_DEL));
	sys_boot(false, false, OSC2_SRC_DOCK, 0);
	CHECK_EQ(sm.state, OSC2_SYS_DOCK_OFF);
	CHECK_EQ(sysout.led_mode, OSC2_LED_AMBER);
	CHECK(!HAS(OSC2_SYS_DO_START));

	/* On battery, in order: 0 % off silently; the keylock; 2 % or less the low-battery
	 * shutdown; a held chord; the battery wait. */
	sys_in_reset();
	sysin.level = 0;
	sysin.keylock = true;
	sys_boot(false, false, OSC2_SRC_BATTERY, 0);
	CHECK_EQ(sm.state, OSC2_SYS_SHUTDOWN_SILENT);
	CHECK(HAS(OSC2_SYS_DO_END));
	CHECK_EQ(sysout.end, OSC2_SYS_END_POWER_OFF);
	CHECK(!HAS(OSC2_SYS_DO_CUE));
	CHECK(!HAS(OSC2_SYS_DO_KEYLOCK_DEL));        /* a battery power-on keeps it */
	sysin.level = 50;
	sys_boot(false, false, OSC2_SRC_BATTERY, 0);
	CHECK_EQ(sm.state, OSC2_SYS_SHUTDOWN_LOCKED);
	CHECK_EQ(sysout.led_mode, OSC2_LED_KEYLOCK);
	CHECK(!HAS(OSC2_SYS_DO_CUE));
	CHECK_EQ(osc2_sys_next(&sm, 0), 500);
	sysin.keylock = false;
	sysin.level = 2;
	sys_boot(false, false, OSC2_SRC_BATTERY, 0);
	CHECK_EQ(sm.state, OSC2_SYS_SHUTDOWN_LOW);
	CHECK_EQ(sysout.led_mode, OSC2_LED_LOW_BATTERY);
	CHECK_EQ(sysout.cue, OSC2_CUE_POWER_OFF);
	sysin.level = 3;
	sysin.chord = OSC2_CHORD_PUCK2;
	sys_boot(false, false, OSC2_SRC_BATTERY, 0);
	CHECK_EQ(sm.state, OSC2_SYS_CHORD);
	CHECK_EQ(sysout.cue, 4);
	CHECK_EQ(sysout.led_mode, OSC2_LED_PUCK);
	CHECK_EQ(sysout.led_flags, OSC2_LED_STEADY);
	CHECK_EQ(sm.chosen, OSC2_TRANSPORT_PUCK2);
	CHECK_EQ(sm.boot_chord, OSC2_CHORD_PUCK2);
	CHECK_EQ(osc2_sys_wait(&sm, 0), 500);
	/* 255, no level yet after a cold start, counts as high. */
	sysin.level = 255;
	sysin.chord = OSC2_CHORD_NONE;
	sys_boot(false, false, OSC2_SRC_BATTERY, 0);
	CHECK_EQ(sm.state, OSC2_SYS_BATTERY_WAIT);
	CHECK_EQ(sysout.cue, 3);
	CHECK(!HAS(OSC2_SYS_DO_START));
	CHECK_EQ(osc2_sys_next(&sm, 0), 1000);
}

OPENSC2_TEST(sys_battery_wait_and_chords)
{
	/* The battery wait: 1 s, then the link and battery operation; chords and STEAM do not
	 * count; USB power reboots (the next boot is wired); the dock goes radio-off. */
	sys_in_reset();
	sys_boot(false, false, OSC2_SRC_BATTERY, 0);
	ev(OSC2_SYS_EV_STEAM_PRESSED, 0, 100);
	CHECK_EQ(sm.state, OSC2_SYS_BATTERY_WAIT);
	ev(OSC2_SYS_EV_CHORD_DOWN, OSC2_CHORD_PUCK2, 200);
	CHECK_EQ(sm.state, OSC2_SYS_BATTERY_WAIT);
	ev(OSC2_SYS_EV_TIMER, 0, 999);
	CHECK_EQ(sm.state, OSC2_SYS_BATTERY_WAIT);   /* not due */
	ev(OSC2_SYS_EV_TIMER, 0, 1000);
	CHECK_EQ(sm.state, OSC2_SYS_BATTERY);
	CHECK(HAS(OSC2_SYS_DO_START));
	CHECK_EQ(sysout.transport, OSC2_TRANSPORT_PUCK1);
	CHECK_EQ(sysout.led_mode, OSC2_LED_PUCK);
	CHECK_EQ(sysout.led_flags, 0);
	sys_boot(false, false, OSC2_SRC_BATTERY, 0);
	ev(OSC2_SYS_EV_POWER_USB, 0, 300);
	CHECK_EQ(sm.state, OSC2_SYS_REBOOT);
	CHECK(HAS(OSC2_SYS_DO_END));
	CHECK_EQ(sysout.end, OSC2_SYS_END_REBOOT);
	CHECK_EQ(sysout.end_cause, OSC2_SYS_EV_POWER_USB);
	sys_boot(false, false, OSC2_SRC_BATTERY, 0);
	ev(OSC2_SYS_EV_POWER_DOCK, 0, 300);
	CHECK_EQ(sm.state, OSC2_SYS_DOCK_OFF);

	/* A power-on chord: released within 4 s, the link without the pairing flag; the release
	 * is handled after the 500 ms pause. */
	sys_in_reset();
	sysin.chord = OSC2_CHORD_STEAM;
	sys_boot(false, false, OSC2_SRC_BATTERY, 0);
	CHECK_EQ(sm.state, OSC2_SYS_CHORD);
	CHECK_EQ(sysout.cue, 3);                    /* STEAM alone: the stored transport's cue */
	CHECK_EQ(osc2_sys_wait(&sm, 200), 300);
	CHECK_EQ(osc2_sys_wait(&sm, 500), 0);
	ev(OSC2_SYS_EV_POWER_USB, 0, 600);        /* power changes inside a chord state are lost */
	CHECK_EQ(sm.state, OSC2_SYS_CHORD);
	ev(OSC2_SYS_EV_CHORD_UP, OSC2_CHORD_PUCK1, 700);   /* not its own release */
	CHECK_EQ(sm.state, OSC2_SYS_CHORD);
	ev(OSC2_SYS_EV_CHORD_UP, OSC2_CHORD_STEAM, 800);
	CHECK_EQ(sm.state, OSC2_SYS_BATTERY);
	CHECK(HAS(OSC2_SYS_DO_START));
	CHECK(!sysout.pairing);
	CHECK(!sm.boot_pairing);
	CHECK_EQ(osc2_sys_next(&sm, 800), -1);   /* the 4 s timer stopped */
	/* Held 4 s: the pairing flag. */
	sysin.chord = OSC2_CHORD_PUCK1;
	sys_boot(false, false, OSC2_SRC_BATTERY, 0);
	ev(OSC2_SYS_EV_TIMER, 0, 4000);
	CHECK_EQ(sm.state, OSC2_SYS_BATTERY);
	CHECK(sysout.pairing);
	CHECK(sm.boot_pairing);
	CHECK_EQ(sysout.transport, OSC2_TRANSPORT_PUCK1);
}

OPENSC2_TEST(sys_battery_rules)
{
	static const uint8_t to_shutdown[] = { OSC2_SYS_EV_STEAM_HELD, OSC2_SYS_EV_IDLE,
					       OSC2_SYS_EV_WIRELESS_OFF, OSC2_SYS_EV_TURN_OFF };

	for (size_t i = 0; i < sizeof(to_shutdown); i++) {
		sys_in_reset();
		sys_boot(false, false, OSC2_SRC_BATTERY, 0);
		ev(OSC2_SYS_EV_TIMER, 0, 1000);
		ev(to_shutdown[i], 0, 2000);
		CHECK_EQ(sm.state, OSC2_SYS_SHUTDOWN);
		CHECK(HAS(OSC2_SYS_DO_IR_OFF) && HAS(OSC2_SYS_DO_DROP) && HAS(OSC2_SYS_DO_CUE));
		CHECK_EQ(sysout.cue, OSC2_CUE_POWER_OFF);
		CHECK_EQ(sysout.led_mode, OSC2_LED_OFF);
		CHECK_EQ(osc2_sys_next(&sm, 2000), 500);
	}
	/* The end waits for STEAM to be up, then powers off (setting 77: reboots). */
	sysin.steam = true;
	ev(OSC2_SYS_EV_TIMER, 0, 2500);
	CHECK(!HAS(OSC2_SYS_DO_END));
	CHECK(sm.steam_wait);
	CHECK_EQ(osc2_sys_next(&sm, 2500), 10);
	ev(OSC2_SYS_EV_TIMER, 0, 2510);
	CHECK(!HAS(OSC2_SYS_DO_END));
	sysin.steam = false;
	ev(OSC2_SYS_EV_STEAM_UP, 0, 2600);
	CHECK(HAS(OSC2_SYS_DO_END));
	CHECK_EQ(sysout.end, OSC2_SYS_END_POWER_OFF);
	CHECK_EQ(sysout.end_state, OSC2_SYS_SHUTDOWN);
	CHECK_EQ(sysout.end_cause, OSC2_SYS_EV_TURN_OFF);
	CHECK(sm.ended);
	ev(OSC2_SYS_EV_POWER_USB, 0, 2700);       /* nothing happens after the end */
	CHECK_EQ(sysout.actions, 0);
	sys_in_reset();
	sysin.reboot_not_off = true;
	sys_boot(false, false, OSC2_SRC_BATTERY, 0);
	ev(OSC2_SYS_EV_TIMER, 0, 1000);
	ev(OSC2_SYS_EV_STEAM_HELD, 0, 8000);
	ev(OSC2_SYS_EV_TIMER, 0, 8500);
	CHECK_EQ(sysout.end, OSC2_SYS_END_REBOOT);

	/* 0 % while running: the low-battery shutdown, which ignores setting 77. */
	ev(OSC2_SYS_EV_LEVEL, 0, 0);
	sys_boot(false, false, OSC2_SRC_BATTERY, 0);
	ev(OSC2_SYS_EV_TIMER, 0, 1000);
	ev(OSC2_SYS_EV_LEVEL, 1, 2000);
	CHECK_EQ(sm.state, OSC2_SYS_BATTERY);
	ev(OSC2_SYS_EV_LEVEL, 0, 3000);
	CHECK_EQ(sm.state, OSC2_SYS_SHUTDOWN_LOW);
	CHECK_EQ(sysout.led_mode, OSC2_LED_LOW_BATTERY);
	ev(OSC2_SYS_EV_TIMER, 0, 3500);
	CHECK_EQ(sysout.end, OSC2_SYS_END_POWER_OFF);

	/* The keylock chord held 3 s: stored, then the ordinary shutdown. */
	sys_in_reset();
	sys_boot(false, false, OSC2_SRC_BATTERY, 0);
	ev(OSC2_SYS_EV_TIMER, 0, 1000);
	ev(OSC2_SYS_EV_KEYLOCK_HELD, 0, 5000);
	CHECK(HAS(OSC2_SYS_DO_KEYLOCK_SET));
	CHECK_EQ(sm.state, OSC2_SYS_SHUTDOWN);

	/* USB plugged in: with the link up charging, the link kept; with no link or STEAM held a
	 * reboot (back wired); the rule's options off: charging. */
	sys_in_reset();
	sys_boot(false, false, OSC2_SRC_BATTERY, 0);
	ev(OSC2_SYS_EV_TIMER, 0, 1000);
	ev(OSC2_SYS_EV_POWER_USB, 0, 2000);
	CHECK_EQ(sm.state, OSC2_SYS_USB_LINK_ON);
	CHECK(!HAS(OSC2_SYS_DO_START));
	CHECK_EQ(sysout.led_flags, OSC2_LED_EXTERNAL);
	sys_boot(false, false, OSC2_SRC_BATTERY, 0);
	ev(OSC2_SYS_EV_TIMER, 0, 1000);
	sysin.link_up = false;
	ev(OSC2_SYS_EV_POWER_USB, 0, 2000);
	CHECK_EQ(sm.state, OSC2_SYS_REBOOT);
	sys_in_reset();
	sys_boot(false, false, OSC2_SRC_BATTERY, 0);
	ev(OSC2_SYS_EV_TIMER, 0, 1000);
	sysin.steam = true;
	ev(OSC2_SYS_EV_POWER_USB, 0, 2000);
	CHECK_EQ(sm.state, OSC2_SYS_REBOOT);
	sys_in_reset();
	sysin.plug_in_reboot = false;
	sysin.link_up = false;
	sys_boot(false, false, OSC2_SRC_BATTERY, 0);
	ev(OSC2_SYS_EV_TIMER, 0, 1000);
	ev(OSC2_SYS_EV_POWER_USB, 0, 2000);
	CHECK_EQ(sm.state, OSC2_SYS_USB_LINK_ON);
	/* The dock: link up on the dock. */
	sys_in_reset();
	sys_boot(false, false, OSC2_SRC_BATTERY, 0);
	ev(OSC2_SYS_EV_TIMER, 0, 1000);
	ev(OSC2_SYS_EV_POWER_DOCK, 0, 2000);
	CHECK_EQ(sm.state, OSC2_SYS_DOCK_ON);
	/* On battery: requests pass, chords and STEAM pressed do nothing. */
	sys_boot(false, false, OSC2_SRC_BATTERY, 0);
	ev(OSC2_SYS_EV_TIMER, 0, 1000);
	ev(OSC2_SYS_EV_REBOOT, 0, 2000);
	CHECK(HAS(OSC2_SYS_DO_PASS));
	ev(OSC2_SYS_EV_BOOTLOADER, 0, 2000);
	CHECK(HAS(OSC2_SYS_DO_PASS));
	ev(OSC2_SYS_EV_CHORD_DOWN, OSC2_CHORD_PUCK2, 2100);
	ev(OSC2_SYS_EV_STEAM_PRESSED, 0, 2200);
	ev(OSC2_SYS_EV_LEVEL, 5, 2300);
	CHECK_EQ(sm.state, OSC2_SYS_BATTERY);
	CHECK_EQ(sm.level, 5);
}

OPENSC2_TEST(sys_usb_rules)
{
	/* Wired: configured, then the cable pulled: the ordinary shutdown. */
	sys_in_reset();
	sys_boot(true, false, OSC2_SRC_USB, 0);
	ev(OSC2_SYS_EV_TIMER, 0, 500);
	CHECK_EQ(sm.state, OSC2_SYS_WIRED_WAIT);
	CHECK(HAS(OSC2_SYS_DO_LED));
	CHECK_EQ(sysout.led_mode, OSC2_LED_AMBER);    /* no host after 0.5 s */
	ev(OSC2_SYS_EV_CONFIGURED, 0, 800);
	CHECK_EQ(sm.state, OSC2_SYS_WIRED);
	CHECK_EQ(sysout.led_mode, OSC2_LED_WIRED);
	ev(OSC2_SYS_EV_TURN_OFF, 0, 900);            /* ignored while wired */
	CHECK_EQ(sm.state, OSC2_SYS_WIRED);
	ev(OSC2_SYS_EV_REBOOT, 0, 900);
	CHECK(HAS(OSC2_SYS_DO_PASS));
	ev(OSC2_SYS_EV_STEAM_HELD, 0, 9000);         /* STEAM does nothing once configured */
	CHECK_EQ(sm.state, OSC2_SYS_WIRED);
	ev(OSC2_SYS_EV_UNPLUGGED, 0, 10000);
	CHECK_EQ(sm.state, OSC2_SYS_SHUTDOWN);
	CHECK_EQ(sysout.cue, OSC2_CUE_POWER_OFF);
	sys_boot(true, false, OSC2_SRC_USB, 0);
	ev(OSC2_SYS_EV_POWER_NONE, 0, 100);          /* unplugged before a host came */
	CHECK_EQ(sm.state, OSC2_SYS_SHUTDOWN);

	/* USB power with the link on: STEAM held 7 s, a link that gives up or the turn-off command
	 * switch wireless off (the chime, amber); unplugged, battery operation with the link up. */
	{
		static const uint8_t to_off[] = { OSC2_SYS_EV_STEAM_HELD, OSC2_SYS_EV_WIRELESS_OFF,
						  OSC2_SYS_EV_TURN_OFF };

		for (size_t i = 0; i < sizeof(to_off); i++) {
			sys_in_reset();
			sys_boot(false, true, OSC2_SRC_USB, 0);
			ev(to_off[i], 0, 100);
			CHECK_EQ(sm.state, OSC2_SYS_USB_LINK_OFF);
			CHECK(HAS(OSC2_SYS_DO_DROP));
			CHECK_EQ(sysout.cue, OSC2_CUE_POWER_OFF);
			CHECK_EQ(sysout.led_mode, OSC2_LED_AMBER);
		}
	}
	/* Wireless off: STEAM pressed reboots (no warm mark: the next boot is wired); the link
	 * started from outside is wireless on again; unplugged, off silently. */
	ev(OSC2_SYS_EV_LINK_STARTED, 0, 200);
	CHECK_EQ(sm.state, OSC2_SYS_USB_LINK_ON);
	ev(OSC2_SYS_EV_TURN_OFF, 0, 300);
	ev(OSC2_SYS_EV_STEAM_PRESSED, 0, 400);
	CHECK_EQ(sm.state, OSC2_SYS_REBOOT);
	CHECK_EQ(sysout.end, OSC2_SYS_END_REBOOT);
	CHECK_EQ(sysout.end_cause, OSC2_SYS_EV_STEAM_PRESSED);
	sys_boot(false, true, OSC2_SRC_USB, 0);
	ev(OSC2_SYS_EV_TURN_OFF, 0, 100);
	ev(OSC2_SYS_EV_POWER_NONE, 0, 200);
	CHECK_EQ(sm.state, OSC2_SYS_SHUTDOWN_SILENT);
	CHECK(HAS(OSC2_SYS_DO_END));
	CHECK(!HAS(OSC2_SYS_DO_CUE));
	sys_boot(false, true, OSC2_SRC_USB, 0);
	ev(OSC2_SYS_EV_POWER_NONE, 0, 100);
	CHECK_EQ(sm.state, OSC2_SYS_BATTERY);
	CHECK(!HAS(OSC2_SYS_DO_DROP));
	CHECK_EQ(sysout.led_flags, 0);
}

OPENSC2_TEST(sys_dock_rules)
{
	sys_in_reset();
	sys_boot(false, false, OSC2_SRC_DOCK, 0);
	/* Radio off on the dock: the plain reboot and the turn-off do nothing, the warm key does. */
	ev(OSC2_SYS_EV_REBOOT, 0, 100);
	CHECK(!HAS(OSC2_SYS_DO_PASS));
	ev(OSC2_SYS_EV_TURN_OFF, 0, 100);
	CHECK_EQ(sm.state, OSC2_SYS_DOCK_OFF);
	ev(OSC2_SYS_EV_WARM_REBOOT, 0, 100);
	CHECK(HAS(OSC2_SYS_DO_PASS));
	/* A bare chord: its cue, its colour steady, 4 s; then the link on the dock. */
	ev(OSC2_SYS_EV_CHORD_DOWN, OSC2_CHORD_PUCK2, 1000);
	CHECK_EQ(sm.state, OSC2_SYS_DOCK_CHORD);
	CHECK_EQ(sysout.cue, 4);
	CHECK_EQ(sysout.led_flags, OSC2_LED_STEADY);
	ev(OSC2_SYS_EV_TIMER, 0, 5000);
	CHECK_EQ(sm.state, OSC2_SYS_DOCK_ON);
	CHECK(sysout.pairing);
	CHECK_EQ(sysout.transport, OSC2_TRANSPORT_PUCK2);
	CHECK_EQ(sysout.led_flags, OSC2_LED_EXTERNAL);
	/* The link on the dock: the turn-off plays the chime and turns the radio off. */
	ev(OSC2_SYS_EV_TURN_OFF, 0, 6000);
	CHECK_EQ(sm.state, OSC2_SYS_DOCK_OFF);
	CHECK_EQ(sysout.cue, OSC2_CUE_POWER_OFF);
	CHECK(HAS(OSC2_SYS_DO_DROP));
	/* Lifted off with the radio off: dark, 5 s, then off silently ... */
	ev(OSC2_SYS_EV_POWER_NONE, 0, 7000);
	CHECK_EQ(sm.state, OSC2_SYS_LIFTED);
	CHECK_EQ(sysout.led_mode, OSC2_LED_OFF);
	ev(OSC2_SYS_EV_TIMER, 0, 12000);
	CHECK_EQ(sm.state, OSC2_SYS_SHUTDOWN_SILENT);
	/* ... back on the dock, a chord (battery operation), or USB power (a reboot). */
	sys_boot(false, false, OSC2_SRC_DOCK, 0);
	ev(OSC2_SYS_EV_POWER_NONE, 0, 100);
	ev(OSC2_SYS_EV_POWER_DOCK, 0, 200);
	CHECK_EQ(sm.state, OSC2_SYS_DOCK_OFF);
	ev(OSC2_SYS_EV_POWER_NONE, 0, 300);
	ev(OSC2_SYS_EV_CHORD_DOWN, OSC2_CHORD_STEAM, 400);
	CHECK_EQ(sm.state, OSC2_SYS_CHORD);
	sys_boot(false, false, OSC2_SRC_DOCK, 0);
	ev(OSC2_SYS_EV_POWER_NONE, 0, 100);
	ev(OSC2_SYS_EV_POWER_USB, 0, 200);
	CHECK_EQ(sm.state, OSC2_SYS_REBOOT);
	/* The link on the dock, lifted off: battery operation, the link stays up. */
	sys_boot(false, true, OSC2_SRC_DOCK, 0);
	ev(OSC2_SYS_EV_POWER_NONE, 0, 100);
	CHECK_EQ(sm.state, OSC2_SYS_BATTERY);
	CHECK(!HAS(OSC2_SYS_DO_DROP));
}

OPENSC2_TEST(sys_sources_cues_levels)
{
	int8_t gain = 0;
	uint8_t script = 0;

	/* Only a change of "power present" is an event. */
	CHECK_EQ(osc2_sys_source_event(OSC2_SRC_BATTERY, OSC2_SRC_USB), OSC2_SYS_EV_POWER_USB);
	CHECK_EQ(osc2_sys_source_event(OSC2_SRC_BATTERY, OSC2_SRC_DOCK), OSC2_SYS_EV_POWER_DOCK);
	CHECK_EQ(osc2_sys_source_event(OSC2_SRC_USB, OSC2_SRC_BATTERY), OSC2_SYS_EV_POWER_NONE);
	CHECK_EQ(osc2_sys_source_event(OSC2_SRC_DOCK, OSC2_SRC_BATTERY), OSC2_SYS_EV_POWER_NONE);
	CHECK_EQ(osc2_sys_source_event(OSC2_SRC_USB, OSC2_SRC_DOCK), -1);
	CHECK_EQ(osc2_sys_source_event(OSC2_SRC_DOCK, OSC2_SRC_USB), -1);
	CHECK_EQ(osc2_sys_source_event(OSC2_SRC_USB, OSC2_SRC_USB), -1);

	/* The cues: scripts and gains at the chime levels. */
	{
		static const uint8_t scripts[11] = { 1, 1, 2, 1, 2, 5, 8, 9, 10, 3, 4 };

		for (uint8_t c = 0; c < 11; c++) {
			CHECK(osc2_cue_map(c, 2, &script, &gain));
			CHECK_EQ(script, scripts[c]);
			CHECK_EQ(gain, c == 8 ? 2 : (c == 6 || c == 7) ? -13 : -12);
			CHECK(osc2_cue_map(c, 1, &script, &gain));
			CHECK_EQ(gain, c == 8 ? 2 : (c == 6 || c == 7) ? -19 : -18);
			CHECK_EQ(osc2_cue_map(c, 0, &script, &gain), c == 8);   /* silent but cue 8 */
			CHECK_EQ(osc2_cue_map(c, 3, &script, &gain), c == 8);
		}
		CHECK(!osc2_cue_map(11, 2, &script, &gain));
		CHECK(!osc2_cue_map(255, 2, &script, &gain));
	}
	CHECK_EQ(osc2_sys_transport_cue(OSC2_TRANSPORT_BT1), 1);
	CHECK_EQ(osc2_sys_transport_cue(OSC2_TRANSPORT_PUCK2), 4);
	CHECK_EQ(osc2_sys_transport_cue(4), 0xFF);
	CHECK_EQ(osc2_sys_chord_transport(OSC2_CHORD_BT2, 2), OSC2_TRANSPORT_BT2);
	CHECK_EQ(osc2_sys_chord_transport(OSC2_CHORD_STEAM, 3), OSC2_TRANSPORT_PUCK2);
	CHECK_EQ(osc2_sys_chord_transport(OSC2_CHORD_STEAM, 200), OSC2_TRANSPORT_PUCK1);

	/* The "supply check gave up" flag: told 100 % while the level is above 0. */
	CHECK_EQ(osc2_sys_level(OPENSC2_CHARGE_CHARGING, 40), 40);
	CHECK_EQ(osc2_sys_level(OPENSC2_CHARGE_DISCHARGING | OPENSC2_CHARGE_GAVE_UP, 40), 100);
	CHECK_EQ(osc2_sys_level(OPENSC2_CHARGE_DISCHARGING | OPENSC2_CHARGE_GAVE_UP, 0), 0);
	/* A charger's status output. */
	CHECK_EQ(opensc2_charge_state_of(false, false), OPENSC2_CHARGE_DISCHARGING);
	CHECK_EQ(opensc2_charge_state_of(false, true), OPENSC2_CHARGE_DISCHARGING);
	CHECK_EQ(opensc2_charge_state_of(true, true), OPENSC2_CHARGE_CHARGING);
	CHECK_EQ(opensc2_charge_state_of(true, false), OPENSC2_CHARGE_DONE);

	/* The settings-by-name keys of the machine. */
	{
		uint8_t name_len = 0;
		uint8_t p[40] = "user/keylock";

		CHECK_EQ(osc2_sys_key_id(p, 17, &name_len), OSC2_SYS_KEY_KEYLOCK);
		CHECK_EQ(name_len, 13);
		CHECK_EQ(osc2_sys_key_size(OSC2_SYS_KEY_KEYLOCK), 4);
		memcpy(p, "user/fully_charged_led_intensity", 33);
		CHECK_EQ(osc2_sys_key_id(p, 33, &name_len), OSC2_SYS_KEY_FULL_LED);
		CHECK_EQ(osc2_sys_key_size(OSC2_SYS_KEY_FULL_LED), 1);
		memcpy(p, "user/keylockx", 14);
		CHECK_EQ(osc2_sys_key_id(p, 14, &name_len), OSC2_SYS_KEY_NONE);
		memcpy(p, "user/keylock", 12);   /* no NUL */
		CHECK_EQ(osc2_sys_key_id(p, 12, &name_len), OSC2_SYS_KEY_NONE);
	}
	CHECK(strcmp(osc2_sys_state_name(OSC2_SYS_USB_LINK_OFF), "usb-link-off") == 0);
	CHECK(strcmp(osc2_sys_event_name(OSC2_SYS_EV_STEAM_HELD), "steam-held") == 0);
	CHECK(strcmp(osc2_sys_state_name(200), "?") == 0);
}

/* ---- the status LED -------------------------------------------------------------------------- */

OPENSC2_TEST(led_brightness)
{
	static const struct {
		int16_t s;
		uint8_t b;
	} curve[] = { { 0, 0 }, { 10, 3 }, { 25, 11 }, { 50, 28 }, { 75, 56 }, { 90, 80 },
		      { 100, 100 } };

	for (size_t i = 0; i < sizeof(curve) / sizeof(curve[0]); i++) {
		CHECK_EQ(opensc2_led_brightness(curve[i].s, 1), curve[i].b);
		CHECK_EQ(opensc2_led_brightness(curve[i].s, 0), curve[i].s);
	}
	CHECK_EQ(opensc2_led_brightness(150, 1), 100);
	CHECK_EQ(opensc2_led_brightness(-3, 0), 0);
	CHECK_EQ(opensc2_led_brightness(50, 2), 28);   /* any other value than 0: the curve */
	/* colour x trim / 100 x brightness / 100, at most full duty */
	CHECK_EQ(opensc2_led_duty(250, 35, 100), 87);
	CHECK_EQ(opensc2_led_duty(200, 120, 50), 120);
	CHECK_EQ(opensc2_led_duty(1000, 199, 100), 1000);
	CHECK_EQ(opensc2_led_duty(500, 80, 0), 0);
}

static struct opensc2_led_player lp;
static uint16_t lc[4];

static void led_at(const struct opensc2_led_status *st, uint32_t t)
{
	opensc2_led_player_step(&lp, st, t, lc);
}

OPENSC2_TEST(led_patterns)
{
	struct opensc2_led_status st;

	/* Wired below 100 %: fade up from dark in 1.8 s and down in 1.8 s, in 10 ms steps. */
	memset(&st, 0, sizeof(st));
	st.mode = OPENSC2_LED_MODE_WIRED;
	st.level = 60;
	opensc2_led_player_init(&lp);
	led_at(&st, 1000);
	CHECK_EQ(lc[1], 0);
	led_at(&st, 1000 + 905);
	CHECK_EQ(lc[1], 250 * 900 / 1800);
	led_at(&st, 1000 + 1800);
	CHECK_EQ(lc[1], 250);
	led_at(&st, 1000 + 2700);
	CHECK_EQ(lc[1], 125);
	led_at(&st, 1000 + 3600);
	CHECK_EQ(lc[1], 0);
	CHECK_EQ(lc[0] + lc[2] + lc[3], 0);

	/* The searching pattern: in over 100 ms, 900 ms on, out over 100 ms, up to 900 ms dark,
	 * cut short when a link comes up. */
	memset(&st, 0, sizeof(st));
	st.mode = OPENSC2_LED_MODE_PUCK;
	st.level = 255;
	opensc2_led_player_init(&lp);
	led_at(&st, 0);
	led_at(&st, 100);
	CHECK_EQ(lc[3], 500);
	CHECK_EQ(lc[2], 50);
	CHECK_EQ(opensc2_led_player_envelope(&lp), 1000);
	led_at(&st, 999);
	CHECK_EQ(lc[3], 500);
	led_at(&st, 1100);
	CHECK_EQ(lc[3], 0);
	CHECK_EQ(opensc2_led_player_envelope(&lp), 0);
	led_at(&st, 1500);
	CHECK_EQ(lc[3], 0);
	st.connected = true;               /* the link came up: the dark part ends at once ... */
	led_at(&st, 1510);
	/* ... and the next cycle is steady (connected on battery). */
	led_at(&st, 1520);
	CHECK_EQ(lc[3], 500);
	CHECK_EQ(lc[2], 50);
	/* Connected on battery at 5 % or less: steady red. */
	st.level = 5;
	led_at(&st, 1700);
	CHECK_EQ(lc[0], 100);
	CHECK_EQ(lc[3], 0);

	/* Bluetooth pairing: two 75 ms flashes 75 ms apart, then 300 ms dark: 525 ms a cycle. */
	memset(&st, 0, sizeof(st));
	st.mode = OPENSC2_LED_MODE_BLUETOOTH;
	st.pairing = true;
	st.level = 80;
	opensc2_led_player_init(&lp);
	led_at(&st, 0);
	CHECK_EQ(lc[2], 200);
	led_at(&st, 80);
	CHECK_EQ(lc[2], 0);
	led_at(&st, 150);
	CHECK_EQ(lc[2], 200);
	led_at(&st, 230);
	CHECK_EQ(lc[2], 0);
	led_at(&st, 525);
	CHECK_EQ(lc[2], 200);              /* the next cycle */

	/* The low-battery shutdown: red flashes at 5 % or less, 1.3 s a cycle; the keylock's
	 * white flashes, 10.3 s. */
	memset(&st, 0, sizeof(st));
	st.mode = OPENSC2_LED_MODE_LOW_BATTERY;
	st.level = 0;
	opensc2_led_player_init(&lp);
	led_at(&st, 0);
	CHECK_EQ(lc[0], 100);
	led_at(&st, 300);
	CHECK_EQ(lc[0], 0);
	led_at(&st, 1300);
	CHECK_EQ(lc[0], 100);
	st.mode = OPENSC2_LED_MODE_KEYLOCK;
	opensc2_led_player_init(&lp);
	led_at(&st, 0);
	CHECK_EQ(lc[3], 200);
	led_at(&st, 10299);
	CHECK_EQ(lc[3], 0);
	led_at(&st, 10300);
	CHECK_EQ(lc[3], 200);

	/* Amber below 100 %: dark to dim amber in 200 ms, full in 1.6 s, back. */
	memset(&st, 0, sizeof(st));
	st.mode = OPENSC2_LED_MODE_AMBER;
	st.level = 50;
	opensc2_led_player_init(&lp);
	led_at(&st, 0);
	led_at(&st, 200);
	CHECK_EQ(lc[0], 15);
	CHECK_EQ(lc[1], 20);
	led_at(&st, 1800);
	CHECK_EQ(lc[0], 200);
	led_at(&st, 3400);
	CHECK_EQ(lc[0], 15);
	led_at(&st, 3600);
	CHECK_EQ(lc[0], 0);

	/* A new mode shows once the cycle in progress is over. */
	memset(&st, 0, sizeof(st));
	st.mode = OPENSC2_LED_MODE_WIRED;
	st.level = 10;
	opensc2_led_player_init(&lp);
	led_at(&st, 0);
	st.mode = OPENSC2_LED_MODE_OFF;
	led_at(&st, 1800);
	CHECK_EQ(lc[1], 250);              /* still the breathe */
	led_at(&st, 3610);
	CHECK_EQ(lc[1], 0);
	CHECK_EQ(lp.count, 1);             /* now the dark display */

	/* Steam's override: its colour, byte / 100 of full duty, whatever the mode. */
	st.override = true;
	st.override_rgbw[0] = 100;
	st.override_rgbw[2] = 20;
	led_at(&st, 3800);
	led_at(&st, 3900);
	CHECK_EQ(lc[0], 1000);
	CHECK_EQ(lc[2], 200);
}

OPENSC2_TEST(led_full_display)
{
	struct opensc2_led_status st;

	/* Wired at 100 %: the fully-charged display follows activity. */
	memset(&st, 0, sizeof(st));
	st.mode = OPENSC2_LED_MODE_WIRED;
	st.level = 100;
	st.full_level = 1;                 /* dim */
	st.input_recent = true;
	opensc2_led_player_init(&lp);
	led_at(&st, 0);
	led_at(&st, 2000);
	CHECK_EQ(lc[1], 250);              /* input seen: full */
	st.input_recent = false;
	led_at(&st, 4000);                 /* the next 2 s cycle: the stored level, dim */
	led_at(&st, 6000);
	CHECK_EQ(lc[1], 50);
	/* 15 s after the level last read below 100 %: full, whatever the rest says. */
	st.below_full_ms = 5000;
	led_at(&st, 8000);
	led_at(&st, 10000);
	CHECK_EQ(lc[1], 250);
	led_at(&st, 20000);
	led_at(&st, 22000);
	CHECK_EQ(lc[1], 50);               /* 15 s over */

	/* Amber does not follow activity: the stored level; medium; 0 is dark. */
	memset(&st, 0, sizeof(st));
	st.mode = OPENSC2_LED_MODE_AMBER;
	st.level = 100;
	st.full_level = 2;
	st.input_recent = true;
	opensc2_led_player_init(&lp);
	led_at(&st, 0);
	led_at(&st, 2000);
	CHECK_EQ(lc[0], 80);
	CHECK_EQ(lc[1], 100);
	st.full_level = 0;
	led_at(&st, 4000);
	led_at(&st, 6000);
	CHECK_EQ(lc[0] + lc[1], 0);
	CHECK_EQ(opensc2_led_player_envelope(&lp), 0);
	/* Any value above 3 is full. */
	st.full_level = 7;
	led_at(&st, 8000);
	led_at(&st, 10000);
	CHECK_EQ(lc[0], 200);

	/* The puck link on external power at 100 %: follows activity; on battery: steady. */
	memset(&st, 0, sizeof(st));
	st.mode = OPENSC2_LED_MODE_PUCK;
	st.level = 255;
	st.connected = true;
	st.external_power = true;
	st.full_level = 1;
	opensc2_led_player_init(&lp);
	led_at(&st, 0);
	led_at(&st, 2000);
	CHECK_EQ(lc[3], 40);               /* no input: dim (0, 0, 0, 0.04) */
	CHECK_EQ(lc[2], 0);
	st.external_power = false;
	led_at(&st, 4000);
	led_at(&st, 4100);
	CHECK_EQ(lc[3], 500);              /* steady, full colour */
	CHECK_EQ(lc[2], 50);
}
