/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * imu_rules.c: the IMU's pure parts (imu_rules.h), written from the real controller's behaviour as
 * the protocol documentation (not public yet; docs/protocol.md lists the pages) describes it. The
 * scaling is the sensor helpers' (single precision, kept to 16 bits as the real controller keeps
 * it).
 */
#include <string.h>
#include <opensc2/sensor_helpers.h>
#include "imu_rules.h"

#define KEY_PREFIX "settings/sensors/imu/"

static const struct {
	const char *name;
	uint8_t size;
} keys[] = {
	[OSC2_IMU_KEY_MODE] = { "mode", 2 },
	[OSC2_IMU_KEY_MOUNTING] = { "mounting_matrix", 9 },
	[OSC2_IMU_KEY_DEAD_ZONE] = { "gyro_dz_threshold", 4 },
	[OSC2_IMU_KEY_USE_BIAS] = { "use_bias", 1 },
	[OSC2_IMU_KEY_THRESHOLD] = { "gyro_threshold", 1 },
};

int osc2_imu_key(const uint8_t *payload, uint8_t len, uint8_t *name_len)
{
	const uint8_t *nul = len ? memchr(payload, 0, len) : NULL;
	size_t prefix = sizeof(KEY_PREFIX) - 1;
	size_t n;

	if (!nul) {
		return -1;
	}
	n = (size_t)(nul - payload);
	if (n <= prefix || memcmp(payload, KEY_PREFIX, prefix) != 0) {
		return -1;
	}
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		if (strlen(keys[i].name) == n - prefix &&
		    memcmp(payload + prefix, keys[i].name, n - prefix) == 0) {
			*name_len = (uint8_t)(n + 1);
			return (int)i;
		}
	}
	return -1;
}

uint8_t osc2_imu_key_size(int key)
{
	return key >= 0 && key < (int)(sizeof(keys) / sizeof(keys[0])) ? keys[key].size : 0;
}

void osc2_imu_convert(const struct osc2_imu_params *p, const struct opensc2_imu_sample *s,
		      struct opensc2_imu *out)
{
	int16_t a[3], g[3], q[4] = { 0 };

	for (int i = 0; i < 3; i++) {
		float rate = s->gyro[i];

		if (p->use_bias && s->has_bias) {
			rate -= s->bias[i];
		}
		a[i] = opensc2_imu_accel_from_ms2(s->accel[i]);
		g[i] = opensc2_imu_gyro_from_rads(rate);
	}
	memset(out, 0, sizeof(*out));
	opensc2_imu_mount(p->mounting, a, out->accel);
	out->timestamp_us = (uint32_t)((s->timestamp_ns + 500u) / 1000u);
	opensc2_imu_mount(p->mounting, g, out->gyro);
	for (int i = 0; i < 3; i++) {
		out->gyro[i] = opensc2_imu_dead_zone(out->gyro[i], p->dead_zone);
	}
	if (s->has_quat) {
		for (int i = 0; i < 4; i++) {
			q[i] = opensc2_imu_quat_from_unit(s->quat[i]);
		}
		opensc2_imu_mount_quat(p->mounting, q, out->quat);
	}
}
