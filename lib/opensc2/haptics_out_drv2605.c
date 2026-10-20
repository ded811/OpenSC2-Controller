/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * haptics_out_drv2605.c: one actuator (an ERM or an LRA) on a TI DRV2605(L) in real-time playback
 * (CONFIG_OPENSC2_HAPTICS_OUT_DRV2605), the node chosen as opensc2,haptics-drv2605. Zephyr's
 * ti,drv2605 driver sets the chip up (its calibration values from the devicetree) and puts it into
 * real-time playback; from then on every block the synthesiser renders becomes one amplitude: the
 * louder of the two paths' block levels, written to the chip's real-time input every 8 ms from the
 * path's thread (never from the radio's). The chip has no drive-frequency control: rumble, clicks
 * and pulses come through as strength, tones, sweeps, chimes and PCM lose their pitch. The input is
 * in the chip's default signed format, 0..127. Built; not yet tested on hardware.
 */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/haptics/drv2605.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <opensc2/haptics.h>

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

#define DRV_NODE DT_CHOSEN(opensc2_haptics_drv2605)
#define REG_RT_PLAYBACK_INPUT 0x02

static const struct device *const drv = DEVICE_DT_GET(DRV_NODE);
static const struct i2c_dt_spec drv_bus = I2C_DT_SPEC_GET(DRV_NODE);
static atomic_t path_level[2];
/* Both paths' threads call on_level(): one at a time works out the louder level and writes it, so
 * the last write always has both paths' latest levels (otherwise, when both fall quiet at once, an
 * older non-zero amplitude could land last and keep the actuator running). */
static K_MUTEX_DEFINE(rtp_lock);
static int16_t rtp_value = -1;              /* the real-time input last written; none yet */

static void on_level(enum opensc2_haptics_path path, uint8_t level, void *user_data)
{
	uint32_t louder;

	ARG_UNUSED(user_data);
	atomic_set(&path_level[path], level);
	(void)k_mutex_lock(&rtp_lock, K_FOREVER);
	louder = MAX((uint32_t)atomic_get(&path_level[0]), (uint32_t)atomic_get(&path_level[1]));
	if ((int16_t)(louder >> 1) != rtp_value &&
	    i2c_reg_write_byte_dt(&drv_bus, REG_RT_PLAYBACK_INPUT, (uint8_t)(louder >> 1)) == 0) {
		rtp_value = (int16_t)(louder >> 1);
	}
	(void)k_mutex_unlock(&rtp_lock);
}

static int drv2605_out_init(void)
{
	static uint32_t hold_us[1];
	static uint8_t input[1];
	static struct drv2605_rtp_data rtp = { .size = 1, .rtp_hold_us = hold_us, .rtp_input = input };
	union drv2605_config_data cfg = { .rtp_data = &rtp };
	int err;

	if (!device_is_ready(drv)) {
		LOG_ERR("Haptics: the DRV2605 is not ready");
		return 0;
	}
	err = drv2605_haptic_config(drv, DRV2605_HAPTICS_SOURCE_RTP, &cfg);
	if (err) {
		LOG_ERR("Haptics: the DRV2605 refused real-time playback (%d)", err);
		return 0;
	}
	(void)opensc2_haptics_set_level_output(OPENSC2_HAPTICS_PADS, on_level, NULL);
	return opensc2_haptics_set_level_output(OPENSC2_HAPTICS_GRIPS, on_level, NULL);
}

SYS_INIT(drv2605_out_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
