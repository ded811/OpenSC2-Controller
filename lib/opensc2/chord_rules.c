/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * chord_rules.c: the button chords (chord_rules.h). Pure logic.
 */
#include <string.h>
#include "chord_rules.h"

#define FIRED_SHORT 0x01
#define FIRED_LONG  0x02

#define B_RB   (OPENSC2_BTN_B | OPENSC2_BTN_R)
#define B_LB   (OPENSC2_BTN_B | OPENSC2_BTN_L)
#define A_RB   (OPENSC2_BTN_A | OPENSC2_BTN_R)
#define A_LB   (OPENSC2_BTN_A | OPENSC2_BTN_L)
#define LOCK   (OPENSC2_BTN_STEAM | OPENSC2_BTN_L4 | OPENSC2_BTN_L5 | OPENSC2_BTN_R4 | \
		OPENSC2_BTN_R5)

enum osc2_chord osc2_chord_match(uint32_t buttons, bool bluetooth)
{
	uint32_t w = buttons & ~(uint32_t)OSC2_CHORD_IGNORED;
	/* A transport chord counts with STEAM or without. */
	uint32_t bare = w & ~(uint32_t)OPENSC2_BTN_STEAM;

	if (w == OPENSC2_BTN_STEAM) {
		return OSC2_CHORD_STEAM;
	}
	if (w == LOCK) {
		return OSC2_CHORD_KEYLOCK;
	}
	if (bluetooth && bare == B_RB) {
		return OSC2_CHORD_BT1;
	}
	if (bluetooth && bare == B_LB) {
		return OSC2_CHORD_BT2;
	}
	if (bare == A_RB) {
		return OSC2_CHORD_PUCK1;
	}
	if (bare == A_LB) {
		return OSC2_CHORD_PUCK2;
	}
	return OSC2_CHORD_NONE;
}

bool osc2_chord_is_transport(enum osc2_chord chord)
{
	return chord >= OSC2_CHORD_STEAM && chord <= OSC2_CHORD_PUCK2;
}

static void begin(struct osc2_chords *c, enum osc2_chord chord, uint32_t now_ms,
		  struct osc2_chord_out *out)
{
	c->current = (uint8_t)chord;
	c->fired = 0;
	c->since_ms = now_ms;
	if (osc2_chord_is_transport(chord)) {
		out->events |= OSC2_CHORD_EV_DOWN;
		out->down = (uint8_t)chord;
	}
}

void osc2_chords_init(struct osc2_chords *c, bool bluetooth, uint32_t buttons, uint32_t now_ms,
		      struct osc2_chord_out *out)
{
	memset(c, 0, sizeof(*c));
	memset(out, 0, sizeof(*out));
	c->bluetooth = bluetooth;
	c->word = buttons;
	begin(c, osc2_chord_match(buttons, bluetooth), now_ms, out);
}

void osc2_chords_update(struct osc2_chords *c, uint32_t buttons, uint32_t now_ms,
			struct osc2_chord_out *out)
{
	enum osc2_chord m;

	memset(out, 0, sizeof(*out));
	if (buttons == c->word) {
		return;
	}
	if ((c->word & OPENSC2_BTN_STEAM) && !(buttons & OPENSC2_BTN_STEAM)) {
		out->events |= OSC2_CHORD_EV_STEAM_UP;
	}
	c->word = buttons;
	m = osc2_chord_match(buttons, c->bluetooth);
	if (m == (enum osc2_chord)c->current) {
		return;   /* another bit changed that the chord does not look at */
	}
	if (osc2_chord_is_transport((enum osc2_chord)c->current)) {
		out->events |= OSC2_CHORD_EV_UP;
		out->up = c->current;
	}
	begin(c, m, now_ms, out);
}

void osc2_chords_poll(struct osc2_chords *c, uint32_t now_ms, struct osc2_chord_out *out)
{
	uint32_t held = now_ms - c->since_ms;
	uint32_t short_ms, long_ms;
	uint8_t short_ev, long_ev;

	memset(out, 0, sizeof(*out));
	if (c->current == OSC2_CHORD_STEAM) {
		short_ms = OSC2_CHORD_STEAM_PRESS_MS;
		long_ms = OSC2_CHORD_STEAM_HOLD_MS;
		short_ev = OSC2_CHORD_EV_STEAM_PRESS;
		long_ev = OSC2_CHORD_EV_STEAM_HOLD;
	} else if (c->current == OSC2_CHORD_KEYLOCK) {
		short_ms = OSC2_CHORD_KEYLOCK_TAP_MS;
		long_ms = OSC2_CHORD_KEYLOCK_HOLD_MS;
		short_ev = OSC2_CHORD_EV_KEYLOCK_TAP;
		long_ev = OSC2_CHORD_EV_KEYLOCK_HOLD;
	} else {
		return;
	}
	if (!(c->fired & FIRED_SHORT) && held >= short_ms) {
		c->fired |= FIRED_SHORT;
		out->events |= short_ev;
	}
	if (!(c->fired & FIRED_LONG) && held >= long_ms) {
		c->fired |= FIRED_LONG;
		out->events |= long_ev;
	}
}

int32_t osc2_chords_next(const struct osc2_chords *c, uint32_t now_ms)
{
	uint32_t held = now_ms - c->since_ms;
	uint32_t at;

	if (c->current == OSC2_CHORD_STEAM) {
		at = (c->fired & FIRED_SHORT) ? OSC2_CHORD_STEAM_HOLD_MS : OSC2_CHORD_STEAM_PRESS_MS;
	} else if (c->current == OSC2_CHORD_KEYLOCK) {
		at = (c->fired & FIRED_SHORT) ? OSC2_CHORD_KEYLOCK_HOLD_MS : OSC2_CHORD_KEYLOCK_TAP_MS;
	} else {
		return -1;
	}
	if (c->fired & FIRED_LONG) {
		return -1;
	}
	return held >= at ? 0 : (int32_t)(at - held);
}
