/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * imu_fifo.c: the IMU the way the real controller reads it (CONFIG_APP_IMU_STREAM, off by
 * default, untested on hardware): an ST LSM6DSV16X (imu0) whose FIFO is streamed with Zephyr's
 * sensor streaming API, its sensor-fusion block's game rotation vector and gyro-bias estimate in
 * the same buffers. One standing read from boot; for each buffer the first accelerometer,
 * gyroscope and quaternion samples (and the bias while controller setting 84 is on) become report
 * units, turned by the mounting matrix; a buffer missing one of them is dropped, and a failed
 * stream is submitted again 500 ms later. Setting 48 = 0 cancels the read and any other value
 * submits it again; while it is off the report keeps the last sample. The stored gyro bias
 * (cal/sensors/gyroscope/bias) is programmed as the fusion block's starting bias.
 *
 * The overlay gives the IMU its interrupt (int1-gpios) and its FIFO and fusion settings
 * (app/imu-stream.overlay).
 */
#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include "app.h"

#if defined(CONFIG_APP_IMU_STREAM)
#include <zephyr/drivers/sensor.h>
#include <zephyr/rtio/rtio.h>
#include <opensc2/cal.h>
#include <opensc2/sensor_helpers.h>

LOG_MODULE_REGISTER(imu_fifo, LOG_LEVEL_INF);

#define USER DT_PATH(zephyr_user)
#define RETRY_MS 500

static const struct device *const imu = DEVICE_DT_GET(DT_ALIAS(imu0));

SENSOR_DT_STREAM_IODEV(imu_iodev, DT_ALIAS(imu0),
		       { SENSOR_TRIG_FIFO_WATERMARK, SENSOR_STREAM_DATA_INCLUDE },
		       { SENSOR_TRIG_FIFO_FULL, SENSOR_STREAM_DATA_NOP });
RTIO_DEFINE_WITH_MEMPOOL(imu_ctx, 4, 4, 16, 256, sizeof(void *));

#if DT_NODE_HAS_PROP(USER, imu_mounting_matrix)
BUILD_ASSERT(DT_PROP_LEN(USER, imu_mounting_matrix) == 9, "imu-mounting-matrix takes 9 values");
static const uint32_t mounting_cells[9] = DT_PROP(USER, imu_mounting_matrix);
#else
static const uint32_t mounting_cells[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
#endif
static int8_t mounting[9];

static K_MUTEX_DEFINE(stream_lock);
static struct rtio_sqe *stream;   /* the standing read, or NULL */
static bool wanted = true;        /* setting 48: on from boot */

static const struct sensor_chan_spec accel_chan = { SENSOR_CHAN_ACCEL_XYZ, 0 };
static const struct sensor_chan_spec gyro_chan = { SENSOR_CHAN_GYRO_XYZ, 0 };
static const struct sensor_chan_spec quat_chan = { SENSOR_CHAN_GAME_ROTATION_VECTOR, 0 };
static const struct sensor_chan_spec bias_chan = { SENSOR_CHAN_GBIAS_XYZ, 0 };

static float q31_value(q31_t v, int8_t shift)
{
	return ldexpf((float)v, shift - 31);
}

/* Submit the standing read when it is wanted and not running. */
static void stream_start(void)
{
	k_mutex_lock(&stream_lock, K_FOREVER);
	if (wanted && stream == NULL && sensor_stream(&imu_iodev, &imu_ctx, NULL, &stream) != 0) {
		stream = NULL;
	}
	k_mutex_unlock(&stream_lock);
}

void imu_setting(uint8_t number, int16_t value)
{
	if (number != OPENSC2_SETTING_IMU_MODE) {
		return;
	}
	k_mutex_lock(&stream_lock, K_FOREVER);
	wanted = value != 0;
	if (!wanted && stream != NULL) {
		(void)rtio_sqe_cancel(stream);
		stream = NULL;
	}
	k_mutex_unlock(&stream_lock);
	stream_start();
}

/* One buffer: its first sample of each kind, or nothing when one is missing. */
static void decode(const struct sensor_decoder_api *dec, const uint8_t *buf)
{
	struct sensor_three_axis_data accel, gyro, bias = { 0 };
	struct sensor_game_rotation_vector_data quat;
	uint32_t fa = 0, fg = 0, fq = 0, fb = 0;
	bool subtract_bias = opensc2_setting_get(OPENSC2_SETTING_IMU_BIAS) != 0;
	struct opensc2_imu out = { 0 };
	int16_t a[3], g[3], q[4];

	if (dec->decode(buf, accel_chan, &fa, 1, &accel) < 1) {
		LOG_DBG("IMU buffer dropped: no acceleration");
		return;
	}
	if (dec->decode(buf, gyro_chan, &fg, 1, &gyro) < 1) {
		LOG_DBG("IMU buffer dropped: no angular rate");
		return;
	}
	if (dec->decode(buf, quat_chan, &fq, 1, &quat) < 1) {
		LOG_DBG("IMU buffer dropped: no game rotation vector");
		return;
	}
	if (subtract_bias && dec->decode(buf, bias_chan, &fb, 1, &bias) < 1) {
		LOG_DBG("IMU buffer dropped: no gyro bias");
		return;
	}
	for (int i = 0; i < 3; i++) {
		float rate = q31_value(gyro.readings[0].values[i], gyro.shift);

		if (subtract_bias) {
			rate -= q31_value(bias.readings[0].values[i], bias.shift);
		}
		a[i] = opensc2_imu_accel_from_ms2(q31_value(accel.readings[0].values[i],
							    accel.shift));
		g[i] = opensc2_imu_gyro_from_rads(rate);
	}
	/* the quaternion: x, y, z, w from the chip, w first in the report, 32767 = 1 */
	q[0] = (int16_t)CLAMP(q31_value(quat.readings[0].w, quat.shift) * 32767.0f, -32767, 32767);
	for (int i = 0; i < 3; i++) {
		q[1 + i] = (int16_t)CLAMP(q31_value(quat.readings[0].values[i], quat.shift) *
					  32767.0f, -32767, 32767);
	}
	opensc2_imu_mount(mounting, a, out.accel);
	opensc2_imu_mount(mounting, g, out.gyro);
	opensc2_imu_mount_quat(mounting, q, out.quat);
	/* the buffer's time, nanoseconds rounded to the nearest microsecond */
	out.timestamp_us = (uint32_t)((accel.header.base_timestamp_ns + 500) / 1000);
	opensc2_set_imu(&out);
}

/* The stored gyro bias (rad/s, the chip's axes) as the fusion block's starting bias. */
static void program_bias(void)
{
	struct sensor_value v[3];
	float b[3];

	if (opensc2_cal_gyro_bias(b) != 0) {
		return;
	}
	for (int i = 0; i < 3; i++) {
		(void)sensor_value_from_float(&v[i], b[i]);
	}
	if (sensor_attr_set(imu, SENSOR_CHAN_GBIAS_XYZ, SENSOR_ATTR_OFFSET, v) != 0) {
		LOG_WRN("The stored gyro bias could not be programmed");
	}
}

static void imu_thread(void *p1, void *p2, void *p3)
{
	const struct sensor_decoder_api *dec;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	if (!device_is_ready(imu) || sensor_get_decoder(imu, &dec) != 0) {
		LOG_ERR("IMU %s not ready", imu->name);
		return;
	}
	program_bias();
	stream_start();
	for (;;) {
		struct rtio_cqe *cqe = rtio_cqe_consume_block(&imu_ctx);
		int result = cqe->result;
		uint8_t *buf = NULL;
		uint32_t len = 0;

		if (result == 0 && rtio_cqe_get_mempool_buffer(&imu_ctx, cqe, &buf, &len) != 0) {
			buf = NULL;
		}
		rtio_cqe_release(&imu_ctx, cqe);
		if (result != 0) {
			k_mutex_lock(&stream_lock, K_FOREVER);
			stream = NULL;   /* the read ended */
			k_mutex_unlock(&stream_lock);
			if (result != -ECANCELED) {
				LOG_WRN("IMU stream ended (%d); submitted again in %d ms", result,
					RETRY_MS);
				k_msleep(RETRY_MS);
				stream_start();
			}
			continue;
		}
		if (buf != NULL) {
			decode(dec, buf);
			rtio_release_buffer(&imu_ctx, buf, len);
		}
	}
}

K_THREAD_DEFINE(imu_tid, 2048, imu_thread, NULL, NULL, NULL, 9, 0, SYS_FOREVER_MS);

int imu_init(void)
{
	for (int i = 0; i < 9; i++) {
		mounting[i] = (int8_t)(int32_t)mounting_cells[i];
	}
	k_thread_start(imu_tid);
	return 0;
}

#endif /* CONFIG_APP_IMU_STREAM */
