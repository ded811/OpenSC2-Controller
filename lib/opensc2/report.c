/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * report.c: what every link shares on the way to the host and back:
 *   - the state report 0x42: sampled, the triggers' digital bits, one sequence number for every
 *     link (incremented per report built, never reset);
 *   - lizard mode: one frame per state report, and the reports at its transitions;
 *   - the battery report 0x43 every CONFIG_OPENSC2_BATTERY_REPORT_INTERVAL_MS, from boot, on
 *     whatever link is up (dropped when there is none, as on the real controller);
 *   - output reports from the host, and the rumble timeout.
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "internal.h"
#include "lizard.h"
#include "report_rules.h"
#include "test_hooks.h"

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

static uint8_t sequence;
static bool trigger_click[2];
static struct opensc2_lizard lizard;
#if defined(CONFIG_OPENSC2_LIZARD_MODE)
/* Held while lizard mode runs a frame or turns on or off, until their reports are with the link,
 * so the reports go out in the order they were made. On USB the commands that turn lizard mode
 * off run in the USB stack's thread and the frames in the report thread: without it, a frame made
 * just before "off" could follow the release reports and leave a key or button held. */
static K_MUTEX_DEFINE(lizard_lock);
#endif

/* ---- the links ------------------------------------------------------------------------------- */

int osc2_input_send(const uint8_t *report, size_t len, bool droppable)
{
	if (len < 1 || len > 64) {
		return -EINVAL;
	}
	/* The hardware test hooks' lizard guard: while armed, mouse and keyboard reports go into the
	 * capture ring instead (the callers see them sent). */
	if (osc2_test_divert(report, len)) {
		return 0;
	}
	if (osc2_wired()) {
#if defined(CONFIG_OPENSC2_USB)
		return osc2_usb_send(report, len, droppable);
#else
		return -ENOTCONN;
#endif
	}
	ARG_UNUSED(droppable);
	if (!osc2_radio_connected()) {
		return -ENOTCONN;
	}
	return osc2_radio_queue_report(report, len);
}

/* ---- state report ---------------------------------------------------------------------------- */

#if defined(CONFIG_OPENSC2_TRIGGER_CLICK_AUTO)
/* The triggers' digital bits: on above setting 68 plus 5 points, off below it minus 5 (90 % by
 * default: on above 95 %, off below 85 %). A fully pulled trigger always counts as on. */
static void trigger_bits(struct opensc2_state *s)
{
	static const uint32_t bit[2] = { OPENSC2_CLICK_LEFT_TRIGGER, OPENSC2_CLICK_RIGHT_TRIGGER };
	int16_t threshold = osc2.settings[OPENSC2_SETTING_TRIGGER_THRESHOLD];

	for (int side = 0; side < 2; side++) {
		trigger_click[side] = osc2_trigger_click(s->trigger[side], threshold,
							 trigger_click[side]);
		s->buttons = trigger_click[side] ? (s->buttons | bit[side]) : (s->buttons & ~bit[side]);
	}
}
#endif

void osc2_report_state(uint8_t state[OPENSC2_STATE_REPORT_LEN], struct opensc2_state *s)
{
	if (osc2.cb && osc2.cb->sample) {
		opensc2_get_state(s);
		osc2.cb->sample(s, osc2.user_data);
		(void)opensc2_set_state(s);
	}
	opensc2_get_state(s);
	/* The hardware test hooks' injected inputs take the application's place here, after its state
	 * was read and never above: there they would be stored as the application's own (and reach
	 * the STEAM, plug-in and wakeup rules). */
	osc2_test_input(s);
#if defined(CONFIG_OPENSC2_TRIGGER_CLICK_AUTO)
	trigger_bits(s);
#endif
	opensc2_encode_state(s, sequence++, state);
	osc2_test_frame(state);
}

void osc2_report_lizard(const struct opensc2_state *s)
{
#if defined(CONFIG_OPENSC2_LIZARD_MODE)
	uint8_t keyboard[9], mouse[6];
	uint32_t made;

	/* A frame built from injected inputs: lizard mode never sees it (unless the test asked). */
	if (osc2_test_skip_lizard()) {
		return;
	}
	if (!opensc2_lizard_mode()) {
		return;
	}
	k_mutex_lock(&lizard_lock, K_FOREVER);
	made = opensc2_lizard_step(&lizard, s,
				   (uint8_t)osc2.settings[OPENSC2_SETTING_PAD_IGNORE_ON_STICK],
				   keyboard, mouse);
	if (made & OPENSC2_LIZARD_KEYBOARD) {
		osc2_test_count(OSC2_TEST_CNT_LIZARD_MADE);
		(void)osc2_input_send(keyboard, sizeof(keyboard), false);
	}
	if (made & OPENSC2_LIZARD_MOUSE) {
		osc2_test_count(OSC2_TEST_CNT_LIZARD_MADE);
		(void)osc2_input_send(mouse, sizeof(mouse), false);
	}
	k_mutex_unlock(&lizard_lock);
#else
	ARG_UNUSED(s);
#endif
}

/* ---- lizard mode transitions ----------------------------------------------------------------- */

void osc2_lizard_set(bool on)
{
#if defined(CONFIG_OPENSC2_LIZARD_MODE)
	uint8_t keyboard[9], mouse[6];
	uint32_t made;

	k_mutex_lock(&lizard_lock, K_FOREVER);
	made = on ? opensc2_lizard_enter(&lizard, keyboard, mouse) :
		    opensc2_lizard_leave(&lizard, keyboard, mouse);
	if (made) {
		osc2_test_note_lizard(on ? OSC2_TEST_LIZARD_ENTER : OSC2_TEST_LIZARD_LEAVE);
		osc2_test_count_n(OSC2_TEST_CNT_LIZARD_MADE, 2);
	}
	/* Off: the keyboard first, then the mouse, releasing whatever was held. On: both, empty. */
	if (made && on) {
		(void)osc2_input_send(mouse, sizeof(mouse), false);
		(void)osc2_input_send(keyboard, sizeof(keyboard), false);
	} else if (made) {
		(void)osc2_input_send(keyboard, sizeof(keyboard), false);
		(void)osc2_input_send(mouse, sizeof(mouse), false);
	}
	k_mutex_unlock(&lizard_lock);
	LOG_DBG("Lizard mode %s", on ? "on" : "off");
	osc2_usblog("lizard mode %s", on ? "on" : "off");
#else
	ARG_UNUSED(on);
#endif
}

void osc2_lizard_switches(bool up)
{
#if defined(CONFIG_OPENSC2_LIZARD_MODE)
	k_mutex_lock(&lizard_lock, K_FOREVER);
	opensc2_lizard_switches(&lizard, up);
	osc2_test_note_lizard(up ? OSC2_TEST_LIZARD_UP : OSC2_TEST_LIZARD_DOWN);
	k_mutex_unlock(&lizard_lock);
#else
	ARG_UNUSED(up);
#endif
}

void osc2_lizard_resync(void)
{
#if defined(CONFIG_OPENSC2_LIZARD_MODE)
	k_mutex_lock(&lizard_lock, K_FOREVER);
	opensc2_lizard_resync(&lizard);
	osc2_test_note_lizard(OSC2_TEST_LIZARD_RESYNC);
	k_mutex_unlock(&lizard_lock);
#else
	osc2_test_note_lizard(OSC2_TEST_LIZARD_RESYNC);
#endif
}

bool osc2_lizard_release_held(void)
{
#if defined(CONFIG_OPENSC2_LIZARD_MODE)
	uint8_t keyboard[9], mouse[6];
	bool sent;

	k_mutex_lock(&lizard_lock, K_FOREVER);
	sent = opensc2_lizard_release(&lizard, keyboard, mouse) != 0;
	if (sent) {
		(void)osc2_input_send(keyboard, sizeof(keyboard), false);
		(void)osc2_input_send(mouse, sizeof(mouse), false);
	}
	k_mutex_unlock(&lizard_lock);
	return sent;
#else
	return false;
#endif
}

/* ---- battery report -------------------------------------------------------------------------- */

#if CONFIG_OPENSC2_BATTERY_REPORT_INTERVAL_MS > 0
static void battery_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(battery_work, battery_fn);

static void battery_fn(struct k_work *work)
{
	uint8_t report[OPENSC2_BATTERY_REPORT_LEN];
	struct opensc2_battery b;
	k_spinlock_key_t key = k_spin_lock(&osc2.lock);

	ARG_UNUSED(work);
	b = osc2.battery;
	k_spin_unlock(&osc2.lock, key);
	osc2_test_battery(&b);
	opensc2_encode_battery(&b, report);
	osc2_test_count(osc2_input_send(report, sizeof(report), true) == 0 ?
			OSC2_TEST_CNT_BATTERY_SENT : OSC2_TEST_CNT_BATTERY_DROPPED);
	k_work_schedule(&battery_work, K_MSEC(CONFIG_OPENSC2_BATTERY_REPORT_INTERVAL_MS));
}
#endif

void osc2_report_battery_now(void)
{
#if CONFIG_OPENSC2_BATTERY_REPORT_INTERVAL_MS > 0
	(void)k_work_reschedule(&battery_work, K_NO_WAIT);
#endif
}

void osc2_report_init(void)
{
	opensc2_lizard_reset(&lizard);
	trigger_click[0] = trigger_click[1] = false;
#if CONFIG_OPENSC2_BATTERY_REPORT_INTERVAL_MS > 0
	k_work_schedule(&battery_work, K_NO_WAIT);   /* the first one is usually lost: no link yet */
#endif
}

/* ---- output reports -------------------------------------------------------------------------- */

static bool rumble_active;

static void rumble_off(void)
{
	struct opensc2_rumble off = { .timeout = true };

	rumble_active = false;
	osc2_test_note_rumble(OSC2_TEST_LINK_NONE, &off);
	if (osc2.cb && osc2.cb->rumble) {
		osc2.cb->rumble(&off, osc2.user_data);
	}
}

static void rumble_timeout_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (rumble_active) {
		rumble_off();
	}
}
static K_WORK_DELAYABLE_DEFINE(rumble_work, rumble_timeout_fn);

int osc2_output_dispatch(enum osc2_link link, const uint8_t *report, size_t len)
{
	struct opensc2_output out[2];
	int n;

	if (len < 1) {
		return -1;
	}
	if (osc2.cb && osc2.cb->output_report) {
		osc2.cb->output_report(report, len, osc2.user_data);
	}
	/* Hosts send an output report at its declared length or longer (zero padded): both decode. */
	n = opensc2_decode_output(report, len, out);
	osc2_test_note_output(link, report, len, n);
	osc2_test_count(OSC2_TEST_CNT_OUT_REPORTS);
	if (n < 0) {
		osc2_test_count(OSC2_TEST_CNT_OUT_SHORT);
		LOG_DBG("Output report 0x%02x too short (%u bytes)", report[0], (unsigned int)len);
		return n;
	}
	for (int i = 0; i < n; i++) {
		if (out[i].type == OPENSC2_OUTPUT_RUMBLE) {
			rumble_active = out[i].rumble.left_speed || out[i].rumble.right_speed;
			if (rumble_active) {
				k_work_reschedule(&rumble_work, K_MSEC(CONFIG_OPENSC2_RUMBLE_TIMEOUT_MS));
			} else {
				(void)k_work_cancel_delayable(&rumble_work);
			}
			osc2_test_note_rumble(link, &out[i].rumble);
			if (osc2.cb && osc2.cb->rumble) {
				osc2.cb->rumble(&out[i].rumble, osc2.user_data);
			}
		} else if (out[i].type == OPENSC2_OUTPUT_HAPTIC) {
			osc2_test_note_haptic(link, &out[i].haptic);
			if (osc2.cb && osc2.cb->haptic) {
				osc2.cb->haptic(&out[i].haptic, osc2.user_data);
			}
		}
	}
	return n;
}

void osc2_output_link_down(void)
{
	(void)k_work_cancel_delayable(&rumble_work);
	if (rumble_active) {
		rumble_off();
	}
}
