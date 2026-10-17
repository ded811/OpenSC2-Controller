/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * An IMU through Zephyr's sensor API. About 250 times a second: read acceleration (m/s^2) and
 * angular rate (rad/s), convert to report 0x42's units (16384 per g, 32767 = 2000 deg/s), run the
 * library's fusion helper for the orientation quaternion, and hand everything to the library.
 * Steam switches the stream on with controller setting 48, as for the real controller; until
 * then the IMU fields stay at zero. In Steam this feeds gyro controls.
 *
 * Axes: the controller's X points right, Y away from the player, Z up when it lies flat. Mount
 * the sensor that way or swap axes below.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/input/input.h>
#include <opensc2/opensc2.h>
#include <opensc2/fusion.h>

static const struct device *const imu = DEVICE_DT_GET(DT_NODELABEL(imu));
static const struct gpio_dt_spec vcc = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), vcc_enable_gpios);

static void on_button(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);
	if (evt->type == INPUT_EV_KEY && evt->code == OPENSC2_KEY_PAIR && evt->value) {
		opensc2_pair_start(0);
	}
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(buttons)), on_button, NULL);

int main(void)
{
	static const struct opensc2_config config = { .start = true };
	struct opensc2_fusion fusion;
	int64_t last = k_uptime_ticks();

	gpio_pin_configure_dt(&vcc, GPIO_OUTPUT_ACTIVE);
	opensc2_init(&config);
	opensc2_fusion_init(&fusion);
	if (!device_is_ready(imu)) {
		printk("IMU not ready\n");
		return 0;
	}

	for (;;) {
		struct sensor_value a[3], g[3];
		struct opensc2_imu out = { 0 };
		float accel[3], gyro[3];
		int64_t now;

		k_msleep(4);
		if (opensc2_setting_get(OPENSC2_SETTING_IMU_MODE) == OPENSC2_IMU_MODE_OFF) {
			opensc2_set_imu(&out);
			continue;
		}
		if (sensor_sample_fetch(imu) != 0 ||
		    sensor_channel_get(imu, SENSOR_CHAN_ACCEL_XYZ, a) != 0 ||
		    sensor_channel_get(imu, SENSOR_CHAN_GYRO_XYZ, g) != 0) {
			continue;
		}
		for (int i = 0; i < 3; i++) {
			accel[i] = sensor_value_to_float(&a[i]) / 9.80665f;   /* m/s^2 to g */
			gyro[i] = sensor_value_to_float(&g[i]) * 57.29578f;   /* rad/s to deg/s */
		}

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
	}
	return 0;
}
