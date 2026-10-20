/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * fwemu_sys.c: the system state machine in the firmware emulator (fwemu.c), the way system.c runs
 * it in the firmware, in fewer lines: the machine (sys_rules.c), the chords and the idle monitor
 * on the buttons the rules see, the power source (USB, unless the test hooks inject another), the
 * battery levels, the link, the turn-off command, the test hooks' system ops (test_sys.c) with a
 * boot-time injection and the last end in "retained" words that outlive the emulated reboots, and
 * the keylock in "flash". Every end reboots, as on a board without a battery.
 */
#include <errno.h>
#include <string.h>
#include "internal.h"
#include "chord_rules.h"
#include "idle_rules.h"
#include "sys_rules.h"
#include "stubs.h"
#include "hooks_stub.h"
#include "test_sys.h"
#include "fwemu_sys.h"

#define KEYLOCK_VALUE 0x41FB0AC3u
#define QUEUE_MAX     10
#define Q_CUE         0x80
#define POWER_GONE_MS 200

static struct osc2_sys machine;
static struct osc2_chords chords;
static struct osc2_idle idle;
static bool booted;
static uint8_t source;
static uint32_t buttons;
static uint8_t level_told = 255;
static uint8_t last_cue = 0xFF;
static bool host_asleep_done;
static bool gone;
static uint32_t gone_at;
static uint8_t led_mode, led_flags;
static bool shutting_down;
static struct {
	uint8_t ev, arg;
} q[QUEUE_MAX];
static int q_len;

/* What survives an emulated reboot: the retained words and the keylock in "flash". */
static uint32_t retained[OSC2_RET_SYS_WORDS];
static uint32_t end_at_boot[OSC2_TEST_SYS_END_WORDS];   /* the last end, as this boot found it */
static uint32_t keylock_flash;
static uint32_t keylock_ram;

static void post(uint8_t ev, uint8_t arg)
{
	if (q_len < QUEUE_MAX) {
		q[q_len].ev = ev;
		q[q_len].arg = arg;
		q_len++;
	}
}

static uint32_t rule_buttons(void)
{
	uint32_t b = 0;   /* nothing wired: the application's word is all zero */

	(void)osc2_test_sys_buttons(&b);
	return b;
}

static uint8_t detect_source(void)
{
	uint8_t s = OSC2_SRC_USB;

	(void)osc2_test_sys_source(&s);
	return s;
}

static uint8_t level_of(const struct opensc2_battery *b)
{
	uint8_t level = 255;   /* the emulator's application never sets a battery */

	(void)osc2_test_sys_level(&level);
	return osc2_sys_level(b->charge_state, level);
}

static void inputs(struct osc2_sys_in *in)
{
	memset(in, 0, sizeof(*in));
	in->stored = (uint8_t)(OSC2_TRANSPORT_PUCK1 + osc2.slot);
	in->chord = chords.current;
	in->level = level_told;
	in->steam = (buttons & OPENSC2_BTN_STEAM) != 0;
	in->keylock = keylock_ram == KEYLOCK_VALUE;
	in->reboot_not_off = osc2.settings[OPENSC2_SETTING_REBOOT_ON_OFF] != 0;
	in->link_up = hk.radio_connected;
	in->plug_in_reboot = osc2.policy == OPENSC2_POLICY_AUTO;
}

static void play_cue(uint8_t cue)
{
	uint8_t script = 0;
	int8_t gain = 0;
	bool play = osc2_cue_map(cue, osc2.chime, &script, &gain);

	osc2_test_sys_note_cue(cue, script, gain, play);
	last_cue = cue;
}

static void end(const struct osc2_sys_out *o)
{
	struct osc2_test_sys_end e = {
		.state = o->end_state,
		.cause = o->end_cause,
		.end = o->end,
		.uptime_ms = (uint32_t)stub.now_ms,
	};
	uint32_t words[OSC2_TEST_SYS_END_WORDS];

	/* No battery: a power-off reboots too. */
	e.how = o->end == OSC2_SYS_END_REBOOT ? OSC2_TEST_SYS_HOW_REBOOT :
		OSC2_TEST_SYS_HOW_NO_BATTERY;
	osc2_test_sys_note_end(o->end_state, o->end_cause, o->end, e.how);
	osc2_test_sys_end_pack(&e, words);
	memcpy(&retained[OSC2_RET_SYS_BOOT_WORDS], words, sizeof(words));
	osc2_test_note_power(o->end_cause == OSC2_SYS_EV_POWER_USB ? OPENSC2_POWER_USB_PLUGGED :
			     OPENSC2_POWER_REBOOT, "system: reboot");
	emu_reboot();
}

static void apply(const struct osc2_sys_out *o)
{
	if (o->actions & OSC2_SYS_DO_STATE) {
		osc2_test_sys_note_state(o->from, o->to, o->cause, machine.chosen, source);
	}
	if (o->actions & OSC2_SYS_DO_IR_OFF) {
		shutting_down = true;
	}
	if (o->actions & OSC2_SYS_DO_LED) {
		led_mode = o->led_mode;
		led_flags = o->led_flags;
	}
	if (o->actions & (OSC2_SYS_DO_IR_OFF | OSC2_SYS_DO_LED)) {
		osc2_test_sys_note_led(led_mode, (uint8_t)(led_flags | (shutting_down ? 0x04 : 0)),
				       level_told, 3);
	}
	if ((o->actions & OSC2_SYS_DO_DROP) && hk.radio_connected) {
		emu_radio(false);
	}
	if (o->actions & OSC2_SYS_DO_CUE) {
		play_cue(o->cue);
	}
	if (o->actions & OSC2_SYS_DO_KEYLOCK_DEL) {
		keylock_ram = 0;
		keylock_flash = 0;
	}
	if (o->actions & OSC2_SYS_DO_KEYLOCK_SET) {
		keylock_flash = KEYLOCK_VALUE;   /* RAM stays: it counts from the next boot */
	}
	if ((o->actions & OSC2_SYS_DO_START) && osc2.mode == OPENSC2_MODE_WIRELESS &&
	    !hk.radio_connected) {
		emu_radio(true);
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
}

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
		osc2_test_sys_note_chord(OSC2_TEST_CHORD_KEYLOCK_TAP, OSC2_CHORD_KEYLOCK);
		keylock_ram = 0;
		keylock_flash = 0;
	}
	if (co->events & OSC2_CHORD_EV_KEYLOCK_HOLD) {
		osc2_test_sys_note_chord(OSC2_TEST_CHORD_KEYLOCK_HOLD, OSC2_CHORD_KEYLOCK);
		post(OSC2_SYS_EV_KEYLOCK_HELD, 0);
	}
	if (co->events & OSC2_CHORD_EV_STEAM_UP) {
		post(OSC2_SYS_EV_STEAM_UP, 0);
	}
}

void emu_sys_process(void)
{
	uint32_t now = (uint32_t)stub.now_ms;
	struct osc2_chord_out co;
	struct osc2_sys_out out;
	uint32_t b;
	uint8_t s;

	if (!booted) {
		return;
	}
	if (osc2_test_sys_next(now) == 0) {
		(void)osc2_test_sys_poll(now);
	}
	b = rule_buttons();
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
	osc2_idle_setting(&idle, osc2.settings[OPENSC2_SETTING_SLEEP_TIMEOUT_S], now);
	if (osc2_idle_next(&idle, now) == 0 && osc2_idle_poll(&idle, now)) {
		osc2_test_sys_note_idle(idle.history);
		post(OSC2_SYS_EV_IDLE, 0);
	}
	s = detect_source();
	if (s == source) {
		gone = false;
	} else if (s == OSC2_SRC_BATTERY && (!gone || now - gone_at < POWER_GONE_MS)) {
		if (!gone) {
			gone = true;
			gone_at = now;
		}
	} else {
		int ev = osc2_sys_source_event(source, s);

		gone = false;
		osc2_test_sys_note_source(source, s, ev);
		source = s;
		if (ev >= 0) {
			post((uint8_t)ev, 0);
		}
	}
	while (q_len > 0 && booted && !machine.ended && osc2_sys_wait(&machine, now) == 0) {
		uint8_t ev = q[0].ev, arg = q[0].arg;

		memmove(&q[0], &q[1], sizeof(q[0]) * (size_t)(q_len - 1));
		q_len--;
		if (ev == Q_CUE) {
			play_cue(arg);
		} else {
			run(ev, arg, now, &out);
		}
	}
	if (booted && !machine.ended && osc2_sys_next(&machine, now) == 0) {
		run(OSC2_SYS_EV_TIMER, 0, now, &out);
	}
}

/* ---- what commands.c asks (the emulator is built with CONFIG_OPENSC2_SYSTEM) ------------------- */

void osc2_sys_setting(uint8_t number)
{
	(void)number;   /* the machine reads setting 50 and 77 when it runs */
}

int osc2_sys_key(uint8_t opcode, const uint8_t *payload, uint8_t len)
{
	uint8_t name_len = 0, id = osc2_sys_key_id(payload, len, &name_len);

	if (id == OSC2_SYS_KEY_NONE) {
		return -ENOENT;
	}
	if (opcode == 0xEE) {
		if ((uint8_t)(len - name_len) != osc2_sys_key_size(id)) {
			return -EINVAL;
		}
		if (id == OSC2_SYS_KEY_KEYLOCK) {
			keylock_ram = opensc2_get_le32(payload + name_len);
		}
		return 0;
	}
	if (name_len != len) {
		return -EINVAL;
	}
	if (id == OSC2_SYS_KEY_KEYLOCK) {
		if (opcode == 0xEF) {
			keylock_flash = keylock_ram;
		} else {
			keylock_ram = keylock_flash = 0;
		}
	}
	return 0;
}

int osc2_sys_key_read(const uint8_t *payload, uint8_t len, uint8_t *out, uint8_t *out_len)
{
	uint8_t name_len = 0, id = osc2_sys_key_id(payload, len, &name_len);

	if (id == OSC2_SYS_KEY_NONE) {
		return -ENOENT;
	}
	if (id == OSC2_SYS_KEY_KEYLOCK) {
		opensc2_put_le32(out, keylock_ram);
		*out_len = keylock_ram ? 4 : 0;
	} else {
		out[0] = 3;
		*out_len = 1;
	}
	return 0;
}

/* ---- the test hooks' environment ---------------------------------------------------------------- */

static void env_status(uint8_t out[OSC2_TEST_SYS_STATUS_LEN])
{
	struct osc2_test_sys_end e;
	bool have = osc2_test_sys_end_unpack(end_at_boot, &e);
	memset(out, 0, OSC2_TEST_SYS_STATUS_LEN);
	out[0] = machine.state;
	out[1] = machine.chosen;
	out[2] = source;
	out[3] = led_mode;
	out[4] = (uint8_t)(led_flags | (shutting_down ? 0x04 : 0));
	out[5] = chords.current;
	out[6] = level_told;
	out[7] = (uint8_t)((keylock_ram == KEYLOCK_VALUE ? 0x01 : 0) | (machine.ended ? 0x80 : 0));
	opensc2_put_le16(&out[8], idle.history);
	opensc2_put_le32(&out[10], idle.period_ms);
	out[14] = last_cue;
	out[15] = machine.boot_state;
	out[16] = machine.boot_cue;
	out[17] = machine.boot_chord;
	out[18] = machine.boot_pairing ? 1 : 0;
	if (have) {
		out[19] = 1;
		out[20] = e.state;
		out[21] = e.cause;
		out[22] = e.how;
		opensc2_put_le32(&out[23], e.uptime_ms);
	}
	out[27] = 3;
	opensc2_put_le16(&out[28], (uint16_t)idle.timeout_s);
}

static void env_changed(void)
{
	emu_sys_process();
}

static int env_boot_store(const uint32_t words[OSC2_TEST_SYS_BOOT_WORDS])
{
	if (words) {
		memcpy(retained, words, sizeof(uint32_t) * OSC2_TEST_SYS_BOOT_WORDS);
	} else {
		memset(retained, 0, sizeof(uint32_t) * OSC2_TEST_SYS_BOOT_WORDS);
	}
	return 0;
}

static void env_event(uint8_t kind)
{
	if (kind == OSC2_TEST_SYS_EV_WIRELESS_OFF) {
		post(OSC2_SYS_EV_WIRELESS_OFF, OPENSC2_REASON_LISTEN_TIMEOUT);
	} else {
		osc2_test_note_host_asleep();
		emu_sys_host_asleep();
	}
	emu_sys_process();
}

static const struct osc2_test_sys_env env = {
	.status = env_status,
	.changed = env_changed,
	.boot_store = env_boot_store,
	.event = env_event,
};

/* ---- what fwemu.c calls ------------------------------------------------------------------------- */

bool emu_sys_boot_wireless(void)
{
	struct osc2_test_sys_boot b;

	return osc2_test_sys_boot_unpack(retained, &b) && (b.flags & OSC2_TEST_SYS_SOURCE) &&
	       b.source != OSC2_SRC_USB;
}

void emu_sys_boot(bool warm)
{
	uint32_t now = (uint32_t)stub.now_ms;
	struct osc2_sys_boot_in b;
	struct osc2_sys_in in;
	struct osc2_sys_out out;
	struct osc2_chord_out co;
	uint32_t words[OSC2_TEST_SYS_BOOT_WORDS];

	/* A fresh boot: RAM state gone, the retained words and the "flash" kept. */
	memset(&machine, 0, sizeof(machine));
	q_len = 0;
	last_cue = 0xFF;
	host_asleep_done = false;
	gone = false;
	led_mode = led_flags = 0;
	shutting_down = false;
	keylock_ram = keylock_flash;
	osc2_test_sys_init();
	osc2_test_sys_env = &env;
	memcpy(words, retained, sizeof(words));
	memset(retained, 0, sizeof(words));   /* a boot-time injection counts once */
	memcpy(end_at_boot, &retained[OSC2_RET_SYS_BOOT_WORDS], sizeof(end_at_boot));
	osc2_test_sys_boot_apply(words, now);
	buttons = rule_buttons();
	osc2_chords_init(&chords, false, buttons, now, &co);
	osc2_idle_start(&idle, osc2.settings[OPENSC2_SETTING_SLEEP_TIMEOUT_S], now);
	source = detect_source();
	level_told = level_of(&osc2.battery);
	b.wired = osc2.mode == OPENSC2_MODE_WIRED;
	b.warm = warm;
	b.source = source;
	inputs(&in);
	osc2_sys_boot(&machine, &in, &b, now, &out);
	booted = true;
	apply(&out);
	if (b.wired) {
		post(OSC2_SYS_EV_CONFIGURED, 0);   /* the emulated host configures the device at once */
	}
}

void emu_sys_battery(const struct opensc2_battery *b)
{
	post(OSC2_SYS_EV_LEVEL, level_of(b));
}

bool emu_sys_power(enum opensc2_power_request request)
{
	struct osc2_sys_out out;

	if (!booted || machine.ended) {
		return false;
	}
	if (request == OPENSC2_POWER_TURN_OFF) {
		run(OSC2_SYS_EV_TURN_OFF, 0, (uint32_t)stub.now_ms, &out);
		return true;
	}
	return false;
}

void emu_sys_link(uint8_t state, uint8_t reason)
{
	static uint8_t last;

	if (state == OPENSC2_LINK_OFF) {
		if (reason == OPENSC2_REASON_LISTEN_TIMEOUT || reason == OPENSC2_REASON_PUCK_TURN_OFF) {
			post(OSC2_SYS_EV_WIRELESS_OFF, reason);
		}
	} else if (last == OPENSC2_LINK_OFF) {
		post(OSC2_SYS_EV_LINK_STARTED, state);
	}
	last = state;
}

void emu_sys_host_asleep(void)
{
	if (!host_asleep_done) {
		host_asleep_done = true;
		post(Q_CUE, OSC2_CUE_HOST_ASLEEP);
	}
}
