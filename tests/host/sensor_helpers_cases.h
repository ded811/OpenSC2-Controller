/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: the sensor helpers of <opensc2/sensor_helpers.h> (lib/opensc2/sensor_helpers.c)
 * against values worked out from the documented rules outside this code: the distance
 * approximation, the capacitive two-word means and levels, a pad's touch with its lift-off
 * damping, the edge push, stretch and scale, the rotation and its angle steps, the touch
 * stabiliser, the position filter's behaviour, the IMU's scaling, mounting, dead zone and bias
 * estimate, and an ADC burst's average.
 */
#include <math.h>
#include <string.h>
#include <opensc2/sensor_helpers.h>

OPENSC2_TEST(sensors_distance)
{
	CHECK_EQ(opensc2_approx_distance(0, 0), 0);
	CHECK_EQ(opensc2_approx_distance(1000, 0), 983);    /* 1.7 % short along an axis */
	CHECK_EQ(opensc2_approx_distance(0, -1000), 983);
	CHECK_EQ(opensc2_approx_distance(1000, 1000), 1375);   /* 2.8 % short on a diagonal */
	/* the fine hold radius 4369: about 4440 along an axis, 4490 on a diagonal */
	CHECK_EQ(opensc2_approx_distance(4440, 0), 4366);
	CHECK_EQ(opensc2_approx_distance(3176, 3176), 4367);
	/* the stick-touch fallback's 12000: about 12200 along an axis, 12340 on a diagonal */
	CHECK_EQ(opensc2_approx_distance(12200, 0), 11997);
	CHECK_EQ(opensc2_approx_distance(8726, 8726), 11998);
	/* the 40 M correction applies only while M < 16 m */
	CHECK_EQ(opensc2_approx_distance(16, 1), 16);
	CHECK_EQ(opensc2_approx_distance(15, 1), 15);
	CHECK_EQ(opensc2_approx_distance(100, 7), 97);
	CHECK_EQ(opensc2_approx_distance(-32767, -32767), opensc2_approx_distance(32767, 32767));
}

OPENSC2_TEST(sensors_capacitive)
{
	static const int32_t words[] = { 600, 0, 1000, 450, 420, 380, 380, 700 };
	static const bool bits[] = { true, false, false, true, true, true, false, true };
	struct opensc2_cap_touch t;

	/* the defaults: stick touch on above 500, held while at least 400 */
	CHECK_EQ(opensc2_stick_touch_on_level(500), 500);
	CHECK_EQ(opensc2_stick_touch_off_level(400), 400);
	CHECK_EQ(opensc2_stick_touch_on_level(5), 100);       /* limited to 100..1000 */
	CHECK_EQ(opensc2_stick_touch_on_level(2000), 1000);
	CHECK_EQ(opensc2_stick_touch_off_level(-3), 0);       /* 0..800 */
	CHECK_EQ(opensc2_stick_touch_off_level(900), 800);
	opensc2_cap_reset(&t);
	/* means 600 (the first word fills both), 300, 500, 725, 435, 400, 380, 540 */
	for (size_t i = 0; i < ARRAY_SIZE(words); i++) {
		CHECK_EQ(opensc2_cap_sample(&t, words[i], 500, 400), bits[i]);
	}
	/* a reset starts the mean over */
	opensc2_cap_reset(&t);
	CHECK(!opensc2_cap_sample(&t, 450, 500, 400));

	/* grip: (s34 / 2 + 12.5) % of 1800, off s35 % of that */
	CHECK_EQ(opensc2_grip_on_level(100), 1125);
	CHECK_EQ(opensc2_grip_on_level(25), 450);
	CHECK_EQ(opensc2_grip_on_level(400), 3825);
	CHECK_EQ(opensc2_grip_on_level(1000), 3825);   /* limited to 25..400 */
	CHECK_EQ(opensc2_grip_on_level(0), 450);
	CHECK_EQ(opensc2_grip_off_level(100, 80), 900);
	CHECK_EQ(opensc2_grip_off_level(100, 25), 281);
	CHECK_EQ(opensc2_grip_off_level(100, 100), 1125);
	CHECK_EQ(opensc2_grip_off_level(100, 7), 281);    /* 35: 25..100 */
}

OPENSC2_TEST(sensors_pad_touch)
{
	/* level, position in, then touched and the position out (settings 72 = 1300, 73 = 1000) */
	static const struct {
		int32_t level, x, y;
		bool touched;
		int32_t ox, oy;
	} f[] = {
		{ 1200, 100000, 100000, false, 500000, 500000 },   /* not above 1300 */
		{ 2000, 600000, 400000, true, 600000, 400000 },    /* touch-down: the average starts */
		{ 2000, 610000, 390000, true, 610000, 390000 },
		{ 1500, 700000, 300000, true, 673000, 327000 },    /* under the average - 200: damped */
		{ 1100, 800000, 200000, true, 712185, 287815 },
		{ 1050, 900000, 100000, true, 761635, 238365 },
		{ 900, 0, 0, false, 500000, 500000 },              /* not above 1000: lifted */
		{ 1400, 550000, 450000, true, 550000, 450000 },
	};
	struct opensc2_pad_touch t;

	opensc2_pad_touch_reset(&t);
	for (size_t i = 0; i < ARRAY_SIZE(f); i++) {
		int32_t x = f[i].x, y = f[i].y;

		CHECK_EQ(opensc2_pad_touch_step(&t, f[i].level, 1300, 1000, &x, &y), f[i].touched);
		CHECK_EQ(x, f[i].ox);
		CHECK_EQ(y, f[i].oy);
	}
}

OPENSC2_TEST(sensors_pad_position)
{
	/* the edge push: 7 (d - 0.35)^2 beyond 0.35 from the centre */
	CHECK_EQ(opensc2_pad_edge(500000), 500000);
	CHECK_EQ(opensc2_pad_edge(850000), 850000);
	CHECK_EQ(opensc2_pad_edge(900000), 917500);
	CHECK_EQ(opensc2_pad_edge(1000000), 1157500);   /* 0.1575 at the very edge */
	CHECK_EQ(opensc2_pad_edge(0), -157500);
	CHECK_EQ(opensc2_pad_edge(100000), 82500);
	CHECK_EQ(opensc2_pad_edge(150000), 150000);
	/* the stretch's per-pad offsets and scales */
	CHECK_EQ(opensc2_pad_stretch(OPENSC2_LEFT, 0, 500000), 566666);
	CHECK_EQ(opensc2_pad_stretch(OPENSC2_RIGHT, 0, 500000), 443181);
	CHECK_EQ(opensc2_pad_stretch(OPENSC2_RIGHT, 1, 500000), 494505);
	CHECK_EQ(opensc2_pad_stretch(OPENSC2_LEFT, 1, 0), -54945);
	CHECK_EQ(opensc2_pad_stretch(OPENSC2_RIGHT, 0, 1000000), 1011363);
	/* the pad scale: (p - 0.5) x 2 x 16383, truncated, limited */
	CHECK_EQ(opensc2_pad_scale(0), -16383);
	CHECK_EQ(opensc2_pad_scale(500000), 0);
	CHECK_EQ(opensc2_pad_scale(1000000), 16383);
	CHECK_EQ(opensc2_pad_scale(750000), 8191);
	CHECK_EQ(opensc2_pad_scale(1200000), 16383);
	CHECK_EQ(opensc2_pad_scale(-10), -16383);
	CHECK_EQ(opensc2_pad_scale(500030), 0);
	CHECK_EQ(opensc2_pad_scale(499969), -1);
}

OPENSC2_TEST(sensors_pad_rotation)
{
	/* the angle steps: 1 degree from -4 to 4, else rounded down to 5; the left pad the other
	 * way (setting 7: +5 and -10 degrees) */
	static const int16_t s2[] = { 1, 3, 4, 5, 6, 7, 30, 90, 355, 356, 360 };
	static const int16_t right[] = { 1, 3, 4, 5, 5, 5, 30, 90, 355, 356, 0 };
	static const int16_t left[] = { 359, 357, 356, 355, 350, 350, 330, 270, 5, 4, 0 };
	int32_t x, y;

	for (size_t i = 0; i < ARRAY_SIZE(s2); i++) {
		CHECK_EQ(opensc2_pad_rotation_angle(OPENSC2_RIGHT, s2[i]), right[i]);
		CHECK_EQ(opensc2_pad_rotation_angle(OPENSC2_LEFT, s2[i]), left[i]);
	}
	CHECK_EQ(opensc2_pad_rotation_angle(OPENSC2_RIGHT, 400), 0);   /* limited to 0..360 */
	CHECK_EQ(opensc2_sin8192(0), 0);
	CHECK_EQ(opensc2_sin8192(1), 142);
	CHECK_EQ(opensc2_sin8192(30), 4096);
	CHECK_EQ(opensc2_sin8192(45), 5792);
	CHECK_EQ(opensc2_sin8192(89), 8190);
	CHECK_EQ(opensc2_sin8192(90), 8192);
	CHECK_EQ(opensc2_sin8192(91), 8190);
	CHECK_EQ(opensc2_sin8192(181), -142);
	CHECK_EQ(opensc2_sin8192(270), -8192);
	CHECK_EQ(opensc2_sin8192(300), -7094);
	CHECK_EQ(opensc2_sin8192(-1), -142);
	CHECK_EQ(opensc2_sin8192(450), 8192);
	CHECK_EQ(opensc2_cos8192(0), 8192);
	CHECK_EQ(opensc2_cos8192(60), 4096);

	/* off unless 2 and 24 are both non-zero */
	x = 1000, y = 0;
	CHECK(!opensc2_pad_rotate(OPENSC2_RIGHT, 0, 20, &x, &y));
	CHECK(!opensc2_pad_rotate(OPENSC2_RIGHT, 90, 0, &x, &y));
	CHECK_EQ(x, 1000);
	CHECK(opensc2_pad_rotate(OPENSC2_RIGHT, 90, 20, &x, &y));
	CHECK_EQ(x, 0);
	CHECK_EQ(y, -1000);
	x = 0, y = 1000;
	CHECK(opensc2_pad_rotate(OPENSC2_RIGHT, 90, 1, &x, &y));
	CHECK_EQ(x, 1000);
	CHECK_EQ(y, 0);
	x = 1000, y = 0;
	CHECK(opensc2_pad_rotate(OPENSC2_LEFT, 90, 1, &x, &y));
	CHECK_EQ(x, 0);
	CHECK_EQ(y, 1000);
	x = 16383, y = 0;
	CHECK(opensc2_pad_rotate(OPENSC2_RIGHT, 30, 1, &x, &y));
	CHECK_EQ(x, 14187);
	CHECK_EQ(y, -8191);
	x = 10000, y = -5000;
	CHECK(opensc2_pad_rotate(OPENSC2_LEFT, 7, 1, &x, &y));
	CHECK_EQ(x, 10715);
	CHECK_EQ(y, -3187);
	x = 10000, y = -5000;
	CHECK(opensc2_pad_rotate(OPENSC2_RIGHT, 7, 1, &x, &y));
	CHECK_EQ(x, 9525);
	CHECK_EQ(y, -5850);
	x = -16383, y = 16383;
	CHECK(opensc2_pad_rotate(OPENSC2_RIGHT, 45, 1, &x, &y));
	CHECK_EQ(x, 0);
	CHECK_EQ(y, 23166);
	x = 1234, y = -4321;
	CHECK(opensc2_pad_rotate(OPENSC2_RIGHT, 360, 1, &x, &y));   /* 360: no rotation */
	CHECK_EQ(x, 1234);
	CHECK_EQ(y, -4321);
}

OPENSC2_TEST(sensors_stabiliser)
{
	static const int32_t in[][2] = {
		{ 1000, 1000 }, { 2000, 1000 }, { 4000, 1000 }, { 5400, 1000 },   /* held */
		{ 5500, 1000 },   /* beyond 4369: follows from here */
		{ 6500, 1000 }, { 8500, 1000 }, { 8500, 1000 }, { 8500, 1000 },  /* stopped: gap stays */
		{ 9500, -2000 }, { 9500, -2000 },
	};
	static const int32_t out[][2] = {
		{ 1000, 1000 }, { 1000, 1000 }, { 1000, 1000 }, { 1000, 1000 }, { 1651, 1000 },
		{ 3133, 1000 }, { 5554, 1000 }, { 5554, 1000 }, { 5554, 1000 }, { 6923, -2000 },
		{ 6923, -2000 },
	};
	static const int32_t neg_in[] = { 0, -4000, -4500, -4501, -4501, -6000 };
	static const int32_t neg_out[] = { 0, 0, -1001, -1441, -1441, -3324 };
	struct opensc2_stabiliser s;

	opensc2_stabiliser_reset(&s);
	for (size_t i = 0; i < ARRAY_SIZE(in); i++) {
		int32_t x = in[i][0], y = in[i][1];

		opensc2_stabiliser_step(&s, OPENSC2_STABILISER_FINE, &x, &y);
		CHECK_EQ(x, out[i][0]);
		CHECK_EQ(y, out[i][1]);
	}
	/* on the negative side the eighth rounds down */
	opensc2_stabiliser_reset(&s);
	for (size_t i = 0; i < ARRAY_SIZE(neg_in); i++) {
		int32_t x = neg_in[i], y = 0;

		opensc2_stabiliser_step(&s, OPENSC2_STABILISER_FINE, &x, &y);
		CHECK_EQ(x, neg_out[i]);
		CHECK_EQ(y, 0);
	}
	/* the coarse radius holds further (8800 reads 8654) */
	opensc2_stabiliser_reset(&s);
	for (int i = 0; i < 3; i++) {
		int32_t x = i == 0 ? 0 : i == 1 ? 8000 : 8800, y = 0;

		opensc2_stabiliser_step(&s, OPENSC2_STABILISER_COARSE, &x, &y);
		CHECK_EQ(x, 0);
	}
	/* a reset takes the next sample as the new touch point */
	opensc2_stabiliser_reset(&s);
	int32_t x = -300, y = 200;

	opensc2_stabiliser_step(&s, OPENSC2_STABILISER_FINE, &x, &y);
	CHECK_EQ(x, -300);
	CHECK_EQ(y, 200);
}

OPENSC2_TEST(sensors_pad_filter)
{
	struct opensc2_pad_filter f;
	int16_t x, y;

	/* the first sample passes unchanged */
	opensc2_pad_filter_reset(&f);
	x = 1234, y = -4321;
	opensc2_pad_filter_step(&f, &x, &y, 0);
	CHECK_EQ(x, 1234);
	CHECK_EQ(y, -4321);
	/* a resting thumb: unchanged */
	for (int i = 0; i < 20; i++) {
		x = 1234, y = -4321;
		opensc2_pad_filter_step(&f, &x, &y, 0);
	}
	CHECK_EQ(x, 1234);
	CHECK_EQ(y, -4321);
	/* jitter of one unit around a resting thumb is almost frozen */
	for (int i = 1; i <= 50; i++) {
		x = (int16_t)(1234 + ((i & 1) ? 1 : -1)), y = (int16_t)(-4321 + ((i & 2) ? 1 : -1));
		opensc2_pad_filter_step(&f, &x, &y, 0);
		CHECK(x >= 1233 && x <= 1235);
		CHECK(y >= -4322 && y <= -4320);
	}
	/* a steady movement (250 units a second) is followed with a short lag */
	for (int i = 1; i <= 100; i++) {
		x = (int16_t)(1234 + i), y = -4321;
		opensc2_pad_filter_step(&f, &x, &y, 0);
	}
	CHECK(x > 1334 - 10);
	CHECK(x <= 1334);
	/* a fast swipe goes through with little lag */
	for (int i = 1; i <= 10; i++) {
		x = (int16_t)(1334 + 1000 * i), y = -4321;
		opensc2_pad_filter_step(&f, &x, &y, 0);
	}
	CHECK(x > 11334 - 1000);
	CHECK(x <= 11334);
	/* the cut-off falls by at most 1 % per sample */
	float before = f.cutoff[0];

	x = 11334, y = -4321;
	opensc2_pad_filter_step(&f, &x, &y, 0);
	CHECK(f.cutoff[0] >= 0.99f * before - 1e-6f);
	/* a reset starts over */
	opensc2_pad_filter_reset(&f);
	x = -16383, y = 16383;
	opensc2_pad_filter_step(&f, &x, &y, 0);
	CHECK_EQ(x, -16383);
	CHECK_EQ(y, 16383);
}

OPENSC2_TEST(sensors_imu)
{
	static const int8_t board[9] = { 0, 1, 0, -1, 0, 0, 0, 0, 1 };
	static const int8_t sum[9] = { 1, 1, 0, 0, 1, 0, 0, 0, -1 };
	const int16_t v[3] = { 100, 200, 300 };
	const int16_t q[4] = { 32767, 10, 20, 30 };
	const int16_t big[3] = { 30000, 30000, -32767 };
	int16_t out[4];

	CHECK_EQ(opensc2_imu_accel_from_ms2(9.80665f), 16383);   /* 1 g */
	CHECK_EQ(opensc2_imu_accel_from_ms2(-19.6133f), -32767);
	CHECK_EQ(opensc2_imu_accel_from_ms2(100.0f), 32767);
	CHECK_EQ(opensc2_imu_accel_from_ms2(-100.0f), -32767);
	CHECK_EQ(opensc2_imu_accel_from_ms2(NAN), 0);
	CHECK_EQ(opensc2_imu_gyro_from_rads(1.0f), 938);
	CHECK_EQ(opensc2_imu_gyro_from_rads(-1.0f), -938);       /* toward zero */
	CHECK_EQ(opensc2_imu_gyro_from_rads(-40.0f), -32767);

	opensc2_imu_mount(board, v, out);
	CHECK_EQ(out[0], 200);    /* x' = y */
	CHECK_EQ(out[1], -100);   /* y' = -x */
	CHECK_EQ(out[2], 300);
	opensc2_imu_mount_quat(board, q, out);
	CHECK_EQ(out[0], 32767);  /* w stays */
	CHECK_EQ(out[1], 20);
	CHECK_EQ(out[2], -10);
	CHECK_EQ(out[3], 30);
	opensc2_imu_mount(sum, big, out);
	CHECK_EQ(out[0], 32767);  /* limited */
	CHECK_EQ(out[1], 30000);
	CHECK_EQ(out[2], 32767);

	CHECK_EQ(opensc2_imu_dead_zone(5, 0), 5);
	CHECK_EQ(opensc2_imu_dead_zone(9, 10), 0);
	CHECK_EQ(opensc2_imu_dead_zone(-9, 10), 0);
	CHECK_EQ(opensc2_imu_dead_zone(10, 10), 10);
	CHECK_EQ(opensc2_imu_dead_zone(-10, 10), -10);
}

OPENSC2_TEST(sensors_gyro_bias)
{
	struct opensc2_gyro_bias b;
	const float rest[3] = { 0.05f, -0.02f, 0.0f };
	const float turn[3] = { 1.0f, 0.0f, 0.0f };

	opensc2_gyro_bias_init(&b, NULL);
	CHECK(b.bias[0] == 0.0f);
	for (int i = 0; i < 4; i++) {
		opensc2_gyro_bias_update(&b, rest, 0.1f, 4);   /* settling: nothing learned yet */
	}
	CHECK(b.bias[0] == 0.0f);
	opensc2_gyro_bias_update(&b, rest, 0.1f, 4);
	CHECK(fabsf(b.bias[0] - 0.05f / 256.0f) < 1e-6f);
	CHECK(fabsf(b.bias[1] + 0.02f / 256.0f) < 1e-6f);
	for (int i = 0; i < 3000; i++) {
		opensc2_gyro_bias_update(&b, rest, 0.1f, 4);
	}
	CHECK(fabsf(b.bias[0] - 0.05f) < 0.001f);
	/* a movement beyond the threshold stops it and starts the settling over */
	float held = b.bias[0];

	opensc2_gyro_bias_update(&b, turn, 0.1f, 4);
	CHECK_EQ(b.still, 0);
	CHECK(b.bias[0] == held);
	/* a starting bias */
	const float start[3] = { 0.5f, 0.25f, -0.125f };

	opensc2_gyro_bias_init(&b, start);
	CHECK(b.bias[2] == -0.125f);
}

OPENSC2_TEST(sensors_adc_average)
{
	/* two channels, four scans: samples[scan * 2 + channel] */
	const int16_t s[8] = { 100, -3, 101, 0, 102, 0, 104, 0 };
	const int16_t over[4] = { 5000, 4096, 5000, 4096 };
	uint16_t out[2];

	opensc2_adc_average(s, 2, 4, out);
	CHECK_EQ(out[0], 101);   /* 407 / 4, toward zero */
	CHECK_EQ(out[1], 0);     /* -3 / 4 is 0; below 0 would be limited */
	opensc2_adc_average(over, 1, 4, out);
	CHECK_EQ(out[0], 4548);   /* not limited to 12 bits */
}
