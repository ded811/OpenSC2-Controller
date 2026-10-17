/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * fusion.c: a Mahony complementary filter (gyroscope integration corrected towards gravity), as
 * described in the open literature on attitude estimation. Floating point; enable the FPU.
 */
#include <math.h>
#include <opensc2/fusion.h>

#define DEG_TO_RAD 0.017453292519943295f

void opensc2_fusion_init(struct opensc2_fusion *f)
{
	f->q[0] = 1.0f;
	f->q[1] = f->q[2] = f->q[3] = 0.0f;
	f->integral[0] = f->integral[1] = f->integral[2] = 0.0f;
	f->kp = 1.0f;
	f->ki = 0.0f;
}

void opensc2_fusion_update(struct opensc2_fusion *f, const float accel_g[3],
			   const float gyro_dps[3], float dt_s)
{
	float q0 = f->q[0], q1 = f->q[1], q2 = f->q[2], q3 = f->q[3];
	float gx = gyro_dps[0] * DEG_TO_RAD;
	float gy = gyro_dps[1] * DEG_TO_RAD;
	float gz = gyro_dps[2] * DEG_TO_RAD;
	float ax = accel_g[0], ay = accel_g[1], az = accel_g[2];
	float norm = sqrtf(ax * ax + ay * ay + az * az);

	if (norm > 1e-6f) {
		/* Error between measured gravity and the gravity the current estimate predicts. */
		ax /= norm;
		ay /= norm;
		az /= norm;
		float vx = 2.0f * (q1 * q3 - q0 * q2);
		float vy = 2.0f * (q0 * q1 + q2 * q3);
		float vz = q0 * q0 - q1 * q1 - q2 * q2 + q3 * q3;
		float ex = ay * vz - az * vy;
		float ey = az * vx - ax * vz;
		float ez = ax * vy - ay * vx;

		if (f->ki > 0.0f) {
			f->integral[0] += f->ki * ex * dt_s;
			f->integral[1] += f->ki * ey * dt_s;
			f->integral[2] += f->ki * ez * dt_s;
			gx += f->integral[0];
			gy += f->integral[1];
			gz += f->integral[2];
		}
		gx += f->kp * ex;
		gy += f->kp * ey;
		gz += f->kp * ez;
	}

	/* Integrate the rate of change of the quaternion. */
	float h = 0.5f * dt_s;
	float nq0 = q0 + (-q1 * gx - q2 * gy - q3 * gz) * h;
	float nq1 = q1 + (q0 * gx + q2 * gz - q3 * gy) * h;
	float nq2 = q2 + (q0 * gy - q1 * gz + q3 * gx) * h;
	float nq3 = q3 + (q0 * gz + q1 * gy - q2 * gx) * h;

	norm = sqrtf(nq0 * nq0 + nq1 * nq1 + nq2 * nq2 + nq3 * nq3);
	if (norm > 1e-6f) {
		f->q[0] = nq0 / norm;
		f->q[1] = nq1 / norm;
		f->q[2] = nq2 / norm;
		f->q[3] = nq3 / norm;
	}
}

static int16_t saturate(float v)
{
	if (v > 32767.0f) {
		return 32767;
	}
	if (v < -32767.0f) {
		return -32767;
	}
	return (int16_t)lrintf(v);
}

void opensc2_fusion_quat(const struct opensc2_fusion *f, int16_t out[4])
{
	for (int i = 0; i < 4; i++) {
		out[i] = saturate(f->q[i] * 32767.0f);
	}
}

int16_t opensc2_accel_from_g(float g)
{
	return saturate(g * 16384.0f);
}

int16_t opensc2_gyro_from_dps(float dps)
{
	return saturate(dps * (32767.0f / 2000.0f));
}
