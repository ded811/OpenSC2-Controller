/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * sensor_helpers.c: the sensor helpers of <opensc2/sensor_helpers.h>, from the rules in the protocol
 * documentation (not public yet; docs/protocol.md lists the pages). Pure logic; only the position
 * filter, the IMU scaling and the bias estimate use floating point.
 */
#include <math.h>
#include <string.h>
#include <opensc2/sensor_helpers.h>

#define PAD_HALF     16383
#define REPORT_FULL  32767
#define MILLION      1000000

static int32_t clamp32(int64_t v, int32_t lo, int32_t hi)
{
	return v < lo ? lo : v > hi ? hi : (int32_t)v;
}

static int32_t abs32(int32_t v)
{
	return v < 0 ? -v : v;
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
	int32_t mean;

	if (!t->primed) {
		t->last = word;   /* the first word fills both */
		t->primed = true;
	}
	mean = (t->last + word) / 2;
	t->last = word;
	if (mean > on_level) {
		t->on = true;
	} else if (mean < off_level) {
		t->on = false;
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
	/* (s / 2 + 12.5) % of 1800 = 9 s + 225: a whole number for every s */
	return 9 * clamp32(setting34, 25, 400) + 225;
}

int32_t opensc2_grip_off_level(int16_t setting34, int16_t setting35)
{
	return opensc2_grip_on_level(setting34) * clamp32(setting35, 25, 100) / 100;
}

/* ---- trackpads: touch and position before the pad scale -------------------------------------- */

void opensc2_pad_touch_reset(struct opensc2_pad_touch *t)
{
	t->touched = false;
	t->average = 0;
	t->x = t->y = OPENSC2_PAD_CENTRE;
}

/* The way from `from` toward `to` while the level lies in the band (off, top): a fraction rising
 * from 0.2 at the off level to 1 at the top, (2 D + 8 (level - off)) / (10 D). */
static int32_t damp(int32_t from, int32_t to, int32_t level, int32_t off, int32_t top)
{
	int64_t d = (int64_t)top - off;

	return from + (int32_t)(((int64_t)to - from) * (2 * d + 8 * ((int64_t)level - off)) / (10 * d));
}

bool opensc2_pad_touch_step(struct opensc2_pad_touch *t, int32_t level, int16_t setting72,
			    int16_t setting73, int32_t *x, int32_t *y)
{
	int32_t on = clamp32(setting72, 0, 16000);
	int32_t off = clamp32(setting73, 0, 16000);
	int32_t top;

	if (!t->touched) {
		if (level <= on) {
			*x = *y = OPENSC2_PAD_CENTRE;
			return false;
		}
		t->touched = true;
		t->average = level;   /* the running average starts at the touch-down level */
		t->x = *x;
		t->y = *y;
		return true;
	}
	if (level <= off) {
		opensc2_pad_touch_reset(t);
		*x = *y = OPENSC2_PAD_CENTRE;
		return false;
	}
	top = t->average - 200;
	if (level < top) {
		*x = damp(t->x, *x, level, off, top);
		*y = damp(t->y, *y, level, off, top);
	}
	t->x = *x;
	t->y = *y;
	t->average = (7 * t->average + level) / 8;
	return true;
}

int32_t opensc2_pad_edge(int32_t millionths)
{
	int32_t from_centre = millionths - OPENSC2_PAD_CENTRE;
	int32_t beyond = abs32(from_centre) - 350000;
	int32_t push;

	if (beyond <= 0) {
		return millionths;
	}
	/* 7 x (d - 0.35)^2, in millionths */
	push = (int32_t)((int64_t)7 * beyond * beyond / MILLION);
	return from_centre < 0 ? millionths - push : millionths + push;
}

int32_t opensc2_pad_stretch(enum opensc2_side side, int axis, int32_t millionths)
{
	/* [side][axis]: offset in millionths, scale in percent */
	static const int32_t offset[2][2] = { { -10000, 50000 }, { 110000, 50000 } };
	static const int32_t scale[2][2] = { { 90, 91 }, { 88, 91 } };
	int s = side == OPENSC2_RIGHT ? 1 : 0;
	int a = axis ? 1 : 0;

	return (int32_t)(((int64_t)millionths - offset[s][a]) * 100 / scale[s][a]);
}

/* ---- trackpads: the pad scale and after ------------------------------------------------------- */

int16_t opensc2_pad_scale(int32_t millionths)
{
	int64_t v = ((int64_t)millionths - OPENSC2_PAD_CENTRE) * (2 * PAD_HALF) / MILLION;

	return (int16_t)clamp32(v, -PAD_HALF, PAD_HALF);
}

void opensc2_pad_filter_reset(struct opensc2_pad_filter *f)
{
	memset(f, 0, sizeof(*f));
}

/* A one-pole stage at `cutoff` Hz: the factor 1 / (1 + rate / (2 pi f)). */
static float smoothing(float cutoff, float rate)
{
	return 1.0f / (1.0f + rate / (6.28318531f * cutoff));
}

void opensc2_pad_filter_step(struct opensc2_pad_filter *f, int16_t *x, int16_t *y, float rate_hz)
{
	int16_t *v[2] = { x, y };
	float rate = rate_hz > 0.0f ? rate_hz : 250.0f;

	if (!f->started) {
		for (int i = 0; i < 2; i++) {
			f->out[i] = (float)*v[i];   /* the first sample passes unchanged */
			f->speed[i] = 0.0f;
			f->cutoff[i] = 0.001f;
		}
		f->started = true;
		return;
	}
	for (int i = 0; i < 2; i++) {
		float in = (float)*v[i];
		float speed = (in - f->out[i]) * rate;
		float cutoff;

		f->speed[i] += smoothing(1.2f, rate) * (speed - f->speed[i]);
		cutoff = 0.001f + 0.1f * fabsf(f->speed[i]);
		if (cutoff < 0.99f * f->cutoff[i]) {
			cutoff = 0.99f * f->cutoff[i];   /* a falling cut-off: at most 1 % per sample */
		}
		f->cutoff[i] = cutoff;
		f->out[i] += smoothing(cutoff, rate) * (in - f->out[i]);
		*v[i] = (int16_t)clamp32((int64_t)f->out[i], -REPORT_FULL, REPORT_FULL);
	}
}

/* sin(d) x 8192 for d = 0..90, truncated toward zero (30 and 90 degrees are exact). */
static const int16_t sine8192[91] = {
	0,    142,  285,  428,  571,  713,  856,  998,  1140, 1281, 1422, 1563, 1703, 1842,
	1981, 2120, 2258, 2395, 2531, 2667, 2801, 2935, 3068, 3200, 3331, 3462, 3591, 3719,
	3845, 3971, 4096, 4219, 4341, 4461, 4580, 4698, 4815, 4930, 5043, 5155, 5265, 5374,
	5481, 5586, 5690, 5792, 5892, 5991, 6087, 6182, 6275, 6366, 6455, 6542, 6627, 6710,
	6791, 6870, 6947, 7021, 7094, 7164, 7233, 7299, 7362, 7424, 7483, 7540, 7595, 7647,
	7697, 7745, 7791, 7834, 7874, 7912, 7948, 7982, 8012, 8041, 8067, 8091, 8112, 8130,
	8147, 8160, 8172, 8180, 8187, 8190, 8192,
};

int32_t opensc2_sin8192(int32_t degrees)
{
	int32_t d = degrees % 360;

	if (d < 0) {
		d += 360;
	}
	if (d <= 90) {
		return sine8192[d];
	}
	if (d <= 180) {
		return sine8192[180 - d];
	}
	if (d <= 270) {
		return -sine8192[d - 180];
	}
	return -sine8192[360 - d];
}

int32_t opensc2_cos8192(int32_t degrees)
{
	return opensc2_sin8192(degrees + 90);
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
	angle = opensc2_pad_rotation_angle(side, setting2);
	s = opensc2_sin8192(angle);
	c = opensc2_cos8192(angle);
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

static int16_t saturate_trunc(float v)
{
	if (!(v > -32767.0f)) {
		return v != v ? 0 : -REPORT_FULL;   /* NaN reads 0 */
	}
	if (v > 32767.0f) {
		return REPORT_FULL;
	}
	return (int16_t)v;   /* toward zero */
}

int16_t opensc2_imu_accel_from_ms2(float ms2)
{
	return saturate_trunc(ms2 / 19.6133f * 32767.0f);
}

int16_t opensc2_imu_gyro_from_rads(float rads)
{
	return saturate_trunc(rads * 57.2957795f * (32767.0f / 2000.0f));
}

void opensc2_imu_mount(const int8_t m[9], const int16_t in[3], int16_t out[3])
{
	int32_t v[3];

	for (int r = 0; r < 3; r++) {
		v[r] = m[3 * r] * in[0] + m[3 * r + 1] * in[1] + m[3 * r + 2] * in[2];
	}
	for (int r = 0; r < 3; r++) {
		out[r] = (int16_t)clamp32(v[r], -REPORT_FULL, REPORT_FULL);
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
		b->bias[i] += (gyro[i] - b->bias[i]) / 64.0f;
	}
}

/* ---- ADC bursts ------------------------------------------------------------------------------ */

void opensc2_adc_average(const int16_t *samples, uint8_t channels, uint8_t scans, uint16_t *out)
{
	for (uint8_t ch = 0; ch < channels; ch++) {
		int32_t sum = 0;

		for (uint8_t s = 0; s < scans; s++) {
			sum += samples[s * channels + ch];
		}
		out[ch] = (uint16_t)clamp32(scans ? sum / scans : 0, 0, 4095);
	}
}
