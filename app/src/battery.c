/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * battery.c: the battery level of input report 0x43 (CONFIG_APP_BATTERY, off by default), the way
 * the real controller's fuel gauge works (<opensc2/gauge.h>): once a second one reading of the
 * battery voltage, through a divider, plus the stored offset cal/volt_offset; the level from the
 * average of the last 40 readings, falling only while discharging and rising only while charging.
 * Untested on a battery.
 *
 * The reading comes from zephyr,user's io-channel named "battery" when the overlay has one, with
 *     battery-divider-ohms = <total bottom>;   (default <1 1>: the pin carries the battery itself)
 *     battery-enable-gpios = <...>;            (optional: the divider on for 5 ms around a reading)
 * and otherwise from the nRF52840's own VDDH / 5 input, for a cell that feeds the chip's VDDH pin
 * (check your board's schematic). The charge state is "charging" while USB power is present and
 * "discharging" otherwise; a charger with a status output could tell more.
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/dt-bindings/adc/nrf-saadc.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_power.h>
#include <opensc2/cal.h>
#include <opensc2/gauge.h>
#include "app.h"

LOG_MODULE_REGISTER(app_battery, LOG_LEVEL_INF);

#if defined(CONFIG_APP_BATTERY)

#define USER DT_PATH(zephyr_user)
#define PERIOD_MS 1000
#define SETTLE_MS 5

#if DT_PROP_HAS_NAME(USER, io_channels, battery)
static const struct adc_dt_spec adc = ADC_DT_SPEC_GET_BY_NAME(USER, battery);
#define DIVIDER_TOTAL  DT_PROP_BY_IDX_OR(USER, battery_divider_ohms, 0, 1)
#define DIVIDER_BOTTOM DT_PROP_BY_IDX_OR(USER, battery_divider_ohms, 1, 1)
#else
/* VDDH / 5 on a channel the overlay leaves free: gain 1/2 of the 0.6 V reference, 1.2 V full
 * scale, enough for a cell up to 6 V. */
#define VDDH_CHANNEL 7
static const struct device *const saadc = DEVICE_DT_GET(DT_NODELABEL(adc));
static const struct adc_channel_cfg vddh_cfg = {
	.gain = ADC_GAIN_1_2,
	.reference = ADC_REF_INTERNAL,
	.acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 40),
	.channel_id = VDDH_CHANNEL,
	.input_positive = NRF_SAADC_VDDHDIV5,
};
#define DIVIDER_TOTAL  5
#define DIVIDER_BOTTOM 1
#endif

#if DT_NODE_HAS_PROP(USER, battery_enable_gpios)
static const struct gpio_dt_spec enable = GPIO_DT_SPEC_GET(USER, battery_enable_gpios);
#endif

static struct opensc2_gauge gauge;

/* The pin's millivolts, or a negative errno. */
static int read_pin_mv(int32_t *mv)
{
	int16_t sample = 0;
	struct adc_sequence seq = {
		.buffer = &sample,
		.buffer_size = sizeof(sample),
	};
	int err;

#if DT_PROP_HAS_NAME(USER, io_channels, battery)
	(void)adc_sequence_init_dt(&adc, &seq);
	err = adc_read_dt(&adc, &seq);
	if (err) {
		return err;
	}
	*mv = sample;
	return adc_raw_to_millivolts_dt(&adc, mv);
#else
	seq.channels = BIT(VDDH_CHANNEL);
	seq.resolution = 12;
	err = adc_read(saadc, &seq);
	if (err) {
		return err;
	}
	*mv = sample;
	return adc_raw_to_millivolts(adc_ref_internal(saadc), vddh_cfg.gain, 12, mv);
#endif
}

static void battery_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	opensc2_gauge_init(&gauge, OPENSC2_GAUGE_NO_LEVEL);
	for (;;) {
		struct opensc2_battery b = { 0 };
		bool usb = nrf_power_usbregstatus_vbusdet_get(NRF_POWER);
		int32_t pin_mv = 0;
		int err;

#if DT_NODE_HAS_PROP(USER, battery_enable_gpios)
		(void)gpio_pin_set_dt(&enable, 1);
		k_msleep(SETTLE_MS);
#endif
		err = read_pin_mv(&pin_mv);
#if DT_NODE_HAS_PROP(USER, battery_enable_gpios)
		(void)gpio_pin_set_dt(&enable, 0);
#endif
		if (err == 0) {
			int32_t mv = opensc2_gauge_divider_mv(pin_mv, DIVIDER_TOTAL, DIVIDER_BOTTOM,
							      opensc2_cal_volt_offset());

			b.charge_state = usb ? OPENSC2_CHARGE_CHARGING : OPENSC2_CHARGE_DISCHARGING;
			b.battery_mv = (uint16_t)CLAMP(mv, 0, UINT16_MAX);
			b.level = opensc2_gauge_step(&gauge, b.battery_mv, b.charge_state);
			b.system_mv = 3300;
			b.input_mv = usb ? 5000 : 0;
			(void)opensc2_set_battery(&b);
		} else {
			LOG_WRN("Battery reading failed (%d)", err);
		}
		k_msleep(PERIOD_MS);
	}
}

K_THREAD_DEFINE(battery_tid, 1024, battery_thread, NULL, NULL, NULL, 10, 0, SYS_FOREVER_MS);

int battery_init(void)
{
#if DT_NODE_HAS_PROP(USER, battery_enable_gpios)
	if (!gpio_is_ready_dt(&enable) || gpio_pin_configure_dt(&enable, GPIO_OUTPUT_INACTIVE)) {
		LOG_ERR("Battery divider enable pin not usable");
	}
#endif
#if DT_PROP_HAS_NAME(USER, io_channels, battery)
	if (!adc_is_ready_dt(&adc) || adc_channel_setup_dt(&adc) != 0) {
		LOG_ERR("Battery ADC channel not usable");
		return -ENODEV;
	}
#else
	if (!device_is_ready(saadc) || adc_channel_setup(saadc, &vddh_cfg) != 0) {
		LOG_ERR("VDDH channel not usable");
		return -ENODEV;
	}
#endif
	k_thread_start(battery_tid);
	return 0;
}

#else

int battery_init(void)
{
	return 0;
}

#endif /* CONFIG_APP_BATTERY */
