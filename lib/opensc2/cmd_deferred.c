/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * cmd_deferred.c: what the vendor commands leave for after their answer, with the real
 * controller's timing (the protocol documentation):
 *   - the host's power requests (0x90, 0x95, 0x9F) run 20 ms later, so the answer still goes out.
 *     One request waits at a time: a later one in those 20 ms takes its place but not its time
 *     (the first request's 20 ms stand). A turn-off is dropped then if the board is wired, which
 *     ignores it.
 *   - the factory reset (0x86): 10 ms later the stored pairings, the transport choice, the chime
 *     level, the kept keys of the "user", "mte" and "debug" groups and the Bluetooth keys are
 *     deleted from flash (the values in RAM stay as they are until the reboot), then the board
 *     reboots 100 ms later. Calibration, the identity and the mode policy are kept.
 *   - a bond written with 0xA2 goes to flash after the answer, for the next boot: the bond in use
 *     does not change. A bond with either ID 0 deletes the stored one instead.
 * The host tests replace this file (tests/host/stubs.c).
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "internal.h"
#include "cmd_deferred.h"
#include "key_store.h"

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

#define HOST_POWER_DELAY_MS    20
#define FACTORY_START_MS       10
#define FACTORY_REBOOT_MS      100

/* ---- power requests ------------------------------------------------------------------------- */

static struct k_spinlock power_lock;
static enum opensc2_power_request power_request;
static const char *power_why;

static void power_fn(struct k_work *work)
{
	enum opensc2_power_request request;
	const char *why;
	k_spinlock_key_t key = k_spin_lock(&power_lock);

	ARG_UNUSED(work);
	request = power_request;
	why = power_why;
	k_spin_unlock(&power_lock, key);
	if (request == OPENSC2_POWER_TURN_OFF && osc2_wired()) {
		LOG_INF("Turn-off request ignored: wired");
		return;
	}
	osc2_power_now(request, why);
}
static K_WORK_DELAYABLE_DEFINE(power_work, power_fn);

void osc2_cmd_power(enum opensc2_power_request request, const char *why)
{
	k_spinlock_key_t key = k_spin_lock(&power_lock);

	power_request = request;
	power_why = why;
	/* Scheduled only if not yet waiting: the first request's time stands. */
	(void)k_work_schedule(&power_work, K_MSEC(HOST_POWER_DELAY_MS));
	k_spin_unlock(&power_lock, key);
}

/* ---- the factory reset ---------------------------------------------------------------------- */

static void factory_fn(struct k_work *work)
{
	int err = 0;

	ARG_UNUSED(work);
	err |= osc2_storage_factory_reset();   /* bonds, transport, chime; Bluetooth's keys */
	err |= osc2_keys_factory_reset();      /* keylock, LED level, charge window, crash record */
	if (err) {
		LOG_ERR("Factory reset: a key could not be deleted");
	}
	LOG_INF("Factory reset done: rebooting");
	osc2_schedule_power(OPENSC2_POWER_FACTORY_RESET, FACTORY_REBOOT_MS, "host: factory reset");
}
static K_WORK_DELAYABLE_DEFINE(factory_work, factory_fn);

void osc2_cmd_factory_reset(void)
{
	LOG_INF("Factory reset");
	(void)k_work_schedule(&factory_work, K_MSEC(FACTORY_START_MS));
}

/* ---- 0xA2: the bond for the next boot ------------------------------------------------------- */

static struct opensc2_bond bond_next;
static struct k_spinlock bond_lock;

static void bond_fn(struct k_work *work)
{
	struct opensc2_bond b;
	k_spinlock_key_t key = k_spin_lock(&bond_lock);

	ARG_UNUSED(work);
	b = bond_next;
	k_spin_unlock(&bond_lock, key);
	if (osc2_storage_store_bond(0, &b) != 0) {
		LOG_ERR("Could not store the bond from 0xA2");
	}
}
static K_WORK_DEFINE(bond_work, bond_fn);

void osc2_cmd_bond_write(const struct opensc2_bond *bond)
{
	k_spinlock_key_t key = k_spin_lock(&bond_lock);

	bond_next = *bond;
	k_spin_unlock(&bond_lock, key);
	(void)k_work_submit(&bond_work);
}
