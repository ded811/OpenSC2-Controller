/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * imu.c: an accelerometer and gyroscope through Zephyr's sensor API (any driver that provides
 * SENSOR_CHAN_ACCEL_XYZ and SENSOR_CHAN_GYRO_XYZ: MPU6050, LSM6DSO, LSM6DSV16X, ICM42688, ...),
 * aliased in the overlay as imu0. Read at about 250 Hz, converted to report 0x42's units, with the
 * orientation quaternion from the library's fusion helper. Like the real controller it only
 * streams while Steam has the IMU switched on (controller setting 48), unless
 * CONFIG_APP_IMU_ALWAYS is set.
 *
 * Axes: the controller's X points right, Y forward (away from the player), Z up, when it lies flat.
 * Mount the sensor that way, or give its mounting matrix in zephyr,user (row major, out = m x in):
 *     imu-mounting-matrix = <0 1 0 (-1) 0 0 0 0 1>;   (the real controller's: x' = y, y' = -x)
 *
 * CONFIG_APP_IMU_BIAS (off by default, untested): a rest-based gyro-bias estimate of OpenSC2's own
 * (<opensc2/sensor_helpers.h>), subtracted while setting 84 is on (the default); setting 85 is its rest
 * threshold in report units. It starts from the stored cal/sensors/gyroscope/bias when there is
 * one, as the real controller starts its IMU's own estimate.
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include "app.h"

#if defined(CONFIG_APP_IMU_STREAM)
/* The IMU's FIFO streamed instead: imu_fifo.c. */
#elif DT_NODE_EXISTS(DT_ALIAS(imu0)) && defined(CONFIG_SENSOR) && defined(CONFIG_OPENSC2_FUSION)
#include <zephyr/drivers/sensor.h>
#include <opensc2/cal.h>
#include <opensc2/fusion.h>
#include <opensc2/sensor_helpers.h>

LOG_MODULE_REGISTER(imu, LOG_LEVEL_INF);

#define IMU_PERIOD_MS 4
#define USER DT_PATH(zephyr_user)
#define RAD_TO_DEG 57.29578f

static const struct device *const imu = DEVICE_DT_GET(DT_ALIAS(imu0));

#if DT_NODE_HAS_PROP(USER, imu_mounting_matrix)
BUILD_ASSERT(DT_PROP_LEN(USER, imu_mounting_matrix) == 9, "imu-mounting-matrix takes 9 values");
/* Stored as cells; a negative value comes back as a two's complement word. */
static const uint32_t mounting[9] = DT_PROP(USER, imu_mounting_matrix);
#endif

static void to_controller_axes(float v[3])
{
#if DT_NODE_HAS_PROP(USER, imu_mounting_matrix)
	float out[3];

	for (int r = 0; r < 3; r++) {
		out[r] = 0.0f;
		for (int c = 0; c < 3; c++) {
			out[r] += (float)(int32_t)mounting[3 * r + c] * v[c];
		}
	}
	for (int r = 0; r < 3; r++) {
		v[r] = out[r];
	}
#else
	/* Identity mounting. Example for a sensor turned 90 degrees: x' = y, y' = -x. */
	ARG_UNUSED(v);
#endif
}

#if defined(CONFIG_APP_IMU_BIAS)
static struct opensc2_gyro_bias bias;

static void bias_start(void)
{
	float start[3];

	if (opensc2_cal_gyro_bias(start) == 0) {
		/* rad/s in the sensor's axes, as the real controller stores it */
		for (int i = 0; i < 3; i++) {
			start[i] *= RAD_TO_DEG;
		}
		to_controller_axes(start);
		opensc2_gyro_bias_init(&bias, start);
		LOG_INF("Gyro bias from cal/sensors/gyroscope/bias");
	} else {
		opensc2_gyro_bias_init(&bias, NULL);
	}
}

/* deg/s in the controller's axes: learn at rest, subtract while setting 84 is on. */
static void bias_apply(float gyro_dps[3])
{
	/* setting 85 in report units (32767 = 2000 deg/s) */
	float threshold = (float)opensc2_setting_get(OPENSC2_SETTING_IMU_THRESHOLD) *
			  (2000.0f / 32767.0f);

	opensc2_gyro_bias_update(&bias, gyro_dps, threshold, 1000 / IMU_PERIOD_MS);
	if (opensc2_setting_get(OPENSC2_SETTING_IMU_BIAS) != 0) {
		for (int i = 0; i < 3; i++) {
			gyro_dps[i] -= bias.bias[i];
		}
	}
}
#endif

static void imu_thread(void *p1, void *p2, void *p3)
{
	struct opensc2_fusion fusion;
	int64_t last = k_uptime_ticks();
	bool streaming = false;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	if (!device_is_ready(imu)) {
		LOG_ERR("IMU %s not ready", imu->name);
		return;
	}
	opensc2_fusion_init(&fusion);
#if defined(CONFIG_APP_IMU_BIAS)
	bias_start();
#endif

	for (;;) {
		struct sensor_value a[3], g[3];
		struct opensc2_imu out = { 0 };
		float accel[3], gyro[3];
		int64_t now;

		k_msleep(IMU_PERIOD_MS);
		if (!IS_ENABLED(CONFIG_APP_IMU_ALWAYS) &&
		    opensc2_setting_get(OPENSC2_SETTING_IMU_MODE) == OPENSC2_IMU_MODE_OFF) {
			if (streaming) {
				opensc2_set_imu(&out);   /* zeros while Steam has it off */
				streaming = false;
			}
			continue;
		}
		if (sensor_sample_fetch(imu) != 0 ||
		    sensor_channel_get(imu, SENSOR_CHAN_ACCEL_XYZ, a) != 0 ||
		    sensor_channel_get(imu, SENSOR_CHAN_GYRO_XYZ, g) != 0) {
			continue;
		}
		streaming = true;
		for (int i = 0; i < 3; i++) {
			accel[i] = sensor_value_to_float(&a[i]) / 9.80665f;         /* m/s^2 to g */
			gyro[i] = sensor_value_to_float(&g[i]) * RAD_TO_DEG;        /* rad/s to deg/s */
		}
		to_controller_axes(accel);
		to_controller_axes(gyro);
#if defined(CONFIG_APP_IMU_BIAS)
		bias_apply(gyro);
#endif

		now = k_uptime_ticks();
		opensc2_fusion_update(&fusion, accel, gyro,
				      (float)k_ticks_to_us_floor32((uint32_t)(now - last)) / 1e6f);
		last = now;

		out.timestamp_us = (uint32_t)k_ticks_to_us_floor64(now);
		for (int i = 0; i < 3; i++) {
			out.accel[i] = opensc2_accel_from_g(accel[i]);
			out.gyro[i] = opensc2_gyro_from_dps(gyro[i]);
		}
		opensc2_fusion_quat(&fusion, out.quat);
		opensc2_set_imu(&out);
	}
}

K_THREAD_DEFINE(imu_tid, 2048, imu_thread, NULL, NULL, NULL, 8, 0, SYS_FOREVER_MS);

int imu_init(void)
{
	k_thread_start(imu_tid);
	return 0;
}

void imu_setting(uint8_t number, int16_t value)
{
	/* Setting 48 is read at every sample. */
	ARG_UNUSED(number);
	ARG_UNUSED(value);
}

#else

int imu_init(void)
{
	return 0;
}

void imu_setting(uint8_t number, int16_t value)
{
	ARG_UNUSED(number);
	ARG_UNUSED(value);
}

#endif
