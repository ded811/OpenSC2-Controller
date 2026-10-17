/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Buttons, a stick and a trigger from the devicetree: a gpio-keys node whose zephyr,code values
 * are bits of the controller's button word, and ADC channels named in zephyr,user. The full
 * version (every input, auto-ranging, unwired-pin bias) is app/src/inputs.c.
 */
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <opensc2/opensc2.h>

#define USER DT_PATH(zephyr_user)

static const struct adc_dt_spec stick_x = ADC_DT_SPEC_GET_BY_NAME(USER, stick_x);
static const struct adc_dt_spec stick_y = ADC_DT_SPEC_GET_BY_NAME(USER, stick_y);
static const struct adc_dt_spec trigger = ADC_DT_SPEC_GET_BY_NAME(USER, trigger);
static const struct gpio_dt_spec vcc = GPIO_DT_SPEC_GET(USER, vcc_enable_gpios);

/* Buttons: each key's code is the bit to set. */
static void on_button(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);
	if (evt->type != INPUT_EV_KEY) {
		return;
	}
	if (evt->code == OPENSC2_KEY_PAIR) {
		if (evt->value) {
			opensc2_pair_start(0);
		}
	} else if (evt->code < 32) {
		opensc2_set_buttons(BIT(evt->code), evt->value ? BIT(evt->code) : 0);
	}
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(buttons)), on_button, NULL);

static int32_t read_adc(const struct adc_dt_spec *spec)
{
	int16_t sample = 0;
	struct adc_sequence seq = { .buffer = &sample, .buffer_size = sizeof(sample) };

	adc_sequence_init_dt(spec, &seq);
	return adc_read_dt(spec, &seq) == 0 ? MAX(sample, 0) : 2048;
}

/* 12-bit reading to -32767..32767 around `centre`: full output 1600 counts away, 100 counts of
 * dead zone in the middle. */
static int16_t to_axis(int32_t raw, int32_t centre)
{
	int32_t d = raw - centre;
	int32_t mag = CLAMP(abs(d) - 100, 0, 1500);

	return (int16_t)((d < 0 ? -mag : mag) * OPENSC2_AXIS_MAX / 1500);
}

int main(void)
{
	static const struct opensc2_config config = { .start = true };
	int32_t centre_x, centre_y, rest;

	gpio_pin_configure_dt(&vcc, GPIO_OUTPUT_ACTIVE);
	adc_channel_setup_dt(&stick_x);
	adc_channel_setup_dt(&stick_y);
	adc_channel_setup_dt(&trigger);
	k_msleep(20);
	centre_x = read_adc(&stick_x);   /* the stick rests in the middle at power-on */
	centre_y = read_adc(&stick_y);
	rest = read_adc(&trigger);

	opensc2_init(&config);
	for (;;) {
		int32_t t = CLAMP(read_adc(&trigger) - rest - 80, 0, 3200);

		opensc2_set_stick(OPENSC2_LEFT, to_axis(read_adc(&stick_x), centre_x),
				  to_axis(read_adc(&stick_y), centre_y));
		opensc2_set_trigger(OPENSC2_RIGHT, (uint16_t)(t * OPENSC2_TRIGGER_MAX / 3200));
		k_msleep(4);
	}
	return 0;
}
