/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * lizard.c: lizard mode, written from the behaviour of the real one as the protocol documentation
 * (not public yet; docs/protocol.md lists the pages) describes it:
 *   keyboard: A = Enter, B and VIEW = Escape, the D-pad = arrow keys, MENU = Tab. It reacts when
 *             one of those eight buttons changes; then every held button, in bit order, takes the
 *             next key slot (its key, or 0 when it has none), and more than six held buttons fill
 *             all six slots with the roll-over code.
 *   mouse:    one frame per state report. The right pad (through the fine touch stabiliser while
 *             setting 80 is 1) moves the pointer, 128 pad units per count; the left pad (always
 *             through the coarse one) scrolls, 1024 per step, moving up giving a negative wheel
 *             value and right a positive pan. Each pad's tracker: the first touched frame gives no
 *             motion; while touched, each frame's movement goes into a history of 20 and the
 *             motion is the mean of the newest 8 (or of those since the touch-down), rounded
 *             toward zero; at lift-off a touch of more than 20 frames and more than 700 units of
 *             travel whose flick speed (the mean of the 15 oldest of the last 20 movements) is
 *             longer than setting 11 glides: the flick speed times a factor that starts at 1 and
 *             drops by 1 / setting 12 every frame. A new touch, or lizard mode turning off, ends
 *             a glide; while setting 82 blanks a pad its motion is held back. The division
 *             remainders are kept until lizard mode is entered again. The right trigger or a
 *             right-pad click is the left button, the left trigger the right one. A report goes
 *             out every frame while any field is non-zero, then one zero report.
 *   haptics:  while setting 70 is exactly 1, a step each time the thumb has moved more than 1500
 *             units from the last step point, and a tick (click A) once the steps times 1500
 *             exceed setting 14; a new touch starts the count over, so a glide is silent. Each
 *             press and release of a pad's click plays click B, unless that pad's click threshold
 *             (52, 53) is -1.
 * Where the documentation leaves a detail open, the choice made here is the simplest one that
 * fits it, and docs/protocol.md's gap list names it: the travel count adds up the same distance
 * approximation as the stabiliser, the flick speed is a whole number (its mean rounded toward
 * zero), setting 12 is read when a glide starts, and a tick or click is worked out once per frame
 * (the real one does it per trackpad sample).
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

static const uint32_t touch_bit[2] = { OPENSC2_TOUCH_LEFT_PAD, OPENSC2_TOUCH_RIGHT_PAD };
static const uint32_t click_bit[2] = { OPENSC2_CLICK_LEFT_PAD, OPENSC2_CLICK_RIGHT_PAD };
static const uint32_t stick_bit[2] = { OPENSC2_TOUCH_LEFT_STICK, OPENSC2_TOUCH_RIGHT_STICK };
static const uint32_t tick_flag[2] = { OPENSC2_LIZARD_TICK_LEFT, OPENSC2_LIZARD_TICK_RIGHT };
static const uint32_t click_flag[2] = { OPENSC2_LIZARD_CLICK_LEFT, OPENSC2_LIZARD_CLICK_RIGHT };

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
	memset(lz->pad, 0, sizeof(lz->pad));
	osc2_stabiliser_reset(&lz->pad[0].stab);
	osc2_stabiliser_reset(&lz->pad[1].stab);
	lz->rem_x = lz->rem_y = lz->rem_pan = lz->rem_wheel = 0;
	lz->mouse_owed = false;
	lz->click_sync = true;
}

void opensc2_lizard_reset(struct opensc2_lizard *lz)
{
	memset(lz, 0, sizeof(*lz));
	clear_tracker(lz);
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
	for (int side = 0; side < 2; side++) {
		lz->pad[side].gliding = false;
		lz->pad[side].ticking = false;   /* counted afresh from the next touch */
	}
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

/* The integer square root, rounded down. */
static uint32_t isqrt(uint64_t v)
{
	uint64_t r = 0, bit = (uint64_t)1 << 62;

	while (bit > v) {
		bit >>= 2;
	}
	while (bit) {
		if (v >= r + bit) {
			v -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return (uint32_t)r;
}

static int16_t setting_in(const int16_t settings[OPENSC2_SETTINGS_COUNT], uint8_t number,
			  int16_t lo, int16_t hi)
{
	int16_t v = settings[number];

	return v < lo ? lo : v > hi ? hi : v;
}

/* The pad's haptics for this frame, on its reported position (before the stabiliser). Runs before
 * the tracker, so p->touched is still the last frame's touch. */
static uint32_t pad_haptics(struct opensc2_lizard *lz, int side, bool touched, bool clicked,
			    int32_t x, int32_t y, const int16_t settings[OPENSC2_SETTINGS_COUNT],
			    uint32_t rules)
{
	struct opensc2_lizard_pad *p = &lz->pad[side];
	bool on = (rules & OSC2_RULE_LIZARD_TICKS) &&
		  settings[OPENSC2_SETTING_HAPTICS_ENABLED] == 1;
	int16_t threshold = settings[side ? OPENSC2_SETTING_RIGHT_CLICK_PRESSURE :
					    OPENSC2_SETTING_LEFT_CLICK_PRESSURE];
	uint32_t made = 0;

	if (touched && !p->touched) {
		/* A new touch: the step point is here and the count starts over. */
		p->ticking = on;
		p->step_x = x;
		p->step_y = y;
		p->steps = 0;
	} else if (touched && p->ticking && on &&
		   osc2_approx_len(x - p->step_x, y - p->step_y) > OPENSC2_LIZARD_TICK_STEP) {
		p->step_x = x;
		p->step_y = y;
		p->steps++;
		if ((int32_t)p->steps * OPENSC2_LIZARD_TICK_STEP >
		    setting_in(settings, OPENSC2_SETTING_HAPTIC_INCREMENT, 0, 32767)) {
			p->steps = 0;
			made |= tick_flag[side];
		}
	}
	if (!on) {
		p->ticking = false;   /* counted afresh from the next touch */
	}

	if (lz->click_sync) {
		p->click = clicked;
	} else if (clicked != p->click) {
		p->click = clicked;
		if (on && threshold > -1) {
			made |= click_flag[side];
		}
	}
	return made;
}

/* The pad's motion for this frame: the tracker on its (stabilised) position. */
static void pad_motion(struct opensc2_lizard_pad *p, bool touched, int32_t x, int32_t y,
		       const int16_t settings[OPENSC2_SETTINGS_COUNT], uint32_t rules, int32_t *mx,
		       int32_t *my)
{
	*mx = 0;
	*my = 0;
	if (touched) {
		if (!p->touched) {
			/* Touch-down: no motion; the history, the travel and any glide start over. */
			p->hist_head = 0;
			p->moves = 0;
			p->travel = 0;
			p->gliding = false;
		} else {
			int32_t dx = x - p->x, dy = y - p->y;

			p->hist_x[p->hist_head] = dx;
			p->hist_y[p->hist_head] = dy;
			p->hist_head = (uint8_t)((p->hist_head + 1) % OPENSC2_LIZARD_HISTORY);
			if (p->moves < UINT32_MAX) {
				p->moves++;
			}
			p->travel += (uint32_t)osc2_approx_len(dx, dy);
			if (p->travel > INT32_MAX) {
				p->travel = INT32_MAX;
			}
			if (rules & OSC2_RULE_LIZARD_SMOOTHING) {
				int32_t n = p->moves < OPENSC2_LIZARD_AVERAGE ? (int32_t)p->moves :
					    OPENSC2_LIZARD_AVERAGE;
				int32_t sx = 0, sy = 0;

				for (int32_t i = 1; i <= n; i++) {
					int at = (p->hist_head + OPENSC2_LIZARD_HISTORY - i) %
						 OPENSC2_LIZARD_HISTORY;

					sx += p->hist_x[at];
					sy += p->hist_y[at];
				}
				*mx = sx / n;
				*my = sy / n;
			} else {
				*mx = dx;
				*my = dy;
			}
		}
		p->touched = true;
		p->x = x;
		p->y = y;
		return;
	}

	if (p->touched) {
		/* Lift-off: a glide when the touch was long, travelled and fast enough. */
		p->touched = false;
		if ((rules & OSC2_RULE_LIZARD_GLIDE) && p->moves >= OPENSC2_LIZARD_HISTORY &&
		    p->travel > OPENSC2_LIZARD_FLICK_TRAVEL) {
			int32_t sx = 0, sy = 0;
			uint64_t len2;

			/* The ring is full: its oldest movement is at hist_head. */
			for (int i = 0; i < OPENSC2_LIZARD_FLICK_MOVES; i++) {
				int at = (p->hist_head + i) % OPENSC2_LIZARD_HISTORY;

				sx += p->hist_x[at];
				sy += p->hist_y[at];
			}
			p->vx = sx / OPENSC2_LIZARD_FLICK_MOVES;
			p->vy = sy / OPENSC2_LIZARD_FLICK_MOVES;
			len2 = (uint64_t)((int64_t)p->vx * p->vx) + (uint64_t)((int64_t)p->vy * p->vy);
			if ((int64_t)isqrt(len2) >
			    setting_in(settings, OPENSC2_SETTING_MOMENTUM_MIN_VEL, 0, 1000)) {
				p->gliding = true;
				p->glide_frame = 0;
				p->glide_frames = (uint16_t)setting_in(
					settings, OPENSC2_SETTING_MOMENTUM_DECAY, 1, 1000);
			}
		}
	}
	if (p->gliding && !(rules & OSC2_RULE_LIZARD_GLIDE)) {
		p->gliding = false;
	}
	if (p->gliding) {
		/* The factor: 1 in the lift-off frame, 1 / setting 12 less in each frame after. */
		int32_t left = p->glide_frames - p->glide_frame;

		*mx = p->vx * left / p->glide_frames;
		*my = p->vy * left / p->glide_frames;
		if (++p->glide_frame >= p->glide_frames) {
			p->gliding = false;
		}
	}
}

uint32_t opensc2_lizard_step(struct opensc2_lizard *lz, const struct opensc2_state *s,
			     const int16_t settings[OPENSC2_SETTINGS_COUNT], uint32_t rules,
			     uint8_t keyboard[9], uint8_t mouse[6])
{
	uint32_t buttons = s->buttons;
	uint8_t ignore_on_stick = (uint8_t)settings[OPENSC2_SETTING_PAD_IGNORE_ON_STICK];
	uint32_t made = 0;
	int32_t mx[2] = { 0, 0 }, my[2] = { 0, 0 };
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

	/* Mouse: each pad's stabiliser, tracker and haptics. */
	for (int side = 0; side < 2; side++) {
		const struct opensc2_trackpad *pad = &s->pad[side];
		struct opensc2_lizard_pad *p = &lz->pad[side];
		bool touched, clicked;
		int32_t px, py;

		/* Setting 82 (normally already applied to the state by the input rules). */
		blanked[side] = (ignore_on_stick & (side ? 2 : 1)) && (buttons & stick_bit[side]);
		touched = !blanked[side] && (pad->touched || (buttons & touch_bit[side]));
		clicked = !blanked[side] && (pad->clicked || (buttons & click_bit[side]));
		px = touched ? pad->x : 0;
		py = touched ? pad->y : 0;

		made |= pad_haptics(lz, side, touched, clicked, px, py, settings, rules);
		if ((rules & OSC2_RULE_LIZARD_STABILISER) &&
		    (side == OPENSC2_LEFT || settings[OPENSC2_SETTING_PAD_STABILIZER] == 1)) {
			osc2_stabiliser_step(&p->stab, side == OPENSC2_LEFT ? OSC2_STABILISER_COARSE :
					     OSC2_STABILISER_FINE, touched, &px, &py);
		} else {
			osc2_stabiliser_reset(&p->stab);
		}
		pad_motion(p, touched, px, py, settings, rules, &mx[side], &my[side]);
		if (blanked[side]) {
			mx[side] = my[side] = 0;   /* held back; a glide's factor keeps running down */
		}
	}
	lz->click_sync = false;
	x = divide(&lz->rem_x, mx[OPENSC2_RIGHT], OPENSC2_LIZARD_POINTER_DIVISOR);
	y = -divide(&lz->rem_y, my[OPENSC2_RIGHT], OPENSC2_LIZARD_POINTER_DIVISOR);
	pan = divide(&lz->rem_pan, mx[OPENSC2_LEFT], OPENSC2_LIZARD_SCROLL_DIVISOR);
	wheel = -divide(&lz->rem_wheel, my[OPENSC2_LEFT], OPENSC2_LIZARD_SCROLL_DIVISOR);

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

static void haptic_event(struct opensc2_haptic *h, int side, uint8_t command, int8_t gain_db)
{
	memset(h, 0, sizeof(*h));
	h->kind = OPENSC2_HAPTIC_COMMAND;
	h->report_id = 0;   /* made by the library */
	h->target = (uint8_t)side;
	h->channels = side ? OPENSC2_HAPTIC_RIGHT_PAD : OPENSC2_HAPTIC_LEFT_PAD;
	h->command.command = command;
	h->command.gain_db = gain_db;
}

uint8_t opensc2_lizard_haptics(uint32_t made, struct opensc2_haptic out[4])
{
	uint8_t n = 0;

	for (int side = 0; side < 2; side++) {
		if (made & tick_flag[side]) {
			haptic_event(&out[n++], side, OPENSC2_HAPTIC_CMD_CLICK_A,
				     OPENSC2_LIZARD_TICK_GAIN_DB);
		}
		if (made & click_flag[side]) {
			haptic_event(&out[n++], side, OPENSC2_HAPTIC_CMD_CLICK_B,
				     OPENSC2_LIZARD_CLICK_GAIN_DB);
		}
	}
	return n;
}
