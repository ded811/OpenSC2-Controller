/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * lizard.h: a simplified "lizard mode", the mouse and keyboard the real controller offers until
 * Steam turns it off (controller setting 9). Pure logic, no kernel: one step per state report.
 */
#ifndef OPENSC2_LIZARD_H_
#define OPENSC2_LIZARD_H_

#include <opensc2/types.h>

#define OPENSC2_LIZARD_KEYBOARD  OPENSC2_BIT(0)   /* `keyboard` holds a new report 0x41 */
#define OPENSC2_LIZARD_MOUSE     OPENSC2_BIT(1)   /* `mouse` holds a new report 0x40 */

struct opensc2_lizard {
	uint8_t keys[6];
	uint8_t mouse_buttons;
	bool pad_touched[2];
	int16_t pad_x[2], pad_y[2];
	int32_t carry_x, carry_y, carry_wheel, carry_pan;
};

void opensc2_lizard_reset(struct opensc2_lizard *lz);

/* Work out the keyboard (9 bytes with ID) and mouse (6 bytes with ID) reports for `s`. Returns
 * OPENSC2_LIZARD_KEYBOARD and/or OPENSC2_LIZARD_MOUSE for each report that should be sent. */
uint32_t opensc2_lizard_step(struct opensc2_lizard *lz, const struct opensc2_state *s,
			     uint8_t keyboard[9], uint8_t mouse[6]);

#endif /* OPENSC2_LIZARD_H_ */
