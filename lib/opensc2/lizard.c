/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * lizard.c: lizard mode, written from the behaviour of the real one: the key map and the mouse
 * mapping as the published documentation gives them (docs/protocol.md lists the pages), the
 * key-slot rule, the scales and when reports go out from the same authors' study of the
 * controller's firmware (not yet on the published pages):
 *   keyboard: A = Enter, B and VIEW = Escape, the D-pad = arrow keys, MENU = Tab. It reacts when
 *             one of those eight buttons changes; then every held button, in bit order, takes the
 *             next key slot (its key, or 0 when it has none), and more than six held buttons fill
 *             all six slots with the roll-over code.
 *   mouse:    the right trackpad moves the pointer (128 pad units per count), the left one scrolls
 *             (1024 per step; moving up scrolls with a negative wheel value, right gives positive
 *             pan); the right trigger or a right-pad click is the left button, the left trigger the
 *             right one. A report goes out every frame while any field is non-zero, then one zero
 *             report. The division remainders are kept until lizard mode is entered again.
 * The real controller also smooths the motion over 8 frames, stabilises a touch-down, keeps
 * gliding after a flick and plays haptic ticks; this one does not (yet).
 */
#include <string.h>
#include <opensc2/codec.h>
#include "lizard.h"

#define KEY_ENTER    0x28
#define KEY_ESCAPE   0x29
#define KEY_TAB      0x2B
#define KEY_RIGHT    0x4F
#define KEY_LEFT     0x50
#define KEY_DOWN     0x51
#define KEY_UP       0x52
#define KEY_ROLLOVER 0x01

static const struct {
	uint32_t button;
	uint8_t key;
} key_map[] = {
	{ OPENSC2_BTN_A, KEY_ENTER },
	{ OPENSC2_BTN_B, KEY_ESCAPE },
	{ OPENSC2_BTN_VIEW, KEY_ESCAPE },
	{ OPENSC2_BTN_DPAD_DOWN, KEY_DOWN },
	{ OPENSC2_BTN_DPAD_RIGHT, KEY_RIGHT },
	{ OPENSC2_BTN_DPAD_LEFT, KEY_LEFT },
	{ OPENSC2_BTN_DPAD_UP, KEY_UP },
	{ OPENSC2_BTN_MENU, KEY_TAB },
};

#define MAPPED_BUTTONS                                                                      \
	(OPENSC2_BTN_A | OPENSC2_BTN_B | OPENSC2_BTN_VIEW | OPENSC2_BTN_DPAD_DOWN |           \
	 OPENSC2_BTN_DPAD_RIGHT | OPENSC2_BTN_DPAD_LEFT | OPENSC2_BTN_DPAD_UP | OPENSC2_BTN_MENU)

static uint8_t key_of(uint32_t button)
{
	for (unsigned int i = 0; i < sizeof(key_map) / sizeof(key_map[0]); i++) {
		if (key_map[i].button == button) {
			return key_map[i].key;
		}
	}
	return 0;
}

static void keyboard_report(uint8_t keyboard[9], const uint8_t keys[6])
{
	keyboard[0] = OPENSC2_RPT_KEYBOARD;
	keyboard[1] = 0;   /* no modifiers */
	keyboard[2] = 0;   /* reserved */
	memcpy(&keyboard[3], keys, 6);
}

static void mouse_report(uint8_t mouse[6], uint8_t buttons, int32_t x, int32_t y, int32_t wheel,
			 int32_t pan)
{
	/* No clamping: the low eight bits of each value, as the real controller sends them. */
	mouse[0] = OPENSC2_RPT_MOUSE;
	mouse[1] = buttons;
	mouse[2] = (uint8_t)x;
	mouse[3] = (uint8_t)y;
	mouse[4] = (uint8_t)wheel;
	mouse[5] = (uint8_t)pan;
}

static void clear_tracker(struct opensc2_lizard *lz)
{
	lz->pad_touched[0] = lz->pad_touched[1] = false;
	lz->pad_x[0] = lz->pad_x[1] = 0;
	lz->pad_y[0] = lz->pad_y[1] = 0;
	lz->rem_x = lz->rem_y = lz->rem_pan = lz->rem_wheel = 0;
	lz->mouse_owed = false;
}

void opensc2_lizard_reset(struct opensc2_lizard *lz)
{
	memset(lz, 0, sizeof(*lz));
	lz->active = true;
	lz->keyboard_on = lz->buttons_on = true;
	lz->keyboard_sync = true;
}

uint32_t opensc2_lizard_enter(struct opensc2_lizard *lz, uint8_t keyboard[9], uint8_t mouse[6])
{
	static const uint8_t none[6];

	clear_tracker(lz);
	lz->active = true;
	lz->keyboard_on = lz->buttons_on = true;
	lz->keyboard_sync = true;   /* keys already held are reported at the next mapped change */
	keyboard_report(keyboard, none);
	mouse_report(mouse, 0, 0, 0, 0, 0);
	return OPENSC2_LIZARD_KEYBOARD | OPENSC2_LIZARD_MOUSE;
}

uint32_t opensc2_lizard_leave(struct opensc2_lizard *lz, uint8_t keyboard[9], uint8_t mouse[6])
{
	static const uint8_t none[6];
	bool was_active = lz->active;

	lz->active = false;
	lz->keyboard_on = lz->buttons_on = false;
	lz->mouse_owed = false;
	if (!was_active) {
		return 0;
	}
	keyboard_report(keyboard, none);
	mouse_report(mouse, 0, 0, 0, 0, 0);
	return OPENSC2_LIZARD_KEYBOARD | OPENSC2_LIZARD_MOUSE;
}

void opensc2_lizard_switches(struct opensc2_lizard *lz, bool up)
{
	if (up && !lz->keyboard_on) {
		lz->keyboard_sync = true;
	}
	lz->keyboard_on = lz->buttons_on = up;
}

void opensc2_lizard_resync(struct opensc2_lizard *lz)
{
	clear_tracker(lz);
	lz->keyboard_sync = true;
}

uint32_t opensc2_lizard_release(struct opensc2_lizard *lz, uint8_t keyboard[9], uint8_t mouse[6])
{
	static const uint8_t none[6];

	if (!lz->active || (lz->mapped == 0 && !lz->mouse_owed)) {
		return 0;
	}
	lz->mapped = 0;
	lz->mouse_owed = false;
	keyboard_report(keyboard, none);
	mouse_report(mouse, 0, 0, 0, 0, 0);
	return OPENSC2_LIZARD_KEYBOARD | OPENSC2_LIZARD_MOUSE;
}

/* (*rem + delta) / divisor, truncated toward zero; the rest stays in *rem. */
static int32_t divide(int32_t *rem, int32_t delta, int32_t divisor)
{
	int32_t total = *rem + delta;
	int32_t q = total / divisor;

	*rem = total - q * divisor;
	return q;
}

uint32_t opensc2_lizard_step(struct opensc2_lizard *lz, const struct opensc2_state *s,
			     uint8_t ignore_on_stick, uint8_t keyboard[9], uint8_t mouse[6])
{
	static const uint32_t touch_bit[2] = { OPENSC2_TOUCH_LEFT_PAD, OPENSC2_TOUCH_RIGHT_PAD };
	static const uint32_t stick_bit[2] = { OPENSC2_TOUCH_LEFT_STICK, OPENSC2_TOUCH_RIGHT_STICK };
	uint32_t buttons = s->buttons;
	uint32_t made = 0;
	int32_t dx[2] = { 0, 0 }, dy[2] = { 0, 0 };
	bool blanked[2];
	int32_t x, y, wheel, pan;
	uint8_t mbuttons = 0;

	if (!lz->active) {
		return 0;
	}

	/* Keyboard: only when one of the eight mapped buttons changed. */
	if (lz->keyboard_on) {
		uint32_t mapped = buttons & MAPPED_BUTTONS;

		if (lz->keyboard_sync) {
			lz->mapped = mapped;
			lz->keyboard_sync = false;
		} else if (mapped != lz->mapped) {
			uint8_t keys[6] = { 0 };
			int count = 0;

			lz->mapped = mapped;
			for (int bit = 0; bit < 20; bit++) {
				if (!(buttons & OPENSC2_BIT(bit))) {
					continue;
				}
				if (count < 6) {
					keys[count] = key_of(OPENSC2_BIT(bit));
				}
				count++;
			}
			if (count > 6) {
				memset(keys, KEY_ROLLOVER, sizeof(keys));
			}
			keyboard_report(keyboard, keys);
			made |= OPENSC2_LIZARD_KEYBOARD;
		}
	}

	/* Mouse: the motion of each pad since the last frame. */
	for (int side = 0; side < 2; side++) {
		const struct opensc2_trackpad *pad = &s->pad[side];
		bool touched;

		blanked[side] = (ignore_on_stick & (side ? 2 : 1)) && (buttons & stick_bit[side]);
		touched = !blanked[side] && (pad->touched || (buttons & touch_bit[side]));
		if (touched && lz->pad_touched[side]) {
			dx[side] = (int32_t)pad->x - lz->pad_x[side];
			dy[side] = (int32_t)pad->y - lz->pad_y[side];
		}
		lz->pad_touched[side] = touched;
		lz->pad_x[side] = touched ? pad->x : 0;
		lz->pad_y[side] = touched ? pad->y : 0;
	}
	x = divide(&lz->rem_x, dx[OPENSC2_RIGHT], OPENSC2_LIZARD_POINTER_DIVISOR);
	y = -divide(&lz->rem_y, dy[OPENSC2_RIGHT], OPENSC2_LIZARD_POINTER_DIVISOR);
	pan = divide(&lz->rem_pan, dx[OPENSC2_LEFT], OPENSC2_LIZARD_SCROLL_DIVISOR);
	wheel = -divide(&lz->rem_wheel, dy[OPENSC2_LEFT], OPENSC2_LIZARD_SCROLL_DIVISOR);

	if (lz->buttons_on) {
		bool right_click = !blanked[OPENSC2_RIGHT] &&
				   ((buttons & OPENSC2_CLICK_RIGHT_PAD) || s->pad[OPENSC2_RIGHT].clicked);

		if ((buttons & OPENSC2_CLICK_RIGHT_TRIGGER) || right_click) {
			mbuttons |= 0x01;
		}
		if (buttons & OPENSC2_CLICK_LEFT_TRIGGER) {
			mbuttons |= 0x02;
		}
	}

	if (x || y || wheel || pan || mbuttons) {
		mouse_report(mouse, mbuttons, x, y, wheel, pan);
		lz->mouse_owed = true;
		made |= OPENSC2_LIZARD_MOUSE;
	} else if (lz->mouse_owed) {
		mouse_report(mouse, 0, 0, 0, 0, 0);
		lz->mouse_owed = false;
		made |= OPENSC2_LIZARD_MOUSE;
	}
	return made;
}
