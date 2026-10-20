/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * system.c: the system state machine on Zephyr (CONFIG_OPENSC2_SYSTEM; sys_rules.h). One delayable
 * work item in the system work queue runs it all, so the machine, the chord matcher and the idle
 * monitor are only ever touched there:
 *   - the buttons the rules see (the application's, or the test hooks' system injection), through
 *     the chord matcher, and their changes for the idle monitor;
 *   - the power source, looked at every CONFIG_OPENSC2_POWER_POLL_MS: only a change of "power
 *     present" is an event, as on the real controller;
 *   - the events the rest of the library posts (battery levels, link and USB events), ten deep
 *     like the real controller's queue, and the power requests it takes over (the turn-off
 *     command, the cable pulled; a host's reboot request in a state that ignores it);
 *   - the machine's actions: the LED status (the led_status callback), cues (the cue callback),
 *     dropping and starting the link, the keylock, and the end: OPENSC2_POWER_OFF, or a reboot
 *     (setting 77, a board without a battery, the test hooks in use).
 * The keylock and the fully-charged LED level are the settings-by-name keys user/keylock and
 * user/fully_charged_led_intensity, whose values the library keeps (keys.c, opensc2/k/): a host's
 * 0xED..0xF0 and the machine see the same value.
 *
 * Without CONFIG_OPENSC2_SYSTEM only the public getters are here, answering as v0.2.0 would.
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include "internal.h"
#include "keys.h"
#include "system.h"

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

static volatile uint8_t app_source;   /* enum opensc2_power_source, opensc2_set_power_source() */

int opensc2_set_power_source(enum opensc2_power_source source)
{
	if (source > OPENSC2_POWER_SOURCE_DOCK) {
		return -EINVAL;
	}
	app_source = (uint8_t)source;
	osc2_sys_buttons_changed();   /* the machine looks at it now */
	return 0;
}

#if defined(CONFIG_OPENSC2_SYSTEM)

#include "chord_rules.h"
#include "idle_rules.h"
#include "sys_rules.h"
#include "test_hooks.h"
#include "test_sys.h"
#if defined(CONFIG_POWEROFF)
#include <zephyr/sys/poweroff.h>
#endif
#if defined(CONFIG_OPENSC2_SHELL)
#include <zephyr/shell/shell.h>
#endif

#define KEYLOCK_VALUE  0x41FB0AC3u   /* user/keylock while locked */
#define FULL_DEFAULT   3             /* user/fully_charged_led_intensity when absent: full */
#define QUEUE_DEPTH    10
#define Q_CUE          0x80          /* not a machine event: play cue `arg` */
#define POWER_GONE_MS  200           /* shorter dips change nothing (a hub resetting its port) */
#define TOUCH_BITS     (OPENSC2_TOUCH_RIGHT_PAD | OPENSC2_TOUCH_LEFT_PAD |                      \
			OPENSC2_TOUCH_RIGHT_STICK | OPENSC2_TOUCH_LEFT_STICK)

struct sys_msg {
	uint8_t ev;
	uint8_t arg;
};

K_MSGQ_DEFINE(osc2_sys_q, sizeof(struct sys_msg), QUEUE_DEPTH, 1);

static void sys_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sys_work, sys_work_fn);

/* Only the work item (and osc2_sys_power_request(), in the same queue) touches these. */
static struct osc2_sys machine;
static struct osc2_chords chords;
static struct osc2_idle idle;
static bool booted, boot_pending;
static uint8_t source;              /* enum osc2_sys_source, as the machine knows it */
static uint32_t buttons;            /* the rules' button word as last seen */
static uint8_t level_told = 255;    /* the level the machine and the LED were told */
static uint8_t last_cue = 0xFF;

static bool start_allowed;
static atomic_t frame_activity;
static atomic_t host_asleep_done;
static volatile bool battery_given;
static volatile bool input_recent = true;
static const struct osc2_bt_transport *bt;
#if defined(CONFIG_OPENSC2_TEST_HOOKS)
static const struct osc2_test_sys_env sys_env;
#endif


/* What the LED shows, as the machine chose it; opensc2_led_status() adds the rest. */
static struct k_spinlock led_lock;
static struct {
	uint8_t mode;
	uint8_t flags;           /* OSC2_LED_EXTERNAL, OSC2_LED_STEADY */
	bool shutting_down;
	uint8_t level;
	uint32_t below_full_ms;
} led = { .level = 255 };

/* ---- the keys (keys.c) ------------------------------------------------------------------------ */

/* user/keylock in RAM: 0x41FB0AC3 while locked. */
static uint32_t keylock_value(void)
{
	uint8_t v[OSC2_KEY_VALUE_MAX];
	uint8_t len = 0;

	return osc2_keys_get(OSC2_KEY_KEYLOCK, v, &len) == 0 && len == 4 ? opensc2_get_le32(v) : 0;
}

/* user/fully_charged_led_intensity in RAM: 0 off, 1 dim, 2 medium, 3 (or another value) full. */
static uint8_t full_level(void)
{
	uint8_t v[OSC2_KEY_VALUE_MAX];
	uint8_t len = 0;

	return osc2_keys_get(OSC2_KEY_FULL_LED, v, &len) == 0 && len == 1 ? v[0] : FULL_DEFAULT;
}

/* The keylock chord's hold: the lock stored for the next power-on, the value in RAM as it is. */
static int keylock_save(uint32_t value)
{
	uint8_t b[4];

	opensc2_put_le32(b, value);
	return osc2_keys_store(OSC2_KEY_KEYLOCK, b, sizeof(b));
}

/* The keylock chord's 5 ms tap, a USB or dock power-on: the key and the RAM value go. */
static void keylock_delete(void)
{
	if (keylock_value() != 0) {
		(void)osc2_keys_delete(OSC2_KEY_KEYLOCK);
		LOG_INF("Keylock deleted");
	}
}

/* ---- inputs ---------------------------------------------------------------------------------- */

/* The boot-time injection the test hooks left for this boot (once). */
static void boot_injection(void)
{
#if defined(CONFIG_OPENSC2_TEST_HOOKS) && defined(CONFIG_OPENSC2_USB)
	static bool done;
	uint32_t words[OSC2_RET_SYS_WORDS];

	if (done) {
		return;
	}
	done = true;
	osc2_retained_sys_at_boot(words);
	osc2_test_sys_boot_apply(words, k_uptime_get_32());
#endif
}

uint32_t osc2_sys_buttons(void)
{
	uint32_t b;

	if (osc2_test_sys_buttons(&b)) {
		return b;
	}
	return osc2_sys_app_buttons();
}

static uint8_t detect_source(void)
{
	uint8_t s;

	if (osc2_test_sys_source(&s)) {
		return s;
	}
	switch (app_source) {
	case OPENSC2_POWER_SOURCE_BATTERY:
		return OSC2_SRC_BATTERY;
	case OPENSC2_POWER_SOURCE_USB:
		return OSC2_SRC_USB;
	case OPENSC2_POWER_SOURCE_DOCK:
		return OSC2_SRC_DOCK;
	default:
		break;
	}
	if (osc2_usb_power()) {
		return OSC2_SRC_USB;
	}
	/* No USB power: the battery, or without one an external supply (it counts as USB power). */
	return IS_ENABLED(CONFIG_OPENSC2_BATTERY) ? OSC2_SRC_BATTERY : OSC2_SRC_USB;
}

bool osc2_sys_wired_power(void)
{
	boot_injection();
	return osc2_usb_power() && detect_source() == OSC2_SRC_USB;
}

enum opensc2_power_source opensc2_power_source(void)
{
	static const enum opensc2_power_source map[] = { OPENSC2_POWER_SOURCE_BATTERY,
							 OPENSC2_POWER_SOURCE_USB,
							 OPENSC2_POWER_SOURCE_DOCK };
	uint8_t s = booted ? source : detect_source();

	return s <= OSC2_SRC_DOCK ? map[s] : OPENSC2_POWER_SOURCE_USB;
}

bool opensc2_keylock(void)
{
	return keylock_value() == KEYLOCK_VALUE;
}

static bool link_connected(void)
{
	return (!osc2_wired() && osc2_radio_connected()) || (bt && bt->connected && bt->connected());
}

/* The level the machine is told: the application's once it set one (255 until then, as after a
 * cold start), or the test hooks' system injection; 100 while the charger gave up (0x80). */
static uint8_t level_of(const struct opensc2_battery *b)
{
	uint8_t level = battery_given ? b->level : 255;

	(void)osc2_test_sys_level(&level);
	return osc2_sys_level(b->charge_state, level);
}

static bool plug_in_allowed(void)
{
	return IS_ENABLED(CONFIG_OPENSC2_USB) && IS_ENABLED(CONFIG_OPENSC2_USB_PLUG_IN_REBOOT) &&
	       osc2.policy == OPENSC2_POLICY_AUTO && osc2.mode_reason != OPENSC2_MODE_APP &&
	       osc2.mode_reason != OPENSC2_MODE_USB_FAILED;
}

static void inputs(struct osc2_sys_in *in)
{
	memset(in, 0, sizeof(*in));
	in->stored = (uint8_t)(OSC2_TRANSPORT_PUCK1 + osc2.slot);
	in->chord = chords.current;
	in->level = level_told;
	in->steam = (buttons & OPENSC2_BTN_STEAM) != 0;
	in->keylock = keylock_value() == KEYLOCK_VALUE;
	in->reboot_not_off = osc2.settings[OPENSC2_SETTING_REBOOT_ON_OFF] != 0;
	in->link_up = link_connected();
	in->plug_in_reboot = plug_in_allowed();
}

/* ---- the LED --------------------------------------------------------------------------------- */

void opensc2_led_status(struct opensc2_led_status *st)
{
	struct opensc2_led l;
	k_spinlock_key_t key;

	memset(st, 0, sizeof(*st));
	key = k_spin_lock(&led_lock);
	st->mode = led.mode;
	st->external_power = (led.flags & OSC2_LED_EXTERNAL) != 0;
	st->steady = (led.flags & OSC2_LED_STEADY) != 0;
	st->shutting_down = led.shutting_down;
	st->level = led.level;
	st->below_full_ms = led.below_full_ms;
	k_spin_unlock(&led_lock, key);
	st->connected = link_connected();
	/* Bluetooth's pairing mode; the radio's over-the-air pairing (OpenSC2's) shows the same. */
	st->pairing = (bt && bt->pairing && bt->pairing()) ||
		      (!osc2_wired() && osc2_radio_state() == OPENSC2_LINK_PAIRING);
	st->input_recent = input_recent;
	st->full_level = full_level();
	st->brightness = opensc2_led_brightness(osc2.settings[OPENSC2_SETTING_LED_BRIGHTNESS],
						osc2.settings[OPENSC2_SETTING_LED_CURVE]);
	opensc2_led_get(&l);
	st->override = l.override;
	st->override_rgbw[0] = l.red;
	st->override_rgbw[1] = l.green;
	st->override_rgbw[2] = l.blue;
	st->override_rgbw[3] = l.white;
}

static void notify_led(void)
{
	struct opensc2_led_status st;

	if (osc2.cb && osc2.cb->led_status) {
		opensc2_led_status(&st);
		osc2.cb->led_status(&st, osc2.user_data);
	}
}

static void level_seen(uint8_t level)
{
	k_spinlock_key_t key = k_spin_lock(&led_lock);
	bool changed = led.level != level;

	led.level = level;
	if (level < 100) {
		/* The fully-charged display shows the full colour for 15 s after this. */
		led.below_full_ms = MAX(k_uptime_get_32(), 1u);
	}
	k_spin_unlock(&led_lock, key);
	if (changed) {
		notify_led();
	}
}

/* ---- cues ------------------------------------------------------------------------------------ */

static void play_cue(uint8_t cue)
{
	struct opensc2_cue_event e = { .cue = cue };
	bool play = osc2_cue_map(cue, osc2.chime, &e.script, &e.gain_db);

	osc2_test_sys_note_cue(cue, e.script, e.gain_db, play);
	last_cue = cue;
	if (!play) {
		LOG_DBG("Cue %u: silent (chime level %u)", cue, osc2.chime);
		return;
	}
	LOG_DBG("Cue %u: script %u at %d dB", cue, e.script, e.gain_db);
	/* A haptics renderer plays OpenSC2's own sound for the cue here, through the callback. */
	if (osc2.cb && osc2.cb->cue) {
		osc2.cb->cue(&e, osc2.user_data);
	}
}

static void post(uint8_t ev, uint8_t arg)
{
	struct sys_msg m = { .ev = ev, .arg = arg };

	if (k_msgq_put(&osc2_sys_q, &m, K_NO_WAIT) != 0) {
		LOG_WRN("System event %s missed: the queue is full",
			ev == Q_CUE ? "cue" : osc2_sys_event_name(ev));
	}
	(void)k_work_reschedule(&sys_work, K_NO_WAIT);
}

void osc2_sys_cue_post(uint8_t cue)
{
	post(Q_CUE, cue);
}

/* ---- the actions ----------------------------------------------------------------------------- */

static void drop_link(const char *why)
{
	if (osc2_wired()) {
#if defined(CONFIG_OPENSC2_USB)
		osc2_usb_detach(why);   /* the zero report, 10 ms, USB off */
#endif
		return;
	}
	if (bt && bt->stop) {
		bt->stop();
	}
	if (osc2_radio_state() != OPENSC2_LINK_OFF) {
		(void)opensc2_stop();   /* the puck notices the silence */
	}
}

static void start_link(uint8_t transport, bool pairing)
{
	uint8_t slot;

	if (!start_allowed || osc2_wired()) {
		LOG_INF("Link not started: the application starts it (opensc2_config.start)");
		return;
	}
	if (transport <= OSC2_TRANSPORT_BT2) {
		if (bt && bt->start) {
			(void)bt->start(transport, pairing);
			return;
		}
		LOG_WRN("Bluetooth slot %u chosen, but this build has no Bluetooth: the main puck",
			transport + 1);
		transport = OSC2_TRANSPORT_PUCK1;
	}
	/* The 2.4 GHz link ignores the pairing flag. */
	slot = (uint8_t)(transport - OSC2_TRANSPORT_PUCK1);
	if (opensc2_selected_slot() != slot) {
		(void)opensc2_select_slot(slot);
	}
	(void)opensc2_start();
}

static void keep_end(const struct osc2_sys_out *o, uint8_t how)
{
#if defined(CONFIG_OPENSC2_TEST_HOOKS) && defined(CONFIG_OPENSC2_USB)
	struct osc2_test_sys_end e = {
		.state = o->end_state,
		.cause = o->end_cause,
		.end = o->end,
		.how = how,
		.uptime_ms = k_uptime_get_32(),
	};
	uint32_t words[OSC2_TEST_SYS_END_WORDS];

	osc2_test_sys_end_pack(&e, words);
	osc2_retained_sys_set(OSC2_RET_SYS_BOOT_WORDS, words, OSC2_TEST_SYS_END_WORDS);
#else
	ARG_UNUSED(o);
	ARG_UNUSED(how);
#endif
}

static void end(const struct osc2_sys_out *o)
{
	enum opensc2_power_request request = OPENSC2_POWER_REBOOT;
	const char *why;
	uint8_t how;

	if (o->end == OSC2_SYS_END_REBOOT) {
		how = OSC2_TEST_SYS_HOW_REBOOT;
		if (o->end_cause == OSC2_SYS_EV_POWER_USB) {
			request = OPENSC2_POWER_USB_PLUGGED;
			why = "USB power: back to wired";
		} else if (o->end_state == OSC2_SYS_SHUTDOWN) {
			why = "shutdown: setting 77, reboot";
		} else {
			why = "STEAM, wireless off: reboot";
		}
	} else if (!IS_ENABLED(CONFIG_OPENSC2_BATTERY)) {
		how = OSC2_TEST_SYS_HOW_NO_BATTERY;
		why = "shutdown: no battery, reboot";
	} else if (osc2_test_sys_keep_on()) {
		how = OSC2_TEST_SYS_HOW_TEST;
		why = "shutdown: test hooks, reboot";
	} else {
		request = OPENSC2_POWER_OFF;
		how = OSC2_TEST_SYS_HOW_POWER_OFF;
		why = "shutdown: power off";
	}
	osc2_test_sys_note_end(o->end_state, o->end_cause, o->end, how);
	keep_end(o, how);
	LOG_INF("%s (%s, after %s)", why, osc2_sys_state_name(o->end_state),
		osc2_sys_event_name(o->end_cause));
	osc2_usblog("system: %s (%s, after %s)", why, osc2_sys_state_name(o->end_state),
		    osc2_sys_event_name(o->end_cause));
	osc2_power_now(request, why);
}

static void apply(const struct osc2_sys_out *o)
{
	if (o->actions & OSC2_SYS_DO_STATE) {
		LOG_INF("System: %s -> %s (%s)", osc2_sys_state_name(o->from),
			osc2_sys_state_name(o->to), osc2_sys_event_name(o->cause));
		osc2_usblog("system: %s -> %s (%s)", osc2_sys_state_name(o->from),
			    osc2_sys_state_name(o->to), osc2_sys_event_name(o->cause));
		osc2_test_sys_note_state(o->from, o->to, o->cause, machine.chosen, source);
	}
	if (o->actions & (OSC2_SYS_DO_IR_OFF | OSC2_SYS_DO_LED)) {
		k_spinlock_key_t key = k_spin_lock(&led_lock);
		uint8_t flags;

		if (o->actions & OSC2_SYS_DO_IR_OFF) {
			led.shutting_down = true;
		}
		if (o->actions & OSC2_SYS_DO_LED) {
			led.mode = o->led_mode;
			led.flags = o->led_flags;
		}
		flags = (uint8_t)(led.flags | (led.shutting_down ? 0x04 : 0));
		k_spin_unlock(&led_lock, key);
		osc2_test_sys_note_led(led.mode, flags, level_told, full_level());
		notify_led();
	}
	if (o->actions & OSC2_SYS_DO_DROP) {
		drop_link(o->cause == OSC2_SYS_EV_UNPLUGGED || o->cause == OSC2_SYS_EV_POWER_NONE ?
			  "USB power gone" : "shutdown");
	}
	if (o->actions & OSC2_SYS_DO_CUE) {
		play_cue(o->cue);
	}
	if (o->actions & OSC2_SYS_DO_KEYLOCK_DEL) {
		keylock_delete();
	}
	if (o->actions & OSC2_SYS_DO_KEYLOCK_SET) {
		/* Saved for the next power-on; the copy in RAM stays as it is. */
		LOG_INF("Keylock set: the next power-on on battery turns straight off");
		(void)keylock_save(KEYLOCK_VALUE);
	}
	if (o->actions & OSC2_SYS_DO_START) {
		start_link(o->transport, o->pairing);
	}
	if (o->actions & OSC2_SYS_DO_END) {
		end(o);
	}
}

static void run(uint8_t ev, uint8_t arg, uint32_t now, struct osc2_sys_out *out)
{
	struct osc2_sys_in in;

	inputs(&in);
	if (ev == OSC2_SYS_EV_LEVEL) {
		level_told = arg;
		in.level = arg;
	}
	osc2_sys_event(&machine, &in, ev, arg, now, out);
	apply(out);
	if (ev == OSC2_SYS_EV_LEVEL) {
		level_seen(arg);
	}
}

/* ---- the work item --------------------------------------------------------------------------- */

static void chord_events(const struct osc2_chord_out *co)
{
	if (co->events & OSC2_CHORD_EV_UP) {
		osc2_test_sys_note_chord(OSC2_TEST_CHORD_UP, co->up);
		post(OSC2_SYS_EV_CHORD_UP, co->up);
	}
	if (co->events & OSC2_CHORD_EV_DOWN) {
		osc2_test_sys_note_chord(OSC2_TEST_CHORD_DOWN, co->down);
		post(OSC2_SYS_EV_CHORD_DOWN, co->down);
	}
	if (co->events & OSC2_CHORD_EV_STEAM_PRESS) {
		osc2_test_sys_note_chord(OSC2_TEST_CHORD_STEAM_PRESS, OSC2_CHORD_STEAM);
		post(OSC2_SYS_EV_STEAM_PRESSED, 0);
	}
	if (co->events & OSC2_CHORD_EV_STEAM_HOLD) {
		osc2_test_sys_note_chord(OSC2_TEST_CHORD_STEAM_HOLD, OSC2_CHORD_STEAM);
		post(OSC2_SYS_EV_STEAM_HELD, 0);
	}
	if (co->events & OSC2_CHORD_EV_KEYLOCK_TAP) {
		/* In any state: holding the chord to set the lock deletes it first. */
		osc2_test_sys_note_chord(OSC2_TEST_CHORD_KEYLOCK_TAP, OSC2_CHORD_KEYLOCK);
		keylock_delete();
	}
	if (co->events & OSC2_CHORD_EV_KEYLOCK_HOLD) {
		osc2_test_sys_note_chord(OSC2_TEST_CHORD_KEYLOCK_HOLD, OSC2_CHORD_KEYLOCK);
		post(OSC2_SYS_EV_KEYLOCK_HELD, 0);
	}
	if (co->events & OSC2_CHORD_EV_STEAM_UP) {
		post(OSC2_SYS_EV_STEAM_UP, 0);
	}
}

static void look_buttons(uint32_t now)
{
	struct osc2_chord_out co;
	uint32_t b = osc2_sys_buttons();

	if (b != buttons) {
		if (osc2_idle_buttons_active(buttons, b)) {
			osc2_idle_activity(&idle);
		}
		osc2_chords_update(&chords, b, now, &co);
		buttons = b;
		chord_events(&co);
	}
	if (osc2_chords_next(&chords, now) == 0) {
		osc2_chords_poll(&chords, now, &co);
		chord_events(&co);
	}
}

static void look_idle(uint32_t now)
{
	osc2_idle_setting(&idle, osc2.settings[OPENSC2_SETTING_SLEEP_TIMEOUT_S], now);
	if (atomic_clear(&frame_activity)) {
		osc2_idle_activity(&idle);
	}
	if (osc2_idle_next(&idle, now) == 0) {
		/* A touch still there at the end of a period counts: touches are sampled without a
		 * link too. */
		if (buttons & TOUCH_BITS) {
			osc2_idle_activity(&idle);
		}
		if (osc2_idle_poll(&idle, now)) {
			osc2_test_sys_note_idle(idle.history);
			post(OSC2_SYS_EV_IDLE, 0);
		}
	}
	input_recent = osc2_idle_recent(&idle);
}

static void look_source(uint32_t now)
{
	static bool gone;
	static uint32_t gone_at;
	uint8_t s = detect_source();
	int ev;

	if (s == source) {
		gone = false;
		return;
	}
	/* Power that goes away must stay away 0.2 s, as the wired device's unplug rule wants; power
	 * that comes counts at once. */
	if (s == OSC2_SRC_BATTERY) {
		if (!gone) {
			gone = true;
			gone_at = now;
		}
		if (now - gone_at < POWER_GONE_MS) {
			return;
		}
	}
	gone = false;
	ev = osc2_sys_source_event(source, s);
	osc2_test_sys_note_source(source, s, ev);
	LOG_INF("Power source %u -> %u", source, s);
	source = s;
	if (ev >= 0) {
		post((uint8_t)ev, 0);
	}
}

static void boot(uint32_t now)
{
	struct osc2_sys_boot_in b;
	struct osc2_sys_in in;
	struct osc2_sys_out out;
	struct osc2_chord_out co;
	struct opensc2_battery battery;
	k_spinlock_key_t key;

	boot_injection();
	buttons = osc2_sys_buttons();
	/* A chord held now counts for the power-on decision, not as a press. */
	osc2_chords_init(&chords, bt != NULL, buttons, now, &co);
	osc2_idle_start(&idle, osc2.settings[OPENSC2_SETTING_SLEEP_TIMEOUT_S], now);
	source = detect_source();
	key = k_spin_lock(&osc2.lock);
	battery = osc2.battery;
	k_spin_unlock(&osc2.lock, key);
	level_told = level_of(&battery);
	b.wired = osc2_wired();
	b.warm = osc2.mode_reason == OPENSC2_MODE_WARM_RESTART;
	b.source = source;
	inputs(&in);
	osc2_sys_boot(&machine, &in, &b, now, &out);
	booted = true;
	LOG_INF("Power-on: %s (power source %u, level %u)", osc2_sys_state_name(machine.state),
		source, level_told);
	apply(&out);
	level_seen(level_told);
}

static int32_t soonest(int32_t a, int32_t b)
{
	if (a < 0) {
		return b;
	}
	if (b < 0) {
		return a;
	}
	return MIN(a, b);
}

static void sys_work_fn(struct k_work *work)
{
	uint32_t now = k_uptime_get_32();
	struct osc2_sys_out out;
	struct sys_msg m;
	int32_t next;

	ARG_UNUSED(work);
	if (!booted) {
		if (!boot_pending) {
			return;
		}
		boot(now);
	}
	if (osc2_test_sys_next(now) == 0) {
		(void)osc2_test_sys_poll(now);   /* the boot injection's buttons let go */
	}
	look_buttons(now);
	look_idle(now);
	look_source(now);
	while (!machine.ended && osc2_sys_wait(&machine, now) == 0 &&
	       k_msgq_get(&osc2_sys_q, &m, K_NO_WAIT) == 0) {
		if (m.ev == Q_CUE) {
			play_cue(m.arg);
		} else {
			run(m.ev, m.arg, now, &out);
		}
	}
	if (!machine.ended && osc2_sys_next(&machine, now) == 0) {
		run(OSC2_SYS_EV_TIMER, 0, now, &out);
	}
	next = CONFIG_OPENSC2_POWER_POLL_MS;
	next = soonest(next, osc2_chords_next(&chords, now));
	next = soonest(next, osc2_idle_next(&idle, now));
	next = soonest(next, osc2_sys_next(&machine, now));
	next = soonest(next, osc2_test_sys_next(now));
	if (machine.ended) {
		/* Nothing more happens (the application has the power-off or the reboot): what still
		 * comes is dropped, so the queue cannot hold this work item at a 1 ms period. */
		k_msgq_purge(&osc2_sys_q);
	} else if (k_msgq_num_used_get(&osc2_sys_q) > 0) {
		next = soonest(next, (int32_t)osc2_sys_wait(&machine, now));
	}
	(void)k_work_reschedule(&sys_work, K_MSEC(MAX(next, 1)));
}

/* ---- what the rest of the library calls ------------------------------------------------------ */

void osc2_sys_init(bool start)
{
#if defined(CONFIG_OPENSC2_TEST_HOOKS)
	osc2_test_sys_env = &sys_env;   /* the test hooks' system ops reach the machine */
#endif
	start_allowed = start;
	boot_pending = true;
	(void)k_work_reschedule(&sys_work, K_NO_WAIT);
}

void osc2_sys_buttons_changed(void)
{
	if (boot_pending) {
		(void)k_work_reschedule(&sys_work, K_NO_WAIT);
	}
}

void osc2_sys_battery_given(void)
{
	battery_given = true;
}

void osc2_sys_battery_report(const struct opensc2_battery *battery)
{
	post(OSC2_SYS_EV_LEVEL, level_of(battery));
}

void osc2_sys_frame(const struct opensc2_state *state)
{
	if (osc2_idle_state_active(state)) {
		atomic_set(&frame_activity, 1);
	}
}

void osc2_sys_setting(uint8_t number)
{
	ARG_UNUSED(number);
	if (boot_pending) {
		(void)k_work_reschedule(&sys_work, K_NO_WAIT);   /* setting 50, the LED's settings */
	}
}

void osc2_sys_link_changed(uint8_t state, uint8_t reason)
{
	static uint8_t last = OPENSC2_LINK_OFF;

	if (state == OPENSC2_LINK_OFF) {
		switch (reason) {
		case OPENSC2_REASON_LISTEN_TIMEOUT:
		case OPENSC2_REASON_NO_BOND_TIMEOUT:
		case OPENSC2_REASON_PUCK_TURN_OFF:
		case OPENSC2_REASON_BAD_VERSION:
			/* The link gave up, or the puck turned it off: "wireless off". A radio
			 * stopped here (opensc2_stop()) is not. */
			post(OSC2_SYS_EV_WIRELESS_OFF, reason);
			break;
		default:
			break;
		}
	} else if (last == OPENSC2_LINK_OFF) {
		post(OSC2_SYS_EV_LINK_STARTED, state);
	}
	/* The 2.4 GHz over-the-air pairing's cues (the real controller's pairing routine has them,
	 * though nothing reaches it there). */
	if (state == OPENSC2_LINK_PAIRING && last != OPENSC2_LINK_PAIRING) {
		osc2_sys_cue_post(OPENSC2_CUE_PAIRING);
	}
	if (reason == OPENSC2_REASON_PAIRED) {
		osc2_sys_cue_post(OPENSC2_CUE_PAIRED);
	}
	last = state;
}

void osc2_sys_host_asleep(void)
{
	/* Once per boot: nothing re-arms it. */
	if (atomic_cas(&host_asleep_done, 0, 1)) {
		osc2_sys_cue_post(OPENSC2_CUE_HOST_ASLEEP);
	}
}

void osc2_sys_usb_changed(const struct opensc2_usb_info *info)
{
	if (info->state == OPENSC2_USB_CONFIGURED) {
		post(OSC2_SYS_EV_CONFIGURED, 0);
	}
}

bool osc2_sys_power_request(enum opensc2_power_request request, const char *why)
{
	uint32_t now = k_uptime_get_32();
	bool host = why && strncmp(why, "host:", 5) == 0;
	struct osc2_sys_out out;
	uint8_t ev;

	if (!booted || machine.ended) {
		return false;   /* its own end (or before the power-on decision): the application */
	}
	switch (request) {
	case OPENSC2_POWER_TURN_OFF:
		ev = OSC2_SYS_EV_TURN_OFF;
		break;
	case OPENSC2_POWER_USB_UNPLUGGED:
		ev = OSC2_SYS_EV_UNPLUGGED;
		break;
	case OPENSC2_POWER_REBOOT:
		ev = OSC2_SYS_EV_REBOOT;
		break;
	case OPENSC2_POWER_REBOOT_KEEP_LINK:
		ev = OSC2_SYS_EV_WARM_REBOOT;
		break;
	case OPENSC2_POWER_BOOTLOADER:
		ev = OSC2_SYS_EV_BOOTLOADER;
		break;
	default:
		return false;
	}
	if (ev >= OSC2_SYS_EV_BOOTLOADER && ev <= OSC2_SYS_EV_WARM_REBOOT && !host) {
		return false;   /* the library's own rules (STEAM before a host, a shell command) */
	}
	if (osc2_sys_wait(&machine, now) > 0 || machine.steam_wait) {
		/* A chord state's pause, or a shutdown waiting for STEAM: neither acts on it. */
		if (ev == OSC2_SYS_EV_TURN_OFF || ev == OSC2_SYS_EV_UNPLUGGED) {
			post(ev, 0);
		}
		return true;
	}
	run(ev, 0, now, &out);
	(void)k_work_reschedule(&sys_work, K_NO_WAIT);
	if (ev == OSC2_SYS_EV_TURN_OFF || ev == OSC2_SYS_EV_UNPLUGGED) {
		return true;
	}
	if (!(out.actions & OSC2_SYS_DO_PASS)) {
		LOG_INF("%s ignored in %s", why, osc2_sys_state_name(machine.state));
		return true;
	}
	return false;
}

void osc2_sys_bluetooth(const struct osc2_bt_transport *transport)
{
	bt = transport;
	chords.bluetooth = transport != NULL;
}

void osc2_sys_bt_event(uint8_t event)
{
	switch (event) {
	case OSC2_SYS_BT_GAVE_UP:
		post(OSC2_SYS_EV_WIRELESS_OFF, 0);
		break;
	case OSC2_SYS_BT_PAIRING:
		osc2_sys_cue_post(OPENSC2_CUE_PAIRING);
		break;
	case OSC2_SYS_BT_PAIRED:
		osc2_sys_cue_post(OPENSC2_CUE_PAIRED);
		break;
	default:
		break;
	}
}

/* ---- the test hooks' environment ------------------------------------------------------------- */

#if defined(CONFIG_OPENSC2_TEST_HOOKS)
static void env_status(uint8_t out[OSC2_TEST_SYS_STATUS_LEN])
{
	struct osc2_test_sys_end e = { 0 };
	bool have_end = false;
	k_spinlock_key_t key;

#if defined(CONFIG_OPENSC2_USB)
	uint32_t words[OSC2_RET_SYS_WORDS];

	osc2_retained_sys_at_boot(words);
	have_end = osc2_test_sys_end_unpack(&words[OSC2_RET_SYS_BOOT_WORDS], &e);
#endif
	memset(out, 0, OSC2_TEST_SYS_STATUS_LEN);
	out[0] = machine.state;
	out[1] = machine.chosen;
	out[2] = source;
	key = k_spin_lock(&led_lock);
	out[3] = led.mode;
	out[4] = (uint8_t)(led.flags | (led.shutting_down ? 0x04 : 0));
	k_spin_unlock(&led_lock, key);
	out[5] = chords.current;
	out[6] = level_told;
	out[7] = (uint8_t)((keylock_value() == KEYLOCK_VALUE ? 0x01 : 0) | (bt ? 0x20 : 0) |
			   (IS_ENABLED(CONFIG_OPENSC2_BATTERY) ? 0x40 : 0) | (machine.ended ? 0x80 : 0));
	opensc2_put_le16(&out[8], idle.history);
	opensc2_put_le32(&out[10], idle.period_ms);
	out[14] = last_cue;
	out[15] = machine.boot_state;
	out[16] = machine.boot_cue;
	out[17] = machine.boot_chord;
	out[18] = machine.boot_pairing ? 1 : 0;
	out[19] = have_end ? 1 : 0;
	out[20] = e.state;
	out[21] = e.cause;
	out[22] = e.how;
	opensc2_put_le32(&out[23], e.uptime_ms);
	out[27] = full_level();
	opensc2_put_le16(&out[28], (uint16_t)idle.timeout_s);
}

static void env_changed(void)
{
	(void)k_work_reschedule(&sys_work, K_NO_WAIT);
}

static int env_boot_store(const uint32_t words[OSC2_TEST_SYS_BOOT_WORDS])
{
#if defined(CONFIG_OPENSC2_USB)
	osc2_retained_sys_set(0, words, OSC2_TEST_SYS_BOOT_WORDS);
	return 0;
#else
	ARG_UNUSED(words);
	return -ENOTSUP;
#endif
}

static void env_event(uint8_t kind)
{
	if (kind == OSC2_TEST_SYS_EV_WIRELESS_OFF) {
		post(OSC2_SYS_EV_WIRELESS_OFF, OPENSC2_REASON_LISTEN_TIMEOUT);
		return;
	}
	/* The puck's "PC asleep" packet, as radio.c handles it. */
	osc2_test_note_host_asleep();
	if (osc2.cb && osc2.cb->host_asleep) {
		osc2.cb->host_asleep(osc2.user_data);
	}
	osc2_sys_host_asleep();
}

static const struct osc2_test_sys_env sys_env = {
	.status = env_status,
	.changed = env_changed,
	.boot_store = env_boot_store,
	.event = env_event,
};
#endif /* CONFIG_OPENSC2_TEST_HOOKS */

/* ---- the shell ------------------------------------------------------------------------------- */

#if defined(CONFIG_OPENSC2_SHELL)
int osc2_sys_shell(const struct shell *sh, size_t argc, char **argv)
{
	static const char *const sources[] = { "battery", "USB", "dock" };
	struct opensc2_led_status st;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	opensc2_led_status(&st);
	shell_print(sh, "source:    %s%s, %s", source <= OSC2_SRC_DOCK ? sources[source] : "?",
		    app_source != OPENSC2_POWER_SOURCE_AUTO ? " (set by the application)" : "",
		    IS_ENABLED(CONFIG_OPENSC2_BATTERY) ? "battery build" : "no battery: off = reboot");
	shell_print(sh, "state:     %s, transport %s, LED mode %u%s%s",
		    osc2_sys_state_name(machine.state),
		    machine.chosen == OSC2_TRANSPORT_NONE ? "none" :
		    machine.chosen <= OSC2_TRANSPORT_BT2 ? "Bluetooth" : "puck",
		    st.mode, st.external_power ? " (external power)" : "",
		    st.steady ? " (steady)" : "");
	shell_print(sh, "battery:   level %u%s; keylock %s; fully-charged LED %u",
		    level_told, battery_given ? "" : " (none given)",
		    keylock_value() == KEYLOCK_VALUE ? "stored" : "none", full_level());
	shell_print(sh, "idle:      setting 50 = %d s, history %04x, input recent %s",
		    idle.timeout_s, idle.history, input_recent ? "yes" : "no");
	return 0;
}
#endif

#else /* !CONFIG_OPENSC2_SYSTEM: the public getters, as v0.2.0 */


enum opensc2_power_source opensc2_power_source(void)
{
	if (app_source != OPENSC2_POWER_SOURCE_AUTO) {
		return (enum opensc2_power_source)app_source;
	}
	return osc2_usb_power() ? OPENSC2_POWER_SOURCE_USB : OPENSC2_POWER_SOURCE_BATTERY;
}

bool opensc2_keylock(void)
{
	return false;
}

void opensc2_led_status(struct opensc2_led_status *st)
{
	struct opensc2_led l;

	memset(st, 0, sizeof(*st));
	st->level = 255;
	st->full_level = 3;
	st->input_recent = true;
	st->brightness = opensc2_led_brightness(osc2.settings[OPENSC2_SETTING_LED_BRIGHTNESS],
						osc2.settings[OPENSC2_SETTING_LED_CURVE]);
	opensc2_led_get(&l);
	st->override = l.override;
	st->override_rgbw[0] = l.red;
	st->override_rgbw[1] = l.green;
	st->override_rgbw[2] = l.blue;
	st->override_rgbw[3] = l.white;
}

#endif /* CONFIG_OPENSC2_SYSTEM */
