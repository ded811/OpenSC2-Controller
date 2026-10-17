/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * status.c: the status LED (the board's led0) shows the link the way the real controller's LED
 * does for the 2.4 GHz link: quick double flashes while pairing, a slow pulse while searching for
 * the puck, steady when connected. A short blink every two seconds means "no bond: pair first".
 *
 * If zephyr,user has PWM channels named "red", "green" and "blue", an RGB LED shows the same
 * pattern in the controller's 2.4 GHz colour (white with a little blue), and Steam's own colour
 * (vendor command 0xC5) once Steam turns its LED override on.
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

static enum opensc2_link_state link = OPENSC2_LINK_OFF;
static struct opensc2_led steam_led = { .brightness = 50 };
static uint32_t tick;

static bool pattern_on(void)
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
	bool on = pattern_on();
	bool steam_colour = link == OPENSC2_LINK_CONNECTED && steam_led.override;

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
		} else if (on) {
			set_rgb(80 * level / 100, 80 * level / 100, 100 * level / 100);
		} else {
			set_rgb(0, 0, 0);
		}
	}
#endif
	k_work_schedule(&tick_work, K_MSEC(TICK_MS));
}

void status_link(enum opensc2_link_state state)
{
	link = state;
	tick = 0;
}

void status_led(const struct opensc2_led *l)
{
	steam_led = *l;
}

int status_init(void)
{
	if (led.port && gpio_is_ready_dt(&led)) {
		gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}
	k_work_schedule(&tick_work, K_MSEC(TICK_MS));
	return 0;
}
