/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * inputs.c: buttons, sticks and triggers, all described in the board overlay.
 *
 *  - Buttons are a gpio-keys node labelled opensc2_buttons; each key's zephyr,code is a bit of
 *    the controller's button word (dt-bindings/opensc2/inputs.h).
 *  - Sticks and analog triggers are ADC channels named in zephyr,user's io-channel-names. An axis
 *    that is not named reads neutral.
 *
 * Calibration needs no setup: at boot each stick's resting value is its centre, the usable range
 * starts at 40 % of full scale either side and grows when the stick goes further (that growth is
 * not saved), and a small centre dead zone hides potentiometer noise. Analog inputs are biased by
 * the SAADC's own resistors, sticks to mid-scale and triggers to their released end, so a pin that
 * is listed but not wired reads neutral instead of drifting.
 */
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#if defined(CONFIG_ADC_NRFX_SAADC)
#include <hal/nrf_saadc.h>
#endif
#include "app.h"

LOG_MODULE_REGISTER(inputs, LOG_LEVEL_INF);

#define USER DT_PATH(zephyr_user)

#define SAMPLE_PERIOD_MS     4      /* the puck polls about every 3 ms */
#define STICK_START_RANGE    40     /* percent of full scale either side of the centre */
#define TRIGGER_START_RANGE  80     /* percent of full scale from released to pulled */
#define TRIGGER_DEADZONE     5      /* percent of the trigger's travel */

enum axis { LEFT_X, LEFT_Y, RIGHT_X, RIGHT_Y, LEFT_TRIGGER, RIGHT_TRIGGER, AXES };

struct axis_cfg {
	bool present;
	bool invert;
	bool stick;
	struct adc_dt_spec adc;
};

#define AXIS(name, inverted, is_stick)                                                      \
	{                                                                                   \
		.present = DT_PROP_HAS_NAME(USER, io_channels, name),                       \
		.invert = DT_PROP_OR(USER, inverted, 0),                                    \
		.stick = is_stick,                                                          \
		.adc = COND_CODE_1(DT_PROP_HAS_NAME(USER, io_channels, name),              \
				   (ADC_DT_SPEC_GET_BY_NAME(USER, name)), ({ 0 })),         \
	}

static const struct axis_cfg axes[AXES] = {
	[LEFT_X] = AXIS(left_stick_x, invert_left_stick_x, true),
	[LEFT_Y] = AXIS(left_stick_y, invert_left_stick_y, true),
	[RIGHT_X] = AXIS(right_stick_x, invert_right_stick_x, true),
	[RIGHT_Y] = AXIS(right_stick_y, invert_right_stick_y, true),
	[LEFT_TRIGGER] = AXIS(left_trigger, invert_left_trigger, false),
	[RIGHT_TRIGGER] = AXIS(right_trigger, invert_right_trigger, false),
};

/* Calibration in raw ADC counts. Sticks: centre and the two extremes. Triggers: rest and pulled. */
static struct {
	int32_t full;
	int32_t centre, low, high;
} cal[AXES];

static const int stick_deadzone = DT_PROP_OR(USER, stick_deadzone_percent, 6);

#if DT_NODE_HAS_PROP(USER, vcc_enable_gpios)
static const struct gpio_dt_spec vcc = GPIO_DT_SPEC_GET(USER, vcc_enable_gpios);
#endif

/* ---- buttons --------------------------------------------------------------------------------- */

#if DT_NODE_EXISTS(DT_NODELABEL(opensc2_buttons))
static void on_button(struct input_event *evt, void *user_data)
{
	uint32_t bit;

	ARG_UNUSED(user_data);
	if (evt->type != INPUT_EV_KEY) {
		return;
	}
	if (evt->value) {
		app_button_pressed();
	}
	if (evt->code == OPENSC2_KEY_PAIR) {
		app_pair_button(evt->value != 0);
		return;
	}
	/* A trigger button makes that trigger digital, unless it has an analog channel. */
	if (evt->code == OPENSC2_KEY_LEFT_TRIGGER_CLICK && !axes[LEFT_TRIGGER].present) {
		opensc2_set_trigger(OPENSC2_LEFT, evt->value ? OPENSC2_TRIGGER_MAX : 0);
		return;
	}
	if (evt->code == OPENSC2_KEY_RIGHT_TRIGGER_CLICK && !axes[RIGHT_TRIGGER].present) {
		opensc2_set_trigger(OPENSC2_RIGHT, evt->value ? OPENSC2_TRIGGER_MAX : 0);
		return;
	}
	if (evt->code < 32) {
		bit = BIT(evt->code);
		opensc2_set_buttons(bit, evt->value ? bit : 0);
	}
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(opensc2_buttons)), on_button, NULL);
#endif

/* ---- analog ---------------------------------------------------------------------------------- */

static int read_axis(const struct axis_cfg *a, int32_t *raw)
{
	int16_t sample = 0;
	struct adc_sequence seq = {
		.buffer = &sample,
		.buffer_size = sizeof(sample),
	};
	int err;

	adc_sequence_init_dt(&a->adc, &seq);
	err = adc_read_dt(&a->adc, &seq);
	if (err) {
		return err;
	}
	*raw = MAX(sample, 0);
	return 0;
}

/* Bias an analog input with the SAADC's internal resistors so that an unconnected pin rests at a
 * neutral value: mid-scale for a stick, the released end for a trigger. About 160 kOhm, far above
 * a potentiometer's own impedance, so a wired input is not disturbed noticeably. */
static void bias_input(const struct axis_cfg *a)
{
#if defined(CONFIG_ADC_NRFX_SAADC) && defined(SAADC_CH_CONFIG_RESP_Msk) && \
	DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(adc))
	uint32_t resp = a->stick ? SAADC_CH_CONFIG_RESP_VDD1_2 :
			a->invert ? SAADC_CH_CONFIG_RESP_Pullup : SAADC_CH_CONFIG_RESP_Pulldown;
	uint32_t config;

	if (a->adc.dev != DEVICE_DT_GET(DT_NODELABEL(adc))) {
		return;   /* an external ADC: no internal resistors to use */
	}
	config = NRF_SAADC->CH[a->adc.channel_id].CONFIG;
	config &= ~SAADC_CH_CONFIG_RESP_Msk;
	config |= resp << SAADC_CH_CONFIG_RESP_Pos;
	NRF_SAADC->CH[a->adc.channel_id].CONFIG = config;
#else
	ARG_UNUSED(a);
#endif
}

static int16_t stick_value(int i, int32_t raw)
{
	int32_t d, half, dz, mag, v;

	/* The range grows when the stick goes past it. */
	cal[i].low = MIN(cal[i].low, raw);
	cal[i].high = MAX(cal[i].high, raw);
	d = raw - cal[i].centre;
	half = d >= 0 ? cal[i].high - cal[i].centre : cal[i].centre - cal[i].low;
	if (half <= 0) {
		return 0;
	}
	dz = half * stick_deadzone / 100;
	mag = abs(d) - dz;
	if (mag <= 0) {
		return 0;
	}
	v = MIN(mag * OPENSC2_AXIS_MAX / MAX(half - dz, 1), OPENSC2_AXIS_MAX);
	return (int16_t)(d >= 0 ? v : -v);
}

static uint16_t trigger_value(int i, int32_t raw)
{
	int32_t span, dz, v;

	cal[i].high = MAX(cal[i].high, raw);
	span = cal[i].high - cal[i].low;
	dz = span * TRIGGER_DEADZONE / 100;
	v = raw - cal[i].low - dz;
	if (span <= 0 || v <= 0) {
		return 0;
	}
	return (uint16_t)MIN(v * OPENSC2_TRIGGER_MAX / MAX(span - dz, 1), OPENSC2_TRIGGER_MAX);
}

static int32_t oriented(int i, int32_t raw)
{
	return axes[i].invert ? cal[i].full - raw : raw;
}

static void calibrate(int i)
{
	int32_t sum = 0, raw = 0;
	int n = 0;

	cal[i].full = BIT(axes[i].adc.resolution) - 1;
	for (int k = 0; k < 8; k++) {
		if (read_axis(&axes[i], &raw) == 0) {
			sum += oriented(i, raw);
			n++;
		}
		k_msleep(1);
	}
	raw = n ? sum / n : cal[i].full / 2;
	if (axes[i].stick) {
		/* A stick should rest near the middle; anything else is not trusted as a centre. */
		bool plausible = raw > cal[i].full / 4 && raw < cal[i].full * 3 / 4;
		int32_t range = cal[i].full * STICK_START_RANGE / 100;

		cal[i].centre = plausible ? raw : cal[i].full / 2;
		cal[i].low = MAX(cal[i].centre - range, 0);
		cal[i].high = MIN(cal[i].centre + range, cal[i].full);
	} else {
		cal[i].low = raw < cal[i].full / 2 ? raw : 0;
		cal[i].high = MIN(cal[i].low + cal[i].full * TRIGGER_START_RANGE / 100, cal[i].full);
	}
	LOG_INF("Axis %d on ADC channel %u: rest %d of %d", i, axes[i].adc.channel_id, raw,
		cal[i].full);
}

static void analog_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (int i = 0; i < AXES; i++) {
		if (!axes[i].present) {
			continue;
		}
		if (!adc_is_ready_dt(&axes[i].adc) || adc_channel_setup_dt(&axes[i].adc) != 0) {
			LOG_ERR("ADC channel for axis %d not usable", i);
			continue;
		}
		bias_input(&axes[i]);
	}
	k_msleep(20);   /* let the inputs and the bias settle */
	for (int i = 0; i < AXES; i++) {
		if (axes[i].present) {
			calibrate(i);
		}
	}

	for (;;) {
		int16_t stick[4] = { 0 };
		int32_t raw;

		for (int i = 0; i < AXES; i++) {
			if (!axes[i].present || read_axis(&axes[i], &raw) != 0) {
				continue;
			}
			raw = oriented(i, raw);
			if (axes[i].stick) {
				stick[i] = stick_value(i, raw);
			} else {
				opensc2_set_trigger(i == LEFT_TRIGGER ? OPENSC2_LEFT : OPENSC2_RIGHT,
						    trigger_value(i, raw));
			}
		}
		if (axes[LEFT_X].present || axes[LEFT_Y].present) {
			opensc2_set_stick(OPENSC2_LEFT, stick[LEFT_X], stick[LEFT_Y]);
		}
		if (axes[RIGHT_X].present || axes[RIGHT_Y].present) {
			opensc2_set_stick(OPENSC2_RIGHT, stick[RIGHT_X], stick[RIGHT_Y]);
		}
		k_msleep(SAMPLE_PERIOD_MS);
	}
}

K_THREAD_DEFINE(analog_tid, 1024, analog_thread, NULL, NULL, NULL, 7, 0, SYS_FOREVER_MS);

int inputs_init(void)
{
	bool any = false;

#if DT_NODE_HAS_PROP(USER, vcc_enable_gpios)
	/* Power the potentiometers before the first reading. */
	if (gpio_is_ready_dt(&vcc)) {
		gpio_pin_configure_dt(&vcc, GPIO_OUTPUT_ACTIVE);
	}
#endif
	for (int i = 0; i < AXES; i++) {
		any |= axes[i].present;
	}
	if (any) {
		k_thread_start(analog_tid);
	}
	return 0;
}
