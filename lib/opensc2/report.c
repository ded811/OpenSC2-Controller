/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * report.c: what every link shares on the way to the host and back:
 *   - the state report 0x42: sampled, the input rules (input_rules.h: the stick-touch fallback,
 *     the pads' clicks by pressure, setting 82, the triggers' digital bits), one sequence number
 *     for every link (incremented per report built, never reset);
 *   - lizard mode: one frame per state report, its haptic events, and the reports at its
 *     transitions;
 *   - the battery report 0x43 every CONFIG_OPENSC2_BATTERY_REPORT_INTERVAL_MS, from boot, on
 *     whatever link is up (dropped when there is none, as on the real controller);
 *   - output reports from the host, the rumble timeout, and the haptic stream status replies to a
 *     stop.
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include "internal.h"
#include "cal_store.h"
#include "dock_link.h"
#include "haptics_glue.h"
#include "input_rules.h"
#include "lizard.h"
#include "proto_rules.h"
#include "system.h"
#include "test_hooks.h"

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

/* The rules of this build (input_rules.h), from Kconfig. The trigger rule for raw trigger output
 * (setting 62) has no option: it is what the protocol asks. */
#define RULES_BUILD                                                                        \
	((IS_ENABLED(CONFIG_OPENSC2_TRIGGER_CLICK_AUTO) ? OSC2_RULE_TRIGGER_BITS : 0) |       \
	 OSC2_RULE_RAW_TRIGGERS |                                                          \
	 (IS_ENABLED(CONFIG_OPENSC2_STICK_TOUCH_FALLBACK) ? OSC2_RULE_STICK_FALLBACK : 0) |   \
	 (IS_ENABLED(CONFIG_OPENSC2_PAD_BLANK_ON_STICK) ? OSC2_RULE_PAD_BLANK : 0) |          \
	 (IS_ENABLED(CONFIG_OPENSC2_PAD_CLICK_AUTO) ? OSC2_RULE_PAD_CLICK : 0) |              \
	 (IS_ENABLED(CONFIG_OPENSC2_LIZARD_STABILISER) ? OSC2_RULE_LIZARD_STABILISER : 0) |   \
	 (IS_ENABLED(CONFIG_OPENSC2_LIZARD_SMOOTHING) ? OSC2_RULE_LIZARD_SMOOTHING : 0) |     \
	 (IS_ENABLED(CONFIG_OPENSC2_LIZARD_GLIDE) ? OSC2_RULE_LIZARD_GLIDE : 0) |             \
	 (IS_ENABLED(CONFIG_OPENSC2_LIZARD_TICKS) ? OSC2_RULE_LIZARD_TICKS : 0))

static uint8_t sequence;
static struct osc2_input_rules input_rules;
/* The rule word in effect: the build's, unless the hardware test hooks change it while armed. */
static atomic_t rules = ATOMIC_INIT(RULES_BUILD);
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
#if defined(CONFIG_OPENSC2_BLE)
	if (osc2_ble_active()) {
		return osc2_ble_send(report, len, droppable);
	}
#endif
	if (!osc2_radio_connected()) {
		return -ENOTCONN;
	}
	return osc2_radio_queue_report(report, len, droppable);
}

uint32_t osc2_rules(void)
{
	return (uint32_t)atomic_get(&rules);
}

uint32_t osc2_rules_build(void)
{
	return RULES_BUILD;
}

void osc2_rules_set(uint32_t value)
{
	(void)atomic_set(&rules, (atomic_val_t)(value & OSC2_RULES_KNOWN));
}

/* ---- state report ---------------------------------------------------------------------------- */

void osc2_report_state(uint8_t state[OPENSC2_STATE_REPORT_LEN], struct opensc2_state *s)
{
	if (osc2.cb && osc2.cb->sample) {
		opensc2_get_state(s);
		osc2.cb->sample(s, osc2.user_data);
		(void)opensc2_set_state(s);
	}
	opensc2_get_state(s);
	osc2_cal_frame(s);   /* raw inputs, processed as the real controller does (cal_store.c) */
	/* The application's sticks, triggers and touches (raw ones processed) for the idle monitor,
	 * never the injected ones below: the report injection does not reach the system rules. */
	osc2_sys_frame(s);
	/* The hardware test hooks' injected inputs take the application's place here, after its state
	 * was read and never above: there they would be stored as the application's own (and reach
	 * the STEAM, plug-in and wakeup rules). */
	osc2_test_input(s);
	osc2_cal_rules(s);   /* the pad rules: rotation, stabiliser */
	/* The rules after injection and the pad rules, so that an injected state goes through them
	 * as the application's does; lizard mode gets the same state. */
	osc2_input_rules_apply(&input_rules, osc2_rules(), osc2.settings, s);
	opensc2_encode_state(s, sequence++, state);
	osc2_test_frame(state);
	osc2_dock_cache_report(state);   /* the dock's copy, while a dock session runs */
}

#if defined(CONFIG_OPENSC2_LIZARD_MODE)
/* Lizard mode's ticks and pad clicks: haptic events of the library's own (report ID 0), noted in
 * the hooks' ring with the link lizard mode runs on, then to the application. */
static void lizard_haptics(const struct opensc2_haptic *h, uint8_t n)
{
	uint8_t link = (uint8_t)osc2_mode_link();   /* USB, Bluetooth or the radio */

	for (uint8_t i = 0; i < n; i++) {
		osc2_test_note_haptic(link, &h[i]);
		if (osc2.cb && osc2.cb->haptic) {
			osc2.cb->haptic(&h[i], osc2.user_data);
		}
	}
	/* The haptics synthesiser plays them as the real controller does: while lizard mode is on and
	 * setting 70 is exactly 1 (lizard.c makes them only then), never on the dock. */
	if (IS_ENABLED(CONFIG_OPENSC2_HAPTICS_SYNTH) && n && opensc2_lizard_mode() &&
	    osc2.settings[OPENSC2_SETTING_HAPTICS_ENABLED] == 1 &&
	    opensc2_power_source() != OPENSC2_POWER_SOURCE_DOCK) {
		osc2_haptics_library(h, n);
	}
}
#endif

void osc2_report_lizard(const struct opensc2_state *s)
{
#if defined(CONFIG_OPENSC2_LIZARD_MODE)
	struct opensc2_haptic haptics[4];
	uint8_t keyboard[9], mouse[6];
	uint8_t n_haptics;
	uint32_t made;

	/* A frame built from injected inputs: lizard mode never sees it (unless the test asked). */
	if (osc2_test_skip_lizard()) {
		return;
	}
	if (!opensc2_lizard_mode()) {
		return;
	}
	k_mutex_lock(&lizard_lock, K_FOREVER);
	if (osc2_mode_link() == OSC2_LINK_RADIO && osc2_radio_backlog()) {
		/* Reports wait for room in the radio's queue: lizard mode waits with them, as the real
		 * controller's mouse and keyboard wait for a free buffer. The next frame it runs takes
		 * up the motion (a difference of positions). */
		k_mutex_unlock(&lizard_lock);
		return;
	}
	made = opensc2_lizard_step(&lizard, s, osc2.settings, osc2_rules(), keyboard, mouse);
	if (made & OPENSC2_LIZARD_KEYBOARD) {
		osc2_test_count(OSC2_TEST_CNT_LIZARD_MADE);
		(void)osc2_input_send(keyboard, sizeof(keyboard), false);
	}
	if (made & OPENSC2_LIZARD_MOUSE) {
		osc2_test_count(OSC2_TEST_CNT_LIZARD_MADE);
		(void)osc2_input_send(mouse, sizeof(mouse), false);
	}
	n_haptics = opensc2_lizard_haptics(made, haptics);
	k_mutex_unlock(&lizard_lock);
	lizard_haptics(haptics, n_haptics);
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
	/* The level reaches the power rules (the application's, or the test hooks' system
	 * injection; never the report injection below). */
	osc2_sys_battery_report(&b);
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
	osc2_input_rules_reset(&input_rules);
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

/* CONFIG_OPENSC2_HAPTIC_STOP_STATUS: the 0x44 {channel, 0x02} replies the real controller sends
 * for a stop (proto_rules.h), after the application heard about the output. */
static void stop_status(enum osc2_link link, const struct opensc2_output *out)
{
#if defined(CONFIG_OPENSC2_HAPTIC_STOP_STATUS)
	uint8_t ids[4];
	uint8_t n = osc2_stop_status(out, ids);

	for (uint8_t i = 0; i < n; i++) {
		uint8_t report[OPENSC2_HAPTIC_STATUS_LEN];

		opensc2_encode_haptic_status(ids[i], OSC2_STATUS_STOPPED, report);
		osc2_test_note_status(link, ids[i], OSC2_STATUS_STOPPED);
		(void)osc2_input_send(report, sizeof(report), false);
	}
#else
	ARG_UNUSED(link);
	ARG_UNUSED(out);
#endif
}

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
		stop_status(link, &out[i]);
	}
	/* The haptics synthesiser (CONFIG_OPENSC2_HAPTICS_SYNTH) plays them too. */
	osc2_haptics_outputs((uint8_t)link, out, n);
	return n;
}

void osc2_output_link_down(void)
{
	(void)k_work_cancel_delayable(&rumble_work);
	if (rumble_active) {
		rumble_off();
	}
}
