/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * pad_rules.h: the trackpads' frame-by-frame processing (<opensc2/trackpad.h>) as pure logic, so
 * the host tests run it as the library does (pad.c adds the lock, the settings and the glue). Not a
 * public interface.
 */
#ifndef OPENSC2_PAD_RULES_H_
#define OPENSC2_PAD_RULES_H_

#include <stdbool.h>
#include <stdint.h>
#include <opensc2/cal.h>
#include <opensc2/sensor_helpers.h>
#include <opensc2/settings.h>
#include <opensc2/trackpad.h>

/* The controller settings a frame reads. */
struct osc2_pad_settings {
	int16_t v[OPENSC2_SETTINGS_COUNT];
};

/* The frame path's touch stabiliser: held at the touch-down point until the thumb is further than
 * the hold radius from it, then following the thumb's movement and closing an eighth of the gap
 * (plus one unit) whenever the thumb moved. Its state is kept while setting 80 is not 2 and starts
 * over only on a frame without a touch (or a start-over of the pad), as on the real controller. */
struct osc2_pad_stab {
	int8_t mode;          /* 0 idle, 1 held, 2 following */
	bool in_touch;        /* the last input had a touch */
	int32_t in[2];        /* the last input */
	int32_t out[2];       /* the last output */
};

struct osc2_pad_proc {
	bool frames;                          /* the application feeds this side frames */
	bool set_up;
	uint32_t pressure_applied;            /* the pressure key load the set-up comes from */
	struct opensc2_pressure_proc pressure;
	struct opensc2_cap_touch stick, grip;
	struct opensc2_pad_filter filter;
	struct osc2_pad_stab stab;
	struct opensc2_pad_result out;        /* the last frame's results */
};

/* What makes both pads start over: settings 30, 31, 33, 34, 35, 52, 53, 66, 72, 73. */
struct osc2_pad_watch {
	bool started;
	int16_t settings[10];
};

/* Before each frame: a pad whose pressure key was loaded again (loads[side] changed; `cal` the key)
 * is set up again and starts over; a change of a watched setting starts both over. */
void osc2_pad_watch_step(struct osc2_pad_watch *w, struct osc2_pad_proc pads[2],
			 const struct osc2_pad_settings *set, const uint32_t loads[2],
			 const struct opensc2_cal_pressure cal[2]);

/* A pad starts over: the means and hysteresis of its stick touch and grip, its click, its filter
 * and stabiliser (the pressure filter is kept). */
void osc2_pad_start_over(struct osc2_pad_proc *p);

/* One frame of `side`'s pad. `stick` is that side's stick as the deflection rule reads it (0, 0 in
 * raw mode); app_stick, app_grip and app_click are the application's own bits, which stand in for
 * a word the frame does not have. The results go to p->out; returns true when the frame had a
 * pressure word (its raw value in *raw, for the factory recording). */
bool osc2_pad_step(struct osc2_pad_proc *p, int side, const struct opensc2_pad_frame *f,
		   const struct osc2_pad_settings *set, const int16_t stick[2], bool app_stick,
		   bool app_grip, bool app_click, int16_t *raw);

/* The results into a state report: the pad's fields and its four bits (touch, click, stick touch,
 * grip). */
void osc2_pad_merge(const struct opensc2_pad_result *out, int side, struct opensc2_state *s);

/* pad.c, for the rest of the library: the sides fed by frames (bit 0 left, bit 1 right); each
 * state report's sticks for the deflection rule and the frames' results merged in; CAL_STATUS's
 * pressure fields of a side fed by frames (true when it is); back to boot (the host tests). */
#if defined(CONFIG_OPENSC2_CAL)
uint8_t osc2_pad_frame_sides(void);
void osc2_pad_report(struct opensc2_state *s, bool raw_sticks);
bool osc2_pad_status(int side, int16_t *filtered, bool *click);
void osc2_pad_reset(void);
#else
static inline uint8_t osc2_pad_frame_sides(void)
{
	return 0;
}
#endif

#endif /* OPENSC2_PAD_RULES_H_ */
