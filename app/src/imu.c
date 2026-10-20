/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * imu.c: an accelerometer and gyroscope through Zephyr's sensor API (any driver that provides
 * SENSOR_CHAN_ACCEL_XYZ and SENSOR_CHAN_GYRO_XYZ: MPU6050, LSM6DSO, ICM42688, ...), aliased in the
 * overlay as imu0. Read at about 250 Hz, converted to report 0x42's units, with the orientation
 * quaternion from the library's fusion helper. Like the real controller it only streams while
 * Steam has the IMU switched on (controller setting 48), unless CONFIG_APP_IMU_ALWAYS is set.
 * With the haptics synthesiser's drop sounds (app/haptics.conf), every sample also goes through the
 * library's free-fall detector, and a fall is reported to the synthesiser (as on the real
 * controller, only while the IMU streams).
 *
 * Axes: the controller's X points right, Y forward (away from the player), Z up, when it lies flat.
 * Mount the sensor that way, or swap and negate axes in to_controller_axes().
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include "app.h"

LOG_MODULE_REGISTER(imu, LOG_LEVEL_INF);

#if DT_NODE_EXISTS(DT_ALIAS(imu0)) && defined(CONFIG_SENSOR) && defined(CONFIG_OPENSC2_FUSION)
#include <zephyr/drivers/sensor.h>
#include <opensc2/fusion.h>
#include <opensc2/haptics.h>

#define IMU_PERIOD_MS 4

static const struct device *const imu = DEVICE_DT_GET(DT_ALIAS(imu0));

static void to_controller_axes(float v[3])
{
	/* Identity mounting. Example for a sensor turned 90 degrees: x' = y, y' = -x. */
	ARG_UNUSED(v);
}

static void imu_thread(void *p1, void *p2, void *p3)
{
	struct opensc2_fusion fusion;
	struct opensc2_freefall fall;
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
	opensc2_freefall_init(&fall);

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
			gyro[i] = sensor_value_to_float(&g[i]) * 57.29578f;         /* rad/s to deg/s */
		}
		to_controller_axes(accel);
		to_controller_axes(gyro);

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
		if (IS_ENABLED(CONFIG_OPENSC2_HAPTICS_DROP_SOUNDS) &&
		    opensc2_freefall_update(&fall, out.accel, k_uptime_get_32())) {
			opensc2_haptics_free_fall();
		}
	}
}

K_THREAD_DEFINE(imu_tid, 2048, imu_thread, NULL, NULL, NULL, 8, 0, SYS_FOREVER_MS);

int imu_init(void)
{
	k_thread_start(imu_tid);
	return 0;
}

#else

int imu_init(void)
{
	return 0;
}

#endif
