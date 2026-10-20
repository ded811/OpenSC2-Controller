/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * status.c: the status LED, as the real controller shows it. The library's system state machine
 * selects a mode (opensc2_led_status()); the library's pattern player (opensc2/led.h) turns it
 * into a colour every 10 ms with the real controller's patterns, fades and the fully-charged
 * display: blue for Bluetooth, white with a little blue for the puck (the searching pulse, steady
 * when connected, breathing on USB power), green while wired, amber on USB power without a host or
 * with wireless off, red and white flashes for the low-battery and keylock shutdowns. Steam's own
 * colour (vendor command 0xC5) shows while Steam's override is on.
 *
 *   - The board's led0 (one colour) shows the rhythm: on while the pattern is at least half as
 *     bright as its peak. Amber, which only a colour could tell from green, adds a 100 ms dark
 *     blink every second on a one-colour LED (OpenSC2's own, so "no host" and "wired" differ).
 *   - PWM channels named "red", "green", "blue" (and optionally "white") in zephyr,user drive an
 *     RGB(W) LED, each channel scaled by its trim (zephyr,user `led-trims`, percent, R G B W; the
 *     real controller's factory values 90 35 120 80 by default) and by the brightness Steam sets
 *     (setting 45, through the curve of setting 38). Without a white channel, white is mixed into
 *     red, green and blue.
 *   - A PWM channel named "ir" is the IR LED: controller setting 36 in percent, off during every
 *     shutdown and reboot (its setting stays).
 *   - Dark once the pairing button has been held 3 s while wired: releasing it then reboots into
 *     wireless mode.
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include "app.h"

#define USER DT_PATH(zephyr_user)

#define HAS_RGB (DT_PROP_HAS_NAME(USER, pwms, red) && DT_PROP_HAS_NAME(USER, pwms, green) && \
		 DT_PROP_HAS_NAME(USER, pwms, blue))
#define HAS_WHITE DT_PROP_HAS_NAME(USER, pwms, white)
#define HAS_IR DT_PROP_HAS_NAME(USER, pwms, ir)

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, { 0 });

#if HAS_RGB
static const struct pwm_dt_spec rgb[3] = {
	PWM_DT_SPEC_GET_BY_NAME(USER, red),
	PWM_DT_SPEC_GET_BY_NAME(USER, green),
	PWM_DT_SPEC_GET_BY_NAME(USER, blue),
};
#endif
#if HAS_WHITE
static const struct pwm_dt_spec white = PWM_DT_SPEC_GET_BY_NAME(USER, white);
#endif
#if HAS_IR
static const struct pwm_dt_spec ir = PWM_DT_SPEC_GET_BY_NAME(USER, ir);
#endif

#if !(HAS_RGB || HAS_WHITE)
/* No colour LED: no trims. */
#elif DT_NODE_HAS_PROP(USER, led_trims)
static const uint8_t trims[4] = {
	DT_PROP_BY_IDX(USER, led_trims, 0), DT_PROP_BY_IDX(USER, led_trims, 1),
	DT_PROP_BY_IDX(USER, led_trims, 2), DT_PROP_BY_IDX(USER, led_trims, 3),
};
#else
static const uint8_t trims[4] = { OPENSC2_LED_TRIM_RED, OPENSC2_LED_TRIM_GREEN,
				  OPENSC2_LED_TRIM_BLUE, OPENSC2_LED_TRIM_WHITE };
#endif

static struct opensc2_led_player player;
static volatile bool hold_cue;   /* the pairing button held 3 s while wired: dark */
static volatile bool off;        /* powering off */
static volatile uint8_t ir_percent;
#if !defined(CONFIG_OPENSC2_SYSTEM)
static enum opensc2_mode mode = OPENSC2_MODE_WIRELESS;
static enum opensc2_usb_state usb = OPENSC2_USB_OFF;
#endif

#if HAS_RGB || HAS_WHITE || HAS_IR
static void set_duty(const struct pwm_dt_spec *ch, uint16_t duty)
{
	if (pwm_is_ready_dt(ch)) {
		(void)pwm_set_pulse_dt(ch, (uint32_t)((uint64_t)ch->period * duty /
						      OPENSC2_LED_FULL_SCALE));
	}
}
#endif

/* Without the system state machine (CONFIG_OPENSC2_SYSTEM off) the library selects no mode: a
 * plain one from the mode and the links. */
static void get_status(struct opensc2_led_status *st)
{
	opensc2_led_status(st);
#if !defined(CONFIG_OPENSC2_SYSTEM)
	if (mode == OPENSC2_MODE_WIRED) {
		st->mode = usb == OPENSC2_USB_CONFIGURED ? OPENSC2_LED_MODE_WIRED :
			   OPENSC2_LED_MODE_AMBER;
	} else {
		st->mode = OPENSC2_LED_MODE_PUCK;
		st->connected = opensc2_link_state() == OPENSC2_LINK_CONNECTED;
		st->pairing = opensc2_link_state() == OPENSC2_LINK_PAIRING;
		st->external_power = true;
	}
#endif
}

static void tick_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(tick_work, tick_work_fn);

static void tick_work_fn(struct k_work *work)
{
	struct opensc2_led_status st;
	uint32_t now = k_uptime_get_32();
	uint16_t c[4] = { 0 };
	bool dark;

	ARG_UNUSED(work);
	get_status(&st);
	opensc2_led_player_step(&player, &st, now, c);
	dark = hold_cue || off;
	if (led.port) {
		bool on = opensc2_led_player_envelope(&player) >= 500;

		if (st.mode == OPENSC2_LED_MODE_AMBER && !st.override && now % 1000 < 100) {
			on = false;   /* a one-colour LED cannot show amber: a dark blink instead */
		}
		gpio_pin_set_dt(&led, on && !dark);
	}
#if HAS_RGB
	{
		uint16_t w = HAS_WHITE ? 0 : opensc2_led_duty(c[3], trims[3], st.brightness);

		for (int i = 0; i < 3; i++) {
			uint32_t d = opensc2_led_duty(c[i], trims[i], st.brightness) + w;

			set_duty(&rgb[i], dark ? 0 : (uint16_t)MIN(d, OPENSC2_LED_FULL_SCALE));
		}
	}
#endif
#if HAS_WHITE
	set_duty(&white, dark ? 0 : opensc2_led_duty(c[3], trims[3], st.brightness));
#endif
#if HAS_IR
	set_duty(&ir, st.shutting_down || off ? 0 : (uint16_t)(ir_percent * 10u));
#endif
	if (!off) {
		k_work_schedule(&tick_work, K_MSEC(OPENSC2_LED_STEP_MS));
	}
}

void status_mode(enum opensc2_mode m)
{
#if !defined(CONFIG_OPENSC2_SYSTEM)
	mode = m;
#else
	ARG_UNUSED(m);
#endif
}

void status_link(enum opensc2_link_state state)
{
	ARG_UNUSED(state);   /* the library's status has it */
}

void status_usb(enum opensc2_usb_state state)
{
#if !defined(CONFIG_OPENSC2_SYSTEM)
	usb = state;
	mode = OPENSC2_MODE_WIRED;   /* only a wired board reports USB states */
#else
	ARG_UNUSED(state);
#endif
}

void status_led(const struct opensc2_led *l)
{
	ARG_UNUSED(l);   /* the library's status has Steam's colour and brightness */
}

void status_setting(uint8_t number, int16_t value)
{
	if (number == OPENSC2_SETTING_IR_LED_BRIGHTNESS) {
		ir_percent = (uint8_t)CLAMP(value, 0, 100);
	} else if (number == OPENSC2_SETTINGS_ALL) {
		ir_percent = (uint8_t)CLAMP(opensc2_setting_get(OPENSC2_SETTING_IR_LED_BRIGHTNESS), 0,
					    100);
	}
}

void status_hold_cue(bool on)
{
	hold_cue = on;
}

void status_off(void)
{
	off = true;
	/* Called in the system work queue, which also runs the tick: waiting for the tick there is
	 * undefined in Zephyr, and not needed. A tick that still runs sees `off`, shows dark and does
	 * not come back. */
	(void)k_work_cancel_delayable(&tick_work);
	if (led.port) {
		gpio_pin_set_dt(&led, 0);
	}
#if HAS_RGB
	for (int i = 0; i < 3; i++) {
		set_duty(&rgb[i], 0);
	}
#endif
#if HAS_WHITE
	set_duty(&white, 0);
#endif
#if HAS_IR
	set_duty(&ir, 0);
#endif
}

int status_init(void)
{
	if (led.port && gpio_is_ready_dt(&led)) {
		gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}
	opensc2_led_player_init(&player);
	k_work_schedule(&tick_work, K_MSEC(OPENSC2_LED_STEP_MS));
	return 0;
}
