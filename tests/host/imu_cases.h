/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: the IMU (<opensc2/imu.h>, imu.c and imu_rules.c) as an application and a host use
 * it: a sample's conversion (scaling, bias, mounting matrix, dead zone, quaternion, timestamp), the
 * stream from boot and setting 48, settings 84 and 85, the settings/sensors/imu keys by name, and
 * the free fall's arming. Uses the helpers of cal_store_cases.h.
 */
#include <opensc2/imu.h>
#include "imu_rules.h"

static int imu_stream_calls, imu_stream_last, imu_threshold_calls, imu_threshold_last;
static int imu_threshold_answer;

static void imu_on_stream(bool on, void *user_data)
{
	(void)user_data;
	imu_stream_calls++;
	imu_stream_last = on;
}

static int imu_on_threshold(uint8_t threshold, void *user_data)
{
	(void)user_data;
	imu_threshold_calls++;
	imu_threshold_last = threshold;
	return imu_threshold_answer;
}

static const struct opensc2_imu_handler imu_handler = {
	.stream = imu_on_stream,
	.gyro_threshold = imu_on_threshold,
};

static void imu_reset_test(void)
{
	stub_reset();
	imu_stream_calls = imu_threshold_calls = 0;
	imu_stream_last = -1;
	imu_threshold_last = -1;
	imu_threshold_answer = 0;
	(void)opensc2_imu_set_handler(&imu_handler, NULL);
}

static int imu_write(uint8_t number, int16_t value)
{
	uint8_t p[3] = { number, (uint8_t)value, (uint8_t)((uint16_t)value >> 8) };

	return cal_set(0x87, p, 3);
}

OPENSC2_TEST(imu_conversion)
{
	static const int8_t board[9] = { 0, 1, 0, -1, 0, 0, 0, 0, 1 };
	struct opensc2_imu_sample smp = {
		.accel = { 0.0f, 9.80665f, -9.80665f },
		.gyro = { 1.0f, 0.5f, 0.0f },
		.bias = { 0.25f, 0.0f, 0.0f },
		.quat = { 1.0f, 0.0f, 0.5f, 0.0f },
		.timestamp_ns = 1234567,
		.has_bias = true,
		.has_quat = true,
	};
	struct opensc2_imu_config cfg;

	imu_reset_test();
	opensc2_imu_config_get(&cfg);
	CHECK(cfg.streaming);   /* from boot, whatever setting 48 reads */
	CHECK(cfg.use_bias);
	CHECK_EQ(cfg.mounting[0], 1);
	CHECK_EQ(cfg.dead_zone, 0);
	CHECK_EQ(opensc2_imu_sample(NULL), -EINVAL);
	CHECK_EQ(opensc2_imu_sample(&smp), 0);
	CHECK_EQ(osc2.state.imu.timestamp_us, 1235);   /* rounded */
	CHECK_EQ(osc2.state.imu.accel[1], 16383);      /* 1 g */
	CHECK_EQ(osc2.state.imu.accel[2], -16383);
	CHECK_EQ(osc2.state.imu.gyro[0], opensc2_imu_gyro_from_rads(0.75f));   /* less the bias */
	CHECK_EQ(osc2.state.imu.gyro[1], opensc2_imu_gyro_from_rads(0.5f));
	CHECK_EQ(osc2.state.imu.quat[0], 32767);       /* W first */
	CHECK_EQ(osc2.state.imu.quat[2], 16383);
	/* the board's matrix (x' = y, y' = -x), on the quaternion's X, Y, Z too */
	CHECK_EQ(opensc2_imu_set_mounting(board), 0);
	CHECK_EQ(opensc2_imu_sample(&smp), 0);
	CHECK_EQ(osc2.state.imu.accel[0], 16383);
	CHECK_EQ(osc2.state.imu.accel[1], 0);
	CHECK_EQ(osc2.state.imu.gyro[1], -opensc2_imu_gyro_from_rads(0.75f));
	CHECK_EQ(osc2.state.imu.quat[1], 16383);
	CHECK_EQ(osc2.state.imu.quat[2], 0);
	/* setting 84 = 0: the bias stays in */
	CHECK_EQ(imu_write(84, 0), 0);
	CHECK_EQ(opensc2_imu_sample(&smp), 0);
	CHECK_EQ(osc2.state.imu.gyro[1], -opensc2_imu_gyro_from_rads(1.0f));
	CHECK_EQ(imu_write(84, 7), 0);   /* any other value: on */
	opensc2_imu_config_get(&cfg);
	CHECK(cfg.use_bias);
	/* no bias or no quaternion from the sensor: nothing subtracted, the quaternion none */
	smp.has_bias = false;
	smp.has_quat = false;
	CHECK_EQ(opensc2_imu_sample(&smp), 0);
	CHECK_EQ(osc2.state.imu.gyro[1], -opensc2_imu_gyro_from_rads(1.0f));
	CHECK_EQ(osc2.state.imu.quat[0], 0);
	CHECK_EQ(opensc2_imu_set_mounting(NULL), 0);   /* identity again */
}

OPENSC2_TEST(imu_stream_and_settings)
{
	struct opensc2_imu_sample smp = { .accel = { 9.80665f, 0.0f, 0.0f } };
	struct opensc2_imu_config cfg;

	imu_reset_test();
	CHECK_EQ(opensc2_imu_sample(&smp), 0);
	CHECK_EQ(osc2.state.imu.accel[0], 16383);
	/* setting 48 = 0: off; the reports keep the last sample, the stream's handler hears it */
	CHECK_EQ(imu_write(48, 0), 0);
	CHECK(!opensc2_imu_streaming());
	CHECK_EQ(imu_stream_calls, 1);
	CHECK_EQ(imu_stream_last, 0);
	smp.accel[0] = 0.0f;
	CHECK_EQ(opensc2_imu_sample(&smp), -EAGAIN);
	CHECK_EQ(osc2.state.imu.accel[0], 16383);
	CHECK_EQ(osc2.settings[48], 0);   /* stored for the application as well */
	/* again 0: already off, nothing more to do */
	CHECK_EQ(imu_write(48, 0), 0);
	CHECK_EQ(imu_stream_calls, 1);
	/* any other value turns it on, at every write */
	CHECK_EQ(imu_write(48, 0x18), 0);
	CHECK(opensc2_imu_streaming());
	CHECK_EQ(imu_write(48, 0x18), 0);
	CHECK_EQ(imu_stream_calls, 3);
	CHECK_EQ(imu_stream_last, 1);
	CHECK_EQ(opensc2_imu_sample(&smp), 0);
	CHECK_EQ(osc2.state.imu.accel[0], 0);
	/* setting 85 goes to the sensor as a byte; the sensor's error fails the command and the
	 * settings after it in the same command */
	CHECK_EQ(imu_write(85, 50), 0);
	CHECK_EQ(imu_threshold_last, 50);
	CHECK_EQ(imu_write(85, 0x1FF), 0);
	CHECK_EQ(imu_threshold_last, 0xFF);
	imu_threshold_answer = -5;
	CHECK(cal_set(0x87, (const uint8_t[]){ 85, 9, 0, 45, 20, 0 }, 6) != 0);
	CHECK_EQ(osc2.settings[45], opensc2_setting_default(45));
	imu_threshold_answer = 0;
	opensc2_imu_config_get(&cfg);
	CHECK(cfg.gyro_threshold_given);
	CHECK_EQ(cfg.gyro_threshold, 9);
}

static int imu_named(uint8_t opcode, const char *name, const void *value, uint8_t len)
{
	return cal_named(opcode, name, value, len);
}

OPENSC2_TEST(imu_keys_by_name)
{
	static const int8_t board[9] = { 0, 1, 0, -1, 0, 0, 0, 0, 1 };
	struct opensc2_imu_sample smp = { .accel = { 9.80665f, 0.0f, 0.0f }, .gyro = { 0.01f } };
	struct opensc2_imu_config cfg;
	uint8_t out[64];

	imu_reset_test();
	/* mode: two bytes; 0 off, any other value on; setting 48 itself is not changed */
	CHECK_EQ(imu_named(0xEE, "settings/sensors/imu/mode", (const uint8_t[]){ 0, 0 }, 2), 0);
	CHECK(!opensc2_imu_streaming());
	CHECK_EQ(osc2.settings[48], 0);
	CHECK_EQ(imu_named(0xEE, "settings/sensors/imu/mode", (const uint8_t[]){ 0, 1 }, 2), 0);
	CHECK(opensc2_imu_streaming());
	CHECK_EQ(imu_named(0xEE, "settings/sensors/imu/mode", (const uint8_t[]){ 1 }, 1), -EINVAL);
	/* the mounting matrix, nine signed bytes */
	CHECK_EQ(imu_named(0xEE, "settings/sensors/imu/mounting_matrix", board, 9), 0);
	CHECK_EQ(opensc2_imu_sample(&smp), 0);
	CHECK_EQ(osc2.state.imu.accel[1], -16383);
	CHECK_EQ(imu_named(0xEE, "settings/sensors/imu/mounting_matrix", board, 8), -EINVAL);
	/* the dead zone, a signed 32-bit value: a rate strictly inside it reads 0 */
	CHECK_EQ(imu_named(0xEE, "settings/sensors/imu/gyro_dz_threshold",
			   (const uint8_t[]){ 100, 0, 0, 0 }, 4), 0);
	CHECK_EQ(opensc2_imu_sample(&smp), 0);
	CHECK_EQ(osc2.state.imu.gyro[1], 0);   /* 9 units */
	smp.gyro[0] = 1.0f;
	CHECK_EQ(opensc2_imu_sample(&smp), 0);
	CHECK_EQ(osc2.state.imu.gyro[1], -938);
	/* use_bias, one byte; gyro_threshold, one byte, to the sensor */
	CHECK_EQ(imu_named(0xEE, "settings/sensors/imu/use_bias", (const uint8_t[]){ 0 }, 1), 0);
	CHECK_EQ(imu_named(0xEE, "settings/sensors/imu/gyro_threshold", (const uint8_t[]){ 77 }, 1),
		 0);
	CHECK_EQ(imu_threshold_last, 77);
	opensc2_imu_config_get(&cfg);
	CHECK(!cfg.use_bias);
	CHECK_EQ(cfg.dead_zone, 100);
	CHECK_EQ(cfg.mounting[1], 1);
	/* set only: 0xED reads nothing, 0xEF fails, 0xF0 changes nothing */
	CHECK_EQ(cal_read("settings/sensors/imu/use_bias", out), 0);
	CHECK(imu_named(0xEF, "settings/sensors/imu/use_bias", NULL, 0) != 0);
	CHECK_EQ(imu_named(0xF0, "settings/sensors/imu/use_bias", NULL, 0), 0);
	opensc2_imu_config_get(&cfg);
	CHECK(!cfg.use_bias);
	/* a name under the prefix that is none of the five is no key of the library's */
	CHECK(imu_named(0xEE, "settings/sensors/imu/other", (const uint8_t[]){ 1 }, 1) != 0);
	CHECK(imu_named(0xEE, "settings/sensors/imu/", (const uint8_t[]){ 1 }, 1) != 0);
	/* a reboot brings the defaults back (RAM only) */
	osc2_imu_reset();
	opensc2_imu_config_get(&cfg);
	CHECK(cfg.use_bias);
	CHECK_EQ(cfg.dead_zone, 0);
	CHECK_EQ(cfg.mounting[1], 0);
}

OPENSC2_TEST(imu_free_fall)
{
	struct opensc2_imu_config cfg;

	imu_reset_test();
	cal_host.free_falls = 0;
	stub.now_ms = 1000;
	opensc2_imu_free_fall();
	CHECK_EQ(cal_host.free_falls, 1);
	stub.now_ms = 5999;
	opensc2_imu_free_fall();   /* within 5 s: does not count */
	CHECK_EQ(cal_host.free_falls, 1);
	stub.now_ms = 6000;
	opensc2_imu_free_fall();
	CHECK_EQ(cal_host.free_falls, 2);
	/* the stream off within the 5 s: falls never count again (until the next boot) */
	stub.now_ms = 7000;
	CHECK_EQ(imu_write(48, 0), 0);
	CHECK_EQ(imu_write(48, 8), 0);
	stub.now_ms = 60000;
	opensc2_imu_free_fall();
	CHECK_EQ(cal_host.free_falls, 2);
	opensc2_imu_config_get(&cfg);
	CHECK(!cfg.free_fall_armed);
	/* off after the 5 s: nothing lost */
	imu_reset_test();
	cal_host.free_falls = 0;
	stub.now_ms = 1000;
	opensc2_imu_free_fall();
	stub.now_ms = 7000;
	CHECK_EQ(imu_write(48, 0), 0);
	opensc2_imu_free_fall();   /* while off: no stream, no fall */
	CHECK_EQ(cal_host.free_falls, 1);
	CHECK_EQ(imu_write(48, 8), 0);
	opensc2_imu_free_fall();
	CHECK_EQ(cal_host.free_falls, 2);
	stub.now_ms = 0;
}

/* The stream going off cancels a drop sound still due (the synthesiser's cancel), once per "off" of
 * a running stream: a second 0, or a 0 before anything ran, does not. */
OPENSC2_TEST(imu_stream_off_cancels_drop)
{
	imu_reset_test();
	cal_host.fall_cancels = 0;
	stub.now_ms = 1000;
	opensc2_imu_free_fall();
	stub.now_ms = 1100;                 /* the sound is still 150 ms away */
	CHECK_EQ(imu_write(48, 0), 0);
	CHECK_EQ(cal_host.fall_cancels, 1);
	CHECK_EQ(imu_write(48, 0), 0);      /* already off */
	CHECK_EQ(cal_host.fall_cancels, 1);
	CHECK_EQ(imu_write(48, 1), 0);
	CHECK_EQ(cal_host.fall_cancels, 1);
	/* the key mode by name does the same */
	CHECK_EQ(imu_named(0xEE, "settings/sensors/imu/mode", (const uint8_t[]){ 0, 0 }, 2), 0);
	CHECK_EQ(cal_host.fall_cancels, 2);
	/* a board without an IMU (no handler, no mounting matrix): nothing to cancel */
	osc2_imu_reset();
	CHECK_EQ(imu_write(48, 0), 0);
	CHECK_EQ(cal_host.fall_cancels, 2);
	stub.now_ms = 0;
}

/* A board that set its IMU up sends all 0 (W too) until its first sample, as the real controller's
 * empty state does; a board that did not keeps "none" as the identity. */
OPENSC2_TEST(imu_before_first_sample)
{
	struct opensc2_imu_sample s = { 0 };
	static const int8_t m[9] = { 0, 1, 0, -1, 0, 0, 0, 0, 1 };

	imu_reset_test();
	osc2_imu_reset();   /* no handler, no mounting: no board IMU */
	CHECK(!osc2_imu_before_first_sample());
	CHECK_EQ(opensc2_imu_set_mounting(m), 0);
	CHECK(osc2_imu_before_first_sample());
	memset(&osc2.state.imu, 0, sizeof(osc2.state.imu));
	osc2_quat_none_to_identity(&osc2.state);
	CHECK_EQ(osc2.state.imu.quat[0], 0);
	/* a sample refused while the stream is off does not count */
	CHECK_EQ(imu_write(48, 0), 0);
	CHECK_EQ(opensc2_imu_sample(&s), -EAGAIN);
	CHECK(osc2_imu_before_first_sample());
	CHECK_EQ(imu_write(48, 1), 0);
	s.quat[0] = 1.0f;
	s.has_quat = true;
	CHECK_EQ(opensc2_imu_sample(&s), 0);
	CHECK(!osc2_imu_before_first_sample());
	CHECK_EQ(osc2.state.imu.quat[0], 32767);
	/* a handler alone counts as the board's set-up too */
	osc2_imu_reset();
	CHECK_EQ(opensc2_imu_set_handler(&imu_handler, NULL), 0);
	CHECK(osc2_imu_before_first_sample());
	memset(&osc2.state.imu, 0, sizeof(osc2.state.imu));
	osc2_quat_none_to_identity(&osc2.state);
	CHECK_EQ(osc2.state.imu.quat[0], 0);
	/* without a set-up: the identity */
	osc2_imu_reset();
	memset(&osc2.state.imu, 0, sizeof(osc2.state.imu));
	osc2_quat_none_to_identity(&osc2.state);
	CHECK_EQ(osc2.state.imu.quat[0], OPENSC2_QUAT_ONE);
	(void)opensc2_imu_set_handler(NULL, NULL);
	osc2_imu_reset();
}

/* The dead zone's edge cases: a threshold of 0 or below never zeroes; INT32_MIN neither (no
 * overflow on the host). */
OPENSC2_TEST(imu_dead_zone_edges)
{
	CHECK_EQ(opensc2_imu_dead_zone(5, 0), 5);
	CHECK_EQ(opensc2_imu_dead_zone(0, 0), 0);
	CHECK_EQ(opensc2_imu_dead_zone(-3, -10), -3);
	CHECK_EQ(opensc2_imu_dead_zone(-32768, INT32_MIN), -32768);
	CHECK_EQ(opensc2_imu_dead_zone(100, INT32_MAX), 0);
	CHECK_EQ(opensc2_imu_dead_zone(9, 10), 0);
	CHECK_EQ(opensc2_imu_dead_zone(-10, 10), -10);
}
