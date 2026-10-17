/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * fusion.h: an optional helper (CONFIG_OPENSC2_FUSION) that turns accelerometer and gyroscope
 * readings into the orientation quaternion of report 0x42, for IMUs without their own sensor
 * fusion. A small Mahony filter in floating point: no magnetometer, so the heading drifts slowly,
 * like any 6-axis "game rotation vector". Without it the quaternion stays at zero.
 */
#ifndef OPENSC2_FUSION_H_
#define OPENSC2_FUSION_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct opensc2_fusion {
	float q[4];          /* w, x, y, z */
	float integral[3];
	float kp;            /* proportional gain, default 1.0 */
	float ki;            /* integral gain, default 0.0 */
};

void opensc2_fusion_init(struct opensc2_fusion *f);

/* One step: acceleration in g (any length, it is normalised), angular rate in degrees per second,
 * dt in seconds. */
void opensc2_fusion_update(struct opensc2_fusion *f, const float accel_g[3],
			   const float gyro_dps[3], float dt_s);

/* The orientation scaled for report 0x42 (32767 = 1.0), order W, X, Y, Z. */
void opensc2_fusion_quat(const struct opensc2_fusion *f, int16_t out[4]);

/* Unit conversions to the report's scale: 16384 per g, 32767 = 2000 deg/s. Saturating. */
int16_t opensc2_accel_from_g(float g);
int16_t opensc2_gyro_from_dps(float dps);

#ifdef __cplusplus
}
#endif

#endif /* OPENSC2_FUSION_H_ */
