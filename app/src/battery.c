/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * battery.c: an optional charger status pin. A charger module's status output (TP4056-style
 * "CHRG", active while it charges) on a pin named in zephyr,user:
 *
 *     charger-status-gpios = <&gpio0 x (GPIO_PULL_UP | GPIO_ACTIVE_LOW)>;
 *
 * gives report 0x43 its charge state once a second: without USB power 1 (on battery), with it 2
 * while the pin is active (charging), else 4 (done, level 100). The level itself stays the
 * library's default (100 %): this board measures no voltage. Untested on hardware: no charger with
 * a status pin has been wired yet. Without the property this file does nothing.
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_power.h>
#include "app.h"

LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

#define USER DT_PATH(zephyr_user)

#if DT_NODE_HAS_PROP(USER, charger_status_gpios)
#define POLL_MS 1000

static const struct gpio_dt_spec chg = GPIO_DT_SPEC_GET(USER, charger_status_gpios);
static struct opensc2_battery battery = {
	.charge_state = OPENSC2_CHARGE_DONE,
	.level = 100,
	.battery_mv = 4100,
	.system_mv = 3300,
	.input_mv = 5000,
};

static bool usb_power(void)
{
#if defined(NRF_POWER) && defined(NRF_POWER_HAS_USBREG) && NRF_POWER_HAS_USBREG
	return nrf_power_usbregstatus_vbusdet_get(NRF_POWER);
#else
	return false;
#endif
}

static void charger_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(charger_work, charger_fn);

static void charger_fn(struct k_work *work)
{
	bool power = usb_power();
	uint8_t state = opensc2_charge_state_of(power, gpio_pin_get_dt(&chg) == 1);

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
	k_work_schedule(&charger_work, K_MSEC(POLL_MS));
}

int battery_init(void)
{
	if (!gpio_is_ready_dt(&chg) || gpio_pin_configure_dt(&chg, GPIO_INPUT) != 0) {
		LOG_ERR("Charger status pin not usable");
		return -ENODEV;
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
#endif
