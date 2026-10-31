/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * lizard.h: "lizard mode", the mouse and keyboard the real controller offers until Steam turns it
 * off (controller setting 9). Pure logic, no kernel.
 *
 * Shaped like the real controller's: a keyboard and a mouse that each turn on and off by events
 * ("on", "off", "a mapped button changed"), which pile up while the call that caused them runs
 * and are then taken up in the order the real one takes them up (opensc2_lizard_setting(),
 * opensc2_lizard_switches()); and one frame per state report (opensc2_lizard_step()), in which the
 * keyboard hears about mapped button changes, the mouse moves, and the pads' haptic ticks and
 * clicks are worked out (made into haptic events with opensc2_lizard_haptics()).
 *
 * Per pad, as the real one: the touch stabilisers, the motion tracker with its 8-frame average,
 * the glide after a flick, and the ticks and clicks. Each can be switched off with the lizard bits
 * of the rule word (input_rules.h); with all four off a frame does what lizard mode did before
 * them (OpenSC2 0.2.0).
 */
#ifndef OPENSC2_LIZARD_H_
#define OPENSC2_LIZARD_H_

#include <opensc2/types.h>
#include <opensc2/settings.h>
#include "input_rules.h"

/* What a call made (its return value). */
#define OPENSC2_LIZARD_KEYBOARD     OPENSC2_BIT(0)   /* `out` holds a keyboard report 0x41 */
#define OPENSC2_LIZARD_MOUSE        OPENSC2_BIT(1)   /* `out` holds a mouse report 0x40 */
#define OPENSC2_LIZARD_TICK_LEFT    OPENSC2_BIT(2)   /* a tick on the left pad (click A) */
#define OPENSC2_LIZARD_TICK_RIGHT   OPENSC2_BIT(3)   /* a tick on the right pad */
#define OPENSC2_LIZARD_CLICK_LEFT   OPENSC2_BIT(4)   /* the left pad's click turned on or off
						      * (click B) */
#define OPENSC2_LIZARD_CLICK_RIGHT  OPENSC2_BIT(5)   /* the right pad's */
#define OPENSC2_LIZARD_HAPTICS      0x3Cu

/* Pad units per mouse count: the right pad moves the pointer, the left one scrolls. */
#define OPENSC2_LIZARD_POINTER_DIVISOR  128
#define OPENSC2_LIZARD_SCROLL_DIVISOR   1024
/* Movements kept per pad (the flick speed is worked out from them) and how many of the newest the
 * motion is the mean of. */
#define OPENSC2_LIZARD_HISTORY          20
#define OPENSC2_LIZARD_AVERAGE          8
/* The flick speed: the mean of the oldest 15 of the last 20 movements (0 until there are 20); a
 * glide needs more travel than this since the touch-down. */
#define OPENSC2_LIZARD_FLICK_MOVES      15
#define OPENSC2_LIZARD_FLICK_TRAVEL     700
/* A step each time the thumb has moved more than this from the last step point; a tick when the
 * steps times this exceed setting 14. */
#define OPENSC2_LIZARD_TICK_STEP        1500
/* The haptic events' levels: ticks play click A at +8 dB, the pad click click B at +6 dB. */
#define OPENSC2_LIZARD_TICK_GAIN_DB     8
#define OPENSC2_LIZARD_CLICK_GAIN_DB    6

/* The reports of one call, in send order: at most two keyboard and two mouse reports (a switch-off
 * that the keyboard takes up together with a pending "on" sends two empty keyboard reports). */
#define OPENSC2_LIZARD_OUT_MAX          6
#define OPENSC2_LIZARD_REPORT_MAX       9

struct opensc2_lizard_out {
	uint8_t n;
	uint8_t len[OPENSC2_LIZARD_OUT_MAX];
	uint8_t report[OPENSC2_LIZARD_OUT_MAX][OPENSC2_LIZARD_REPORT_MAX];
};

/* One pad. */
struct opensc2_lizard_pad {
	/* The mouse's tracker. */
	bool touched;               /* the last frame's touch */
	int32_t x, y;               /* the last frame's position (after the stabiliser) */
	struct osc2_stabiliser stab;
	int32_t hist_x[OPENSC2_LIZARD_HISTORY], hist_y[OPENSC2_LIZARD_HISTORY];
	uint8_t count;              /* movements in the history (up to 20) */
	uint8_t head;               /* once it is full: the oldest one, where the next one goes */
	float travel;               /* the movements' lengths added up since the touch-down */
	float glide;                /* the glide's factor: 1 in the lift-off frame, then less */
	int32_t vx, vy;             /* the flick speed, pad units per frame */
	int32_t rem_x, rem_y;       /* the remainders of the divisions */
	/* The ticks and clicks. */
	bool fb_touched;            /* the touch they saw last */
	int16_t step_x, step_y;     /* the last step point */
	uint32_t steps;
	bool fb_clicked;            /* the click they saw last */
};

struct opensc2_lizard {
	/* The keyboard. */
	bool kb_switch;         /* its switch (0x85 raises it, 0x81 lowers it) */
	bool kb_mode;           /* its own copy of "setting 9 is not 0" */
	bool kb_on;             /* running: it listens to the mapped buttons */
	uint8_t kb_events;      /* events it has not taken up yet */
	uint32_t kb_word;       /* the buttons at the last mapped change it heard of */
	bool kb_held;           /* its last report had a key in it */
	/* The buttons as a button driver sees them: the last frame's, compared with the next. */
	uint32_t buttons;
	bool buttons_sync;      /* take the next frame's buttons as they are, without a change */
	/* The mouse. */
	bool ms_switch;         /* the mouse-button switch */
	bool ms_on;             /* running: a frame per state report */
	uint8_t ms_events;
	bool mouse_owed;        /* a non-zero mouse report went out: one zero report is owed */
	bool fb_sync;           /* take the pads' clicks as they are at the next frame, silently */
	struct opensc2_lizard_pad pad[2];
};

/* At boot: lizard mode on (setting 9 is 1 at every boot), both switches up, the keyboard and the
 * mouse running; nothing sent (the reports of "on" have no link to go to). */
void opensc2_lizard_reset(struct opensc2_lizard *lz);

/* Setting 9 changed to `on` (0x87, 0x81, 0x8E, the Steam watchdog; only on a change): the
 * switches follow it, the keyboard and the mouse are told, and both take it up. Turning on sends
 * the empty keyboard report, then the all-zero mouse report, and starts both trackers and both
 * stabilisers over; turning off sends the same pair, releasing whatever was held. Returns the
 * flags of what `out` holds (0 when nothing started or stopped). */
uint32_t opensc2_lizard_setting(struct opensc2_lizard *lz, bool on, struct opensc2_lizard_out *out);

/* The two switches alone (0x85 raises them, 0x81 lowers them first). Lowering them stops a running
 * keyboard (with its empty report). Raising them while setting 9 is on tells a running keyboard
 * "on", which it only takes up at its next switch-off: then it sends a second empty report and
 * runs on with setting 9 at 0 until the switch-off after that, as the real controller does. */
uint32_t opensc2_lizard_switches(struct opensc2_lizard *lz, bool up,
				 struct opensc2_lizard_out *out);

/* Take the inputs of the next frame as the starting point: the trackers are cleared (pads not
 * touched, no history, no glide, no remainders, no zero report owed) and the next frame adopts the
 * buttons and pad clicks held without a report or a click. What runs and the switches stay. The
 * hardware test hooks use it when arming starts and ends, so that nothing lizard mode made while
 * its reports were held back produces a report later (a glide's momentum included). */
void opensc2_lizard_resync(struct opensc2_lizard *lz);

/* While the keyboard holds a key (its last report had one) or the mouse owes its zero report:
 * the empty keyboard report and the all-zero mouse report that release it (keyboard first); what
 * was held is forgotten. Returns the flags, 0 when there is nothing to release. */
uint32_t opensc2_lizard_release(struct opensc2_lizard *lz, struct opensc2_lizard_out *out);

/* One frame on the state of one report (after the input rules), on a link lizard mode runs on
 * (USB, Bluetooth, the puck; not the dock): the keyboard hears of a change of a mapped button (it
 * sends a report while it runs), the mouse moves while it runs, and the pads' ticks and clicks
 * are worked out while setting 9 is on and setting 70 is exactly 1. `settings` are the 86
 * controller settings: 9, 11, 12, 14, 52, 53, 70, 80 and 82 count. `rules` is the rule word: its
 * OSC2_RULE_LIZARD_* bits count. Returns the OPENSC2_LIZARD_* flags. */
uint32_t opensc2_lizard_step(struct opensc2_lizard *lz, const struct opensc2_state *s,
			     const int16_t settings[OPENSC2_SETTINGS_COUNT], uint32_t rules,
			     struct opensc2_lizard_out *out);

/* The haptic events of a frame's flags, as the haptic callback gets them: report ID 0 (made by the
 * library, not sent by a host), kind OPENSC2_HAPTIC_COMMAND, the pad as target (0 left, 1 right,
 * as 0x82 numbers them) and channel, click A at +8 dB for a tick and click B at +6 dB for the pad
 * click; the left pad's first, a tick before a click. Returns how many (0..4). */
uint8_t opensc2_lizard_haptics(uint32_t made, struct opensc2_haptic out[4]);

#endif /* OPENSC2_LIZARD_H_ */
