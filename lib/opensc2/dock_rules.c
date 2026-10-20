/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * dock_rules.c: the controller's end of the dock as pure rules (dock_rules.h): the answer to one
 * frame, the pilot check, the supply rule and the docked states. No kernel, no state of its own.
 */
#include <string.h>
#include "dock_rules.h"

/* ---- one frame in, one answer out ------------------------------------------------------------ */

enum osc2_dock_outcome osc2_dock_answer(const uint8_t *req, size_t len,
					uint8_t ans[OPENSC2_DOCK_FRAME_LEN],
					const struct osc2_dock_ops *ops, void *ctx)
{
	uint8_t *payload = &ans[OPENSC2_DOCK_PAYLOAD_OFF];
	int32_t result = 0;
	uint8_t type;

	switch (opensc2_dock_frame_check(req, len)) {
	case OPENSC2_DOCK_FRAME_BAD_SIZE:
		return OSC2_DOCK_BAD_SIZE;
	case OPENSC2_DOCK_FRAME_BAD_CRC:
		return OSC2_DOCK_BAD_CRC;
	default:
		break;
	}
	/* Only the type is read, and for a SET its payload: the start byte and bytes 2 to 7 are
	 * ignored, as on the real controller. */
	type = req[OPENSC2_DOCK_TYPE_OFF];
	memset(ans, 0, OPENSC2_DOCK_FRAME_LEN);
	ans[OPENSC2_DOCK_TYPE_OFF] = type;
	switch (type) {
	case OPENSC2_DOCK_PROBE:
		/* The identity record; the type and result are 0 here (the real controller leaves its
		 * previous answer's, which the puck does not read). */
		ops->identity(ctx, payload);
		break;
	case OPENSC2_DOCK_GET:
		result = ops->get(ctx, payload);
		break;
	case OPENSC2_DOCK_SET:
		result = ops->set(ctx, &req[OPENSC2_DOCK_PAYLOAD_OFF]);
		break;
	case OPENSC2_DOCK_INPUT:
		ops->input(ctx, payload);
		break;
	default:
		/* The puck never sends another type (its types 4 and 5 stay inside it). */
		result = OSC2_DOCK_RESULT_INVALID;
		break;
	}
	opensc2_put_le32(&ans[OPENSC2_DOCK_RESULT_OFF], (uint32_t)result);
	opensc2_dock_frame_seal(ans, OPENSC2_DOCK_START_CONTROLLER);
	return OSC2_DOCK_ANSWERED;
}

/* ---- the pilot ------------------------------------------------------------------------------- */

int32_t osc2_dock_pilot_mv(int32_t count)
{
	if (count < 0) {
		return -1;   /* a failed conversion */
	}
	return (int32_t)(((int64_t)count * 3600) / 4096);
}

bool osc2_dock_pilot_in_window(int32_t mv)
{
	return mv >= OSC2_DOCK_PILOT_MIN_MV && mv <= OSC2_DOCK_PILOT_MAX_MV;
}

bool osc2_dock_pilot_present(const int32_t mv[OSC2_DOCK_PILOT_READS], uint8_t *inside)
{
	uint8_t bits = 0, n = 0;

	for (int i = 0; i < OSC2_DOCK_PILOT_READS; i++) {
		if (osc2_dock_pilot_in_window(mv[i])) {
			bits |= (uint8_t)(1u << i);
			n++;
		}
	}
	if (inside) {
		*inside = bits;
	}
	return n >= 2;
}

/* ---- the supply rule ------------------------------------------------------------------------- */

enum osc2_dock_supply osc2_dock_supply_kind(const struct osc2_dock_power_in *in)
{
	if (in->plus && in->pilot) {
		return OSC2_DOCK_SUPPLY_DOCK;
	}
	return (in->usb || in->plus) ? OSC2_DOCK_SUPPLY_USB : OSC2_DOCK_SUPPLY_NONE;
}

int osc2_dock_power_step(struct osc2_dock_power_rule *rule, const struct osc2_dock_power_in *in)
{
	bool present = in->usb || in->plus;

	if (rule->reported && present == rule->present) {
		return -1;   /* the kind was decided when the supply appeared */
	}
	rule->reported = true;
	rule->present = present;
	rule->kind = present ? osc2_dock_supply_kind(in) : OSC2_DOCK_SUPPLY_NONE;
	return (int)rule->kind;
}

/* ---- the docked states ----------------------------------------------------------------------- */

void osc2_dock_sm_init(struct osc2_dock_sm *sm)
{
	memset(sm, 0, sizeof(*sm));
	sm->state = OSC2_DOCK_AWAY;
}

/* Into `to`, with the actions of leaving the state the machine is in and of entering `to`. */
static void go(struct osc2_dock_sm *sm, enum osc2_dock_state to, uint32_t actions,
	       struct osc2_dock_step *out)
{
	switch (sm->state) {
	case OSC2_DOCK_CHORD:
		/* Leaving a chord: the transport's own pattern, the 4 s timer stopped. */
		actions |= OSC2_DOCK_DO_LED_TRANSPORT | OSC2_DOCK_DO_TIMER_STOP;
		break;
	case OSC2_DOCK_LIFTED:
		actions |= OSC2_DOCK_DO_TIMER_STOP;
		break;
	default:
		break;
	}
	switch (to) {
	case OSC2_DOCK_RADIO_OFF:
		actions |= OSC2_DOCK_DO_LED_CHARGING;
		if (sm->link_selected) {
			actions |= OSC2_DOCK_DO_STOP_LINK;
		}
		break;
	case OSC2_DOCK_LIFTED:
		actions |= OSC2_DOCK_DO_LED_OFF | OSC2_DOCK_DO_TIMER_5S;
		break;
	case OSC2_DOCK_CHORD:
		actions |= OSC2_DOCK_DO_SELECT | OSC2_DOCK_DO_CUE | OSC2_DOCK_DO_LED_COLOUR |
			   OSC2_DOCK_DO_TIMER_4S | OSC2_DOCK_DO_PAUSE;
		break;
	case OSC2_DOCK_LINK_UP:
		actions |= OSC2_DOCK_DO_LED_TRANSPORT;
		break;
	default:
		break;
	}
	/* "Selected since boot" stays true once a transport was chosen or started; stopping the
	 * link again later does no harm. */
	if (actions & (OSC2_DOCK_DO_START_LINK | OSC2_DOCK_DO_SELECT)) {
		sm->link_selected = true;
	}
	out->to = to;
	out->actions = actions;
	out->handled = true;
	sm->state = to;
}

void osc2_dock_sm_enter(struct osc2_dock_sm *sm, enum osc2_dock_entry how, bool warm,
			struct osc2_dock_step *out)
{
	memset(out, 0, sizeof(*out));
	out->from = sm->state;
	out->to = sm->state;
	if (sm->state != OSC2_DOCK_AWAY) {
		return;   /* already in a dock state */
	}
	switch (how) {
	case OSC2_DOCK_AT_POWER_ON:
		/* The keylock goes; after a warm reboot the stored transport starts again (no cue,
		 * no pairing flag), otherwise the controller charges with the radio off. */
		if (warm) {
			go(sm, OSC2_DOCK_LINK_UP, OSC2_DOCK_DO_KEYLOCK_CLEAR | OSC2_DOCK_DO_START_LINK,
			   out);
		} else {
			go(sm, OSC2_DOCK_RADIO_OFF, OSC2_DOCK_DO_KEYLOCK_CLEAR, out);
		}
		break;
	case OSC2_DOCK_IN_BATTERY_WAIT:
		/* The battery wait had made the stored transport the choice: it is stopped again. */
		sm->link_selected = true;
		go(sm, OSC2_DOCK_RADIO_OFF, 0, out);
		break;
	case OSC2_DOCK_ON_BATTERY:
		/* The link stays as it is. */
		sm->link_selected = true;
		go(sm, OSC2_DOCK_LINK_UP, 0, out);
		break;
	default:
		break;
	}
}

void osc2_dock_sm_step(struct osc2_dock_sm *sm, enum osc2_dock_event ev, uint8_t chord,
		       struct osc2_dock_step *out)
{
	memset(out, 0, sizeof(*out));
	out->from = sm->state;
	out->to = sm->state;

	switch (sm->state) {
	case OSC2_DOCK_RADIO_OFF:
		if (ev == OSC2_DOCK_EV_CHORD && chord < OSC2_DOCK_CHORDS) {
			sm->chord = chord;
			go(sm, OSC2_DOCK_CHORD, 0, out);
		} else if (ev == OSC2_DOCK_EV_POWER_GONE) {
			go(sm, OSC2_DOCK_LIFTED, 0, out);
		} else if (ev == OSC2_DOCK_EV_WARM_REBOOT) {
			/* The only reboot that does anything here; the command set the warm mark. */
			go(sm, OSC2_DOCK_AWAY, OSC2_DOCK_DO_REBOOT, out);
		}
		break;
	case OSC2_DOCK_LIFTED:
		if (ev == OSC2_DOCK_EV_POWER_DOCK) {
			go(sm, OSC2_DOCK_RADIO_OFF, 0, out);
		} else if (ev == OSC2_DOCK_EV_POWER_USB) {
			go(sm, OSC2_DOCK_AWAY, OSC2_DOCK_DO_REBOOT, out);
		} else if (ev == OSC2_DOCK_EV_TIMER) {
			go(sm, OSC2_DOCK_AWAY, OSC2_DOCK_DO_SHUTDOWN, out);
		} else if (ev == OSC2_DOCK_EV_CHORD && chord < OSC2_DOCK_CHORDS) {
			/* A transport chord starts the link on battery, through the battery chord
			 * states, which are not this module's. */
			sm->chord = chord;
			go(sm, OSC2_DOCK_AWAY, OSC2_DOCK_DO_TO_CHORD, out);
		}
		break;
	case OSC2_DOCK_CHORD:
		/* Supply changes are ignored here, also a lift-off: the controller then reaches the
		 * link-up state on battery, until the supply next changes. */
		if (ev == OSC2_DOCK_EV_TIMER) {
			go(sm, OSC2_DOCK_LINK_UP, OSC2_DOCK_DO_START_LINK | OSC2_DOCK_DO_PAIRING, out);
		} else if (ev == OSC2_DOCK_EV_CHORD_UP && chord == sm->chord) {
			go(sm, OSC2_DOCK_LINK_UP, OSC2_DOCK_DO_START_LINK, out);
		}
		break;
	case OSC2_DOCK_LINK_UP:
		switch (ev) {
		case OSC2_DOCK_EV_BOOTLOADER:
			out->actions = OSC2_DOCK_DO_UPDATE_WINDOW;
			out->handled = true;
			break;
		case OSC2_DOCK_EV_REBOOT:
		case OSC2_DOCK_EV_WARM_REBOOT:
			go(sm, OSC2_DOCK_AWAY, OSC2_DOCK_DO_REBOOT, out);
			break;
		case OSC2_DOCK_EV_STEAM_7S:
		case OSC2_DOCK_EV_LINK_OFF:
		case OSC2_DOCK_EV_TURN_OFF:
			/* "Turn off" on the dock: the power-off chime and back to charging with the
			 * radio off, never a power-off. */
			go(sm, OSC2_DOCK_RADIO_OFF, OSC2_DOCK_DO_CHIME_OFF, out);
			break;
		case OSC2_DOCK_EV_POWER_GONE:
			go(sm, OSC2_DOCK_AWAY, OSC2_DOCK_DO_TO_BATTERY, out);
			break;
		default:
			break;
		}
		break;
	default:
		break;   /* AWAY: osc2_dock_sm_enter() */
	}
}
