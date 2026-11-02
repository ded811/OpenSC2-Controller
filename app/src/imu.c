/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * imu.c: an accelerometer and gyroscope through Zephyr's sensor API (any driver that provides
 * SENSOR_CHAN_ACCEL_XYZ and SENSOR_CHAN_GYRO_XYZ: MPU6050, LSM6DSO, LSM6DSV16X, ICM42688, ...),
 * aliased in the overlay as imu0, read about every 4 ms (250 a second, the real controller's rate)
 * and handed to the library's IMU (<opensc2/imu.h>), which runs it as the real controller runs its
 * own: on from boot, off and on again when a host writes controller setting 48, the last sample
 * kept while off; the real controller's scaling, mounting matrix, gyro bias (setting 84) and dead
 * zone. The orientation quaternion comes from the library's fusion helper, in the sensor's axes as
 * a sensor's own fusion gives it. With the haptics synthesiser's drop sounds (app/haptics.conf),
 * every sample also goes through the library's free-fall detector, and a fall goes to the IMU's
 * free-fall rule.
 *
 * Axes: the controller's X points right, Y forward (away from the player), Z up, when it lies flat.
 * Mount the sensor that way, or give its mounting matrix in zephyr,user (row major, out = m x in):
 *     imu-mounting-matrix = <0 1 0 (-1) 0 0 0 0 1>;   (the real controller's: x' = y, y' = -x)
 *
 * CONFIG_APP_IMU_BIAS (off by default, untested): a rest-based gyro-bias estimate of OpenSC2's own
 * (<opensc2/sensor_helpers.h>) as the sensor's bias, which the library subtracts while setting 84
 * is on (the default); setting 85 is its rest threshold in report units. It starts from the stored
 * cal/sensors/gyroscope/bias when there is one, as the real controller starts its IMU's own
 * estimate.
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
#include <opensc2/haptics.h>
#include <opensc2/imu.h>
#include <opensc2/sensor_helpers.h>

LOG_MODULE_REGISTER(imu, LOG_LEVEL_INF);

#define IMU_PERIOD_MS 4
#define USER DT_PATH(zephyr_user)
#define RAD_TO_DEG 57.29578f
#define MS2_PER_G 9.80665f

static const struct device *const imu = DEVICE_DT_GET(DT_ALIAS(imu0));

#if DT_NODE_HAS_PROP(USER, imu_mounting_matrix)
BUILD_ASSERT(DT_PROP_LEN(USER, imu_mounting_matrix) == 9, "imu-mounting-matrix takes 9 values");
/* Stored as cells; a negative value comes back as a two's complement word. */
static const uint32_t mounting_cells[9] = DT_PROP(USER, imu_mounting_matrix);
#endif

#if defined(CONFIG_APP_IMU_BIAS)
static struct opensc2_gyro_bias bias;   /* deg/s, the sensor's axes */

static void bias_start(void)
{
	float start[3];
	int err = opensc2_cal_gyro_bias(start);

	if (err == -ERANGE) {
		/* The real controller then leaves its IMU stream off; this polled IMU (OpenSC2's own
		 * way of reading it) only starts without the stored bias. */
		LOG_WRN("One of the gyro bias values is out of range: not used");
	}
	if (err == 0) {
		/* rad/s in the sensor's axes, as the real controller stores it */
		for (int i = 0; i < 3; i++) {
			start[i] *= RAD_TO_DEG;
		}
		opensc2_gyro_bias_init(&bias, start);
		LOG_INF("Gyro bias from cal/sensors/gyroscope/bias");
	} else {
		opensc2_gyro_bias_init(&bias, NULL);
	}
}

/* The estimate, learnt at rest (setting 85 in report units, 32767 = 2000 deg/s), as the sample's
 * bias in rad/s. */
static void bias_learn(const float gyro_rads[3], struct opensc2_imu_sample *s)
{
	float threshold = (float)opensc2_setting_get(OPENSC2_SETTING_IMU_THRESHOLD) *
			  (2000.0f / 32767.0f);
	float dps[3];

	for (int i = 0; i < 3; i++) {
		dps[i] = gyro_rads[i] * RAD_TO_DEG;
	}
	opensc2_gyro_bias_update(&bias, dps, threshold, 1000 / IMU_PERIOD_MS);
	for (int i = 0; i < 3; i++) {
		s->bias[i] = bias.bias[i] / RAD_TO_DEG;
	}
	s->has_bias = true;
}
#endif

static void imu_thread(void *p1, void *p2, void *p3)
{
	struct opensc2_fusion fusion;
	struct opensc2_freefall fall;
	struct opensc2_imu_config cfg;
	int64_t last = k_uptime_ticks();

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
	opensc2_freefall_init(&fall);

	for (;;) {
		struct sensor_value a[3], g[3];
		struct opensc2_imu_sample s = { 0 };
		float accel_g[3], gyro_dps[3];
		int16_t accel[3];
		int64_t now;

		k_msleep(IMU_PERIOD_MS);
		if (!opensc2_imu_streaming()) {
			/* off: the reports keep the last sample, no fall is carried over the pause */
			opensc2_freefall_init(&fall);
			continue;
		}
		if (sensor_sample_fetch(imu) != 0 ||
		    sensor_channel_get(imu, SENSOR_CHAN_ACCEL_XYZ, a) != 0 ||
		    sensor_channel_get(imu, SENSOR_CHAN_GYRO_XYZ, g) != 0) {
			continue;
		}
		now = k_uptime_ticks();
		for (int i = 0; i < 3; i++) {
			s.accel[i] = sensor_value_to_float(&a[i]);   /* m/s^2 */
			s.gyro[i] = sensor_value_to_float(&g[i]);    /* rad/s */
		}
#if defined(CONFIG_APP_IMU_BIAS)
		bias_learn(s.gyro, &s);
#endif
		/* the orientation from the rates the library will report (less the bias while
		 * setting 84 is on) */
		opensc2_imu_config_get(&cfg);
		for (int i = 0; i < 3; i++) {
			accel_g[i] = s.accel[i] / MS2_PER_G;
			gyro_dps[i] = (s.gyro[i] - (cfg.use_bias && s.has_bias ? s.bias[i] : 0.0f)) *
				      RAD_TO_DEG;
		}
		opensc2_fusion_update(&fusion, accel_g, gyro_dps,
				      (float)k_ticks_to_us_floor32((uint32_t)(now - last)) / 1e6f);
		last = now;
		for (int i = 0; i < 4; i++) {
			s.quat[i] = fusion.q[i];
		}
		s.has_quat = true;
		s.timestamp_ns = k_ticks_to_ns_floor64(now);
		(void)opensc2_imu_sample(&s);

		if (IS_ENABLED(CONFIG_OPENSC2_HAPTICS_DROP_SOUNDS)) {
			for (int i = 0; i < 3; i++) {
				accel[i] = opensc2_accel_from_g(accel_g[i]);
			}
			if (opensc2_freefall_update(&fall, accel, k_uptime_get_32())) {
				opensc2_imu_free_fall();
			}
		}
	}
}

K_THREAD_DEFINE(imu_tid, 2048, imu_thread, NULL, NULL, NULL, 8, 0, SYS_FOREVER_MS);

int imu_init(void)
{
#if DT_NODE_HAS_PROP(USER, imu_mounting_matrix)
	int8_t m[9];

	for (int i = 0; i < 9; i++) {
		m[i] = (int8_t)(int32_t)mounting_cells[i];
	}
	(void)opensc2_imu_set_mounting(m);   /* the board's, as the real controller's sets its own */
#endif
	k_thread_start(imu_tid);
	return 0;
}

void imu_setting(uint8_t number, int16_t value)
{
	/* Settings 48, 84 and 85 are the library's IMU's (<opensc2/imu.h>). */
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
