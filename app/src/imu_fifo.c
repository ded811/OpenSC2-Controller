/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * imu_fifo.c: the IMU the way the real controller reads it (CONFIG_APP_IMU_STREAM, off by
 * default, untested on hardware): an ST LSM6DSV16X (imu0) whose FIFO is streamed with Zephyr's
 * sensor streaming API, its sensor-fusion block's game rotation vector and gyro-bias estimate in
 * the same buffers. One standing read from boot; for each buffer the first accelerometer,
 * gyroscope and quaternion samples (and the bias while the IMU subtracts it: setting 84) go to the
 * library's IMU (<opensc2/imu.h>), which scales and turns them as the real controller does; a
 * buffer missing one of them is dropped, and a failed stream is submitted again 500 ms later. The
 * library's IMU switches the stream (setting 48 and the key mode: 0 cancels the read, any other
 * value submits it again; while it is off the report keeps the last sample). The stored gyro bias
 * (cal/sensors/gyroscope/bias) is programmed as the fusion block's starting bias; one the real
 * controller refuses (2^31 or more in magnitude) keeps the stream from starting at boot, as there.
 * Setting 85 goes to the real controller's fusion block through a driver attribute of its own; the
 * stock driver has none, so it is taken and not applied.
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
#include <zephyr/sys/mem_blocks.h>
#include <opensc2/cal.h>
#include <opensc2/haptics.h>
#include <opensc2/imu.h>
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

/* The standing read and its id (the userdata of its completions). A cancelled read still completes
 * once more, with the next buffer, which RTIO then frees itself: until then no new read is
 * submitted, so the two never share the driver (it would drop the cancelled one unfinished).
 * Completions of it that were already queued when it was cancelled still hold their buffers: the
 * last one is the one whose buffer RTIO has freed (or that has none). */
static K_MUTEX_DEFINE(stream_lock);
static struct rtio_sqe *stream;   /* the standing read, or NULL */
static uintptr_t stream_id;       /* its id; ids start at 1 */
static uintptr_t cancelled_id;    /* a cancelled read not completed yet, or 0 */
static bool wanted = true;        /* setting 48: on from boot */
static struct opensc2_freefall fall;   /* the drop sounds' detector, afresh at each start */

static const struct sensor_chan_spec accel_chan = { SENSOR_CHAN_ACCEL_XYZ, 0 };
static const struct sensor_chan_spec gyro_chan = { SENSOR_CHAN_GYRO_XYZ, 0 };
static const struct sensor_chan_spec quat_chan = { SENSOR_CHAN_GAME_ROTATION_VECTOR, 0 };
static const struct sensor_chan_spec bias_chan = { SENSOR_CHAN_GBIAS_XYZ, 0 };

static float q31_value(q31_t v, int8_t shift)
{
	return ldexpf((float)v, shift - 31);
}

/* Submit the standing read when it is wanted, not running and no cancelled one is still out. */
static void stream_start(void)
{
	k_mutex_lock(&stream_lock, K_FOREVER);
	if (wanted && stream == NULL && cancelled_id == 0) {
		uintptr_t id = stream_id + 1 != 0 ? stream_id + 1 : 1;

		if (sensor_stream(&imu_iodev, &imu_ctx, (void *)id, &stream) == 0) {
			stream_id = id;
			opensc2_freefall_init(&fall);   /* no fall carried over a pause */
		} else {
			stream = NULL;
		}
	}
	k_mutex_unlock(&stream_lock);
}

/* The library's IMU switches the stream: setting 48 and the key mode, every write. */
static void on_stream(bool on, void *user_data)
{
	ARG_UNUSED(user_data);
	k_mutex_lock(&stream_lock, K_FOREVER);
	wanted = on;
	if (!wanted && stream != NULL) {
		(void)rtio_sqe_cancel(stream);
		stream = NULL;
		cancelled_id = stream_id;
	}
	k_mutex_unlock(&stream_lock);
	stream_start();
}

/* Setting 85: the real controller's driver takes it as an attribute of its fusion block; the
 * stock driver has no such attribute, so it is taken and not applied. */
static int on_gyro_threshold(uint8_t threshold, void *user_data)
{
	ARG_UNUSED(threshold);
	ARG_UNUSED(user_data);
	return 0;
}

static const struct opensc2_imu_handler handler = {
	.stream = on_stream,
	.gyro_threshold = on_gyro_threshold,
};

void imu_setting(uint8_t number, int16_t value)
{
	/* Settings 48, 84 and 85 are the library's IMU's (on_stream(), on_gyro_threshold()). */
	ARG_UNUSED(number);
	ARG_UNUSED(value);
}

/* One buffer: its first sample of each kind, or nothing when one is missing. */
static void decode(const struct sensor_decoder_api *dec, const uint8_t *buf)
{
	struct sensor_three_axis_data accel, gyro, bias = { 0 };
	struct sensor_game_rotation_vector_data quat;
	uint32_t fa = 0, fg = 0, fq = 0, fb = 0;
	struct opensc2_imu_config cfg;
	struct opensc2_imu_sample s = { 0 };
	bool subtract_bias;
	int16_t a[3];

	opensc2_imu_config_get(&cfg);
	subtract_bias = cfg.use_bias;

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
		s.accel[i] = q31_value(accel.readings[0].values[i], accel.shift);
		s.gyro[i] = q31_value(gyro.readings[0].values[i], gyro.shift);
		s.bias[i] = q31_value(bias.readings[0].values[i], bias.shift);
	}
	s.has_bias = subtract_bias;
	/* the quaternion: x, y, z, w from the chip, W first for the library */
	s.quat[0] = q31_value(quat.readings[0].w, quat.shift);
	for (int i = 0; i < 3; i++) {
		s.quat[1 + i] = q31_value(quat.readings[0].values[i], quat.shift);
	}
	s.has_quat = true;
	s.timestamp_ns = accel.header.base_timestamp_ns;
	(void)opensc2_imu_sample(&s);
	/* The haptics synthesiser's drop sounds (app/haptics.conf): the stock driver reports no free
	 * fall in the stream, so every sample goes through the library's free-fall detector, and a
	 * fall to the IMU's free-fall rule. */
	if (IS_ENABLED(CONFIG_OPENSC2_HAPTICS_DROP_SOUNDS)) {
		for (int i = 0; i < 3; i++) {
			a[i] = opensc2_imu_accel_from_ms2(s.accel[i]);
		}
		if (opensc2_freefall_update(&fall, a, k_uptime_get_32())) {
			opensc2_imu_free_fall();
		}
	}
}

/* The stored gyro bias (rad/s, the chip's axes) as the fusion block's starting bias. False when the
 * stored bias is refused (2^31 or more in magnitude): the real controller then does not start its
 * stream at boot. */
static bool program_bias(void)
{
	struct sensor_value v[3];
	float b[3];
	int err = opensc2_cal_gyro_bias(b);

	if (err == -ERANGE) {
		LOG_WRN("One of the gyro bias values is out of range: the IMU stream is not started");
		return false;
	}
	if (err != 0) {
		return true;   /* none stored */
	}
	for (int i = 0; i < 3; i++) {
		(void)sensor_value_from_float(&v[i], b[i]);
	}
	if (sensor_attr_set(imu, SENSOR_CHAN_GBIAS_XYZ, SENSOR_ATTR_OFFSET, v) != 0) {
		LOG_WRN("The stored gyro bias could not be programmed");
	}
	return true;
}

/* A completion's buffer while it is still this thread's to release: a successful completion's,
 * unless RTIO has freed it already, as it does for a cancelled read's last completion (and for a
 * failed one). NULL when there is none to release. */
static uint8_t *held_buffer(struct rtio_cqe *cqe, uint32_t *len)
{
	uint8_t *buf = NULL;
	size_t block = rtio_mempool_block_size(&imu_ctx);

	if (cqe->result != 0 || block == 0 ||
	    rtio_cqe_get_mempool_buffer(&imu_ctx, cqe, &buf, len) != 0 || buf == NULL) {
		return NULL;
	}
	if (sys_mem_blocks_is_region_free(imu_ctx.block_pool, buf, *len / block)) {
		return NULL;
	}
	return buf;
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
	if (program_bias()) {
		stream_start();   /* from boot; setting 48 stops and starts it later */
	}
	for (;;) {
		struct rtio_cqe *cqe = rtio_cqe_consume_block(&imu_ctx);
		int result = cqe->result;
		uintptr_t id = (uintptr_t)cqe->userdata;
		bool current, failed = false, cancel_done = false;
		uint32_t len = 0;
		uint8_t *buf = held_buffer(cqe, &len);

		k_mutex_lock(&stream_lock, K_FOREVER);
		current = stream != NULL && id == stream_id;
		if (id != 0 && id == cancelled_id) {
			/* The cancelled read: a completion queued before the cancel still holds its
			 * buffer; the last one comes with RTIO's release done, or without a buffer. */
			if (buf == NULL) {
				cancelled_id = 0;
				cancel_done = true;
			}
		} else if (current && result != 0) {
			stream = NULL;      /* the read ended */
			failed = true;
		}
		k_mutex_unlock(&stream_lock);
		rtio_cqe_release(&imu_ctx, cqe);
		if (buf != NULL) {
			/* Only the running read's buffers are decoded (setting 48 = 0 keeps the last
			 * sample); every held buffer goes back to the pool. */
			if (current) {
				decode(dec, buf);
			}
			rtio_release_buffer(&imu_ctx, buf, len);
		}
		if (failed) {
			LOG_WRN("IMU stream ended (%d); submitted again in %d ms", result, RETRY_MS);
			k_msleep(RETRY_MS);
		}
		if (failed || cancel_done) {
			stream_start();   /* if setting 48 wants it (again) */
		}
	}
}

K_THREAD_DEFINE(imu_tid, 2048, imu_thread, NULL, NULL, NULL, 9, 0, SYS_FOREVER_MS);

int imu_init(void)
{
	int8_t m[9];

	for (int i = 0; i < 9; i++) {
		m[i] = (int8_t)(int32_t)mounting_cells[i];
	}
	(void)opensc2_imu_set_mounting(m);   /* the board's, as the real controller's sets its own */
	(void)opensc2_imu_set_handler(&handler, NULL);
	k_thread_start(imu_tid);
	return 0;
}

#endif /* CONFIG_APP_IMU_STREAM */
