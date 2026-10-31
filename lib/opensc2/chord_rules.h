/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * chord_rules.h: the button chords of the real controller, as pure logic (the host tests build
 * it). A chord matches when the button word, with the stick-touch, trigger-digital and grip bits
 * masked out, equals its combination exactly, and releases when it stops matching; the trackpad
 * touch and click bits stay in, so a thumb resting on a pad spoils a chord. The combinations and
 * timings are the protocol documentation's (not public yet; docs/protocol.md lists the pages):
 *   STEAM alone                      the stored transport, and the power chord: "pressed" once it
 *                                    has matched 20 ms, "held" at 7 s
 *   B + right bumper, with STEAM and without   Bluetooth slot 1   (only when Bluetooth exists)
 *   B + left bumper, with STEAM and without    Bluetooth slot 2   (only when Bluetooth exists)
 *   A + right bumper, with STEAM and without   the puck, bond 1
 *   A + left bumper, with STEAM and without    the puck, bond 2
 *   STEAM + L4 + L5 + R4 + R5          the keylock: a 5 ms tap deletes it, 3 s sets it
 * As on the real controller, each transport chord is two separate combinations (with STEAM and
 * without) that report the same chord, looked at one after the other in a fixed order on every
 * change of the word: adding or lifting STEAM in the middle of a chord ends one combination and
 * begins the other, so the chord is reported released and pressed again (in the order of the
 * table in chord_rules.c, which decides which edge comes first). Every edge is reported, in that
 * order; what the chords do is the system machine's (sys_rules.h).
 */
#ifndef OPENSC2_CHORD_RULES_H_
#define OPENSC2_CHORD_RULES_H_

#include <stdbool.h>
#include <stdint.h>
#include <opensc2/types.h>

/* The bits a chord does not look at. */
#define OSC2_CHORD_IGNORED (OPENSC2_TOUCH_RIGHT_STICK | OPENSC2_TOUCH_LEFT_STICK |                \
			    OPENSC2_CLICK_RIGHT_TRIGGER | OPENSC2_CLICK_LEFT_TRIGGER |              \
			    OPENSC2_TOUCH_RIGHT_GRIP | OPENSC2_TOUCH_LEFT_GRIP)

#define OSC2_CHORD_STEAM_PRESS_MS   20
#define OSC2_CHORD_STEAM_HOLD_MS    7000
#define OSC2_CHORD_KEYLOCK_TAP_MS   5
#define OSC2_CHORD_KEYLOCK_HOLD_MS  3000

enum osc2_chord {
	OSC2_CHORD_NONE = 0,
	OSC2_CHORD_STEAM,       /* STEAM alone */
	OSC2_CHORD_BT1,         /* B + right bumper */
	OSC2_CHORD_BT2,         /* B + left bumper */
	OSC2_CHORD_PUCK1,       /* A + right bumper */
	OSC2_CHORD_PUCK2,       /* A + left bumper */
	OSC2_CHORD_KEYLOCK,     /* STEAM + L4 + L5 + R4 + R5 */
};

/* What the matcher reports, as bits of osc2_chord_out.events. */
#define OSC2_CHORD_EV_EDGES         0x01   /* transport chords pressed or released: `edge` */
#define OSC2_CHORD_EV_STEAM_PRESS   0x04   /* STEAM alone matched 20 ms */
#define OSC2_CHORD_EV_STEAM_HOLD    0x08   /* STEAM alone matched 7 s */
#define OSC2_CHORD_EV_KEYLOCK_TAP   0x10   /* the keylock chord matched 5 ms: delete the keylock */
#define OSC2_CHORD_EV_KEYLOCK_HOLD  0x20   /* the keylock chord matched 3 s */
#define OSC2_CHORD_EV_STEAM_UP      0x40   /* the STEAM bit went up (whatever else is held) */

/* The most edges one change of the word can give (every combination of the table). */
#define OSC2_CHORD_EDGES_MAX 12

struct osc2_chord_edge {
	bool down;              /* pressed (true) or released */
	uint8_t chord;          /* enum osc2_chord, a transport chord */
};

struct osc2_chord_out {
	uint8_t events;
	uint8_t edges;          /* how many of `edge`, in the order they happened */
	struct osc2_chord_edge edge[OSC2_CHORD_EDGES_MAX];
};

struct osc2_chords {
	uint32_t word;          /* the last button word, as fed */
	uint16_t matched;       /* the combinations of the table that match now (bit per entry) */
	uint8_t held;           /* per transport chord (bit `chord`): pressed and not released since;
				 * what a power-on decision looks at */
	uint8_t current;        /* enum osc2_chord matching now (osc2_chord_match()) */
	uint8_t fired;          /* the timers of `current` that have fired (bit 0 the short, 1 the long) */
	uint32_t since_ms;      /* when `current` began to match */
	bool bluetooth;         /* the Bluetooth chords exist */
};

/* The chord a button word matches (OSC2_CHORD_NONE for none). */
enum osc2_chord osc2_chord_match(uint32_t buttons, bool bluetooth);

/* Is it a transport chord (STEAM alone, or one of the four)? */
bool osc2_chord_is_transport(enum osc2_chord chord);

/* At boot: the buttons as they are, so a chord already held matches at once and STEAM alone
 * starts its timers from this moment. `out` gets the presses of such a chord. */
void osc2_chords_init(struct osc2_chords *c, bool bluetooth, uint32_t buttons, uint32_t now_ms,
		      struct osc2_chord_out *out);

/* The transport chord a power-on decision takes: the first held one in the real controller's
 * order (STEAM alone, Bluetooth slot 1, slot 2, the puck's bond 1, bond 2), else OSC2_CHORD_NONE.
 * "Held" follows the edges, so a chord whose combination with STEAM was pressed after the one
 * without (A + RB, then STEAM) counts as released, as on the real controller. */
uint8_t osc2_chords_boot_chord(const struct osc2_chords *c);

/* A new button word at `now_ms` (any change; the same word again does nothing). */
void osc2_chords_update(struct osc2_chords *c, uint32_t buttons, uint32_t now_ms,
			struct osc2_chord_out *out);

/* The timers of the chord that matches: call at the deadline osc2_chords_next() gives. */
void osc2_chords_poll(struct osc2_chords *c, uint32_t now_ms, struct osc2_chord_out *out);

/* Milliseconds from `now_ms` to the next timer (0: due), or -1 when none runs. */
int32_t osc2_chords_next(const struct osc2_chords *c, uint32_t now_ms);

#endif /* OPENSC2_CHORD_RULES_H_ */
