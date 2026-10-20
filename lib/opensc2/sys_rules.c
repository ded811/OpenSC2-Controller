/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * sys_rules.c: the system state machine (sys_rules.h). Pure logic.
 */
#include <string.h>
#include "sys_rules.h"

/* ---- small helpers --------------------------------------------------------------------------- */

static uint8_t usable_transport(uint8_t stored)
{
	return stored <= OSC2_TRANSPORT_PUCK2 ? stored : OSC2_TRANSPORT_PUCK1;
}

uint8_t osc2_sys_chord_transport(uint8_t chord, uint8_t stored)
{
	switch (chord) {
	case OSC2_CHORD_BT1:   return OSC2_TRANSPORT_BT1;
	case OSC2_CHORD_BT2:   return OSC2_TRANSPORT_BT2;
	case OSC2_CHORD_PUCK1: return OSC2_TRANSPORT_PUCK1;
	case OSC2_CHORD_PUCK2: return OSC2_TRANSPORT_PUCK2;
	default:               return usable_transport(stored);
	}
}

uint8_t osc2_sys_transport_cue(uint8_t transport)
{
	return transport <= OSC2_TRANSPORT_PUCK2 ? (uint8_t)(transport + 1) : 0xFF;
}

/* The cue of the transport a chord (or the stored value) chooses: none for a stored value above 3
 * (STEAM alone, or no chord). */
static uint8_t chosen_cue(uint8_t chord, uint8_t stored)
{
	if (!osc2_chord_is_transport((enum osc2_chord)chord) || chord == OSC2_CHORD_STEAM) {
		return osc2_sys_transport_cue(stored);
	}
	return osc2_sys_transport_cue(osc2_sys_chord_transport(chord, stored));
}

static uint8_t transport_led(uint8_t transport)
{
	return transport <= OSC2_TRANSPORT_BT2 ? OSC2_LED_BLUETOOTH : OSC2_LED_PUCK;
}

static void led(struct osc2_sys_out *o, uint8_t mode, uint8_t flags)
{
	o->actions |= OSC2_SYS_DO_LED;
	o->led_mode = mode;
	o->led_flags = flags;
}

static void cue(struct osc2_sys_out *o, uint8_t c)
{
	if (c < OSC2_CUE_COUNT) {
		o->actions |= OSC2_SYS_DO_CUE;
		o->cue = c;
	}
}

static void start_link(struct osc2_sys *m, struct osc2_sys_out *o, bool pairing)
{
	o->actions |= OSC2_SYS_DO_START;
	o->transport = m->chosen;
	o->pairing = pairing;
}

static void timer(struct osc2_sys *m, uint32_t now_ms, uint32_t ms)
{
	m->timer_on = true;
	m->timer_at = now_ms + ms;
}

/* ---- ending ---------------------------------------------------------------------------------- */

static uint8_t end_of(const struct osc2_sys *m, const struct osc2_sys_in *in)
{
	switch (m->state) {
	case OSC2_SYS_SHUTDOWN:
		/* Setting 77 is read here, at the end, and only by this shutdown. */
		return in->reboot_not_off ? OSC2_SYS_END_REBOOT : OSC2_SYS_END_POWER_OFF;
	case OSC2_SYS_REBOOT:
		return OSC2_SYS_END_REBOOT;
	default:
		return OSC2_SYS_END_POWER_OFF;
	}
}

/* Off or reboot once STEAM is up (a shutdown caused by holding STEAM ends only when it is let go,
 * so the press does not wake the controller again); a reboot does not wait. */
static void finish(struct osc2_sys *m, const struct osc2_sys_in *in, struct osc2_sys_out *o)
{
	if (in->steam && m->state != OSC2_SYS_REBOOT) {
		m->steam_wait = true;
		return;
	}
	m->steam_wait = false;
	m->ended = true;
	o->actions |= OSC2_SYS_DO_END;
	o->end = end_of(m, in);
	o->end_state = m->state;
	o->end_cause = m->end_cause;
}

/* ---- entering a state ------------------------------------------------------------------------ */

static void enter(struct osc2_sys *m, const struct osc2_sys_in *in, uint8_t state, uint8_t cause,
		  uint32_t now_ms, struct osc2_sys_out *o)
{
	/* Every exit stops the one state timer. */
	m->timer_on = false;
	if (!(o->actions & OSC2_SYS_DO_STATE)) {
		o->from = m->state;
	}
	o->actions |= OSC2_SYS_DO_STATE;
	o->to = state;
	o->cause = cause;
	m->state = state;

	switch (state) {
	case OSC2_SYS_BATTERY_WAIT:
		/* The stored transport becomes the choice; its cue; 1 s for USB or the dock. */
		m->chosen = usable_transport(in->stored);
		cue(o, osc2_sys_transport_cue(in->stored));
		timer(m, now_ms, OSC2_SYS_BATTERY_WAIT_MS);
		break;
	case OSC2_SYS_WIRED_WAIT:
		/* The power-on chime; the LED is left as it is until 0.5 s without a host. */
		cue(o, OSC2_CUE_POWER_ON);
		timer(m, now_ms, OSC2_SYS_NO_HOST_MS);
		break;
	case OSC2_SYS_WIRED:
		led(o, OSC2_LED_WIRED, 0);
		break;
	case OSC2_SYS_USB_LINK_ON:
		led(o, transport_led(m->chosen), OSC2_LED_EXTERNAL);
		break;
	case OSC2_SYS_USB_LINK_OFF:
		o->actions |= OSC2_SYS_DO_DROP;
		cue(o, OSC2_CUE_POWER_OFF);
		led(o, OSC2_LED_AMBER, 0);
		break;
	case OSC2_SYS_DOCK_OFF:
		led(o, OSC2_LED_AMBER, 0);
		o->actions |= OSC2_SYS_DO_DROP;   /* a link chosen since boot is switched off */
		break;
	case OSC2_SYS_LIFTED:
		led(o, OSC2_LED_OFF, 0);
		timer(m, now_ms, OSC2_SYS_LIFTED_MS);
		break;
	case OSC2_SYS_DOCK_CHORD:
	case OSC2_SYS_CHORD:
		/* The chord's transport becomes the choice: its cue, its colour steady as if
		 * connected, 4 s, and a 500 ms pause before the next event. */
		m->chosen = osc2_sys_chord_transport(m->chord, in->stored);
		cue(o, chosen_cue(m->chord, in->stored));
		led(o, transport_led(m->chosen), OSC2_LED_STEADY);
		timer(m, now_ms, OSC2_SYS_CHORD_HOLD_MS);
		m->busy = true;
		m->busy_until = now_ms + OSC2_SYS_CHORD_PAUSE_MS;
		break;
	case OSC2_SYS_DOCK_ON:
		led(o, transport_led(m->chosen), OSC2_LED_EXTERNAL);
		break;
	case OSC2_SYS_BATTERY:
		led(o, transport_led(m->chosen), 0);
		break;
	case OSC2_SYS_SHUTDOWN:
	case OSC2_SYS_SHUTDOWN_LOW:
	case OSC2_SYS_SHUTDOWN_LOCKED:
		m->end_cause = cause;
		o->actions |= OSC2_SYS_DO_IR_OFF | OSC2_SYS_DO_DROP;
		led(o, state == OSC2_SYS_SHUTDOWN_LOW ? OSC2_LED_LOW_BATTERY :
		       state == OSC2_SYS_SHUTDOWN_LOCKED ? OSC2_LED_KEYLOCK : OSC2_LED_OFF, 0);
		if (state != OSC2_SYS_SHUTDOWN_LOCKED) {
			cue(o, OSC2_CUE_POWER_OFF);
		}
		timer(m, now_ms, OSC2_SYS_SHUTDOWN_MS);
		break;
	case OSC2_SYS_SHUTDOWN_SILENT:
	case OSC2_SYS_REBOOT:
		m->end_cause = cause;
		o->actions |= OSC2_SYS_DO_IR_OFF | OSC2_SYS_DO_DROP;
		led(o, OSC2_LED_OFF, 0);
		finish(m, in, o);
		break;
	default:
		break;
	}
}

/* ---- the power-on decision ------------------------------------------------------------------- */

void osc2_sys_boot(struct osc2_sys *m, const struct osc2_sys_in *in,
		   const struct osc2_sys_boot_in *boot, uint32_t now_ms, struct osc2_sys_out *out)
{
	memset(m, 0, sizeof(*m));
	memset(out, 0, sizeof(*out));
	m->state = OSC2_SYS_START;
	m->chosen = OSC2_TRANSPORT_NONE;
	m->level = in->level;
	m->boot_cue = 0xFF;

	if (boot->wired) {
		/* The wired start. */
		if (in->keylock) {
			out->actions |= OSC2_SYS_DO_KEYLOCK_DEL;
		}
		enter(m, in, OSC2_SYS_WIRED_WAIT, OSC2_SYS_EV_POWER_USB, now_ms, out);
	} else if (boot->source == OSC2_SRC_USB) {
		/* Wireless on USB power: the stored transport's cue, its link started. OpenSC2 boots
		 * wireless on USB power after the warm restart, or when the policy or the application
		 * chose wireless: each starts the same way. */
		if (in->keylock) {
			out->actions |= OSC2_SYS_DO_KEYLOCK_DEL;
		}
		m->chosen = usable_transport(in->stored);
		cue(out, osc2_sys_transport_cue(in->stored));
		start_link(m, out, false);
		enter(m, in, OSC2_SYS_USB_LINK_ON, OSC2_SYS_EV_POWER_USB, now_ms, out);
	} else if (boot->source == OSC2_SRC_DOCK) {
		if (in->keylock) {
			out->actions |= OSC2_SYS_DO_KEYLOCK_DEL;
		}
		if (boot->warm) {
			/* Back on the dock after the warm restart: the stored link, no cue. */
			m->chosen = usable_transport(in->stored);
			start_link(m, out, false);
			enter(m, in, OSC2_SYS_DOCK_ON, OSC2_SYS_EV_POWER_DOCK, now_ms, out);
		} else {
			/* Charging on the dock: the radio stays off. */
			enter(m, in, OSC2_SYS_DOCK_OFF, OSC2_SYS_EV_POWER_DOCK, now_ms, out);
		}
	} else if (in->level == 0) {
		enter(m, in, OSC2_SYS_SHUTDOWN_SILENT, OSC2_SYS_EV_LEVEL, now_ms, out);
	} else if (in->keylock) {
		enter(m, in, OSC2_SYS_SHUTDOWN_LOCKED, OSC2_SYS_EV_KEYLOCK_HELD, now_ms, out);
	} else if (in->level <= 2) {
		enter(m, in, OSC2_SYS_SHUTDOWN_LOW, OSC2_SYS_EV_LEVEL, now_ms, out);
	} else if (osc2_chord_is_transport((enum osc2_chord)in->chord)) {
		m->chord = in->chord;
		m->boot_chord = in->chord;
		enter(m, in, OSC2_SYS_CHORD, OSC2_SYS_EV_CHORD_DOWN, now_ms, out);
	} else {
		enter(m, in, OSC2_SYS_BATTERY_WAIT, OSC2_SYS_EV_POWER_NONE, now_ms, out);
	}
	m->boot_state = m->state;
	if (out->actions & OSC2_SYS_DO_CUE) {
		m->boot_cue = out->cue;
	}
}

/* ---- events ---------------------------------------------------------------------------------- */

static bool is_request(uint8_t ev)
{
	return ev == OSC2_SYS_EV_BOOTLOADER || ev == OSC2_SYS_EV_REBOOT ||
	       ev == OSC2_SYS_EV_WARM_REBOOT;
}

/* A chord state ends: the link starts, with the pairing flag when the 4 s ran out first. */
static void chord_done(struct osc2_sys *m, const struct osc2_sys_in *in, uint8_t ev, bool pairing,
		       uint32_t now_ms, struct osc2_sys_out *o)
{
	if (m->boot_state == OSC2_SYS_CHORD && m->state == OSC2_SYS_CHORD) {
		m->boot_pairing = pairing;
	}
	start_link(m, o, pairing);
	enter(m, in, m->state == OSC2_SYS_DOCK_CHORD ? OSC2_SYS_DOCK_ON : OSC2_SYS_BATTERY, ev, now_ms,
	      o);
}

/* A transport chosen at run time from outside the machine (the application, the shell's `opensc2
 * transport`): the machine's choice from now on, so a state that shows the link's colour shows the
 * new one, wireless on again shows it, and the battery wait's timer starts it. A chord state keeps
 * its chord's choice. */
static void transport_chosen(struct osc2_sys *m, uint8_t transport, struct osc2_sys_out *o)
{
	uint8_t t = usable_transport(transport);

	if (t == m->chosen || m->state == OSC2_SYS_CHORD || m->state == OSC2_SYS_DOCK_CHORD) {
		return;   /* the machine's own start_link() selects what it chose: nothing new */
	}
	m->chosen = t;
	switch (m->state) {
	case OSC2_SYS_USB_LINK_ON:
	case OSC2_SYS_DOCK_ON:
		led(o, transport_led(t), OSC2_LED_EXTERNAL);
		break;
	case OSC2_SYS_BATTERY:
		led(o, transport_led(t), 0);
		break;
	default:
		break;
	}
}

void osc2_sys_event(struct osc2_sys *m, const struct osc2_sys_in *in, uint8_t ev, uint8_t arg,
		    uint32_t now_ms, struct osc2_sys_out *out)
{
	bool timer_due;

	memset(out, 0, sizeof(*out));
	if (m->busy && (int32_t)(now_ms - m->busy_until) >= 0) {
		m->busy = false;   /* the chord state's pause is over */
	}
	if (ev == OSC2_SYS_EV_LEVEL) {
		m->level = arg;
	}
	if (m->ended) {
		return;
	}
	if (m->steam_wait) {
		/* Waiting for STEAM to be up holds the machine: nothing else is handled. */
		if (!in->steam) {
			finish(m, in, out);
		}
		return;
	}
	timer_due = ev == OSC2_SYS_EV_TIMER && m->timer_on &&
		    (int32_t)(now_ms - m->timer_at) >= 0;
	if (ev == OSC2_SYS_EV_TIMER && !timer_due) {
		return;
	}
	if (timer_due) {
		m->timer_on = false;
	}
	if (ev == OSC2_SYS_EV_TRANSPORT) {
		transport_chosen(m, arg, out);
		return;
	}

	switch (m->state) {
	case OSC2_SYS_BATTERY_WAIT:
		/* Chords and STEAM do not count here. */
		if (ev == OSC2_SYS_EV_POWER_USB) {
			enter(m, in, OSC2_SYS_REBOOT, ev, now_ms, out);   /* back wired */
		} else if (ev == OSC2_SYS_EV_POWER_DOCK) {
			enter(m, in, OSC2_SYS_DOCK_OFF, ev, now_ms, out);
		} else if (timer_due) {
			start_link(m, out, false);
			enter(m, in, OSC2_SYS_BATTERY, ev, now_ms, out);
		}
		break;
	case OSC2_SYS_WIRED_WAIT:
		if (ev == OSC2_SYS_EV_CONFIGURED) {
			enter(m, in, OSC2_SYS_WIRED, ev, now_ms, out);
		} else if (timer_due) {
			led(out, OSC2_LED_AMBER, 0);   /* no host after 0.5 s; still waiting */
		} else if (ev == OSC2_SYS_EV_POWER_NONE || ev == OSC2_SYS_EV_UNPLUGGED) {
			enter(m, in, OSC2_SYS_SHUTDOWN, ev, now_ms, out);
		}
		break;
	case OSC2_SYS_WIRED:
		if (ev == OSC2_SYS_EV_POWER_NONE || ev == OSC2_SYS_EV_UNPLUGGED) {
			enter(m, in, OSC2_SYS_SHUTDOWN, ev, now_ms, out);
		} else if (is_request(ev)) {
			out->actions |= OSC2_SYS_DO_PASS;
		}
		break;
	case OSC2_SYS_USB_LINK_ON:
		if (is_request(ev)) {
			out->actions |= OSC2_SYS_DO_PASS;
		} else if (ev == OSC2_SYS_EV_STEAM_HELD || ev == OSC2_SYS_EV_WIRELESS_OFF ||
			   ev == OSC2_SYS_EV_TURN_OFF) {
			enter(m, in, OSC2_SYS_USB_LINK_OFF, ev, now_ms, out);
		} else if (ev == OSC2_SYS_EV_POWER_NONE) {
			enter(m, in, OSC2_SYS_BATTERY, ev, now_ms, out);   /* the link stays up */
		}
		break;
	case OSC2_SYS_USB_LINK_OFF:
		if (ev == OSC2_SYS_EV_STEAM_PRESSED) {
			enter(m, in, OSC2_SYS_REBOOT, ev, now_ms, out);   /* no warm mark: back wired */
		} else if (ev == OSC2_SYS_EV_POWER_NONE) {
			enter(m, in, OSC2_SYS_SHUTDOWN_SILENT, ev, now_ms, out);
		} else if (ev == OSC2_SYS_EV_LINK_STARTED) {
			enter(m, in, OSC2_SYS_USB_LINK_ON, ev, now_ms, out);
		}
		break;
	case OSC2_SYS_DOCK_OFF:
		/* The plain reboot, the turn-off and the bootloader request do nothing here. */
		if (ev == OSC2_SYS_EV_CHORD_DOWN && osc2_chord_is_transport((enum osc2_chord)arg)) {
			m->chord = arg;
			enter(m, in, OSC2_SYS_DOCK_CHORD, ev, now_ms, out);
		} else if (ev == OSC2_SYS_EV_POWER_NONE) {
			enter(m, in, OSC2_SYS_LIFTED, ev, now_ms, out);
		} else if (ev == OSC2_SYS_EV_WARM_REBOOT) {
			out->actions |= OSC2_SYS_DO_PASS;
		}
		break;
	case OSC2_SYS_LIFTED:
		if (ev == OSC2_SYS_EV_POWER_DOCK) {
			enter(m, in, OSC2_SYS_DOCK_OFF, ev, now_ms, out);
		} else if (ev == OSC2_SYS_EV_POWER_USB) {
			enter(m, in, OSC2_SYS_REBOOT, ev, now_ms, out);
		} else if (timer_due) {
			enter(m, in, OSC2_SYS_SHUTDOWN_SILENT, ev, now_ms, out);
		} else if (ev == OSC2_SYS_EV_CHORD_DOWN &&
			   osc2_chord_is_transport((enum osc2_chord)arg)) {
			m->chord = arg;
			enter(m, in, OSC2_SYS_CHORD, ev, now_ms, out);
		}
		break;
	case OSC2_SYS_DOCK_CHORD:
	case OSC2_SYS_CHORD:
		/* Only the timer and the chord's own release count here; power changes are lost. */
		if (timer_due) {
			chord_done(m, in, ev, true, now_ms, out);
		} else if (ev == OSC2_SYS_EV_CHORD_UP && arg == m->chord) {
			chord_done(m, in, ev, false, now_ms, out);
		}
		break;
	case OSC2_SYS_DOCK_ON:
		if (is_request(ev)) {
			out->actions |= OSC2_SYS_DO_PASS;
		} else if (ev == OSC2_SYS_EV_STEAM_HELD || ev == OSC2_SYS_EV_WIRELESS_OFF ||
			   ev == OSC2_SYS_EV_TURN_OFF) {
			cue(out, OSC2_CUE_POWER_OFF);
			enter(m, in, OSC2_SYS_DOCK_OFF, ev, now_ms, out);
		} else if (ev == OSC2_SYS_EV_POWER_NONE) {
			enter(m, in, OSC2_SYS_BATTERY, ev, now_ms, out);   /* the link stays up */
		}
		break;
	case OSC2_SYS_BATTERY:
		if (is_request(ev)) {
			out->actions |= OSC2_SYS_DO_PASS;
		} else if (ev == OSC2_SYS_EV_POWER_USB) {
			/* Plugged in: with STEAM held or no link, a reboot that comes back wired; with
			 * the link up, charging with the link kept. */
			if (in->plug_in_reboot && (in->steam || !in->link_up)) {
				enter(m, in, OSC2_SYS_REBOOT, ev, now_ms, out);
			} else {
				enter(m, in, OSC2_SYS_USB_LINK_ON, ev, now_ms, out);
			}
		} else if (ev == OSC2_SYS_EV_POWER_DOCK) {
			enter(m, in, OSC2_SYS_DOCK_ON, ev, now_ms, out);
		} else if (ev == OSC2_SYS_EV_STEAM_HELD || ev == OSC2_SYS_EV_IDLE ||
			   ev == OSC2_SYS_EV_WIRELESS_OFF || ev == OSC2_SYS_EV_TURN_OFF) {
			enter(m, in, OSC2_SYS_SHUTDOWN, ev, now_ms, out);
		} else if (ev == OSC2_SYS_EV_LEVEL && arg == 0) {
			enter(m, in, OSC2_SYS_SHUTDOWN_LOW, ev, now_ms, out);
		} else if (ev == OSC2_SYS_EV_KEYLOCK_HELD) {
			out->actions |= OSC2_SYS_DO_KEYLOCK_SET;
			enter(m, in, OSC2_SYS_SHUTDOWN, ev, now_ms, out);
		}
		break;
	case OSC2_SYS_SHUTDOWN:
	case OSC2_SYS_SHUTDOWN_LOW:
	case OSC2_SYS_SHUTDOWN_LOCKED:
		if (timer_due) {
			finish(m, in, out);
		}
		break;
	default:
		break;
	}
}

uint32_t osc2_sys_wait(const struct osc2_sys *m, uint32_t now_ms)
{
	int32_t left;

	if (!m->busy) {
		return 0;
	}
	left = (int32_t)(m->busy_until - now_ms);
	/* A pause is never longer than 500 ms: anything else is one long over, on a clock that has
	 * wrapped since (about 25 days later). */
	return left > 0 && left <= OSC2_SYS_CHORD_PAUSE_MS ? (uint32_t)left : 0;
}

int32_t osc2_sys_next(const struct osc2_sys *m, uint32_t now_ms)
{
	int32_t left;

	if (m->ended) {
		return -1;
	}
	if (m->steam_wait) {
		return OSC2_SYS_STEAM_POLL_MS;
	}
	if (!m->timer_on) {
		return -1;
	}
	left = (int32_t)(m->timer_at - now_ms);
	return left < 0 ? 0 : left;
}

int osc2_sys_source_event(uint8_t before, uint8_t after)
{
	bool was = before != OSC2_SRC_BATTERY, is = after != OSC2_SRC_BATTERY;

	if (was == is) {
		return -1;
	}
	if (!is) {
		return OSC2_SYS_EV_POWER_NONE;
	}
	return after == OSC2_SRC_DOCK ? OSC2_SYS_EV_POWER_DOCK : OSC2_SYS_EV_POWER_USB;
}

/* ---- cues and levels ------------------------------------------------------------------------- */

bool osc2_cue_map(uint8_t c, uint8_t level, uint8_t *script, int8_t *gain_db)
{
	static const uint8_t scripts[OSC2_CUE_COUNT] = { 1, 1, 2, 1, 2, 5, 8, 9, 10, 3, 4 };
	int8_t gain;

	if (c >= OSC2_CUE_COUNT) {
		return false;
	}
	*script = scripts[c];
	if (c == 8) {
		*gain_db = 2;   /* the factory charge window's sweep, whatever the level */
		return true;
	}
	if (level == 1) {
		gain = -18;
	} else if (level == 2) {
		gain = -12;
	} else {
		return false;   /* silent */
	}
	if (c == OSC2_CUE_BT_PAIRING || c == OSC2_CUE_BT_PAIRED) {
		gain = (int8_t)(gain - 1);
	}
	*gain_db = gain;
	return true;
}

uint8_t osc2_sys_level(uint8_t charge_state, uint8_t level)
{
	return (charge_state & 0x80) && level > 0 ? 100 : level;
}

uint8_t opensc2_charge_state_of(bool input_power, bool charging)
{
	if (!input_power) {
		return OPENSC2_CHARGE_DISCHARGING;
	}
	return charging ? OPENSC2_CHARGE_CHARGING : OPENSC2_CHARGE_DONE;
}

/* ---- keys ------------------------------------------------------------------------------------ */

/* ---- names ----------------------------------------------------------------------------------- */

const char *osc2_sys_state_name(uint8_t state)
{
	static const char *const names[OSC2_SYS_STATES] = {
		"start", "battery-wait", "wired-wait", "wired", "usb-link-on", "usb-link-off",
		"dock-off", "lifted", "dock-chord", "dock-on", "chord", "battery", "shutdown",
		"shutdown-silent", "shutdown-low", "shutdown-locked", "reboot",
	};

	return state < OSC2_SYS_STATES ? names[state] : "?";
}

const char *osc2_sys_event_name(uint8_t ev)
{
	static const char *const names[OSC2_SYS_EVENTS] = {
		"power-none", "power-usb", "power-dock", "configured", "steam-held",
		"steam-pressed", "idle", "wireless-off", "level", "keylock-held", "chord-down",
		"chord-up", "timer", "bootloader", "reboot", "warm-reboot", "turn-off", "steam-up",
		"link-started", "unplugged", "transport",
	};

	return ev < OSC2_SYS_EVENTS ? names[ev] : "?";
}
