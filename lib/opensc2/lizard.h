/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * lizard.h: "lizard mode", the mouse and keyboard the real controller offers until Steam turns it
 * off (controller setting 9). Pure logic, no kernel: one step per state report, plus the
 * transitions in and out of the mode.
 *
 * Not reproduced (yet): the touch-down stabiliser, the 8-frame motion average, the glide after a
 * flick and the haptic ticks.
 */
#ifndef OPENSC2_LIZARD_H_
#define OPENSC2_LIZARD_H_

#include <opensc2/types.h>

#define OPENSC2_LIZARD_KEYBOARD  OPENSC2_BIT(0)   /* `keyboard` holds a new report 0x41 */
#define OPENSC2_LIZARD_MOUSE     OPENSC2_BIT(1)   /* `mouse` holds a new report 0x40 */

/* Pad units per mouse count: the right pad moves the pointer, the left one scrolls. */
#define OPENSC2_LIZARD_POINTER_DIVISOR  128
#define OPENSC2_LIZARD_SCROLL_DIVISOR   1024

struct opensc2_lizard {
	bool active;            /* between enter and leave */
	bool keyboard_on;       /* the keyboard switch */
	bool buttons_on;        /* the mouse-button switch */
	bool keyboard_sync;     /* take the mapped buttons as they are at the next step */
	uint32_t mapped;        /* mapped buttons at the last keyboard report */
	bool pad_touched[2];
	int16_t pad_x[2], pad_y[2];
	int32_t rem_x, rem_y, rem_pan, rem_wheel;   /* remainders of the divisions */
	bool mouse_owed;        /* a non-zero mouse report went out: one zero report is owed */
};

/* At boot: lizard mode on, nothing pending (the reports of "on" have no link to go to). */
void opensc2_lizard_reset(struct opensc2_lizard *lz);

/* Lizard mode turns on (setting 9 becomes 1, 0x8E, the Steam watchdog): the motion tracker is
 * cleared and the switches raised. Fills the all-zero mouse report and the empty keyboard report
 * the real controller sends at that moment; returns both flags. */
uint32_t opensc2_lizard_enter(struct opensc2_lizard *lz, uint8_t keyboard[9], uint8_t mouse[6]);

/* Lizard mode turns off (0x81, setting 9 becomes 0): the switches are lowered. Fills the empty
 * keyboard report and the all-zero mouse report that release whatever lizard mode held; send the
 * keyboard one first. Returns both flags, or 0 when lizard mode was already off. */
uint32_t opensc2_lizard_leave(struct opensc2_lizard *lz, uint8_t keyboard[9], uint8_t mouse[6]);

/* Raise or lower the two switches alone (0x85 raises them, 0x81 lowers them). */
void opensc2_lizard_switches(struct opensc2_lizard *lz, bool up);

/* Take the inputs of the next step as the starting point: the motion tracker is cleared (pads not
 * touched, no remainders, no zero report owed) and the next keyboard step adopts the buttons held
 * without a report. `active` and the switches stay. The hardware test hooks use it when arming
 * starts and ends, so that nothing lizard mode made while its reports were held back produces a
 * report later. */
void opensc2_lizard_resync(struct opensc2_lizard *lz);

/* While lizard mode is on and holds something towards the host (its keyboard reports had a key,
 * or a non-zero mouse report went out): fill the empty keyboard report and the all-zero mouse
 * report that release it (send the keyboard one first), forget what was held and return both
 * flags. 0 when there is nothing to release. */
uint32_t opensc2_lizard_release(struct opensc2_lizard *lz, uint8_t keyboard[9], uint8_t mouse[6]);

/* One frame: the keyboard (only when a mapped button changed) and mouse (every frame while any
 * field is non-zero, then one zero report) reports for `s`. `ignore_on_stick` is controller
 * setting 82: bit 0 left, bit 1 right, ignore that pad while its stick is touched. Returns
 * OPENSC2_LIZARD_KEYBOARD and/or OPENSC2_LIZARD_MOUSE for each report to send (0 when lizard mode
 * is off). */
uint32_t opensc2_lizard_step(struct opensc2_lizard *lz, const struct opensc2_state *s,
			     uint8_t ignore_on_stick, uint8_t keyboard[9], uint8_t mouse[6]);

#endif /* OPENSC2_LIZARD_H_ */
