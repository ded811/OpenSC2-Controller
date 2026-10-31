/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * imu_rules.h: the IMU's pure parts (<opensc2/imu.h>), so the host tests run them as the library
 * does: a sample's conversion and the settings/sensors/imu keys. imu.c adds the state, the lock and
 * the glue. Not a public interface.
 */
#ifndef OPENSC2_IMU_RULES_H_
#define OPENSC2_IMU_RULES_H_

#include <stdbool.h>
#include <stdint.h>
#include <opensc2/imu.h>

/* What a sample's conversion takes from the IMU's state. */
struct osc2_imu_params {
	bool use_bias;
	int8_t mounting[9];
	int32_t dead_zone;
};

/* A sample into the state report's units, as the real controller converts it: the acceleration
 * scaled and turned; the timestamp's microseconds rounded; the rate less the bias (with use_bias
 * and a bias given), scaled, turned and its dead zone applied; the quaternion scaled, W first, its
 * X, Y, Z turned (all four 0 without one). */
void osc2_imu_convert(const struct osc2_imu_params *p, const struct opensc2_imu_sample *s,
		      struct opensc2_imu *out);

/* The keys by name, settings/sensors/imu/<name>. */
enum osc2_imu_key {
	OSC2_IMU_KEY_MODE,          /* "mode": u16, 0 off, else on */
	OSC2_IMU_KEY_MOUNTING,      /* "mounting_matrix": 9 signed bytes, row major */
	OSC2_IMU_KEY_DEAD_ZONE,     /* "gyro_dz_threshold": s32 */
	OSC2_IMU_KEY_USE_BIAS,      /* "use_bias": u8 */
	OSC2_IMU_KEY_THRESHOLD,     /* "gyro_threshold": u8 */
};

/* The key a payload names (name, NUL, value), or -1; *name_len counts the NUL. */
int osc2_imu_key(const uint8_t *payload, uint8_t len, uint8_t *name_len);
/* The exact size a key's value must have. */
uint8_t osc2_imu_key_size(int key);

/* imu.c, for the rest of the library: a write of setting 48, 84 or 85 (every write, before it is
 * stored; another number does nothing): 0 or the sensor's error for 85, which ends the command;
 * the keys by name with 0xED (*answer_len 0), 0xEE, 0xEF and 0xF0, or -ENOENT when the payload names
 * none of them; back to boot (the host tests). */
int osc2_imu_setting(uint8_t number, int16_t value);
int osc2_imu_by_name(uint8_t opcode, const uint8_t *payload, uint8_t len, uint8_t *answer_len);
void osc2_imu_reset(void);

#endif /* OPENSC2_IMU_RULES_H_ */
