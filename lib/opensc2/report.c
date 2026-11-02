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
#include "pad_rules.h"
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

/* The calibration's part in the trigger bits: a set-up of the triggers clears them, and a raw
 * trigger's bit is its calibration's (unless the test hooks injected another value). */
static void trigger_decisions(const struct opensc2_state *s)
{
	static uint32_t setups;
	uint32_t now = osc2_cal_setups();

	if (now != setups) {
		setups = now;
		osc2_input_rules_triggers_set_up(&input_rules);
	}
	for (int side = 0; side < 2; side++) {
		bool bit;

		if (osc2_cal_trigger_decided(side, s->trigger[side], &bit)) {
			osc2_input_rules_decide(&input_rules, side, bit);
		}
	}
}

void osc2_report_state(uint8_t state[OPENSC2_STATE_REPORT_LEN], struct opensc2_state *s)
{
	if (osc2.cb && osc2.cb->sample) {
		opensc2_get_state(s);
		osc2.cb->sample(s, osc2.user_data);
		(void)opensc2_set_state(s);
	}
	opensc2_get_state(s);
	osc2_cal_frame(s);   /* raw inputs, processed as the real controller does (cal_store.c) */
	osc2_quat_none_to_identity(s);   /* no rotation goes out as the identity, never W = 0 */
	/* The application's sticks, triggers and touches (raw ones processed) for the idle monitor,
	 * never the injected ones below: the report injection does not reach the system rules. */
	osc2_sys_frame(s);
	/* The hardware test hooks' injected inputs take the application's place here, after its state
	 * was read and never above: there they would be stored as the application's own (and reach
	 * the STEAM, plug-in and wakeup rules). */
	osc2_test_input(s);
	osc2_cal_rules(s);   /* the pad rules: rotation, stabiliser */
	trigger_decisions(s);
	input_rules.pad_sides = osc2_pad_frame_sides();   /* trackpads fed by frames */
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
static void lizard_feedback(const struct opensc2_haptic *h, uint8_t n)
{
	uint8_t link = (uint8_t)osc2_mode_link();   /* USB, Bluetooth or the radio */

	for (uint8_t i = 0; i < n; i++) {
		osc2_test_note_haptic(link, &h[i]);
		if (osc2.cb && osc2.cb->haptic) {
			osc2.cb->haptic(&h[i], osc2.user_data);
		}
	}
	/* The haptics synthesiser plays them as the real controller does: while lizard mode is on,
	 * setting 70 is exactly 1 (lizard.c makes them only then) and the current link is USB, the
	 * puck or Bluetooth, not the dock. */
	if (IS_ENABLED(CONFIG_OPENSC2_HAPTICS_SYNTH) && n && opensc2_lizard_mode() &&
	    osc2.settings[OPENSC2_SETTING_HAPTICS_ENABLED] == 1 && !osc2_dock_alone()) {
		osc2_haptics_library(h, n);
	}
}

/* Send what a lizard call made, in order (the lizard lock is held). */
static void lizard_send(const struct opensc2_lizard_out *out)
{
	for (uint8_t i = 0; i < out->n; i++) {
		osc2_test_count(OSC2_TEST_CNT_LIZARD_MADE);
		(void)osc2_input_send(out->report[i], out->len[i], false);
	}
}

/* ---- the keyboard hears the buttons when they change ---- */

#if defined(CONFIG_OPENSC2_LIZARD_KEYS_AT_ONCE)
/* As the real controller's keyboard listens to its button driver, the application's setters wake
 * this thread on every change of the buttons, and the keyboard takes the change up at once (its
 * report goes nowhere while no link runs). While the hardware test hooks are armed the frames
 * bring the buttons instead (the injected ones included), as they do without this option. */
static K_SEM_DEFINE(keys_sem, 0, 1);
static atomic_t keys_ready;
static bool keys_deferred;   /* a change waits for room in the radio's queue (lizard_lock) */

static bool keys_at_once(void)
{
	return !osc2_test_lizard_frames_only();
}

static void keys_thread_fn(void *p1, void *p2, void *p3)
{
	struct opensc2_lizard_out out;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	for (;;) {
		(void)k_sem_take(&keys_sem, K_FOREVER);
		if (!atomic_get(&keys_ready)) {
			continue;   /* before the library starts: its first frame adopts the buttons */
		}
		k_mutex_lock(&lizard_lock, K_FOREVER);
		lizard.driver = keys_at_once();
		if (!lizard.driver) {
			/* the frames bring the buttons */
		} else if (osc2_mode_link() == OSC2_LINK_RADIO && osc2_radio_backlog()) {
			keys_deferred = true;   /* the first frame that runs wakes this thread again */
		} else {
			(void)opensc2_lizard_buttons(&lizard, osc2_sys_app_buttons(), &out);
			lizard_send(&out);
		}
		k_mutex_unlock(&lizard_lock);
	}
}

K_THREAD_DEFINE(osc2_lizard_keys_tid, CONFIG_OPENSC2_LIZARD_KEYS_STACK_SIZE, keys_thread_fn, NULL,
		NULL, NULL, K_PRIO_PREEMPT(CONFIG_OPENSC2_LIZARD_KEYS_PRIORITY), 0, 0);

void osc2_lizard_buttons_changed(void)
{
	k_sem_give(&keys_sem);
}

/* In each frame (lizard_lock held): who brings the buttons, and a wake-up for the thread when a
 * change waited, the buttons are to be adopted afresh (lizard mode's resync), or the frames have
 * just handed the buttons over to it (the hooks' guard ended: a change the thread passed over
 * while the frames brought the buttons is compared with the last frame's buttons, not lost). */
static void lizard_keys_frame(void)
{
	bool handover = !lizard.driver;

	lizard.driver = keys_at_once();
	if (lizard.driver && (handover || keys_deferred || lizard.buttons_sync)) {
		keys_deferred = false;
		k_sem_give(&keys_sem);
	}
}

static void lizard_keys_start(void)
{
	atomic_set(&keys_ready, 1);
	k_sem_give(&keys_sem);
}
#else
void osc2_lizard_buttons_changed(void)
{
}

static inline void lizard_keys_frame(void)
{
}

static inline void lizard_keys_start(void)
{
}
#endif
#endif

void osc2_report_lizard(const struct opensc2_state *s)
{
#if defined(CONFIG_OPENSC2_LIZARD_MODE)
	struct opensc2_haptic haptics[4];
	struct opensc2_lizard_out out;
	uint8_t n_haptics;
	uint32_t made;

	/* A frame built from injected inputs: lizard mode never sees it (unless the test asked). */
	if (osc2_test_skip_lizard()) {
		return;
	}
	/* Every frame, also with lizard mode off: the keyboard sees the buttons change as a button
	 * driver reports them, and a keyboard that runs on with setting 9 at 0 (0x85, then 0x81)
	 * still types. */
	k_mutex_lock(&lizard_lock, K_FOREVER);
	if (osc2_mode_link() == OSC2_LINK_RADIO && osc2_radio_backlog()) {
		/* Reports wait for room in the radio's queue (CONFIG_OPENSC2_REPORT_WAIT_DEPTH above
		 * 0, OpenSC2's option; the real controller drops them): lizard mode waits with them.
		 * The next frame it runs takes up the motion (a difference of positions). */
		k_mutex_unlock(&lizard_lock);
		return;
	}
	lizard_keys_frame();
	made = opensc2_lizard_step(&lizard, s, osc2.settings, osc2_rules(), &out);
	lizard_send(&out);
	n_haptics = opensc2_lizard_haptics(made, haptics);
	k_mutex_unlock(&lizard_lock);
	lizard_feedback(haptics, n_haptics);
#else
	ARG_UNUSED(s);
#endif
}

/* ---- lizard mode transitions ----------------------------------------------------------------- */

void osc2_lizard_set(bool on)
{
#if defined(CONFIG_OPENSC2_LIZARD_MODE)
	struct opensc2_lizard_out out;
	uint32_t made;

	k_mutex_lock(&lizard_lock, K_FOREVER);
	/* The keyboard first, then the mouse, both ways: off, releasing whatever was held; on, both
	 * empty (the protocol documentation's order, inferred there from the off order). */
	made = opensc2_lizard_setting(&lizard, on, &out);
	if (made) {
		osc2_test_note_lizard(on ? OSC2_TEST_LIZARD_ENTER : OSC2_TEST_LIZARD_LEAVE);
	}
	lizard_send(&out);
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
	struct opensc2_lizard_out out;

	k_mutex_lock(&lizard_lock, K_FOREVER);
	(void)opensc2_lizard_switches(&lizard, up, &out);
	osc2_test_note_lizard(up ? OSC2_TEST_LIZARD_UP : OSC2_TEST_LIZARD_DOWN);
	lizard_send(&out);   /* a running keyboard that stops (or stops and starts again) */
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
	struct opensc2_lizard_out out;
	bool sent;

	k_mutex_lock(&lizard_lock, K_FOREVER);
	sent = opensc2_lizard_release(&lizard, &out) != 0;
	for (uint8_t i = 0; i < out.n; i++) {
		(void)osc2_input_send(out.report[i], out.len[i], false);
	}
	k_mutex_unlock(&lizard_lock);
	return sent;
#else
	return false;
#endif
}

int opensc2_lizard_status(struct opensc2_lizard_status *status)
{
	if (status == NULL) {
		return -EINVAL;
	}
	memset(status, 0, sizeof(*status));
	status->mode = opensc2_lizard_mode();
#if defined(CONFIG_OPENSC2_LIZARD_MODE)
	k_mutex_lock(&lizard_lock, K_FOREVER);
	status->keyboard = lizard.kb_on;
	status->mouse = lizard.ms_on;
	status->keyboard_switch = lizard.kb_switch;
	status->button_switch = lizard.ms_switch;
	status->key_held = lizard.kb_on && lizard.kb_held;
	status->mouse_active = lizard.ms_on && lizard.mouse_owed;
	for (int side = 0; side < 2; side++) {
		status->gliding[side] = lizard.ms_on && lizard.pad[side].glide > 0.0f &&
					!lizard.pad[side].touched;
	}
	k_mutex_unlock(&lizard_lock);
	return 0;
#else
	return -ENOTSUP;
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
	osc2_battery_stats(&b);
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
#if defined(CONFIG_OPENSC2_LIZARD_MODE)
	lizard_keys_start();
#endif
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
 * for a stop (proto_rules.h), after the application heard about the output. With the haptics
 * synthesiser its sequencers send them, as the real ones do, when they take the stop. */
static void stop_status(enum osc2_link link, const struct opensc2_output *out)
{
#if defined(CONFIG_OPENSC2_HAPTIC_STOP_STATUS) && !defined(CONFIG_OPENSC2_HAPTICS_SYNTH)
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
