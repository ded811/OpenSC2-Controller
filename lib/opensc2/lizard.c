/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * lizard.c: lizard mode, written from the behaviour of the real one as the protocol documentation
 * (not public yet; docs/protocol.md lists the pages) describes it:
 *   on, off: setting 9. Turning on or off tells the keyboard and the mouse, and each takes it up:
 *             the keyboard sends an empty report, the mouse an all-zero one (keyboard first).
 *             Vendor command 0x85 raises the two switches (the keyboard and the mouse buttons),
 *             0x81 lowers them; the keyboard hears of a switch through its own view of setting 9,
 *             so a raise while it runs stays pending until its next "off" (docs/protocol.md,
 *             gap 7).
 *   keyboard: A = Enter, B and VIEW = Escape, the D-pad = arrow keys, MENU = Tab. While it runs it
 *             hears of every change of one of those eight buttons, the way a button driver
 *             reports it: then every held button, in bit order, takes the next key slot (its key,
 *             or 0 when it has none), and more than six held buttons fill all six slots with the
 *             roll-over code.
 *   mouse:    one frame per state report while it runs. The right pad (through the fine touch
 *             stabiliser while setting 80 is 1) moves the pointer, 128 pad units per count; the
 *             left pad (always through the coarse one) scrolls, 1024 per step, moving up giving a
 *             negative wheel value and right a positive pan. Each pad's tracker: the first touched
 *             frame gives no motion; while touched, each frame's movement goes into a history of
 *             20, its exact length is added to the travel, and the motion is the mean of the
 *             newest 8 (or of those since the touch-down), rounded toward zero; at lift-off, the
 *             flick speed is the mean of the oldest 15 of the last 20 movements (0 with fewer than
 *             20), and with more than 700 units of travel and a speed longer than setting 11 the
 *             pad glides: the speed times a factor that starts at 1 and drops by 1 / setting 12
 *             every frame (setting 12 read each frame), in single precision as on the real
 *             controller. A new touch on that pad, or the mouse stopping, ends a glide. While
 *             setting 82 blanks a pad its motion is worked out (the remainders take it) and then
 *             dropped. The right trigger or a right-pad click (only while the pad is touched) is
 *             the left button, the left trigger the right one. A report goes out every frame while
 *             one of its bytes is not zero, then one all-zero report.
 *   haptics:  per pad, while setting 9 is on and setting 70 is exactly 1 (otherwise the touch is
 *             forgotten, the click is not): a new touch makes its point the step point; then each
 *             time the thumb is more than 1500 units from the step point a step is counted and the
 *             step point moves, and whenever the steps times 1500 exceed setting 14 the count
 *             starts over with a tick (click A). Each press and release of the pad's click plays
 *             click B, unless that pad's click threshold (52, 53) is -1.
 * Where the documentation leaves a detail open, docs/protocol.md's gap list names the choice.
 */
#include <math.h>
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

/* The events the keyboard and the mouse take up. */
#define EV_ON        0x01u
#define EV_OFF       0x02u
#define EV_KEYS      0x04u   /* the keyboard: a mapped button changed */

/* The 20 buttons a button driver reports. */
#define DIGITAL_BUTTONS 0x000FFFFFu

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

/* ---- the reports ------------------------------------------------------------------------------ */

static uint8_t *out_add(struct opensc2_lizard_out *out, uint8_t len)
{
	uint8_t *r;

	if (out->n >= OPENSC2_LIZARD_OUT_MAX) {
		return NULL;   /* cannot happen: no call makes more */
	}
	r = out->report[out->n];
	out->len[out->n++] = len;
	return r;
}

static uint32_t keyboard_report(struct opensc2_lizard_out *out, const uint8_t keys[6])
{
	uint8_t *r = out_add(out, 9);

	if (r) {
		r[0] = OPENSC2_RPT_KEYBOARD;
		r[1] = 0;   /* no modifiers */
		r[2] = 0;   /* reserved */
		memcpy(&r[3], keys, 6);
	}
	return OPENSC2_LIZARD_KEYBOARD;
}

/* The bytes as they go out: each value's low eight bits, no clamping. */
static uint32_t mouse_report(struct opensc2_lizard_out *out, const uint8_t bytes[5])
{
	uint8_t *r = out_add(out, 6);

	if (r) {
		r[0] = OPENSC2_RPT_MOUSE;
		memcpy(&r[1], bytes, 5);
	}
	return OPENSC2_LIZARD_MOUSE;
}

static const uint8_t no_keys[6];
static const uint8_t no_mouse[5];

/* ---- the keyboard and the mouse take up their events ------------------------------------------ */

static uint32_t keys_report(struct opensc2_lizard *lz, struct opensc2_lizard_out *out)
{
	uint8_t keys[6] = { 0 };
	uint32_t word = lz->kb_word;
	int count = 0;

	for (int bit = 0; bit < 20; bit++) {
		if (!(word & OPENSC2_BIT(bit))) {
			continue;
		}
		if (count == 6) {
			memset(keys, KEY_ROLLOVER, sizeof(keys));
			break;
		}
		keys[count++] = key_of(OPENSC2_BIT(bit));
	}
	lz->kb_held = false;
	for (int i = 0; i < 6; i++) {
		lz->kb_held |= keys[i] != 0;
	}
	return keyboard_report(out, keys);
}

/* Stopped, the keyboard waits for "on" alone, which also drops everything else pending; running,
 * it takes up "off" and mapped changes, and an "on" stays pending. */
static uint32_t keyboard_run(struct opensc2_lizard *lz, struct opensc2_lizard_out *out)
{
	uint32_t made = 0;

	for (;;) {
		uint8_t ev;

		if (!lz->kb_on) {
			if (!(lz->kb_events & EV_ON)) {
				return made;
			}
			lz->kb_events = 0;
			lz->kb_on = true;
			lz->kb_held = false;
			made |= keyboard_report(out, no_keys);
			continue;
		}
		ev = lz->kb_events & (EV_OFF | EV_KEYS);
		if (!ev) {
			return made;
		}
		lz->kb_events &= (uint8_t)~ev;
		if (ev & EV_OFF) {
			lz->kb_on = false;
			lz->kb_held = false;
			made |= keyboard_report(out, no_keys);
			continue;
		}
		made |= keys_report(lz, out);
	}
}

static void pad_restart(struct opensc2_lizard_pad *p)
{
	p->touched = false;
	p->x = p->y = 0;
	osc2_stabiliser_reset(&p->stab);
	memset(p->hist_x, 0, sizeof(p->hist_x));
	memset(p->hist_y, 0, sizeof(p->hist_y));
	p->count = p->head = 0;
	p->travel = 0.0f;
	p->glide = 0.0f;
	p->vx = p->vy = 0;
	p->rem_x = p->rem_y = 0;
}

static uint32_t mouse_run(struct opensc2_lizard *lz, struct opensc2_lizard_out *out)
{
	uint32_t made = 0;

	for (;;) {
		if (!lz->ms_on) {
			if (!(lz->ms_events & EV_ON)) {
				return made;
			}
			/* Both trackers and both stabilisers start over. */
			lz->ms_events = 0;
			lz->ms_on = true;
			pad_restart(&lz->pad[0]);
			pad_restart(&lz->pad[1]);
			lz->mouse_owed = false;
			made |= mouse_report(out, no_mouse);
			continue;
		}
		if (!(lz->ms_events & EV_OFF)) {
			return made;
		}
		lz->ms_events &= (uint8_t)~EV_OFF;
		lz->ms_on = false;
		lz->mouse_owed = false;
		made |= mouse_report(out, no_mouse);
	}
}

/* The keyboard first: its events are always posted first. */
static uint32_t take_up(struct opensc2_lizard *lz, struct opensc2_lizard_out *out)
{
	uint32_t made = keyboard_run(lz, out);

	return made | mouse_run(lz, out);
}

/* The switches change; the keyboard decides by its own view of setting 9. */
static void set_switches(struct opensc2_lizard *lz, bool up)
{
	lz->kb_switch = up;
	lz->kb_events |= (up && lz->kb_mode) ? EV_ON : EV_OFF;
	lz->ms_switch = up;
}

void opensc2_lizard_reset(struct opensc2_lizard *lz)
{
	memset(lz, 0, sizeof(*lz));
	lz->kb_switch = lz->ms_switch = true;
	lz->kb_mode = true;
	lz->kb_on = lz->ms_on = true;
	/* A button held at boot made its key report while no link was there to take it. */
	lz->buttons_sync = true;
	pad_restart(&lz->pad[0]);
	pad_restart(&lz->pad[1]);
}

uint32_t opensc2_lizard_setting(struct opensc2_lizard *lz, bool on, struct opensc2_lizard_out *out)
{
	out->n = 0;
	/* In the real controller's order: the switches follow the mode, then the keyboard hears of
	 * the mode itself, then the mouse. */
	set_switches(lz, on);
	lz->kb_mode = on;
	lz->kb_events |= (on && lz->kb_switch) ? EV_ON : EV_OFF;
	lz->ms_events |= on ? EV_ON : EV_OFF;
	return take_up(lz, out);
}

uint32_t opensc2_lizard_switches(struct opensc2_lizard *lz, bool up,
				 struct opensc2_lizard_out *out)
{
	out->n = 0;
	set_switches(lz, up);
	return take_up(lz, out);
}

void opensc2_lizard_resync(struct opensc2_lizard *lz)
{
	for (int side = 0; side < 2; side++) {
		pad_restart(&lz->pad[side]);
		lz->pad[side].fb_touched = false;
		lz->pad[side].steps = 0;
	}
	lz->mouse_owed = false;
	lz->buttons_sync = true;
	lz->fb_sync = true;
}

uint32_t opensc2_lizard_release(struct opensc2_lizard *lz, struct opensc2_lizard_out *out)
{
	out->n = 0;
	if (!(lz->kb_on && lz->kb_held) && !(lz->ms_on && lz->mouse_owed)) {
		return 0;
	}
	lz->kb_held = false;
	lz->mouse_owed = false;
	return keyboard_report(out, no_keys) | mouse_report(out, no_mouse);
}

/* The buttons, as a button driver reports them: a change of a mapped one reaches the keyboard
 * while it runs, with the whole word (all 20 buttons) of that moment. */
static void hear_buttons(struct opensc2_lizard *lz, uint32_t word)
{
	if (lz->buttons_sync) {
		lz->buttons_sync = false;
	} else if (((word ^ lz->buttons) & MAPPED_BUTTONS) && lz->kb_on) {
		lz->kb_word = word;
		lz->kb_events |= EV_KEYS;
	}
	lz->buttons = word;
}

uint32_t opensc2_lizard_buttons(struct opensc2_lizard *lz, uint32_t buttons,
				struct opensc2_lizard_out *out)
{
	out->n = 0;
	hear_buttons(lz, buttons & DIGITAL_BUTTONS);
	return take_up(lz, out);
}

/* ---- the frame -------------------------------------------------------------------------------- */

/* The movement history: filled from the start, then a ring whose oldest entry is at `head`. */
static void history_push(struct opensc2_lizard_pad *p, int32_t mx, int32_t my)
{
	if (p->count == OPENSC2_LIZARD_HISTORY) {
		p->hist_x[p->head] = mx;
		p->hist_y[p->head] = my;
		p->head = (uint8_t)((p->head + 1) % OPENSC2_LIZARD_HISTORY);
	} else {
		p->hist_x[p->count] = mx;
		p->hist_y[p->count] = my;
		p->count++;
	}
}

/* The mean of `n` entries ending `skip` entries before the newest, rounded toward zero; 0 when
 * the history holds fewer than n + skip. */
static int32_t history_mean(const struct opensc2_lizard_pad *p, const int32_t *hist, int n,
			    int skip)
{
	int at;
	int32_t sum = 0;

	if (n <= 0 || p->count < n + skip) {
		return 0;
	}
	at = (p->count == OPENSC2_LIZARD_HISTORY ? p->head : p->count) - (n + skip);
	while (at < 0) {
		at += OPENSC2_LIZARD_HISTORY;
	}
	for (int i = 0; i < n; i++) {
		sum += hist[at];
		at = (at + 1) % OPENSC2_LIZARD_HISTORY;
	}
	return sum / n;
}

/* sqrt(a * a + b * b) in single precision, with the first square fused into the addition, as the
 * real controller computes the travel and the flick speed. */
static float length_f(int32_t a, int32_t b)
{
	float fa = (float)a, fb = (float)b;

	return sqrtf(fmaf(fa, fa, fb * fb));
}

/* The pad's motion this frame (before the division), on its (stabilised) position. */
static void pad_motion(struct opensc2_lizard_pad *p, bool touched, int32_t x, int32_t y,
		       const int16_t settings[OPENSC2_SETTINGS_COUNT], uint32_t rules, int32_t *sx,
		       int32_t *sy)
{
	*sx = 0;
	*sy = 0;
	if (touched) {
		if (p->touched) {
			int32_t mx = x - p->x, my = y - p->y;

			p->travel += length_f(mx, my);
			history_push(p, mx, my);
			if (rules & OSC2_RULE_LIZARD_SMOOTHING) {
				int n = p->count < OPENSC2_LIZARD_AVERAGE ? p->count :
					OPENSC2_LIZARD_AVERAGE;

				*sx = history_mean(p, p->hist_x, n, 0);
				*sy = history_mean(p, p->hist_y, n, 0);
			} else {
				*sx = mx;
				*sy = my;
			}
		} else {
			/* Touch-down: no motion; the history and the travel start over. */
			p->count = p->head = 0;
			p->travel = 0.0f;
		}
	} else {
		if (p->touched) {
			/* Lift-off: a glide when the touch travelled enough and ended fast enough. */
			int skip = OPENSC2_LIZARD_HISTORY - OPENSC2_LIZARD_FLICK_MOVES;

			p->vx = history_mean(p, p->hist_x, OPENSC2_LIZARD_FLICK_MOVES, skip);
			p->vy = history_mean(p, p->hist_y, OPENSC2_LIZARD_FLICK_MOVES, skip);
			p->glide = 0.0f;
			if ((rules & OSC2_RULE_LIZARD_GLIDE) &&
			    p->travel > (float)OPENSC2_LIZARD_FLICK_TRAVEL &&
			    (int32_t)length_f(p->vx, p->vy) >
				    settings[OPENSC2_SETTING_MOMENTUM_MIN_VEL]) {
				p->glide = 1.0f;
			}
		}
		if (!(rules & OSC2_RULE_LIZARD_GLIDE)) {
			p->glide = 0.0f;
		}
		if (p->glide > 0.0f) {
			*sx = (int32_t)((float)p->vx * p->glide);
			*sy = (int32_t)((float)p->vy * p->glide);
			p->glide -= 1.0f / (float)settings[OPENSC2_SETTING_MOMENTUM_DECAY];
		}
	}
	p->touched = touched;
	p->x = x;
	p->y = y;
}

/* (*rem + total) / divisor, truncated toward zero; the rest stays in *rem. */
static int32_t divide(int32_t *rem, int32_t total, int32_t divisor)
{
	int32_t sum = *rem + total;

	*rem = sum % divisor;
	return sum / divisor;
}

/* A pad's ticks and clicks for one sample, on its reported position (not the stabilised one). */
static uint32_t pad_feedback(struct opensc2_lizard_pad *p, int side, bool touched, bool clicked,
			     int16_t x, int16_t y, const int16_t settings[OPENSC2_SETTINGS_COUNT],
			     bool sync)
{
	uint32_t made = 0;

	if (settings[OPENSC2_SETTING_HAPTICS_ENABLED] != 1 ||
	    settings[OPENSC2_SETTING_LIZARD_MODE] == 0) {
		p->fb_touched = false;   /* the click state is kept */
		return 0;
	}
	if (touched) {
		if (!p->fb_touched) {
			p->step_x = x;
			p->step_y = y;
			p->steps = 0;
		} else {
			if (osc2_approx_len(x - p->step_x, y - p->step_y) >
			    OPENSC2_LIZARD_TICK_STEP) {
				p->steps++;
				p->step_x = x;
				p->step_y = y;
			}
			if ((int32_t)(p->steps * OPENSC2_LIZARD_TICK_STEP) >
			    settings[OPENSC2_SETTING_HAPTIC_INCREMENT]) {
				p->steps = 0;
				made |= tick_flag[side];
			}
		}
	}
	p->fb_touched = touched;
	if (!sync && clicked != p->fb_clicked &&
	    settings[side ? OPENSC2_SETTING_RIGHT_CLICK_PRESSURE :
			    OPENSC2_SETTING_LEFT_CLICK_PRESSURE] != -1) {
		made |= click_flag[side];
	}
	p->fb_clicked = clicked;
	return made;
}

static void stabilise(struct osc2_stabiliser *st, int32_t radius, bool touched, int32_t *x,
		      int32_t *y)
{
	if (touched) {
		osc2_stabiliser_step(st, radius, true, x, y);
	} else {
		osc2_stabiliser_reset(st);
	}
}

static uint32_t mouse_frame(struct opensc2_lizard *lz, const bool touched[2],
			    const int32_t px[2], const int32_t py[2], const bool blanked[2],
			    bool right_click, uint32_t buttons,
			    const int16_t settings[OPENSC2_SETTINGS_COUNT], uint32_t rules,
			    struct opensc2_lizard_out *out)
{
	static const int32_t divisor[2] = { OPENSC2_LIZARD_SCROLL_DIVISOR,
					    OPENSC2_LIZARD_POINTER_DIVISOR };
	int32_t dx[2], dy[2];
	uint8_t bytes[5] = { 0 };
	bool active = false;

	for (int side = 0; side < 2; side++) {
		struct opensc2_lizard_pad *p = &lz->pad[side];
		int32_t x = px[side], y = py[side], sx, sy;

		if (rules & OSC2_RULE_LIZARD_STABILISER) {
			/* The right pad's only while setting 80 is 1; otherwise it is left as it
			 * is (not started over), and taken up again when 80 is back at 1. */
			if (side == OPENSC2_LEFT) {
				stabilise(&p->stab, OSC2_STABILISER_COARSE, touched[side], &x, &y);
			} else if (settings[OPENSC2_SETTING_PAD_STABILIZER] == 1) {
				stabilise(&p->stab, OSC2_STABILISER_FINE, touched[side], &x, &y);
			}
		} else {
			osc2_stabiliser_reset(&p->stab);
		}
		pad_motion(p, touched[side], x, y, settings, rules, &sx, &sy);
		dx[side] = divide(&p->rem_x, sx, divisor[side]);
		dy[side] = divide(&p->rem_y, sy, divisor[side]);
		if (blanked[side]) {
			dx[side] = dy[side] = 0;   /* worked out, then dropped */
		}
	}

	if (lz->ms_switch) {
		if ((buttons & OPENSC2_CLICK_RIGHT_TRIGGER) || right_click) {
			bytes[0] |= 0x01;
		}
		if (buttons & OPENSC2_CLICK_LEFT_TRIGGER) {
			bytes[0] |= 0x02;
		}
	}
	bytes[1] = (uint8_t)dx[OPENSC2_RIGHT];
	bytes[2] = (uint8_t)-dy[OPENSC2_RIGHT];
	bytes[3] = (uint8_t)-dy[OPENSC2_LEFT];
	bytes[4] = (uint8_t)dx[OPENSC2_LEFT];
	for (int i = 0; i < 5; i++) {
		active |= bytes[i] != 0;
	}
	if (active) {
		lz->mouse_owed = true;
		return mouse_report(out, bytes);
	}
	if (lz->mouse_owed) {
		lz->mouse_owed = false;
		return mouse_report(out, no_mouse);
	}
	return 0;
}

uint32_t opensc2_lizard_step(struct opensc2_lizard *lz, const struct opensc2_state *s,
			     const int16_t settings[OPENSC2_SETTINGS_COUNT], uint32_t rules,
			     struct opensc2_lizard_out *out)
{
	uint32_t buttons = s->buttons;
	uint32_t word = buttons & DIGITAL_BUTTONS;
	int16_t ignore_on_stick = settings[OPENSC2_SETTING_PAD_IGNORE_ON_STICK];
	bool touched[2], clicked[2], blanked[2];
	int32_t px[2], py[2];
	uint32_t made = 0;

	out->n = 0;

	/* The buttons as the frame carries them: a change that opensc2_lizard_buttons() has not
	 * already brought (the application's sample callback, the test hooks' injection). */
	if (!lz->driver) {
		hear_buttons(lz, word);
	}
	made |= take_up(lz, out);

	/* The pads, as the trackpad hands them over: setting 82 blanks a pad whose stick is
	 * touched, and a click only counts while its pad is touched. */
	for (int side = 0; side < 2; side++) {
		const struct opensc2_trackpad *pad = &s->pad[side];

		blanked[side] = (ignore_on_stick & (side ? 2 : 1)) && (buttons & stick_bit[side]);
		touched[side] = !blanked[side] && (pad->touched || (buttons & touch_bit[side]));
		clicked[side] = touched[side] && (pad->clicked || (buttons & click_bit[side]));
		px[side] = touched[side] ? pad->x : 0;
		py[side] = touched[side] ? pad->y : 0;
		if (rules & OSC2_RULE_LIZARD_TICKS) {
			made |= pad_feedback(&lz->pad[side], side, touched[side], clicked[side],
					     (int16_t)px[side], (int16_t)py[side], settings,
					     lz->fb_sync);
		}
	}
	lz->fb_sync = false;

	if (lz->ms_on) {
		made |= mouse_frame(lz, touched, px, py, blanked, clicked[OPENSC2_RIGHT], buttons,
				    settings, rules, out);
	}
	return made;
}

/* ---- the haptic events ------------------------------------------------------------------------ */

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
