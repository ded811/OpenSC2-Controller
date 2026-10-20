/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * transport.c: which wireless transport this boot runs: the puck's 2.4 GHz radio (radio.c), or,
 * in a build with CONFIG_OPENSC2_BLE, Bluetooth LE (ble.c). Only one runs at a time, as on the
 * real controller.
 *
 * The choice is the real controller's user/wireless_transport: 0 and 1 the two Bluetooth slots,
 * 2 and 3 the puck with bond 1 or 2 (transport_rules.h). A boot starts the stored one. A choice
 * made at run time (opensc2_select_transport(), the shell, the slot) runs at once and is saved
 * only once its link is up, as the October 2026 firmware does (Bluetooth when it passes its gate,
 * the puck when the radio connects); a host's 0xEE and 0xEF of the key set and save it directly
 * (commands.c).
 */
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "internal.h"

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

#define HAVE_BLE IS_ENABLED(CONFIG_OPENSC2_BLE)

/* The transport this boot runs (whether or not its link is up). */
static uint8_t running = OSC2_TRANSPORT_DEFAULT;

void osc2_transport_init(void)
{
	/* The stored choice, else the puck slot an older firmware stored (osc2.slot), else the
	 * default. A puck transport uses the slot in osc2.slot, which the application may have
	 * chosen for this boot (opensc2_config.bond_slot). */
	uint8_t t = osc2_transport_at_boot(osc2.transport_saved, osc2.slot, HAVE_BLE);

	if (osc2_transport_is_puck(t)) {
		t = (uint8_t)(OSC2_TRANSPORT_PUCK_1 + osc2.slot);
	}
	osc2.transport = t;
	running = t;
}

uint8_t osc2_transport_running(void)
{
	/* The puck's slot is always the radio's (osc2.slot), which a host's 0xEE of
	 * user/wireless_transport, its 0xF0 or a factory reset change at once while the puck runs. */
	return osc2_transport_is_puck(running) ? (uint8_t)(OSC2_TRANSPORT_PUCK_1 + osc2.slot) :
						 running;
}

enum osc2_link osc2_mode_link(void)
{
	if (osc2_wired()) {
		return OSC2_LINK_USB;
	}
	return osc2_transport_is_ble(running) ? OSC2_LINK_BLE : OSC2_LINK_RADIO;
}

bool osc2_wireless_up(void)
{
	return !osc2_wired() && (osc2_radio_connected() || osc2_ble_active());
}

int osc2_wireless_start(void)
{
#if defined(CONFIG_OPENSC2_BLE)
	if (osc2_transport_is_ble(running)) {
		return osc2_ble_start(running, false);
	}
#endif
	osc2_radio_post(OSC2_EV_START);
	return 0;
}

int osc2_wireless_stop(void)
{
#if defined(CONFIG_OPENSC2_BLE)
	if (osc2_transport_is_ble(running)) {
		return osc2_ble_stop(false, false);
	}
#endif
	osc2_radio_post(OSC2_EV_STOP);
	return 0;
}

#if !defined(CONFIG_OPENSC2_BLE)
bool osc2_ble_active(void)
{
	return false;
}

uint32_t osc2_ble_interval_us(void)
{
	return 0;
}
#endif

/* ---- October's rule: saved once the link is up ------------------------------------------------ */

static atomic_t link_up_value = ATOMIC_INIT(OSC2_TRANSPORT_NONE);

static void save_fn(struct k_work *work)
{
	uint8_t t = (uint8_t)atomic_get(&link_up_value);
	int err;

	ARG_UNUSED(work);
	if (!osc2_transport_save_due(t, osc2.transport_saved)) {
		return;
	}
	err = osc2_storage_save_transport(t);
	if (err) {
		LOG_WRN("Could not store the transport %s (%d)", osc2_transport_name(t), err);
	} else {
		LOG_INF("Transport %s stored (its link is up)", osc2_transport_name(t));
	}
}
static K_WORK_DEFINE(save_work, save_fn);

void osc2_transport_link_up(uint8_t t)
{
	if (!osc2_transport_save_due(t, osc2.transport_saved)) {
		return;   /* the usual case: nothing to write */
	}
	atomic_set(&link_up_value, t);
	k_work_submit(&save_work);
}

/* ---- the run-time switch -------------------------------------------------------------------- */

/* Run `t` from now on. Within one kind of transport only the slot changes: the radio listens with
 * the other bond (as a new slot always did), Bluetooth advertises for the other slot. Between the
 * two, the running one stops and hands the radio to the other (ble.c does the order: Bluetooth
 * waits until ESB is off; ESB starts once the Bluetooth stack is disabled). */
static int switch_to(uint8_t t, bool pair)
{
	uint8_t was = running;

	running = t;
	if (osc2_transport_is_puck(t) && osc2_transport_is_puck(was)) {
		osc2_radio_post(OSC2_EV_BOND);   /* as opensc2_select_slot() always did */
		return 0;
	}
#if defined(CONFIG_OPENSC2_BLE)
	if (osc2_transport_is_ble(t)) {
		if (osc2_transport_is_puck(was)) {
			osc2_radio_post(OSC2_EV_STOP);   /* Bluetooth waits until the radio is idle */
		}
		return osc2_ble_start(t, pair);
	}
	return osc2_ble_stop(true, true);   /* the stack disabled, then the radio starts */
#else
	ARG_UNUSED(pair);
	return 0;
#endif
}

/* `pair`: the pairing flag of a transport chord held 4 s, which only Bluetooth acts on. */
static int select_transport(uint8_t t, bool pair)
{
	if (!osc2.initialised) {
		return -EACCES;
	}
	if (!osc2_transport_usable(t, HAVE_BLE)) {
		return osc2_transport_is_ble(t) ? -ENOTSUP : -EINVAL;
	}
	osc2.transport = t;
	if (osc2_transport_is_puck(t)) {
		osc2.slot = (uint8_t)(t - OSC2_TRANSPORT_PUCK_1);
	}
	if (osc2_wired()) {
		/* No wireless link this boot to wait for: stored now, for the next wireless boot. */
		running = t;
		return osc2_storage_save_transport(t);
	}
	LOG_INF("Transport %s%s", osc2_transport_name(t),
		pair && osc2_transport_is_ble(t) ? ", pairing" : "");
	return switch_to(t, pair);
}

int opensc2_select_transport(enum opensc2_transport transport)
{
	return select_transport((uint8_t)transport, false);
}

int opensc2_select_transport_pairing(enum opensc2_transport transport)
{
	return select_transport((uint8_t)transport, true);
}

enum opensc2_transport opensc2_selected_transport(void)
{
	return (enum opensc2_transport)osc2_transport_running();
}
