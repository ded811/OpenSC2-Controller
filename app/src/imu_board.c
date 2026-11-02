/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * imu_board.c: the set-up the real controller gives its IMU (an ST LSM6DSV16X) after the driver
 * has started it, following the protocol documentation, for the stream build
 * (CONFIG_APP_IMU_STREAM with an "st,lsm6dsv16x" as imu0; untested on hardware):
 *  - the free-fall time window 63 samples (0 on a board that reports hardware ID 0x45);
 *  - the sensor-fusion block's alpha-blending factor 0.05 (a half float in the embedded-function
 *    registers 0x9E and 0x9F) on boards that report hardware ID 0x48 or lower;
 *  - the accelerometer, then the gyroscope, in high-accuracy mode, where the overlay's rate code
 *    of the high-accuracy table (LSM6DSVXXX_DT_ODR_HA01_AT_250Hz) makes them run at 250 Hz.
 * The hardware ID is the one this board reports (the identity's board revision, 0x48 by
 * default). Each step logs a warning when it fails and the next one still runs, except that the
 * gyroscope is not tried after the accelerometer's mode failed, as on the real controller.
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include "app.h"

#if defined(CONFIG_APP_IMU_STREAM) && DT_NODE_HAS_COMPAT(DT_ALIAS(imu0), st_lsm6dsv16x)
#include <zephyr/drivers/sensor.h>
#include <lsm6dsv16x_reg.h>
#include <opensc2/opensc2.h>

LOG_MODULE_REGISTER(imu_board, LOG_LEVEL_INF);

#define FREE_FALL_WINDOW   63
#define ALPHA_BLEND_LO     0x9E
#define ALPHA_BLEND_HI     0x9F
#define HIGH_ACCURACY      1
#define HWID_NO_FREE_FALL  0x45
#define HWID_LAST_BLEND    0x48

static const struct device *const imu = DEVICE_DT_GET(DT_ALIAS(imu0));

/* The driver's configuration starts with the ST register library's context. */
static const stmdev_ctx_t *imu_ctx(void)
{
	return (const stmdev_ctx_t *)imu->config;
}

/* A positive value as an IEEE 754 half float, the format the fusion block takes: scaled into
 * [1, 2) by powers of two (exact in single precision), then ten fraction bits, rounded to the
 * nearest. Enough for the one small constant set here; no zero, subnormal or overflow handling. */
static uint16_t half_float(float value)
{
	int exponent = 0;
	uint32_t fraction;

	while (value >= 2.0f) {
		value *= 0.5f;
		exponent++;
	}
	while (value < 1.0f) {
		value *= 2.0f;
		exponent--;
	}
	fraction = (uint32_t)((value - 1.0f) * 1024.0f + 0.5f);
	if (fraction == 1024u) {
		fraction = 0;
		exponent++;
	}
	return (uint16_t)(((uint32_t)(exponent + 15) << 10) | fraction);
}

void imu_board_setup(void)
{
	struct opensc2_identity id;
	struct sensor_value mode = { .val1 = HIGH_ACCURACY };
	uint16_t half;
	uint8_t lo, hi;
	int32_t ret;

	if (!device_is_ready(imu)) {
		LOG_WRN("IMU not ready: its board set-up is skipped");
		return;
	}
	opensc2_identity_get(&id);

	if (lsm6dsv16x_ff_time_windows_set(imu_ctx(), id.board_revision == HWID_NO_FREE_FALL ?
					   0 : FREE_FALL_WINDOW) != 0) {
		LOG_WRN("IMU: free-fall window not set");
	}

	if (id.board_revision <= HWID_LAST_BLEND) {
		half = half_float(0.05f);   /* 0x2A66 */
		lo = (uint8_t)(half & 0xFFu);
		hi = (uint8_t)(half >> 8);
		ret = lsm6dsv16x_ln_pg_write(imu_ctx(), ALPHA_BLEND_LO, &lo, 1);
		ret += lsm6dsv16x_ln_pg_write(imu_ctx(), ALPHA_BLEND_HI, &hi, 1);
		if (ret != 0) {
			LOG_WRN("IMU: fusion alpha-blending factor not set");
		}
	}

	if (sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_CONFIGURATION, &mode) != 0) {
		LOG_WRN("IMU: accelerometer high-accuracy mode not set");
		return;
	}
	if (sensor_attr_set(imu, SENSOR_CHAN_GYRO_XYZ, SENSOR_ATTR_CONFIGURATION, &mode) != 0) {
		LOG_WRN("IMU: gyroscope high-accuracy mode not set");
	}
}

#else

void imu_board_setup(void)
{
}

#endif
