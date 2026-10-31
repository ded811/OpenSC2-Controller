/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * battery.c: the board's battery parts for report 0x43, as far as it has them. Both are untested
 * on hardware; without either this file does nothing and the library's default battery (full,
 * charge done: a board without a battery) stands.
 *
 *  - The battery (CONFIG_APP_BATTERY, off by default): the library's fuel gauge
 *    (<opensc2/battery.h>) runs the real controller's rules on this board's readings. Its
 *    readings come from zephyr,user's io-channel named "battery" when the overlay has one, with
 *        battery-divider-ohms = <total bottom>;   (default <1 1>: the pin carries the battery)
 *        battery-enable-gpios = <...>;            (optional: the divider on for 5 ms, then the
 *                                                  readings, then off again)
 *    and otherwise from the nRF52840's own VDDH / 5 input, for a cell that feeds the chip's VDDH
 *    pin (check your board's schematic). The charger's values the board can measure: the system
 *    rail as the chip's own supply (VDD), the input supply from an optional io-channel named
 *    "input" (with input-divider-ohms = <total bottom>); the currents and the temperature it
 *    cannot measure stay 0.
 *  - The charge state from an optional charger status pin: a charger module's status output
 *    (TP4056-style "CHRG", active while it charges) on a pin named in zephyr,user,
 *        charger-status-gpios = <&gpio0 x (GPIO_PULL_UP | GPIO_ACTIVE_LOW)>;
 *    read at every pass of the gauge: active is charging, inactive with input power done,
 *    inactive without it off. Without the pin the library's charger model decides. On its own
 *    (no CONFIG_APP_BATTERY) the pin gives the charge state once a second and the level stays the
 *    library's default, 100 %, as this board then measures no voltage.
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/dt-bindings/adc/nrf-saadc.h>
#include <zephyr/logging/log.h>
#include <opensc2/battery.h>
#include <opensc2/gauge.h>
#include "app.h"

LOG_MODULE_REGISTER(app_battery, LOG_LEVEL_INF);

#define USER DT_PATH(zephyr_user)
#define CHARGER_PIN DT_NODE_HAS_PROP(USER, charger_status_gpios)

#if CHARGER_PIN || defined(CONFIG_APP_BATTERY)

#if CHARGER_PIN
static const struct gpio_dt_spec chg = GPIO_DT_SPEC_GET(USER, charger_status_gpios);
#endif

/* The charger's status from its status pin: charging while active; done while inactive with input
 * power; off without. Without the pin: unknown, the library's model decides. */
static enum opensc2_charger_status charger_status(void)
{
#if CHARGER_PIN
	if (gpio_pin_get_dt(&chg) == 1) {
		return OPENSC2_CHARGER_CHARGING;
	}
	return opensc2_power_source() != OPENSC2_POWER_SOURCE_BATTERY ? OPENSC2_CHARGER_DONE :
									  OPENSC2_CHARGER_OFF;
#else
	return OPENSC2_CHARGER_UNKNOWN;
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

static const struct device *const saadc = DEVICE_DT_GET(DT_NODELABEL(adc));

#if DT_PROP_HAS_NAME(USER, io_channels, battery)
static const struct adc_dt_spec adc = ADC_DT_SPEC_GET_BY_NAME(USER, battery);
#define DIVIDER_TOTAL  DT_PROP_BY_IDX_OR(USER, battery_divider_ohms, 0, 1)
#define DIVIDER_BOTTOM DT_PROP_BY_IDX_OR(USER, battery_divider_ohms, 1, 1)
#else
/* VDDH / 5 on a channel the overlay leaves free: gain 1/2 of the 0.6 V reference, 1.2 V full
 * scale, enough for a cell up to 6 V. */
#define VDDH_CHANNEL 7
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

/* The chip's own supply, the board's system rail: gain 1/6 of the 0.6 V reference (3.6 V). */
#define VDD_CHANNEL 6
static const struct adc_channel_cfg vdd_cfg = {
	.gain = ADC_GAIN_1_6,
	.reference = ADC_REF_INTERNAL,
	.acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 40),
	.channel_id = VDD_CHANNEL,
	.input_positive = NRF_SAADC_VDD,
};
static bool vdd_ok;

#if DT_PROP_HAS_NAME(USER, io_channels, input)
static const struct adc_dt_spec input_adc = ADC_DT_SPEC_GET_BY_NAME(USER, input);
#define INPUT_TOTAL  DT_PROP_BY_IDX_OR(USER, input_divider_ohms, 0, 1)
#define INPUT_BOTTOM DT_PROP_BY_IDX_OR(USER, input_divider_ohms, 1, 1)
static bool input_ok;
#endif

#if DT_NODE_HAS_PROP(USER, battery_enable_gpios)
static const struct gpio_dt_spec enable = GPIO_DT_SPEC_GET(USER, battery_enable_gpios);
#endif

/* One conversion of a channel set up on the SAADC by this file, in millivolts at the pin. */
static int read_channel_mv(uint8_t channel, enum adc_gain gain, int32_t *mv)
{
	int16_t sample = 0;
	struct adc_sequence seq = {
		.channels = BIT(channel),
		.buffer = &sample,
		.buffer_size = sizeof(sample),
		.resolution = 12,
	};
	int err = adc_read(saadc, &seq);

	if (err) {
		return err;
	}
	*mv = sample;
	return adc_raw_to_millivolts(adc_ref_internal(saadc), gain, 12, mv);
}

/* One conversion of an io-channel of the overlay, in millivolts at the pin. */
static int __maybe_unused read_dt_mv(const struct adc_dt_spec *spec, int32_t *mv)
{
	int16_t sample = 0;
	struct adc_sequence seq = {
		.buffer = &sample,
		.buffer_size = sizeof(sample),
	};
	int err;

	(void)adc_sequence_init_dt(spec, &seq);
	err = adc_read_dt(spec, &seq);
	if (err) {
		return err;
	}
	*mv = sample;
	return adc_raw_to_millivolts_dt(spec, mv);
}

/* The battery pin's millivolts, or a negative errno. */
static int read_pin_mv(int32_t *mv)
{
#if DT_PROP_HAS_NAME(USER, io_channels, battery)
	return read_dt_mv(&adc, mv);
#else
	return read_channel_mv(VDDH_CHANNEL, vddh_cfg.gain, mv);
#endif
}

/* The gauge asks for n readings in a row: the divider on once for all of them. */
static int read_mv(void *user, uint16_t *mv, size_t n)
{
	size_t got = 0;
	int err = 0;

	ARG_UNUSED(user);
#if DT_NODE_HAS_PROP(USER, battery_enable_gpios)
	(void)gpio_pin_set_dt(&enable, 1);
	k_msleep(SETTLE_MS);
#endif
	while (got < n) {
		int32_t pin_mv = 0;

		err = read_pin_mv(&pin_mv);
		if (err) {
			break;
		}
		/* the offset (cal/volt_offset) is the library's to add */
		pin_mv = opensc2_gauge_divider_mv(pin_mv, DIVIDER_TOTAL, DIVIDER_BOTTOM, 0);
		mv[got++] = (uint16_t)CLAMP(pin_mv, 0, UINT16_MAX);
	}
#if DT_NODE_HAS_PROP(USER, battery_enable_gpios)
	(void)gpio_pin_set_dt(&enable, 0);
#endif
	if (got == 0 && err) {
		LOG_WRN("Battery reading failed (%d)", err);
		return err;
	}
	return (int)got;
}

/* At every pass of the gauge: the charger's status and what this board measures of its values. */
static int charger_update(void *user)
{
	struct opensc2_charger_values v = { 0 };
	int32_t mv;

	ARG_UNUSED(user);
	if (vdd_ok && read_channel_mv(VDD_CHANNEL, vdd_cfg.gain, &mv) == 0) {
		v.system_mv = (uint16_t)CLAMP(mv, 0, UINT16_MAX);
	}
#if DT_PROP_HAS_NAME(USER, io_channels, input)
	if (input_ok && read_dt_mv(&input_adc, &mv) == 0) {
		mv = opensc2_gauge_divider_mv(mv, INPUT_TOTAL, INPUT_BOTTOM, 0);
		v.input_mv = (uint16_t)CLAMP(mv, 0, UINT16_MAX);
	}
#endif
	(void)opensc2_charger_set_values(&v);
	return opensc2_charger_set_status(charger_status());
}

static const struct opensc2_battery_board board = {
	.read_mv = read_mv,
	.charger_update = charger_update,
	/* No charger controls and no cut-off on this board: a charger module charges on its own.
	 * A board with a charger chip adds set_charge_voltage, set_input_voltage_limit and
	 * battery_cutoff (docs/api.md, "The battery and the charger"). */
};

int battery_init(void)
{
	int err;

	(void)charger_pin_init();   /* without it, the library's charger model decides */
#if DT_NODE_HAS_PROP(USER, battery_enable_gpios)
	if (!gpio_is_ready_dt(&enable) || gpio_pin_configure_dt(&enable, GPIO_OUTPUT_INACTIVE)) {
		LOG_ERR("Battery divider enable pin not usable");
	}
#endif
	vdd_ok = device_is_ready(saadc) && adc_channel_setup(saadc, &vdd_cfg) == 0;
	if (!vdd_ok) {
		LOG_WRN("VDD channel not usable: the system voltage reads 0");
	}
#if DT_PROP_HAS_NAME(USER, io_channels, input)
	input_ok = adc_is_ready_dt(&input_adc) && adc_channel_setup_dt(&input_adc) == 0;
	if (!input_ok) {
		LOG_WRN("Input voltage channel not usable: the input voltage reads 0");
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
	err = opensc2_battery_start(&board);
	if (err) {
		LOG_ERR("Fuel gauge not started (%d)", err);
	}
	return err;
}

#elif CHARGER_PIN

/* The charger's status pin only: the charge state once a second, the level the library's
 * default (100 %). */
static uint8_t last = 0xFF;

static void charger_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(charger_work, charger_fn);

static void charger_fn(struct k_work *work)
{
	enum opensc2_charger_status status = charger_status();

	ARG_UNUSED(work);
	if (status != last) {
		LOG_INF("Charger status %u", status);
		last = (uint8_t)status;
		(void)opensc2_charger_set_status(status);
	}
	k_work_schedule(&charger_work, K_MSEC(1000));
}

int battery_init(void)
{
	int err = charger_pin_init();

	if (err) {
		return err;
	}
	k_work_schedule(&charger_work, K_NO_WAIT);
	return 0;
}

#else

int battery_init(void)
{
	return 0;
}

#endif /* CONFIG_APP_BATTERY */
