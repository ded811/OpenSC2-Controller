/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * sensor_helpers.h: helpers that turn sensor readings into report 0x42's values the way the real
 * controller does, following the protocol documentation (not public yet; docs/protocol.md lists the
 * pages). Pure functions, no kernel; an application uses the ones its parts need:
 *   - capacitive touch: the stick-touch and grip bits from their words (settings 30, 31, 34, 35);
 *   - trackpads: touch from a signal level with the lift-off damping (settings 72, 73), the
 *     position's edge push and stretch (setting 66), its scale, the speed-adaptive filter (65),
 *     the rotation (2 and 24) and the touch stabiliser (80);
 *   - the IMU: the real controller's scaling, a mounting matrix, the gyro bias and its dead zone,
 *     and a rest-based bias estimate of OpenSC2's own (setting 85) for IMUs without one;
 *   - an ADC burst's average.
 * The values in the controller's units (signal levels, capacitive words) belong to Valve's sensor
 * chip: an application scales its own part's readings to them.
 *
 * Arithmetic: where the real controller computes in single-precision floating point, so do these,
 * in its order of operations and with its fused multiply-adds (fmaf()), and float-to-integer
 * conversions as its floating-point unit does them: the results are the real controller's to the
 * last unit, quirks included.
 */
#ifndef OPENSC2_SENSOR_HELPERS_H_
#define OPENSC2_SENSOR_HELPERS_H_

#include <stdbool.h>
#include <stdint.h>
#include <opensc2/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Distance -------------------------------------------------------------------------------- */

/* The real controller's approximation of sqrt(dx^2 + dy^2): with M the larger and m the smaller of
 * |dx| and |dy|, (1007 M + 441 m, less 40 M when M < 16 m) / 1024, rounded to the nearest whole
 * number; within about 3 % of the true length. */
uint32_t opensc2_approx_distance(int32_t dx, int32_t dy);

/* ---- Capacitive touch (stick touch, grip) ------------------------------------------------------
 * Each word (taken as a signed 16-bit value) goes through the mean of the last two words (the
 * first after a reset fills both); the bit turns on when the mean exceeds the on level and stays
 * on while the mean is at least the off level. With the on level below the off level (settings
 * 30 and 31 allow it) a mean between the two turns the bit off and on again every other word, as
 * on the real controller.
 */

struct opensc2_cap_touch {
	int32_t last;
	bool primed;
	bool on;
};

void opensc2_cap_reset(struct opensc2_cap_touch *t);
bool opensc2_cap_sample(struct opensc2_cap_touch *t, int32_t word, int32_t on_level,
			int32_t off_level);

/* Stick touch: setting 30 (on, 100..1000, default 500) and 31 (off, 0..800, default 400), limited
 * to their ranges. */
int32_t opensc2_stick_touch_on_level(int16_t setting30);
int32_t opensc2_stick_touch_off_level(int16_t setting31);
/* Grip: on = (setting 34 / 2 + 12.5) % of 1800, off = setting 35 % of on, worked out in single
 * precision as the real controller's trackpad driver does and truncated (34: 25..400, default 100,
 * on 1125; 35: 25..100, default 80, off 900); so for 15 of the odd values of 34 the level is one
 * below the exact 9 x setting + 225. */
int32_t opensc2_grip_on_level(int16_t setting34);
int32_t opensc2_grip_off_level(int16_t setting34, int16_t setting35);

/* ---- Trackpads: touch and position before the pad scale ----------------------------------------
 * Positions here are fractions of the pad in millionths: 0..1000000 on each axis, 500000 the
 * centre, Y counted as the sensor counts it (the report's negation comes last).
 */

#define OPENSC2_PAD_CENTRE  500000

/* Touch from a signal level: on above setting 72 (default 1300), held while above setting 73
 * (default 1000). While touched, a running average of the level ((7 x average + level) / 8,
 * started at the touch-down level, and updated with this frame's level before it is used) damps
 * the position near lift-off: while the level lies between the off level and the new average -
 * 200, the position moves only a fraction of the way from the previous one to the new one, from
 * 0.2 at the off level to 1 at the average - 200. Without a touch the position is the centre.
 * The position is kept in floating point between frames, as the real controller's driver keeps
 * it. */
struct opensc2_pad_touch {
	bool touched;
	int32_t average;
	float x, y;          /* the previous position, fractions of the pad */
};

void opensc2_pad_touch_reset(struct opensc2_pad_touch *t);
/* One frame: the level and the new position; *x and *y become the position to use. Returns the
 * touch. */
bool opensc2_pad_touch_step(struct opensc2_pad_touch *t, int32_t level, int16_t setting72,
			    int16_t setting73, int32_t *x, int32_t *y);

/* Edge push: a position further than 0.35 from the centre is pushed outward by
 * 7 x (distance - 0.35)^2, up to 0.1575 at the very edge (it may pass 0..1). */
int32_t opensc2_pad_edge(int32_t millionths);
/* Stretch (setting 66, on by default): (position - offset) / scale with the real controller's
 * fixed values for its own pads: left X -0.01, 0.90, left Y 0.05, 0.91, right X 0.11, 0.88, right
 * Y 0.05, 0.91. They fit Valve's pads only; a pad of another shape needs its own
 * (opensc2_pad_stretch_by()). */
int32_t opensc2_pad_stretch(enum opensc2_side side, int axis, int32_t millionths);
/* The same with an offset and a scale of the application's (fractions of the pad). */
int32_t opensc2_pad_stretch_by(int32_t millionths, float offset, float scale);
/* A sensor's raw centroid (millionths of the pad) through the real controller's driver in one go,
 * in floating point all the way: the edge push on each axis, then, with `stretch` (setting 66 on),
 * (p - offset) / scale with offset_scale = { X offset, X scale, Y offset, Y scale } (NULL: 0, 1);
 * the result in millionths, limited to -1..1 x 10^6 as an absolute event. */
void opensc2_pad_centroid(int32_t *x, int32_t *y, bool stretch, const float offset_scale[4]);

/* ---- Trackpads: the pad scale and what follows -------------------------------------------------
 * The pad scale runs from -16383 to 16383 ((position - 0.5) x 2 x 16383, truncated); the report's
 * from -32767 to 32767 (the pad scale doubled and limited), Y up positive after the negation.
 */

int16_t opensc2_pad_scale(int32_t millionths);

/* The position filter (setting 65): per axis a speed-adaptive low-pass of the "one euro" kind,
 * for 250 samples a second: the speed (the change since the previous input, times the rate)
 * smoothed at 1.2 Hz, the cut-off 0.001 Hz + 0.1 x |speed|, a rising cut-off at once, a falling
 * one by at most 1 % per sample; each stage y = alpha x + (1 - alpha) y with
 * alpha = 1 / (1 + rate / (2 pi cut-off)). The first sample after a reset passes (as far as
 * single-precision rounding lets it: the stage is applied to it too); reset it while the pad is
 * not touched. The output is truncated toward zero. Single-precision floating point, as on the
 * real controller. */
struct opensc2_pad_filter_axis {
	bool started;        /* the value stage has its first sample */
	float value;         /* the value stage's output */
	float last;          /* the previous input */
	bool speed_started;
	float speed;         /* the speed stage's output */
	float cutoff;        /* the last cut-off (0 after a reset) */
};

struct opensc2_pad_filter {
	struct opensc2_pad_filter_axis axis[2];
};

void opensc2_pad_filter_reset(struct opensc2_pad_filter *f);
/* One touched sample in the pad scale; `rate_hz` 0 means 250. */
void opensc2_pad_filter_step(struct opensc2_pad_filter *f, int16_t *x, int16_t *y, float rate_hz);

/* The rotation (settings 2 and 24, both non-zero): the right pad turns by setting 2's angle, the
 * left by minus it, both looked up in 1 degree steps from -4 to 4 degrees and 5 degree steps
 * elsewhere (rounded down to a step; 360 is no rotation). With s and c the sine and cosine x 8192
 * as the real controller's table has them: x' = (c x + s y) / 8192, y' = (c y - s x) / 8192,
 * toward zero, y before the report's negation. Works in any scale. Returns false (nothing changed)
 * when off. */
bool opensc2_pad_rotate(enum opensc2_side side, int16_t setting2, int16_t setting24, int32_t *x,
			int32_t *y);
/* The angle a pad turns by, in degrees 0..359 (the lookup above). */
int16_t opensc2_pad_rotation_angle(enum opensc2_side side, int16_t setting2);
/* sin and cos of whole degrees x 8192, truncated toward zero, as the real controller's table was
 * worked out: with pi taken as 3.141592, so 30 degrees give 4095 and 90 degrees 8191, not 4096
 * and 8192 (the rotation's own angles, 0..4 and the multiples of 5, are its table's entries). */
int32_t opensc2_sin8192(int32_t degrees);
int32_t opensc2_cos8192(int32_t degrees);

/* The touch stabiliser (setting 80 = 2 on both pads' reported position with the fine radius;
 * lizard mode uses it too). The first touched sample is held while the thumb stays within the
 * hold radius of it (by opensc2_approx_distance()); beyond it the output follows: each sample it
 * moves by the thumb's own movement and, if the thumb moved at all, closes another eighth of the
 * remaining gap (rounded down) plus one unit per axis. Reset it on a sample without a touch.
 * Report scale, y before the negation. */
#define OPENSC2_STABILISER_FINE    4369
#define OPENSC2_STABILISER_COARSE  8738

struct opensc2_stabiliser {
	bool started, following;
	int32_t anchor[2], out[2], prev[2];
};

void opensc2_stabiliser_reset(struct opensc2_stabiliser *s);
void opensc2_stabiliser_step(struct opensc2_stabiliser *s, uint32_t hold_radius, int32_t *x,
			     int32_t *y);

/* ---- IMU -------------------------------------------------------------------------------------- */

/* The real controller's scaling in single precision, truncated toward zero to a 32-bit integer
 * and then kept to its low 16 bits as the real controller stores it: m/s^2 / 19.6133 x 32767
 * (32767 = 2 g) and rad/s x 180 / pi x 32767 / 2000 (32767 = 2000 deg/s); a quaternion component
 * x 32767. Beyond full scale the value wraps round (2.5 g reads -24576), as on the real controller;
 * a NaN reads 0. */
int16_t opensc2_imu_accel_from_ms2(float ms2);
int16_t opensc2_imu_gyro_from_rads(float rads);
int16_t opensc2_imu_quat_from_unit(float q);

/* A mounting matrix, row major (out = m x in), applied to report units in whole numbers and kept
 * to 16 bits (so -(-32768) wraps to -32768, as on the real controller). The real controller's:
 * { 0, 1, 0, -1, 0, 0, 0, 0, 1 } (x' = y, y' = -x). */
void opensc2_imu_mount(const int8_t m[9], const int16_t in[3], int16_t out[3]);
/* The same for a quaternion's x, y and z (W, X, Y, Z order; W unchanged). */
void opensc2_imu_mount_quat(const int8_t m[9], const int16_t in[4], int16_t out[4]);
/* The dead zone after the bias: values strictly between -t and t become 0 (t 0: off). */
int16_t opensc2_imu_dead_zone(int16_t v, int32_t t);

/* A gyro-bias estimate of OpenSC2's own for IMUs without one (the real controller takes its IMU's):
 * while every axis stays within `threshold` of the current estimate, and has for `settle` samples
 * in a row, the estimate moves 1/256 of the way toward the reading. Use setting 85 as the
 * threshold in report units (default 100, about 6 deg/s; Steam writes 50) and subtract the
 * estimate while setting 84 is not 0. Units are whatever the caller uses, rad/s for example. */
struct opensc2_gyro_bias {
	float bias[3];
	uint16_t still;      /* samples in a row within the threshold */
};

void opensc2_gyro_bias_init(struct opensc2_gyro_bias *b, const float start[3]);
void opensc2_gyro_bias_update(struct opensc2_gyro_bias *b, const float gyro[3], float threshold,
			      uint16_t settle);

/* ---- Buttons ----------------------------------------------------------------------------------
 * The real controller's debounce: a button's change counts at once (at the first look after its
 * pin moved), then that button is held at its new level for OPENSC2_BUTTON_HOLD_MS whatever the
 * pin does (bounces are ignored, and a press shorter than the hold is still reported for its whole
 * length); when the hold ends the pin's level counts again (a change then starts another hold).
 * Feed it the pins' levels at every change and when opensc2_button_hold_next() says a hold ends.
 */
#define OPENSC2_BUTTON_HOLD_MS  16

struct opensc2_button_hold {
	uint32_t word;          /* the reported levels */
	uint32_t holding;       /* bits being held */
	uint32_t until[32];     /* when each bit's hold ends, ms */
};

/* Start from `word` (the buttons held at power-up), nothing held. */
void opensc2_button_hold_init(struct opensc2_button_hold *h, uint32_t word);
/* The pins' levels `level` at `now_ms` (a wrapping millisecond clock): the word to report. */
uint32_t opensc2_button_hold_step(struct opensc2_button_hold *h, uint32_t level, uint32_t now_ms);
/* Milliseconds from `now_ms` until the first hold ends (at least 1), or 0 when nothing is held. */
uint32_t opensc2_button_hold_next(const struct opensc2_button_hold *h, uint32_t now_ms);

/* ---- ADC bursts ------------------------------------------------------------------------------- */

/* The average of `scans` scans of `channels` channels (samples[scan * channels + channel]), each
 * sum divided by the number of scans toward zero and kept to 16 bits, as the real controller
 * stores it: it converts each channel four times in a row, and a negative average (a pin at 0 V
 * reads a little below 0) wraps round to a value near 65535. */
void opensc2_adc_average(const int16_t *samples, uint8_t channels, uint8_t scans, uint16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* OPENSC2_SENSOR_HELPERS_H_ */
