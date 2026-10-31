/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * sensor_helpers.c: the sensor helpers of <opensc2/sensor_helpers.h>, from the rules in the protocol
 * documentation (not public yet; docs/protocol.md lists the pages). Pure logic. Where the real
 * controller computes in single-precision floating point (the grip levels, the pad's touch damping,
 * edge push, stretch and scale, the position filter, the IMU scaling) so does this file, in the
 * same order, with fmaf() where its build fuses a multiply and an add, and float-to-integer
 * conversions as its floating-point unit does them (float_rules.h). Built with -ffp-contract=off.
 */
#include <math.h>
#include <string.h>
#include <opensc2/sensor_helpers.h>
#include "float_rules.h"

#define PAD_HALF     16383
#define REPORT_FULL  32767
#define MILLION      1000000

static int32_t clamp32(int64_t v, int32_t lo, int32_t hi)
{
	return v < lo ? lo : v > hi ? hi : (int32_t)v;
}

/* ---- distance -------------------------------------------------------------------------------- */

uint32_t opensc2_approx_distance(int32_t dx, int32_t dy)
{
	int64_t a = dx < 0 ? -(int64_t)dx : dx;
	int64_t b = dy < 0 ? -(int64_t)dy : dy;
	int64_t big = a > b ? a : b;
	int64_t small = a > b ? b : a;
	int64_t sum = 1007 * big + 441 * small;

	if (big < 16 * small) {
		sum -= 40 * big;
	}
	return (uint32_t)((sum + 512) / 1024);
}

/* ---- capacitive touch ------------------------------------------------------------------------ */

void opensc2_cap_reset(struct opensc2_cap_touch *t)
{
	memset(t, 0, sizeof(*t));
}

bool opensc2_cap_sample(struct opensc2_cap_touch *t, int32_t word, int32_t on_level,
			int32_t off_level)
{
	int16_t w = (int16_t)word;
	int32_t mean;

	if (!t->primed) {
		t->last = w;   /* the first word fills both */
		t->primed = true;
	}
	mean = (int16_t)((t->last + w) / 2);
	t->last = w;
	if (t->on) {
		/* held while the mean is at least the off level; below it, off for this word */
		t->on = mean >= off_level;
	} else {
		t->on = mean > on_level;
	}
	return t->on;
}

int32_t opensc2_stick_touch_on_level(int16_t setting30)
{
	return clamp32(setting30, 100, 1000);
}

int32_t opensc2_stick_touch_off_level(int16_t setting31)
{
	return clamp32(setting31, 0, 800);
}

int32_t opensc2_grip_on_level(int16_t setting34)
{
	float s = (float)(uint16_t)clamp32(setting34, 25, 400);

	/* (s / 2 + 12.5) / 100 x 1800, the multiply and the add fused, kept to 16 bits */
	return (uint16_t)osc2_f32_to_u32(fmaf(s, 0.5f, 12.5f) / 100.0f * 1800.0f);
}

int32_t opensc2_grip_off_level(int16_t setting34, int16_t setting35)
{
	float s = (float)(uint16_t)clamp32(setting35, 25, 100);

	return (uint16_t)osc2_f32_to_u32(s / 100.0f * (float)opensc2_grip_on_level(setting34));
}

/* ---- trackpads: touch and position before the pad scale -------------------------------------- */

void opensc2_pad_touch_reset(struct opensc2_pad_touch *t)
{
	t->touched = false;
	t->average = 0;
	t->x = t->y = 0.5f;
}

/* millionths of the pad (an absolute event's value, -1..1 x 10^6) as a fraction, and back */
static float from_millionths(int32_t m)
{
	return (float)clamp32(m, -MILLION, MILLION) / 1000000.0f;
}

static int32_t to_millionths(float p)
{
	p = p < -1.0f ? -1.0f : p > 1.0f ? 1.0f : p;
	return osc2_f32_to_s32(p * 1000000.0f);
}

/* x0..x1 onto y0..y1, the fraction limited to 0..1 (the last step fused). */
static float lerp_limited(float x, float x0, float x1, float y0, float y1)
{
	float t = (x - x0) / (x1 - x0);

	t = t > 1.0f ? 1.0f : t < 0.0f ? 0.0f : t;
	return fmaf(t, y1 - y0, y0);
}

bool opensc2_pad_touch_step(struct opensc2_pad_touch *t, int32_t level, int16_t setting72,
			    int16_t setting73, int32_t *x, int32_t *y)
{
	int32_t on = clamp32(setting72, 0, 16000);
	int32_t off = clamp32(setting73, 0, 16000);
	float px = from_millionths(*x), py = from_millionths(*y);
	int32_t limit;

	if (!t->touched) {
		t->average = level;   /* the running average starts at the touch-down level */
		limit = on;
	} else {
		int32_t avg = (t->average * 7 + level) / 8;
		int32_t hi = avg - 200;
		float f = 1.0f;

		t->average = avg;
		if (level > off && off < hi && level < hi) {
			f = lerp_limited((float)level, (float)off, (float)hi, 0.2f, 1.0f);
		}
		px = fmaf(px - t->x, f, t->x);
		py = fmaf(py - t->y, f, t->y);
		limit = off;
	}
	t->touched = level > limit;
	if (!t->touched) {
		px = py = 0.5f;
	}
	t->x = px;
	t->y = py;
	*x = to_millionths(px);
	*y = to_millionths(py);
	return t->touched;
}

int32_t opensc2_pad_stretch_by(int32_t millionths, float offset, float scale)
{
	return to_millionths((from_millionths(millionths) - offset) / scale);
}

static float edge_push(float p)
{
	float d = fabsf(p - 0.5f);

	if (d > 0.35f) {
		float e = (d - 0.35f) * 7.0f * (d - 0.35f);

		p = p > 0.5f ? p + e : p - e;
	}
	return p;
}

int32_t opensc2_pad_edge(int32_t millionths)
{
	return to_millionths(edge_push(from_millionths(millionths)));
}

void opensc2_pad_centroid(int32_t *x, int32_t *y, bool stretch, const float offset_scale[4])
{
	float px = edge_push(from_millionths(*x));
	float py = edge_push(from_millionths(*y));

	if (stretch && offset_scale) {
		px = (px - offset_scale[0]) / offset_scale[1];
		py = (py - offset_scale[2]) / offset_scale[3];
	}
	*x = to_millionths(px);
	*y = to_millionths(py);
}

int32_t opensc2_pad_stretch(enum opensc2_side side, int axis, int32_t millionths)
{
	/* [side][axis]: offset, scale */
	static const float offset[2][2] = { { -0.01f, 0.05f }, { 0.11f, 0.05f } };
	static const float scale[2][2] = { { 0.90f, 0.91f }, { 0.88f, 0.91f } };
	int s = side == OPENSC2_RIGHT ? 1 : 0;
	int a = axis ? 1 : 0;

	return opensc2_pad_stretch_by(millionths, offset[s][a], scale[s][a]);
}

/* ---- trackpads: the pad scale and after ------------------------------------------------------- */

int16_t opensc2_pad_scale(int32_t millionths)
{
	float p = from_millionths(millionths);

	return (int16_t)clamp32(osc2_f32_to_s32((p - 0.5f) * 2.0f * (float)PAD_HALF), -PAD_HALF,
				PAD_HALF);
}

void opensc2_pad_filter_reset(struct opensc2_pad_filter *f)
{
	memset(f, 0, sizeof(*f));
}

#define FILTER_MIN_CUTOFF  0.001f
#define FILTER_BETA        0.1f
#define FILTER_D_CUTOFF    1.2f

/* A one-pole stage's factor at `cutoff` Hz: 1 / (1 + tau / te), tau = 1 / (2 pi f), te = 1 / rate. */
static float smoothing(float cutoff, float rate)
{
	float tau = 1.0f / (cutoff * 6.28318530718f);
	float te = 1.0f / rate;

	return 1.0f / (1.0f + tau / te);
}

/* y = alpha x + (1 - alpha) y, the first sample taken as y first (the add fused). */
static float one_pole(float *y, bool *started, float x, float alpha)
{
	if (!*started) {
		*started = true;
		*y = x;
	}
	*y = fmaf(x, alpha, (1.0f - alpha) * *y);
	return *y;
}

static float filter_axis(struct opensc2_pad_filter_axis *a, float x, float rate)
{
	float speed = a->started ? (x - a->last) * rate : 0.0f;
	float cutoff;

	speed = one_pole(&a->speed, &a->speed_started, speed, smoothing(FILTER_D_CUTOFF, rate));
	cutoff = fmaf(FILTER_BETA, fabsf(speed), FILTER_MIN_CUTOFF);
	if (a->cutoff > cutoff && a->cutoff * 0.99f >= cutoff) {
		cutoff = a->cutoff * 0.99f;   /* a falling cut-off: at most 1 % per sample */
	}
	a->cutoff = cutoff;
	a->last = x;
	return one_pole(&a->value, &a->started, x, smoothing(cutoff, rate));
}

void opensc2_pad_filter_step(struct opensc2_pad_filter *f, int16_t *x, int16_t *y, float rate_hz)
{
	float rate = rate_hz > 0.0f ? rate_hz : 250.0f;

	*x = (int16_t)osc2_f32_to_s32(filter_axis(&f->axis[0], (float)*x, rate));
	*y = (int16_t)osc2_f32_to_s32(filter_axis(&f->axis[1], (float)*y, rate));
}

/* The rotation's sine and cosine x 8192, truncated, worked out as the real controller's table was:
 * with pi taken as 3.141592 (so 30 degrees give 4095 and 90 degrees 8191). */
#define TABLE_PI 3.141592

static int32_t sin_entry(int32_t degrees)
{
	return (int32_t)(8192.0 * sin((double)degrees * TABLE_PI / 180.0));
}

static int32_t cos_entry(int32_t degrees)
{
	return (int32_t)(8192.0 * cos((double)degrees * TABLE_PI / 180.0));
}

int32_t opensc2_sin8192(int32_t degrees)
{
	int32_t d = degrees % 360;

	return sin_entry(d < 0 ? d + 360 : d);
}

int32_t opensc2_cos8192(int32_t degrees)
{
	int32_t d = degrees % 360;

	return cos_entry(d < 0 ? d + 360 : d);
}

/* The real controller's lookup for an angle of -360..359 degrees: its 1 degree entries for 0..4
 * (and 356..359 mirrored, the sine negated), else its 5 degree entries (angle / 5). */
static void rotation_sincos(int32_t angle, int32_t *s, int32_t *c)
{
	if (angle < 0) {
		angle += 360;
	}
	if (angle < 5 || angle > 355) {
		if (angle <= 4) {
			*s = sin_entry(angle);
			*c = cos_entry(angle);
		} else {
			*s = -sin_entry(360 - angle);
			*c = cos_entry(360 - angle);
		}
		return;
	}
	angle = angle / 5 * 5;
	*s = sin_entry(angle);
	*c = cos_entry(angle);
}

int16_t opensc2_pad_rotation_angle(enum opensc2_side side, int16_t setting2)
{
	int32_t a = clamp32(setting2, 0, 360);

	if (side == OPENSC2_LEFT) {
		a = 360 - a;   /* the left pad turns the other way */
	}
	if (a >= 360) {
		return 0;
	}
	if (a <= 4 || a >= 356) {
		return (int16_t)a;   /* 1 degree steps from -4 to 4 */
	}
	return (int16_t)(a - a % 5);   /* 5 degree steps, rounded down */
}

bool opensc2_pad_rotate(enum opensc2_side side, int16_t setting2, int16_t setting24, int32_t *x,
			int32_t *y)
{
	int32_t angle, s, c;
	int64_t x0 = *x, y0 = *y;

	if (setting2 == 0 || setting24 == 0) {
		return false;
	}
	/* the right pad by the angle, the left by minus it */
	angle = clamp32(setting2, -360, 360);
	rotation_sincos(side == OPENSC2_LEFT ? -angle : angle, &s, &c);
	*x = (int32_t)((c * x0 + s * y0) / 8192);
	*y = (int32_t)((c * y0 - s * x0) / 8192);
	return true;
}

void opensc2_stabiliser_reset(struct opensc2_stabiliser *s)
{
	memset(s, 0, sizeof(*s));
}

/* Another eighth of the gap, rounded down, plus one unit toward the thumb. */
static int32_t close_gap(int32_t gap)
{
	if (gap > 0) {
		return (gap >> 3) + 1;
	}
	if (gap < 0) {
		return (gap >> 3) - 1;   /* an arithmetic shift rounds down: one unit over at most */
	}
	return 0;
}

void opensc2_stabiliser_step(struct opensc2_stabiliser *s, uint32_t hold_radius, int32_t *x,
			     int32_t *y)
{
	int32_t in[2] = { *x, *y };
	bool moved;

	if (!s->started) {
		for (int i = 0; i < 2; i++) {
			s->anchor[i] = s->out[i] = s->prev[i] = in[i];
		}
		s->started = true;
		s->following = false;
		return;
	}
	if (!s->following) {
		if (opensc2_approx_distance(in[0] - s->anchor[0], in[1] - s->anchor[1]) <= hold_radius) {
			s->prev[0] = in[0];
			s->prev[1] = in[1];
			*x = s->out[0];
			*y = s->out[1];
			return;   /* held at the touch point */
		}
		s->following = true;
	}
	moved = in[0] != s->prev[0] || in[1] != s->prev[1];
	for (int i = 0; i < 2; i++) {
		s->out[i] += in[i] - s->prev[i];
		if (moved) {
			s->out[i] += close_gap(in[i] - s->out[i]);
		}
		s->out[i] = clamp32(s->out[i], -REPORT_FULL, REPORT_FULL);
		s->prev[i] = in[i];
	}
	*x = s->out[0];
	*y = s->out[1];
}

/* ---- IMU ------------------------------------------------------------------------------------- */

/* toward zero to 32 bits, then the low 16 bits, as the real controller stores its IMU values */
static int16_t low16(float v)
{
	return (int16_t)(uint16_t)(uint32_t)osc2_f32_to_s32(v);
}

int16_t opensc2_imu_accel_from_ms2(float ms2)
{
	return low16(ms2 / 19.6133f * 32767.0f);
}

int16_t opensc2_imu_gyro_from_rads(float rads)
{
	return low16(rads * 180.0f / 3.14159265f * 32767.0f / 2000.0f);
}

int16_t opensc2_imu_quat_from_unit(float q)
{
	return low16(q * 32767.0f);
}

void opensc2_imu_mount(const int8_t m[9], const int16_t in[3], int16_t out[3])
{
	int32_t v[3];

	for (int r = 0; r < 3; r++) {
		v[r] = m[3 * r] * in[0] + m[3 * r + 1] * in[1] + m[3 * r + 2] * in[2];
	}
	for (int r = 0; r < 3; r++) {
		out[r] = (int16_t)(uint16_t)(uint32_t)v[r];
	}
}

void opensc2_imu_mount_quat(const int8_t m[9], const int16_t in[4], int16_t out[4])
{
	int16_t xyz[3] = { in[1], in[2], in[3] };

	out[0] = in[0];
	opensc2_imu_mount(m, xyz, &out[1]);
}

int16_t opensc2_imu_dead_zone(int16_t v, int32_t t)
{
	return (v > -t && v < t) ? 0 : v;
}

void opensc2_gyro_bias_init(struct opensc2_gyro_bias *b, const float start[3])
{
	for (int i = 0; i < 3; i++) {
		b->bias[i] = start ? start[i] : 0.0f;
	}
	b->still = 0;
}

void opensc2_gyro_bias_update(struct opensc2_gyro_bias *b, const float gyro[3], float threshold,
			      uint16_t settle)
{
	bool still = true;

	for (int i = 0; i < 3; i++) {
		if (!(fabsf(gyro[i] - b->bias[i]) < threshold)) {
			still = false;   /* NaN counts as moving */
		}
	}
	if (!still) {
		b->still = 0;
		return;
	}
	if (b->still < settle) {
		b->still++;
		return;
	}
	for (int i = 0; i < 3; i++) {
		b->bias[i] += (gyro[i] - b->bias[i]) / 256.0f;
	}
}

/* ---- buttons --------------------------------------------------------------------------------- */

void opensc2_button_hold_init(struct opensc2_button_hold *h, uint32_t word)
{
	memset(h, 0, sizeof(*h));
	h->word = word;
}

uint32_t opensc2_button_hold_step(struct opensc2_button_hold *h, uint32_t level, uint32_t now_ms)
{
	uint32_t changed;

	/* holds that are over: the pin counts again */
	for (int b = 0; b < 32; b++) {
		if ((h->holding & (1UL << b)) && (int32_t)(now_ms - h->until[b]) >= 0) {
			h->holding &= ~(1UL << b);
		}
	}
	changed = (level ^ h->word) & ~h->holding;
	h->word ^= changed;
	for (int b = 0; b < 32; b++) {
		if (changed & (1UL << b)) {
			h->holding |= 1UL << b;
			h->until[b] = now_ms + OPENSC2_BUTTON_HOLD_MS;
		}
	}
	return h->word;
}

uint32_t opensc2_button_hold_next(const struct opensc2_button_hold *h, uint32_t now_ms)
{
	uint32_t next = 0;

	for (int b = 0; b < 32; b++) {
		if (h->holding & (1UL << b)) {
			int32_t left = (int32_t)(h->until[b] - now_ms);
			uint32_t ms = left < 1 ? 1u : (uint32_t)left;

			next = next == 0 || ms < next ? ms : next;
		}
	}
	return next;
}

/* ---- ADC bursts ------------------------------------------------------------------------------ */

void opensc2_adc_average(const int16_t *samples, uint8_t channels, uint8_t scans, uint16_t *out)
{
	for (uint8_t ch = 0; ch < channels; ch++) {
		int32_t sum = 0;

		for (uint8_t s = 0; s < scans; s++) {
			sum += samples[s * channels + ch];
		}
		out[ch] = (uint16_t)(uint32_t)(scans ? sum / scans : 0);
	}
}
