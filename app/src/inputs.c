/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * inputs.c: buttons, sticks and triggers, all described in the board overlay.
 *
 *  - Buttons are a gpio-keys node labelled opensc2_buttons; each key's zephyr,code is a bit of
 *    the controller's button word (dt-bindings/opensc2/inputs.h). A second one labelled
 *    opensc2_buttons_polled may add keys that gpio-keys polls, for example on an I2C port
 *    expander (expander.overlay); the chosen node opensc2,touch-keys may name any input device
 *    whose key codes are such bits, for example a capacitive sensor's stick-touch and grip keys
 *    (sensors.overlay). A touch only sets its bit: it is not a button press, so it does not
 *    bring a turned-off controller back.
 *  - Sticks and analog triggers are ADC channels named in zephyr,user's io-channel-names. An axis
 *    that is not named reads neutral.
 *
 * Calibration needs no setup: at boot each stick's resting value is its centre, the usable range
 * starts at 40 % of full scale either side and grows when the stick goes further (that growth is
 * not saved), and a small centre dead zone hides potentiometer noise. Analog inputs are biased by
 * the SAADC's own resistors, sticks to mid-scale and triggers to their released end, so a pin that
 * is listed but not wired reads neutral instead of drifting.
 *
 * Two options (app/Kconfig, both off by default) follow the real controller more closely:
 *  - CONFIG_APP_ANALOG_SAMPLE: no thread of its own; the axes are read in the library's `sample`
 *    callback, right before each state report, four conversions each averaged, with an optional
 *    sensor supply (zephyr,user sensor-supply-gpios) on only while they are read;
 *  - CONFIG_APP_ANALOG_RAW (with it): a stick or trigger that has a stored calibration goes to the
 *    library as raw counts (the real controller's processing, settings 46 and 62, the recordings
 *    of vendor commands 0xD8 and 0xC0, `opensc2 cal`); one without keeps the processing above, so a
 *    new board works before it is calibrated.
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
#include <opensc2/cal.h>
#include <opensc2/sensor_helpers.h>
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

#if DT_NODE_EXISTS(DT_NODELABEL(opensc2_buttons)) || \
	DT_NODE_EXISTS(DT_NODELABEL(opensc2_buttons_polled))
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
#endif
#if DT_NODE_EXISTS(DT_NODELABEL(opensc2_buttons))
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(opensc2_buttons)), on_button, NULL);
#endif
#if DT_NODE_EXISTS(DT_NODELABEL(opensc2_buttons_polled))
INPUT_CALLBACK_DEFINE_NAMED(DEVICE_DT_GET(DT_NODELABEL(opensc2_buttons_polled)), on_button, NULL,
			    on_polled_button);
#endif
#if DT_HAS_CHOSEN(opensc2_touch_keys)
/* Touch keys from a sensor that decides on its own, for example the stock CAP12xx driver's: its
 * thresholds apply, not settings 30, 31, 34 and 35 (those apply to the raw words of
 * opensc2_set_touch_raw()). Only their bits: a touch is not a button press (on_button() would
 * bring a turned-off controller back whenever a hand closed on a grip). */
static void on_touch_key(struct input_event *evt, void *user_data)
{
	uint32_t bit;

	ARG_UNUSED(user_data);
	if (evt->type != INPUT_EV_KEY || evt->code >= 32) {
		return;
	}
	bit = BIT(evt->code);
	opensc2_set_buttons(bit, evt->value ? bit : 0);
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_CHOSEN(opensc2_touch_keys)), on_touch_key, NULL);
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

static void setup_axes(void)
{
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
}

#if defined(CONFIG_APP_ANALOG_SAMPLE)

/* ---- sampled right before each state report (CONFIG_APP_ANALOG_SAMPLE) ------------------------ */

#define BURST 4   /* conversions per axis, averaged: the real controller's four scans */

#if DT_NODE_HAS_PROP(USER, sensor_supply_gpios)
static const struct gpio_dt_spec supply = GPIO_DT_SPEC_GET(USER, sensor_supply_gpios);
#endif

/* The sensors' supply on, the settle time (200 us on the real controller), or off. */
static void sensor_supply(bool on)
{
#if DT_NODE_HAS_PROP(USER, sensor_supply_gpios)
	(void)gpio_pin_set_dt(&supply, on ? 1 : 0);
	if (on) {
		k_busy_wait(DT_PROP_OR(USER, sensor_supply_settle_us, 200));
	}
#else
	ARG_UNUSED(on);
#endif
}

/* BURST conversions of each axis, averaged toward zero. The axes on the first axis's ADC go in one
 * sequence, all their channels scanned BURST times in a row as the real controller does; an axis
 * on another ADC is read BURST times on its own. */
static void burst_read(int32_t raw[AXES], bool ok[AXES])
{
	const struct device *dev = NULL;
	int16_t samples[BURST * AXES];
	uint16_t avg[AXES];
	uint32_t mask = 0;
	int rank[AXES] = { 0 }, n = 0;

	for (int i = 0; i < AXES; i++) {
		ok[i] = false;
		if (axes[i].present && (dev == NULL || axes[i].adc.dev == dev)) {
			dev = axes[i].adc.dev;
			mask |= BIT(axes[i].adc.channel_id);
		}
	}
	/* the samples of one scan come in the order of the channel numbers */
	for (int ch = 0; ch < 32; ch++) {
		for (int i = 0; i < AXES; i++) {
			if ((mask & BIT(ch)) && axes[i].present && axes[i].adc.dev == dev &&
			    axes[i].adc.channel_id == ch) {
				rank[i] = n++;
			}
		}
	}
	if (dev != NULL && n > 0) {
		struct adc_sequence_options opt = { .extra_samplings = BURST - 1 };
		struct adc_sequence seq = {
			.options = &opt,
			.channels = mask,
			.buffer = samples,
			.buffer_size = sizeof(samples),
			.resolution = axes[0].present ? axes[0].adc.resolution : 12,
		};

		for (int i = 0; i < AXES; i++) {
			if (axes[i].present && axes[i].adc.dev == dev) {
				seq.resolution = axes[i].adc.resolution;
				break;
			}
		}
		if (adc_read(dev, &seq) == 0) {
			opensc2_adc_average(samples, (uint8_t)n, BURST, avg);
			for (int i = 0; i < AXES; i++) {
				if (axes[i].present && axes[i].adc.dev == dev) {
					raw[i] = avg[rank[i]];
					ok[i] = true;
				}
			}
		}
	}
	for (int i = 0; i < AXES; i++) {
		int32_t sum = 0, one;
		int k;

		if (!axes[i].present || axes[i].adc.dev == dev) {
			continue;
		}
		for (k = 0; k < BURST && read_axis(&axes[i], &one) == 0; k++) {
			sum += one;
		}
		if (k == BURST) {
			raw[i] = sum / BURST;
			ok[i] = true;
		}
	}
}

/* A reading as 12-bit counts, what the library's calibration takes. */
static uint16_t counts12(int i, int32_t raw)
{
	return (uint16_t)MIN(raw * OPENSC2_CAL_ADC_MAX / MAX(cal[i].full, 1), OPENSC2_CAL_ADC_MAX);
}

/* With CONFIG_APP_ANALOG_RAW, a calibrated stick or trigger goes to the library as raw counts
 * (true), and so does one whose recording runs (0xD8, 0xC0, "opensc2 cal"), which takes its
 * samples from raw inputs only; otherwise it is given back to the finished values (false). */
static bool raw_stick(enum opensc2_side side, uint16_t x, uint16_t y)
{
#if defined(CONFIG_APP_ANALOG_RAW)
	if (opensc2_cal_stick_ok(side) || opensc2_cal_recording(OPENSC2_CAL_REC_STICKS)) {
		return opensc2_set_stick_raw(side, x, y) == 0;
	}
	(void)opensc2_cal_raw_release((uint8_t)(OPENSC2_RAW_STICK_L << side));
#else
	ARG_UNUSED(side);
	ARG_UNUSED(x);
	ARG_UNUSED(y);
#endif
	return false;
}

static bool raw_trigger(enum opensc2_side side, uint16_t counts)
{
#if defined(CONFIG_APP_ANALOG_RAW)
	if (opensc2_cal_trigger_ok(side) || opensc2_cal_recording(OPENSC2_CAL_REC_TRIGGERS)) {
		return opensc2_set_trigger_raw(side, counts) == 0;
	}
	(void)opensc2_cal_raw_release((uint8_t)(OPENSC2_RAW_TRIGGER_L << side));
#else
	ARG_UNUSED(side);
	ARG_UNUSED(counts);
#endif
	return false;
}

void inputs_sample(struct opensc2_state *state)
{
	int32_t raw[AXES] = { 0 };
	bool ok[AXES];

	sensor_supply(true);
	burst_read(raw, ok);
	sensor_supply(false);

	for (int side = 0; side < 2; side++) {
		enum opensc2_side s = side ? OPENSC2_RIGHT : OPENSC2_LEFT;
		int x = side ? RIGHT_X : LEFT_X, y = x + 1;
		int t = side ? RIGHT_TRIGGER : LEFT_TRIGGER;

		if (ok[x] || ok[y]) {
			int32_t rx = ok[x] ? oriented(x, raw[x]) : cal[x].centre;
			int32_t ry = ok[y] ? oriented(y, raw[y]) : cal[y].centre;

			if (!raw_stick(s, counts12(x, rx), counts12(y, ry))) {
				/* Inside the callback the state is written in place, not with the
				 * setters (the library writes `state` back after the callback). */
				state->stick[side].x = ok[x] ? stick_value(x, rx) : 0;
				state->stick[side].y = ok[y] ? stick_value(y, ry) : 0;
			}
		}
		if (ok[t]) {
			int32_t rt = oriented(t, raw[t]);

			if (!raw_trigger(s, counts12(t, rt))) {
				state->trigger[side] = trigger_value(t, rt);
			}
		}
	}
}

int inputs_init(void)
{
#if DT_NODE_HAS_PROP(USER, vcc_enable_gpios)
	if (gpio_is_ready_dt(&vcc)) {
		gpio_pin_configure_dt(&vcc, GPIO_OUTPUT_ACTIVE);
	}
#endif
#if DT_NODE_HAS_PROP(USER, sensor_supply_gpios)
	if (!gpio_is_ready_dt(&supply) || gpio_pin_configure_dt(&supply, GPIO_OUTPUT_INACTIVE) != 0) {
		LOG_ERR("Sensor supply pin not usable");
	}
#endif
	/* The supply stays on while the resting values are taken (about 30 ms at start-up). */
	sensor_supply(true);
	setup_axes();
	sensor_supply(false);
	return 0;
}

#else /* !CONFIG_APP_ANALOG_SAMPLE */

static void analog_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	setup_axes();

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

#endif /* CONFIG_APP_ANALOG_SAMPLE */
