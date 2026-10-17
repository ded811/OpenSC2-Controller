/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * lizard.c: a simplified lizard mode, written from the documented behaviour of the real one:
 *   keyboard: A = Enter, B and VIEW = Escape, the D-pad = arrow keys, MENU = Tab;
 *   mouse: the right trackpad moves the pointer, the left one scrolls; the right trigger or a
 *          right-pad click is the left button, the left trigger the right button.
 * The real controller adds pointer momentum and haptic ticks; this one does not.
 */
#include <string.h>
#include <opensc2/codec.h>
#include "lizard.h"

#define KEY_ENTER   0x28
#define KEY_ESCAPE  0x29
#define KEY_TAB     0x2B
#define KEY_RIGHT   0x4F
#define KEY_LEFT    0x50
#define KEY_DOWN    0x51
#define KEY_UP      0x52
#define KEY_ROLLOVER 0x01

/* Pad units per pointer step: a full swipe across a pad (65534 units) moves about 1000 pixels. */
#define POINTER_DIVISOR 64
/* Pad units per scroll step. */
#define SCROLL_DIVISOR  2048

static const struct {
	uint32_t button;
	uint8_t key;
} key_map[] = {
	{ OPENSC2_BTN_A, KEY_ENTER },
	{ OPENSC2_BTN_B, KEY_ESCAPE },
	{ OPENSC2_BTN_VIEW, KEY_ESCAPE },
	{ OPENSC2_BTN_MENU, KEY_TAB },
	{ OPENSC2_BTN_DPAD_UP, KEY_UP },
	{ OPENSC2_BTN_DPAD_DOWN, KEY_DOWN },
	{ OPENSC2_BTN_DPAD_LEFT, KEY_LEFT },
	{ OPENSC2_BTN_DPAD_RIGHT, KEY_RIGHT },
};

void opensc2_lizard_reset(struct opensc2_lizard *lz)
{
	memset(lz, 0, sizeof(*lz));
}

static int8_t take_steps(int32_t *carry, int32_t delta, int32_t divisor)
{
	int32_t steps;

	*carry += delta;
	steps = *carry / divisor;
	if (steps > 127) {
		steps = 127;
	} else if (steps < -127) {
		steps = -127;
	}
	*carry -= steps * divisor;
	return (int8_t)steps;
}

uint32_t opensc2_lizard_step(struct opensc2_lizard *lz, const struct opensc2_state *s,
			     uint8_t keyboard[9], uint8_t mouse[6])
{
	uint8_t keys[6] = { 0 };
	int count = 0;
	uint32_t made = 0;
	uint32_t buttons = s->buttons;

	/* Keyboard: the keys held now, without duplicates (B and VIEW are both Escape). */
	for (unsigned int i = 0; i < sizeof(key_map) / sizeof(key_map[0]); i++) {
		bool seen = false;

		if (!(buttons & key_map[i].button)) {
			continue;
		}
		for (int k = 0; k < count && k < 6; k++) {
			seen |= keys[k] == key_map[i].key;
		}
		if (seen) {
			continue;
		}
		if (count < 6) {
			keys[count] = key_map[i].key;
		}
		count++;
	}
	if (count > 6) {
		memset(keys, KEY_ROLLOVER, sizeof(keys));
	}
	if (memcmp(keys, lz->keys, sizeof(keys)) != 0) {
		memcpy(lz->keys, keys, sizeof(keys));
		keyboard[0] = OPENSC2_RPT_KEYBOARD;
		keyboard[1] = 0;   /* no modifiers */
		keyboard[2] = 0;
		memcpy(&keyboard[3], keys, 6);
		made |= OPENSC2_LIZARD_KEYBOARD;
	}

	/* Mouse. */
	uint8_t mbuttons = 0;
	int8_t dx = 0, dy = 0, wheel = 0, pan = 0;

	if (buttons & (OPENSC2_CLICK_RIGHT_TRIGGER | OPENSC2_CLICK_RIGHT_PAD)) {
		mbuttons |= 0x01;
	}
	if (buttons & OPENSC2_CLICK_LEFT_TRIGGER) {
		mbuttons |= 0x02;
	}
	for (int side = 0; side < 2; side++) {
		const struct opensc2_trackpad *pad = &s->pad[side];
		bool touched = pad->touched ||
			       (buttons & (side ? OPENSC2_TOUCH_RIGHT_PAD : OPENSC2_TOUCH_LEFT_PAD));

		if (touched && lz->pad_touched[side]) {
			int32_t ddx = (int32_t)pad->x - lz->pad_x[side];
			int32_t ddy = (int32_t)pad->y - lz->pad_y[side];

			if (side == OPENSC2_RIGHT) {
				dx = take_steps(&lz->carry_x, ddx, POINTER_DIVISOR);
				dy = take_steps(&lz->carry_y, -ddy, POINTER_DIVISOR);   /* pad up = screen up */
			} else {
				wheel = take_steps(&lz->carry_wheel, ddy, SCROLL_DIVISOR);
				pan = take_steps(&lz->carry_pan, ddx, SCROLL_DIVISOR);
			}
		} else if (side == OPENSC2_RIGHT) {
			lz->carry_x = lz->carry_y = 0;
		} else {
			lz->carry_wheel = lz->carry_pan = 0;
		}
		lz->pad_touched[side] = touched;
		lz->pad_x[side] = pad->x;
		lz->pad_y[side] = pad->y;
	}
	if (dx || dy || wheel || pan || mbuttons != lz->mouse_buttons) {
		lz->mouse_buttons = mbuttons;
		mouse[0] = OPENSC2_RPT_MOUSE;
		mouse[1] = mbuttons;
		mouse[2] = (uint8_t)dx;
		mouse[3] = (uint8_t)dy;
		mouse[4] = (uint8_t)wheel;
		mouse[5] = (uint8_t)pan;
		made |= OPENSC2_LIZARD_MOUSE;
	}
	return made;
}
