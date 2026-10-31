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
	/* A transport chord matches with STEAM or without (two combinations, chord_rules.h). */
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

/* The real controller's combinations, in the order it looks at them on every change of the word.
 * STEAM alone is there twice: the power chord (the timers, no edge) and the stored transport's
 * chord. `bt`: only when Bluetooth exists. */
static const struct {
	uint32_t word;
	uint8_t chord;
	bool edge;
	bool bt;
} TABLE[] = {
	{ LOCK,                     OSC2_CHORD_KEYLOCK, false, false },
	{ OPENSC2_BTN_STEAM,        OSC2_CHORD_STEAM,   false, false },
	{ OPENSC2_BTN_STEAM | B_RB, OSC2_CHORD_BT1,     true,  true },
	{ OPENSC2_BTN_STEAM | B_LB, OSC2_CHORD_BT2,     true,  true },
	{ B_LB,                     OSC2_CHORD_BT2,     true,  true },
	{ B_RB,                     OSC2_CHORD_BT1,     true,  true },
	{ OPENSC2_BTN_STEAM,        OSC2_CHORD_STEAM,   true,  false },
	{ OPENSC2_BTN_STEAM | A_RB, OSC2_CHORD_PUCK1,   true,  false },
	{ OPENSC2_BTN_STEAM | A_LB, OSC2_CHORD_PUCK2,   true,  false },
	{ A_LB,                     OSC2_CHORD_PUCK2,   true,  false },
	{ A_RB,                     OSC2_CHORD_PUCK1,   true,  false },
};

#define ENTRIES (sizeof(TABLE) / sizeof(TABLE[0]))

static void edge(struct osc2_chords *c, bool down, uint8_t chord, struct osc2_chord_out *out)
{
	if (down) {
		c->held |= (uint8_t)(1u << chord);
	} else {
		c->held &= (uint8_t)~(1u << chord);
	}
	if (out->edges < OSC2_CHORD_EDGES_MAX) {
		out->edge[out->edges].down = down;
		out->edge[out->edges].chord = chord;
		out->edges++;
	}
	out->events |= OSC2_CHORD_EV_EDGES;
}

/* Every combination against the word, in the table's order. */
static void scan(struct osc2_chords *c, uint32_t buttons, struct osc2_chord_out *out)
{
	uint32_t w = buttons & ~(uint32_t)OSC2_CHORD_IGNORED;

	for (unsigned int i = 0; i < ENTRIES; i++) {
		uint16_t bit = (uint16_t)(1u << i);
		bool now = (!TABLE[i].bt || c->bluetooth) && w == TABLE[i].word;
		bool was = (c->matched & bit) != 0;

		if (now == was) {
			continue;
		}
		if (now) {
			c->matched |= bit;
		} else {
			c->matched &= (uint16_t)~bit;
		}
		if (TABLE[i].edge) {
			edge(c, now, TABLE[i].chord, out);
		}
	}
}

/* The timers belong to the combination matching now (STEAM alone, the keylock). */
static void timers(struct osc2_chords *c, uint32_t buttons, uint32_t now_ms)
{
	enum osc2_chord m = osc2_chord_match(buttons, c->bluetooth);

	if (m != (enum osc2_chord)c->current) {
		c->current = (uint8_t)m;
		c->fired = 0;
		c->since_ms = now_ms;
	}
}

void osc2_chords_init(struct osc2_chords *c, bool bluetooth, uint32_t buttons, uint32_t now_ms,
		      struct osc2_chord_out *out)
{
	memset(c, 0, sizeof(*c));
	memset(out, 0, sizeof(*out));
	c->bluetooth = bluetooth;
	c->word = buttons;
	scan(c, buttons, out);
	timers(c, buttons, now_ms);
}

uint8_t osc2_chords_boot_chord(const struct osc2_chords *c)
{
	static const uint8_t order[] = { OSC2_CHORD_STEAM, OSC2_CHORD_BT1, OSC2_CHORD_BT2,
					 OSC2_CHORD_PUCK1, OSC2_CHORD_PUCK2 };

	for (unsigned int i = 0; i < sizeof(order); i++) {
		if (c->held & (1u << order[i])) {
			return order[i];
		}
	}
	return OSC2_CHORD_NONE;
}

void osc2_chords_update(struct osc2_chords *c, uint32_t buttons, uint32_t now_ms,
			struct osc2_chord_out *out)
{
	memset(out, 0, sizeof(*out));
	if (buttons == c->word) {
		return;
	}
	if ((c->word & OPENSC2_BTN_STEAM) && !(buttons & OPENSC2_BTN_STEAM)) {
		out->events |= OSC2_CHORD_EV_STEAM_UP;
	}
	c->word = buttons;
	scan(c, buttons, out);
	timers(c, buttons, now_ms);
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
