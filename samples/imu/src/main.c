/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * An IMU through Zephyr's sensor API, wired or wireless. About 250 times a second: read
 * acceleration (m/s^2) and angular rate (rad/s), convert to report 0x42's units (16384 per g,
 * 32767 = 2000 deg/s), run the library's fusion helper for the orientation quaternion, and hand
 * everything to the library. Steam switches the stream on with controller setting 48, as for the
 * real controller; until then the IMU fields stay at zero. In Steam this feeds gyro controls.
 *
 * Axes: the controller's X points right, Y away from the player, Z up when it lies flat. Mount
 * the sensor that way or swap axes below.
 * There is no power_request callback: the library does the reboots Steam asks for itself, and it
 * ignores Steam's request to enter a bootloader for a firmware update.
 */
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <opensc2/opensc2.h>
#include <opensc2/fusion.h>

LOG_MODULE_REGISTER(imu_sample, LOG_LEVEL_INF);

static const struct device *const imu = DEVICE_DT_GET(DT_NODELABEL(imu));
static const struct gpio_dt_spec vcc = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), vcc_enable_gpios);

/* The sensor runs from the board's switched VCC, and its driver talks to it while Zephyr starts,
 * before main(). So VCC goes on first: GPIO is ready by then (PRE_KERNEL_1), and the sensor
 * driver starts later in POST_KERNEL (CONFIG_SENSOR_INIT_PRIORITY, 90). */
static int vcc_on(void)
{
	if (gpio_is_ready_dt(&vcc)) {
		gpio_pin_configure_dt(&vcc, GPIO_OUTPUT_ACTIVE);
		k_msleep(100);   /* the MPU-6050 needs up to 100 ms after power-up */
	}
	return 0;
}
SYS_INIT(vcc_on, POST_KERNEL, 0);

/* ---- The pairing button: the same rules in every sample ------------------------------------- */

static const struct gpio_dt_spec pair_pin = GPIO_DT_SPEC_GET(DT_NODELABEL(opensc2_pair), gpios);
static bool pair_held_at_boot;   /* the press that chose wireless: ignored until it is released */
static bool pair_down;

/* Is the button held while the board starts? gpio-keys has already set the pin up with its
 * pull-up. Read twice, 5 ms apart: a bouncing contact counts as released. */
static bool pair_held(void)
{
	if (!gpio_is_ready_dt(&pair_pin) || gpio_pin_get_dt(&pair_pin) != 1) {
		return false;
	}
	k_msleep(5);
	return gpio_pin_get_dt(&pair_pin) == 1;
}

/* Wired: pressed and released, reboot into wireless mode (on the release, so that the button is
 * up again at the next boot). Wireless: pressed, answer a puck's open pairing window (untested
 * with Steam, which pairs controllers over the cable). */
static void pair_button(bool pressed)
{
	if (pair_held_at_boot) {
		pair_held_at_boot = pressed;   /* released: a normal button from now on */
		return;
	}
	if (opensc2_mode() == OPENSC2_MODE_WIRED) {
		if (pair_down && !pressed) {
			opensc2_reboot_later(OPENSC2_REBOOT_WIRELESS, "pair button", 0);
		}
	} else if (pressed) {
		opensc2_pair_start(0);   /* 0: the default window, 60 s */
	}
	pair_down = pressed;
}

static void on_button(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);
	if (evt->type == INPUT_EV_KEY && evt->code == OPENSC2_KEY_PAIR) {
		pair_button(evt->value != 0);
	}
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(opensc2_buttons)), on_button, NULL);

/* ---- The IMU --------------------------------------------------------------------------------- */

int main(void)
{
	struct opensc2_config config = { .start = true };
	struct opensc2_fusion fusion;
	int64_t last;
	int err;

	if (pair_held()) {
		pair_held_at_boot = true;
		config.boot_mode = OPENSC2_BOOT_WIRELESS;   /* held while plugging in: wireless */
	}
	err = opensc2_init(&config);
	if (err) {
		/* The console still starts (in wireless mode), so this can be read there. */
		LOG_ERR("opensc2_init failed (%d)", err);
	}
	if (!device_is_ready(imu)) {
		LOG_ERR("The IMU did not start: check its wiring and its I2C address");
		return 0;   /* the controller goes on without IMU data */
	}

	opensc2_fusion_init(&fusion);
	last = k_uptime_ticks();
	for (;;) {
		struct sensor_value a[3], g[3];
		struct opensc2_imu out = { 0 };
		float accel[3], gyro[3];
		int64_t now;

		k_msleep(4);
		now = k_uptime_ticks();
		if (opensc2_setting_get(OPENSC2_SETTING_IMU_MODE) == OPENSC2_IMU_MODE_OFF) {
			opensc2_set_imu(&out);   /* Steam has the IMU off: zeros, as the real one */
			last = now;
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
