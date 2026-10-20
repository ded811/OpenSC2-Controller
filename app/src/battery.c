/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * battery.c: report 0x43's battery, as far as the board has parts for it. Both are untested on
 * hardware; without either this file does nothing and the library's default battery stands.
 *
 *  - The level (CONFIG_APP_BATTERY, off by default), the way the real controller's fuel gauge
 *    works (<opensc2/gauge.h>): once a second one reading of the battery voltage, through a
 *    divider, plus the stored offset cal/volt_offset; the level from the average of the last 40
 *    readings, falling only while discharging and rising only while charging. The reading comes
 *    from zephyr,user's io-channel named "battery" when the overlay has one, with
 *        battery-divider-ohms = <total bottom>;   (default <1 1>: the pin carries the battery)
 *        battery-enable-gpios = <...>;            (optional: the divider on for 5 ms around it)
 *    and otherwise from the nRF52840's own VDDH / 5 input, for a cell that feeds the chip's VDDH
 *    pin (check your board's schematic).
 *  - The charge state from an optional charger status pin: a charger module's status output
 *    (TP4056-style "CHRG", active while it charges) on a pin named in zephyr,user,
 *        charger-status-gpios = <&gpio0 x (GPIO_PULL_UP | GPIO_ACTIVE_LOW)>;
 *    gives without USB power 1 (on battery), with it 2 while the pin is active (charging), else
 *    4 (done, level 100), once a second (opensc2_charge_state_of()). Without the pin the charge
 *    state is "charging" while USB power is present and "discharging" otherwise. On its own (no
 *    CONFIG_APP_BATTERY) the pin only sets the charge state: the level stays the library's
 *    default, 100 %, as this board then measures no voltage.
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

#define USER DT_PATH(zephyr_user)
#define CHARGER_PIN DT_NODE_HAS_PROP(USER, charger_status_gpios)

#if CHARGER_PIN || defined(CONFIG_APP_BATTERY)

#define PERIOD_MS 1000

static bool usb_power(void)
{
#if defined(NRF_POWER) && defined(NRF_POWER_HAS_USBREG) && NRF_POWER_HAS_USBREG
	return nrf_power_usbregstatus_vbusdet_get(NRF_POWER);
#else
	return false;
#endif
}

#if CHARGER_PIN
static const struct gpio_dt_spec chg = GPIO_DT_SPEC_GET(USER, charger_status_gpios);
#endif

/* The charge state: the charger's status pin when the board has one, else charging while USB
 * power is there. */
static uint8_t charge_state(bool usb)
{
#if CHARGER_PIN
	return opensc2_charge_state_of(usb, gpio_pin_get_dt(&chg) == 1);
#else
	return usb ? OPENSC2_CHARGE_CHARGING : OPENSC2_CHARGE_DISCHARGING;
#endif
}

static int charger_pin_init(void)
{
#if CHARGER_PIN
	if (!gpio_is_ready_dt(&chg) || gpio_pin_configure_dt(&chg, GPIO_INPUT) != 0) {
		LOG_ERR("Charger status pin not usable");
		return -ENODEV;
	}
#endif
	return 0;
}

#endif /* CHARGER_PIN || CONFIG_APP_BATTERY */

#if defined(CONFIG_APP_BATTERY)

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
		bool usb = usb_power();
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

			b.charge_state = charge_state(usb);
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
	(void)charger_pin_init();   /* without it, the charge state follows USB power */
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

#elif CHARGER_PIN

/* The charger's status pin only: the charge state once a second, the level the library's
 * default (100 %). */
static struct opensc2_battery battery = {
	.charge_state = OPENSC2_CHARGE_DONE,
	.level = 100,
	.battery_mv = 4100,
	.system_mv = 3300,
	.input_mv = 5000,
};

static void charger_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(charger_work, charger_fn);

static void charger_fn(struct k_work *work)
{
	bool power = usb_power();
	uint8_t state = charge_state(power);

	ARG_UNUSED(work);
	if (state != battery.charge_state) {
		LOG_INF("Charge state %u", state);
		battery.charge_state = state;
		battery.input_mv = power ? 5000 : 0;
		if (state == OPENSC2_CHARGE_DONE) {
			battery.level = 100;
		}
		(void)opensc2_set_battery(&battery);
	}
	k_work_schedule(&charger_work, K_MSEC(PERIOD_MS));
}

int battery_init(void)
{
	int err = charger_pin_init();

	if (err) {
		return err;
	}
	battery.charge_state = 0xFF;   /* the first look sets it */
	k_work_schedule(&charger_work, K_NO_WAIT);
	return 0;
}

#else

int battery_init(void)
{
	return 0;
}

#endif /* CONFIG_APP_BATTERY */
