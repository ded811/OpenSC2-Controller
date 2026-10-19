/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * status.c: the status LED (the board's led0).
 *   Wireless, the way the real controller's LED shows the 2.4 GHz link: quick double flashes
 *   while pairing, a slow pulse while searching for the puck, steady when connected. A short blink
 *   every two seconds means "no bond: pair first".
 *   Wired: off for the first half second on the bus (and while a host that configured the board has
 *   let go of it, a bus reset for example); a fast blink (100 ms on, 100 ms off) while no host has
 *   configured the board (a charger, a power bank); on with a short dark blink every two seconds
 *   once a PC has configured it. These cannot be mistaken for the radio's patterns. Dark once the
 *   pairing button has been held 3 s: releasing it then reboots into wireless mode. In the charger
 *   test build (test-charger.conf, a testing aid) the fast blink lasts until the library gives up
 *   on a host (5 s without a request from one); the board then reboots into wireless mode and the
 *   radio's patterns follow.
 *
 * If zephyr,user has PWM channels named "red", "green" and "blue", an RGB LED shows the same
 * patterns in the real controller's colours (the 2.4 GHz white with a little blue; green on a PC,
 * amber without a host), and Steam's own colour (vendor command 0xC5) once Steam turns its LED
 * override on.
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include "app.h"

#define USER DT_PATH(zephyr_user)
#define TICK_MS 50

#define HAS_RGB (DT_PROP_HAS_NAME(USER, pwms, red) && DT_PROP_HAS_NAME(USER, pwms, green) && \
		 DT_PROP_HAS_NAME(USER, pwms, blue))

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, { 0 });

#if HAS_RGB
static const struct pwm_dt_spec rgb[3] = {
	PWM_DT_SPEC_GET_BY_NAME(USER, red),
	PWM_DT_SPEC_GET_BY_NAME(USER, green),
	PWM_DT_SPEC_GET_BY_NAME(USER, blue),
};
#endif

static enum opensc2_mode mode = OPENSC2_MODE_WIRELESS;
static enum opensc2_link_state link = OPENSC2_LINK_OFF;
static enum opensc2_usb_state usb = OPENSC2_USB_OFF;
static struct opensc2_led steam_led = { .brightness = 50 };
static uint32_t tick;
static volatile bool hold_cue;   /* the pairing button held 3 s while wired: dark */

static bool wired_pattern_on(void)
{
	switch (usb) {
	case OPENSC2_USB_NO_HOST:
		return tick % 4 < 2;                         /* 100 ms on, 100 ms off */
	case OPENSC2_USB_CONFIGURED:
		return tick % 40 >= 2;                       /* on, 100 ms dark every 2 s */
	default:
		return false;   /* the first 0.5 s, deconfigured, USB power gone, or leaving */
	}
}

static bool link_pattern_on(void)
{
	switch (link) {
	case OPENSC2_LINK_NO_BOND:
		return tick % 40 == 0;                       /* 50 ms every 2 s */
	case OPENSC2_LINK_PAIRING: {
		uint32_t phase = tick % 8;                   /* two short flashes every 400 ms */

		return phase == 0 || phase == 2;
	}
	case OPENSC2_LINK_LISTENING:
		return tick % 20 < 4;                        /* 200 ms every second */
	case OPENSC2_LINK_CONNECTED:
		return true;
	default:
		return false;
	}
}

#if HAS_RGB
static void set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
	const uint8_t pct[3] = { r, g, b };

	for (int i = 0; i < 3; i++) {
		if (pwm_is_ready_dt(&rgb[i])) {
			pwm_set_pulse_dt(&rgb[i], rgb[i].period * MIN(pct[i], 100) / 100);
		}
	}
}
#endif

static void tick_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(tick_work, tick_work_fn);

static void tick_work_fn(struct k_work *work)
{
	bool wired = mode == OPENSC2_MODE_WIRED;
	bool on = !hold_cue && (wired ? wired_pattern_on() : link_pattern_on());
	bool host = wired ? usb == OPENSC2_USB_CONFIGURED : link == OPENSC2_LINK_CONNECTED;
	bool steam_colour = host && steam_led.override && !hold_cue;

	ARG_UNUSED(work);
	tick++;
	if (led.port) {
		/* A one-colour LED shows Steam's override as on or off. */
		gpio_pin_set_dt(&led, steam_colour ? (steam_led.red | steam_led.green |
						      steam_led.blue | steam_led.white) != 0 : on);
	}
#if HAS_RGB
	{
		uint32_t level = steam_led.brightness;   /* controller setting 45, 0..100 */

		if (steam_colour) {
			/* Steam's colour; the white channel is mixed into all three. */
			set_rgb((MIN(steam_led.red + steam_led.white, 100)) * level / 100,
				(MIN(steam_led.green + steam_led.white, 100)) * level / 100,
				(MIN(steam_led.blue + steam_led.white, 100)) * level / 100);
		} else if (!on) {
			set_rgb(0, 0, 0);
		} else if (wired && usb == OPENSC2_USB_CONFIGURED) {
			set_rgb(0, 25 * level / 50, 0);                       /* green on a PC */
		} else if (wired) {
			set_rgb(20 * level / 50, 20 * level / 50, 0);         /* amber, no host */
		} else {
			set_rgb(80 * level / 100, 80 * level / 100, 100 * level / 100);
		}
	}
#endif
	k_work_schedule(&tick_work, K_MSEC(TICK_MS));
}

void status_mode(enum opensc2_mode m)
{
	mode = m;
	tick = 0;
}

void status_link(enum opensc2_link_state state)
{
	link = state;
	tick = 0;
}

void status_usb(enum opensc2_usb_state state)
{
	if (state != usb) {
		tick = 0;
	}
	usb = state;
	mode = OPENSC2_MODE_WIRED;   /* only a wired board reports USB states */
}

void status_led(const struct opensc2_led *l)
{
	steam_led = *l;
}

void status_hold_cue(bool on)
{
	hold_cue = on;
}

int status_init(void)
{
	if (led.port && gpio_is_ready_dt(&led)) {
		gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}
	k_work_schedule(&tick_work, K_MSEC(TICK_MS));
	return 0;
}
