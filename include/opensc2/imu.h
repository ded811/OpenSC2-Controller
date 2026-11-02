/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * imu.h: the IMU the way the real controller runs it, following the protocol documentation (not
 * public yet; docs/protocol.md lists the pages), driven by the samples the application's sensor
 * gives.
 *
 * The real controller streams its IMU from boot, whatever controller setting 48 reads, and turns
 * the stream off and on only when a host writes setting 48 (0: off, any other value: on) or the
 * key settings/sensors/imu/mode by name (0xEE); while it is off the state reports keep the last
 * sample. For each sample it scales the acceleration (32767 = 2 g), the angular rate (32767 =
 * 2000 deg/s, after subtracting the sensor's own gyro-bias estimate while use_bias is on: setting
 * 84, or the key use_bias) and the quaternion (32767 = 1, W first), turns all three by its
 * mounting matrix (identity until the board sets its own; the key mounting_matrix replaces it until
 * the next boot), and zeroes a rate strictly inside the dead zone (the key gyro_dz_threshold, 0 =
 * off). Setting 85 (and the key gyro_threshold) goes to the sensor's fusion block. A free fall that
 * the sensor detects plays the drop sounds 250 ms later and counts again 5 s after; turning the
 * stream off within those 5 s keeps further falls from counting until the next boot.
 *
 * The library does all of that (the keys by name included, RAM only as on the real controller);
 * the application gives samples (opensc2_imu_sample()) and, through a handler, runs its sensor:
 * the stream on or off, setting 85 into its fusion block.
 */
#ifndef OPENSC2_IMU_H_
#define OPENSC2_IMU_H_

#include <stdbool.h>
#include <stdint.h>
#include <opensc2/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One sample in the sensor's axes and SI units, as Zephyr's sensor API gives it. */
struct opensc2_imu_sample {
	float accel[3];          /* m/s^2 */
	float gyro[3];           /* rad/s */
	float bias[3];           /* the sensor's own gyro-bias estimate, rad/s (with has_bias) */
	float quat[4];           /* W, X, Y, Z: the sensor's game rotation vector (with has_quat) */
	uint64_t timestamp_ns;   /* the sample's time; the report carries microseconds (rounded) */
	bool has_bias;           /* without one nothing is subtracted (a sensor without fusion) */
	bool has_quat;           /* without one the quaternion is "none" (the reports' identity) */
};

/* One sample: processed as the real controller does (see above) and set as the state reports' IMU
 * fields. Any thread. -EAGAIN while the stream is off (the reports keep the last sample). */
int opensc2_imu_sample(const struct opensc2_imu_sample *sample);

/* Is the stream on? From boot yes; setting 48 and the key mode turn it off and on. */
bool opensc2_imu_streaming(void);

/* The board's mounting matrix, row major (report axes = m x sensor axes; the real controller's
 * board: { 0, 1, 0, -1, 0, 0, 0, 0, 1 }), set at start-up as the real controller's board does; a
 * host's key mounting_matrix replaces it until the next boot. NULL: identity. */
int opensc2_imu_set_mounting(const int8_t m[9]);

/* What the IMU runs with now. */
struct opensc2_imu_config {
	bool streaming;
	bool use_bias;           /* subtract the sensor's bias (setting 84, the key use_bias) */
	int8_t mounting[9];
	int32_t dead_zone;       /* the key gyro_dz_threshold, report units (0: off) */
	uint8_t gyro_threshold;  /* the last setting 85 / key gyro_threshold given (the low byte) */
	bool gyro_threshold_given;
	bool free_fall_armed;
};

void opensc2_imu_config_get(struct opensc2_imu_config *config);

/* The application's sensor, run by the library. Any callback may be NULL. They run in the thread
 * of the command that caused them (a link's thread) or at boot: return quickly. */
struct opensc2_imu_handler {
	/* The stream on (true) or off: a host wrote setting 48 or the key mode, every write (also
	 * of the same value), as on the real controller. Stop reading the sensor while off, or keep
	 * reading: opensc2_imu_sample() refuses samples then. */
	void (*stream)(bool on, void *user_data);
	/* Setting 85 or the key gyro_threshold: the threshold of the sensor's fusion block (its
	 * meaning is the sensor's). A negative errno fails the command, as on the real controller,
	 * where the sensor driver's answer is the command's. */
	int (*gyro_threshold)(uint8_t threshold, void *user_data);
};

int opensc2_imu_set_handler(const struct opensc2_imu_handler *handler, void *user_data);

/* The sensor detected a free fall (its own detector; any thread, an interrupt too): the drop
 * sounds (the haptics synthesiser's, CONFIG_OPENSC2_HAPTICS_DROP_SOUNDS) if falls count now. */
void opensc2_imu_free_fall(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENSC2_IMU_H_ */
