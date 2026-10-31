/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * imu.c: the IMU as the real controller runs it (<opensc2/imu.h>), following the protocol
 * documentation (not public yet; docs/protocol.md lists the pages): the stream on from boot and
 * switched by setting 48 and the key mode; the scaling, the gyro bias, the mounting matrix and the
 * dead zone of every sample; settings 84 and 85; the five settings/sensors/imu keys by name (RAM
 * only); the free fall's arming. The sensor itself is the application's (its handler).
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <opensc2/opensc2.h>
#include <opensc2/codec.h>
#include <opensc2/haptics.h>
#include <opensc2/imu.h>
#include <opensc2/sensor_helpers.h>
#include "imu_rules.h"

#define FALL_REARM_MS  5000   /* falls count again 5 s after one */

static struct k_spinlock imu_lock;
static struct {
	bool streaming;
	bool use_bias;
	int8_t mounting[9];
	int32_t dead_zone;
	uint8_t threshold;
	bool threshold_given;
	bool fall_armed;
	bool fall_cancelled;      /* the stream went off while falls waited: none count again */
	int64_t fall_rearm_ms;
	const struct opensc2_imu_handler *handler;
	void *user_data;
} imu = {
	.streaming = true,
	.use_bias = true,
	.mounting = { 1, 0, 0, 0, 1, 0, 0, 0, 1 },
	.fall_armed = true,
};

/* Falls count again once their 5 s have passed (unless the stream went off in between). */
static void fall_rearm_locked(int64_t now)
{
	if (!imu.fall_armed && !imu.fall_cancelled && now >= imu.fall_rearm_ms) {
		imu.fall_armed = true;
	}
}

int opensc2_imu_sample(const struct opensc2_imu_sample *sample)
{
	struct opensc2_imu out;
	struct osc2_imu_params p;
	k_spinlock_key_t lk;

	if (!sample) {
		return -EINVAL;
	}
	lk = k_spin_lock(&imu_lock);
	if (!imu.streaming) {
		k_spin_unlock(&imu_lock, lk);
		return -EAGAIN;   /* off: the reports keep the last sample */
	}
	p.use_bias = imu.use_bias;
	memcpy(p.mounting, imu.mounting, sizeof(p.mounting));
	p.dead_zone = imu.dead_zone;
	k_spin_unlock(&imu_lock, lk);

	osc2_imu_convert(&p, sample, &out);
	return opensc2_set_imu(&out);
}

bool opensc2_imu_streaming(void)
{
	k_spinlock_key_t lk = k_spin_lock(&imu_lock);
	bool on = imu.streaming;

	k_spin_unlock(&imu_lock, lk);
	return on;
}

int opensc2_imu_set_mounting(const int8_t m[9])
{
	static const int8_t identity[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
	k_spinlock_key_t lk = k_spin_lock(&imu_lock);

	memcpy(imu.mounting, m ? m : identity, sizeof(imu.mounting));
	k_spin_unlock(&imu_lock, lk);
	return 0;
}

void opensc2_imu_config_get(struct opensc2_imu_config *config)
{
	k_spinlock_key_t lk = k_spin_lock(&imu_lock);

	fall_rearm_locked(k_uptime_get());
	config->streaming = imu.streaming;
	config->use_bias = imu.use_bias;
	memcpy(config->mounting, imu.mounting, sizeof(config->mounting));
	config->dead_zone = imu.dead_zone;
	config->gyro_threshold = imu.threshold;
	config->gyro_threshold_given = imu.threshold_given;
	config->free_fall_armed = imu.fall_armed;
	k_spin_unlock(&imu_lock, lk);
}

int opensc2_imu_set_handler(const struct opensc2_imu_handler *handler, void *user_data)
{
	k_spinlock_key_t lk = k_spin_lock(&imu_lock);

	imu.handler = handler;
	imu.user_data = user_data;
	k_spin_unlock(&imu_lock, lk);
	return 0;
}

void opensc2_imu_free_fall(void)
{
	int64_t now = k_uptime_get();
	k_spinlock_key_t lk = k_spin_lock(&imu_lock);
	bool counts;

	fall_rearm_locked(now);
	counts = imu.fall_armed && imu.streaming;
	if (counts) {
		imu.fall_armed = false;
		imu.fall_rearm_ms = now + FALL_REARM_MS;
	}
	k_spin_unlock(&imu_lock, lk);
	if (counts) {
		opensc2_haptics_free_fall();   /* the drop sounds 250 ms from now */
	}
}

/* The stream on or off, as a write of setting 48 or of the key mode asks (every write). Off while
 * falls wait for their 5 s: they never count again (the real controller cancels the work that
 * would arm them). */
static void stream(bool on)
{
	const struct opensc2_imu_handler *h;
	void *user;
	k_spinlock_key_t lk = k_spin_lock(&imu_lock);
	bool was = imu.streaming;

	if (!on && was) {
		fall_rearm_locked(k_uptime_get());
		if (!imu.fall_armed) {
			imu.fall_cancelled = true;
		}
	}
	imu.streaming = on;
	h = imu.handler;
	user = imu.user_data;
	k_spin_unlock(&imu_lock, lk);
	if (h && h->stream && (on || was)) {
		h->stream(on, user);
	}
}

static int threshold(uint8_t value)
{
	const struct opensc2_imu_handler *h;
	void *user;
	k_spinlock_key_t lk = k_spin_lock(&imu_lock);

	imu.threshold = value;
	imu.threshold_given = true;
	h = imu.handler;
	user = imu.user_data;
	k_spin_unlock(&imu_lock, lk);
	return h && h->gyro_threshold ? h->gyro_threshold(value, user) : 0;
}

int osc2_imu_setting(uint8_t number, int16_t value)
{
	k_spinlock_key_t lk;

	switch (number) {
	case OPENSC2_SETTING_IMU_MODE:
		stream(value != 0);
		return 0;
	case OPENSC2_SETTING_IMU_BIAS:
		lk = k_spin_lock(&imu_lock);
		imu.use_bias = value != 0;
		k_spin_unlock(&imu_lock, lk);
		return 0;
	case OPENSC2_SETTING_IMU_THRESHOLD:
		return threshold((uint8_t)value);
	default:
		return 0;
	}
}

int osc2_imu_by_name(uint8_t opcode, const uint8_t *payload, uint8_t len, uint8_t *answer_len)
{
	uint8_t name_len;
	int key = osc2_imu_key(payload, len, &name_len);
	const uint8_t *v = payload + name_len;
	uint8_t n = (uint8_t)(len - name_len);
	k_spinlock_key_t lk;

	if (key < 0) {
		return -ENOENT;
	}
	switch (opcode) {
	case 0xED:
		*answer_len = 0;   /* set only: nothing to read back */
		return 0;
	case 0xEF:
		return -ENOENT;    /* no read function: nothing to save, as on the real controller */
	case 0xF0:
		return 0;          /* nothing stored, and RAM stays as it is */
	case 0xEE:
		break;
	default:
		return -ENOTSUP;
	}
	if (n != osc2_imu_key_size(key)) {
		return -EINVAL;
	}
	switch (key) {
	case OSC2_IMU_KEY_MODE:
		stream(opensc2_get_le16(v) != 0);
		return 0;
	case OSC2_IMU_KEY_MOUNTING:
		lk = k_spin_lock(&imu_lock);
		memcpy(imu.mounting, v, sizeof(imu.mounting));
		k_spin_unlock(&imu_lock, lk);
		return 0;
	case OSC2_IMU_KEY_DEAD_ZONE:
		lk = k_spin_lock(&imu_lock);
		imu.dead_zone = (int32_t)opensc2_get_le32(v);
		k_spin_unlock(&imu_lock, lk);
		return 0;
	case OSC2_IMU_KEY_USE_BIAS:
		lk = k_spin_lock(&imu_lock);
		imu.use_bias = v[0] != 0;
		k_spin_unlock(&imu_lock, lk);
		return 0;
	case OSC2_IMU_KEY_THRESHOLD:
		return threshold(v[0]);
	default:
		return -ENOENT;
	}
}

void osc2_imu_reset(void)
{
	k_spinlock_key_t lk = k_spin_lock(&imu_lock);

	memset(&imu, 0, sizeof(imu));
	imu.streaming = true;
	imu.use_bias = true;
	imu.mounting[0] = imu.mounting[4] = imu.mounting[8] = 1;
	imu.fall_armed = true;
	k_spin_unlock(&imu_lock, lk);
}
