/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * lizard.h: "lizard mode", the mouse and keyboard the real controller offers until Steam turns it
 * off (controller setting 9). Pure logic, no kernel: one step per state report, plus the
 * transitions in and out of the mode.
 *
 * Per pad, as the real one: the touch stabilisers, the motion tracker with its 8-frame average,
 * the glide after a flick, and the haptic ticks and clicks, which the step returns as flags (made
 * into haptic events with opensc2_lizard_haptics()). Each can be switched off with the lizard bits
 * of the rule word (input_rules.h); with all four off a step does what lizard mode did before
 * them (OpenSC2 0.2.0).
 */
#ifndef OPENSC2_LIZARD_H_
#define OPENSC2_LIZARD_H_

#include <opensc2/types.h>
#include <opensc2/settings.h>
#include "input_rules.h"

#define OPENSC2_LIZARD_KEYBOARD     OPENSC2_BIT(0)   /* `keyboard` holds a new report 0x41 */
#define OPENSC2_LIZARD_MOUSE        OPENSC2_BIT(1)   /* `mouse` holds a new report 0x40 */
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
/* The flick speed: the mean of the oldest 15 of the last 20 movements; a glide needs more travel
 * than this since the touch-down. */
#define OPENSC2_LIZARD_FLICK_MOVES      15
#define OPENSC2_LIZARD_FLICK_TRAVEL     700
/* A step each time the thumb has moved more than this from the last step point; a tick when the
 * steps times this exceed setting 14. */
#define OPENSC2_LIZARD_TICK_STEP        1500
/* The haptic events' levels: ticks play click A at +8 dB, the pad click click B at +6 dB. */
#define OPENSC2_LIZARD_TICK_GAIN_DB     8
#define OPENSC2_LIZARD_CLICK_GAIN_DB    6

/* One pad's tracker. */
struct opensc2_lizard_pad {
	bool touched;               /* the last frame's touch */
	int32_t x, y;               /* the last frame's position (after the stabiliser) */
	struct osc2_stabiliser stab;
	int32_t hist_x[OPENSC2_LIZARD_HISTORY], hist_y[OPENSC2_LIZARD_HISTORY];
	uint8_t hist_head;          /* where the next movement goes */
	uint32_t moves;             /* movements since the touch-down */
	uint32_t travel;            /* their lengths added up */
	bool gliding;
	int32_t vx, vy;             /* the flick speed, pad units per frame */
	uint16_t glide_frame, glide_frames;
	bool ticking;               /* counting steps in this touch */
	int32_t step_x, step_y;     /* the last step point */
	uint16_t steps;
	bool click;                 /* the pad's click at the last frame */
};

struct opensc2_lizard {
	bool active;            /* between enter and leave */
	bool keyboard_on;       /* the keyboard switch */
	bool buttons_on;        /* the mouse-button switch */
	bool keyboard_sync;     /* take the mapped buttons as they are at the next step */
	bool click_sync;        /* take the pads' clicks as they are at the next step */
	uint32_t mapped;        /* mapped buttons at the last keyboard report */
	struct opensc2_lizard_pad pad[2];
	int32_t rem_x, rem_y, rem_pan, rem_wheel;   /* remainders of the divisions */
	bool mouse_owed;        /* a non-zero mouse report went out: one zero report is owed */
};

/* At boot: lizard mode on, nothing pending (the reports of "on" have no link to go to). */
void opensc2_lizard_reset(struct opensc2_lizard *lz);

/* Lizard mode turns on (setting 9 becomes 1, 0x8E, the Steam watchdog): both trackers and
 * stabilisers start over and the switches are raised. Fills the all-zero mouse report and the
 * empty keyboard report the real controller sends at that moment; returns both flags. */
uint32_t opensc2_lizard_enter(struct opensc2_lizard *lz, uint8_t keyboard[9], uint8_t mouse[6]);

/* Lizard mode turns off (0x81, setting 9 becomes 0): the switches are lowered, a glide ends and
 * the ticks stop. Fills the empty keyboard report and the all-zero mouse report that release
 * whatever lizard mode held; send the keyboard one first. Returns both flags, or 0 when lizard
 * mode was already off. */
uint32_t opensc2_lizard_leave(struct opensc2_lizard *lz, uint8_t keyboard[9], uint8_t mouse[6]);

/* Raise or lower the two switches alone (0x85 raises them, 0x81 lowers them). */
void opensc2_lizard_switches(struct opensc2_lizard *lz, bool up);

/* Take the inputs of the next step as the starting point: the trackers are cleared (pads not
 * touched, no history, no glide, no remainders, no zero report owed) and the next step adopts the
 * buttons and pad clicks held without a report or a click. `active` and the switches stay. The
 * hardware test hooks use it when arming starts and ends, so that nothing lizard mode made while
 * its reports were held back produces a report later (a glide's momentum included). */
void opensc2_lizard_resync(struct opensc2_lizard *lz);

/* While lizard mode is on and holds something towards the host (its keyboard reports had a key,
 * or a non-zero mouse report went out): fill the empty keyboard report and the all-zero mouse
 * report that release it (send the keyboard one first), forget what was held and return both
 * flags. 0 when there is nothing to release. */
uint32_t opensc2_lizard_release(struct opensc2_lizard *lz, uint8_t keyboard[9], uint8_t mouse[6]);

/* One frame on the state of one report (after the input rules): the keyboard report (only when a
 * mapped button changed), the mouse report (every frame while any field is non-zero, then one
 * zero report) and the pads' haptic flags. `settings` are the 86 controller settings; lizard mode
 * reads 11, 12, 14, 52, 53, 70, 80 and 82 (not 9: the caller steps only while lizard mode is on).
 * `rules` is the rule word: its OSC2_RULE_LIZARD_* bits count. Returns the OPENSC2_LIZARD_* flags
 * (0 when lizard mode is off). */
uint32_t opensc2_lizard_step(struct opensc2_lizard *lz, const struct opensc2_state *s,
			     const int16_t settings[OPENSC2_SETTINGS_COUNT], uint32_t rules,
			     uint8_t keyboard[9], uint8_t mouse[6]);

/* The haptic events of a step's flags, as the haptic callback gets them: report ID 0 (made by the
 * library, not sent by a host), kind OPENSC2_HAPTIC_COMMAND, the pad as target (0 left, 1 right,
 * as 0x82 numbers them) and channel, click A at +8 dB for a tick and click B at +6 dB for the pad
 * click; the left pad's first, a tick before a click. Returns how many (0..4). */
uint8_t opensc2_lizard_haptics(uint32_t made, struct opensc2_haptic out[4]);

#endif /* OPENSC2_LIZARD_H_ */
