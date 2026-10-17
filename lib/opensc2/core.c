/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * core.c: initialisation, the input state, identity, settings and the other public setters.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#if defined(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif
#include "internal.h"

LOG_MODULE_REGISTER(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

BUILD_ASSERT(CONFIG_ESB_MAX_PAYLOAD_LENGTH >= 128,
	     "OpenSC2 needs CONFIG_ESB_MAX_PAYLOAD_LENGTH=128: the puck sends packets of up to 128 bytes");
BUILD_ASSERT(sizeof(CONFIG_OPENSC2_UNIT_SERIAL) == OPENSC2_SERIAL_LEN + 1,
	     "CONFIG_OPENSC2_UNIT_SERIAL must be exactly 13 characters (0-9, A-Z)");
BUILD_ASSERT(sizeof(CONFIG_OPENSC2_BOARD_SERIAL) == OPENSC2_SERIAL_LEN + 1,
	     "CONFIG_OPENSC2_BOARD_SERIAL must be exactly 13 characters (0-9, A-Z)");
BUILD_ASSERT(sizeof(CONFIG_OPENSC2_GIT_REVISION) <= OPENSC2_GIT_REVISION_MAX + 1,
	     "CONFIG_OPENSC2_GIT_REVISION must be at most 12 characters");

#if CONFIG_OPENSC2_BUILD_TIME != 0
#define DEFAULT_BUILD_TIME ((uint32_t)CONFIG_OPENSC2_BUILD_TIME)
#else
#define DEFAULT_BUILD_TIME ((uint32_t)OPENSC2_CMAKE_BUILD_TIME)   /* when the build was configured */
#endif

struct osc2_ctx osc2;

/* ---- identity -------------------------------------------------------------------------------- */

static void copy_string(char *dst, size_t size, const char *src)
{
	size_t n = 0;

	while (n + 1 < size && src[n] != '\0') {
		dst[n] = src[n];
		n++;
	}
	memset(&dst[n], 0, size - n);
}

static void kconfig_identity(struct opensc2_identity *id)
{
	memset(id, 0, sizeof(*id));
	copy_string(id->unit_serial, sizeof(id->unit_serial), CONFIG_OPENSC2_UNIT_SERIAL);
	copy_string(id->board_serial, sizeof(id->board_serial), CONFIG_OPENSC2_BOARD_SERIAL);
	copy_string(id->git_revision, sizeof(id->git_revision), CONFIG_OPENSC2_GIT_REVISION);
	id->product_id = CONFIG_OPENSC2_PRODUCT_ID;
	id->board_revision = CONFIG_OPENSC2_BOARD_REVISION;
	id->build_time = DEFAULT_BUILD_TIME;
	id->bootloader_build_time = CONFIG_OPENSC2_BOOTLOADER_BUILD_TIME;

#if defined(CONFIG_OPENSC2_SERIAL_FROM_CHIP)
	/* Development serials made from the chip's unique ID, so two boards never share one. "OSC"
	 * and "OSB" mark them as this library's, not a real controller's. */
	uint8_t chip[8] = { 0 };

	if (hwinfo_get_device_id(chip, sizeof(chip)) >= 5) {
		snprintf(id->unit_serial, sizeof(id->unit_serial), "OSC%02X%02X%02X%02X%02X",
			 chip[0], chip[1], chip[2], chip[3], chip[4]);
		snprintf(id->board_serial, sizeof(id->board_serial), "OSB%02X%02X%02X%02X%02X",
			 chip[7], chip[6], chip[5], chip[4], chip[3]);
	}
#endif
}

/* Fields of `over` that are set replace those of `id`; returns which ones were set. */
static uint8_t overlay_identity(struct opensc2_identity *id, const struct opensc2_identity *over)
{
	uint8_t mask = 0;

	if (over->unit_serial[0]) {
		memcpy(id->unit_serial, over->unit_serial, sizeof(id->unit_serial));
		mask |= OSC2_ID_UNIT;
	}
	if (over->board_serial[0]) {
		memcpy(id->board_serial, over->board_serial, sizeof(id->board_serial));
		mask |= OSC2_ID_BOARD;
	}
	if (over->git_revision[0]) {
		memcpy(id->git_revision, over->git_revision, sizeof(id->git_revision));
		mask |= OSC2_ID_GIT;
	}
	if (over->product_id) {
		id->product_id = over->product_id;
		mask |= OSC2_ID_PID;
	}
	if (over->board_revision) {
		id->board_revision = over->board_revision;
		mask |= OSC2_ID_REV;
	}
	if (over->build_time) {
		id->build_time = over->build_time;
		mask |= OSC2_ID_BUILD;
	}
	if (over->bootloader_build_time) {
		id->bootloader_build_time = over->bootloader_build_time;
		mask |= OSC2_ID_BL_BUILD;
	}
	return mask;
}

static void apply_masked(struct opensc2_identity *id, const struct osc2_id_override *o)
{
	if (o->mask & OSC2_ID_UNIT) {
		memcpy(id->unit_serial, o->id.unit_serial, sizeof(id->unit_serial));
	}
	if (o->mask & OSC2_ID_BOARD) {
		memcpy(id->board_serial, o->id.board_serial, sizeof(id->board_serial));
	}
	if (o->mask & OSC2_ID_GIT) {
		memcpy(id->git_revision, o->id.git_revision, sizeof(id->git_revision));
	}
	if (o->mask & OSC2_ID_PID) {
		id->product_id = o->id.product_id;
	}
	if (o->mask & OSC2_ID_REV) {
		id->board_revision = o->id.board_revision;
	}
	if (o->mask & OSC2_ID_BUILD) {
		id->build_time = o->id.build_time;
	}
	if (o->mask & OSC2_ID_BL_BUILD) {
		id->bootloader_build_time = o->id.bootloader_build_time;
	}
}

void osc2_identity_refresh(void)
{
	struct opensc2_identity id = osc2.base;
	k_spinlock_key_t key;

	apply_masked(&id, &osc2.stored);
	if (opensc2_identity_check(&id) != 0) {
		LOG_ERR("The stored identity is not valid (0x%x): using the configured one",
			opensc2_identity_check(&id));
		id = osc2.base;
	}
	key = k_spin_lock(&osc2.lock);
	osc2.identity = id;
	k_spin_unlock(&osc2.lock, key);
}

void opensc2_identity_get(struct opensc2_identity *identity)
{
	k_spinlock_key_t key = k_spin_lock(&osc2.lock);

	*identity = osc2.identity;
	k_spin_unlock(&osc2.lock, key);
}

int opensc2_identity_store(const struct opensc2_identity *identity)
{
	struct osc2_id_override next = osc2.stored;
	struct opensc2_identity check = osc2.base;
	uint8_t mask;

	mask = overlay_identity(&next.id, identity);
	next.mask |= mask;
	apply_masked(&check, &next);
	if (opensc2_identity_check(&check) != 0) {
		return -EINVAL;
	}
	osc2.stored = next;
	osc2_identity_refresh();
	return osc2_storage_save_identity();
}

int opensc2_identity_clear(void)
{
	memset(&osc2.stored, 0, sizeof(osc2.stored));
	osc2_identity_refresh();
	return osc2_storage_save_identity();
}

/* ---- settings, LED, chime -------------------------------------------------------------------- */

void osc2_settings_restore_defaults(void)
{
	for (int i = 0; i < OPENSC2_SETTINGS_COUNT; i++) {
		osc2.settings[i] = opensc2_setting_default((uint8_t)i);
	}
	osc2.led.override = false;
	osc2.led.brightness = (uint8_t)osc2.settings[OPENSC2_SETTING_LED_BRIGHTNESS];
}

int16_t opensc2_setting_get(uint8_t number)
{
	return number < OPENSC2_SETTINGS_COUNT ? osc2.settings[number] : 0;
}

bool opensc2_lizard_mode(void)
{
	return osc2.settings[OPENSC2_SETTING_LIZARD_MODE] != 0;
}

void osc2_watchdog_kick(void)
{
	osc2.watchdog_kick_ms = k_uptime_get();
}

void opensc2_led_get(struct opensc2_led *led)
{
	k_spinlock_key_t key = k_spin_lock(&osc2.lock);

	*led = osc2.led;
	k_spin_unlock(&osc2.lock, key);
}

void osc2_notify_led(void)
{
	struct opensc2_led led;

	opensc2_led_get(&led);
	if (osc2.cb && osc2.cb->led) {
		osc2.cb->led(&led, osc2.user_data);
	}
}

uint8_t opensc2_chime_level(void)
{
	return osc2.chime;
}

/* ---- power requests: acted on a little later, from the system work queue --------------------- */

static enum opensc2_power_request pending_power;

static void power_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	LOG_INF("Power request %d", pending_power);
	if (osc2.cb && osc2.cb->power_request) {
		osc2.cb->power_request(pending_power, osc2.user_data);
	}
}
static K_WORK_DELAYABLE_DEFINE(power_work, power_work_fn);

void osc2_schedule_power(enum opensc2_power_request request, uint32_t delay_ms)
{
	pending_power = request;
	k_work_reschedule(&power_work, K_MSEC(delay_ms));
}

/* ---- input state ----------------------------------------------------------------------------- */

int opensc2_set_state(const struct opensc2_state *state)
{
	k_spinlock_key_t key = k_spin_lock(&osc2.lock);

	osc2.state = *state;
	k_spin_unlock(&osc2.lock, key);
	return 0;
}

void opensc2_get_state(struct opensc2_state *state)
{
	k_spinlock_key_t key = k_spin_lock(&osc2.lock);

	*state = osc2.state;
	k_spin_unlock(&osc2.lock, key);
}

int opensc2_set_buttons(uint32_t mask, uint32_t value)
{
	k_spinlock_key_t key = k_spin_lock(&osc2.lock);

	osc2.state.buttons = (osc2.state.buttons & ~mask) | (value & mask);
	k_spin_unlock(&osc2.lock, key);
	return 0;
}

int opensc2_set_stick(enum opensc2_side side, int16_t x, int16_t y)
{
	k_spinlock_key_t key;

	if (side > OPENSC2_RIGHT) {
		return -EINVAL;
	}
	key = k_spin_lock(&osc2.lock);
	osc2.state.stick[side].x = x;
	osc2.state.stick[side].y = y;
	k_spin_unlock(&osc2.lock, key);
	return 0;
}

int opensc2_set_trigger(enum opensc2_side side, uint16_t value)
{
	k_spinlock_key_t key;

	if (side > OPENSC2_RIGHT) {
		return -EINVAL;
	}
	key = k_spin_lock(&osc2.lock);
	osc2.state.trigger[side] = MIN(value, OPENSC2_TRIGGER_MAX);
	k_spin_unlock(&osc2.lock, key);
	return 0;
}

int opensc2_set_trackpad(enum opensc2_side side, const struct opensc2_trackpad *pad)
{
	k_spinlock_key_t key;

	if (side > OPENSC2_RIGHT) {
		return -EINVAL;
	}
	key = k_spin_lock(&osc2.lock);
	osc2.state.pad[side] = *pad;
	k_spin_unlock(&osc2.lock, key);
	return 0;
}

int opensc2_set_imu(const struct opensc2_imu *imu)
{
	k_spinlock_key_t key = k_spin_lock(&osc2.lock);

	osc2.state.imu = *imu;
	k_spin_unlock(&osc2.lock, key);
	return 0;
}

int opensc2_set_battery(const struct opensc2_battery *battery)
{
	k_spinlock_key_t key = k_spin_lock(&osc2.lock);

	osc2.battery = *battery;
	k_spin_unlock(&osc2.lock, key);
	return 0;
}

int opensc2_send_report(const uint8_t *report, size_t len)
{
	if (len < 1 || len > 64) {
		return -EINVAL;
	}
	if (!osc2_radio_connected()) {
		return -ENOTCONN;
	}
	return osc2_radio_queue_report(report, len);
}

int opensc2_send_haptic_status(uint8_t channel_id, uint32_t flags)
{
	uint8_t report[OPENSC2_HAPTIC_STATUS_LEN];

	opensc2_encode_haptic_status(channel_id, flags, report);
	return opensc2_send_report(report, sizeof(report));
}

/* ---- bonds ----------------------------------------------------------------------------------- */

int opensc2_bond_get(uint8_t slot, struct opensc2_bond *bond)
{
	k_spinlock_key_t key;

	if (slot >= OPENSC2_BOND_SLOTS) {
		return -EINVAL;
	}
	key = k_spin_lock(&osc2.lock);
	*bond = osc2.bonds[slot];
	k_spin_unlock(&osc2.lock, key);
	return 0;
}

int opensc2_bond_set(uint8_t slot, const struct opensc2_bond *bond)
{
	k_spinlock_key_t key;
	int err;

	if (slot >= OPENSC2_BOND_SLOTS) {
		return -EINVAL;
	}
	key = k_spin_lock(&osc2.lock);
	if (opensc2_bond_valid(bond)) {
		osc2.bonds[slot] = *bond;
	} else {
		memset(&osc2.bonds[slot], 0, sizeof(osc2.bonds[slot]));
	}
	k_spin_unlock(&osc2.lock, key);
	err = osc2_storage_save_bond(slot);
	osc2_radio_post(OSC2_EV_BOND);
	return err;
}

int opensc2_select_slot(uint8_t slot)
{
	int err;

	if (slot >= OPENSC2_BOND_SLOTS) {
		return -EINVAL;
	}
	osc2.slot = slot;
	err = osc2_storage_save_slot();
	osc2_radio_post(OSC2_EV_BOND);
	return err;
}

uint8_t opensc2_selected_slot(void)
{
	return osc2.slot;
}

/* ---- control --------------------------------------------------------------------------------- */

int opensc2_init(const struct opensc2_config *config)
{
	int err;

	if (osc2.initialised) {
		return -EALREADY;
	}
	if (!config) {
		return -EINVAL;
	}

	kconfig_identity(&osc2.base);
	if (config->identity) {
		overlay_identity(&osc2.base, config->identity);
	}
	if (opensc2_identity_check(&osc2.base) != 0) {
		LOG_ERR("Invalid identity (problems 0x%x): serials must be 13 characters 0-9/A-Z, "
			"the git revision 1 to 12 characters", opensc2_identity_check(&osc2.base));
		return -EINVAL;
	}
	if (osc2.base.product_id != OPENSC2_PRODUCT_ID) {
		LOG_WRN("Product ID 0x%04x: Steam knows the Steam Controller 2 as 0x%04x",
			osc2.base.product_id, OPENSC2_PRODUCT_ID);
	}

	osc2.cb = config->callbacks;
	osc2.user_data = config->user_data;
	osc2.chime = 2;
	osc2.battery = (struct opensc2_battery){
		.charge_state = OPENSC2_CHARGE_DONE,
		.level = 100,
		.battery_mv = 4100,
		.system_mv = 3300,
		.input_mv = 5000,
	};
	osc2_settings_restore_defaults();
	osc2_watchdog_kick();

	err = osc2_storage_load();
	if (err) {
		LOG_WRN("Could not load stored bonds and identity (%d)", err);
	}
	if (config->bond_slot < OPENSC2_BOND_SLOTS) {
		osc2.slot = config->bond_slot;
	}
	osc2_identity_refresh();
	osc2_commands_reset();

	LOG_INF("Controller %s (board %s, revision 0x%x, build 0x%08x), bond slot %u%s",
		osc2.identity.unit_serial, osc2.identity.board_serial, osc2.identity.board_revision,
		osc2.identity.build_time, osc2.slot + 1,
		opensc2_bond_valid(&osc2.bonds[osc2.slot]) ? "" : " (empty: pair first)");

	osc2.initialised = true;
	return osc2_radio_init(config->start);
}

int opensc2_start(void)
{
	if (!osc2.initialised) {
		return -EACCES;
	}
	osc2_radio_post(OSC2_EV_START);
	return 0;
}

int opensc2_stop(void)
{
	if (!osc2.initialised) {
		return -EACCES;
	}
	osc2_radio_post(OSC2_EV_STOP);
	return 0;
}

int opensc2_pair_start(uint16_t seconds)
{
	if (!osc2.initialised) {
		return -EACCES;
	}
	osc2_radio_set_pair_seconds(seconds ? seconds : CONFIG_OPENSC2_PAIRING_TIMEOUT_S);
	osc2_radio_post(OSC2_EV_PAIR);
	return 0;
}

int opensc2_pair_cancel(void)
{
	if (!osc2.initialised) {
		return -EACCES;
	}
	osc2_radio_post(OSC2_EV_CANCEL);
	return 0;
}

enum opensc2_link_state opensc2_link_state(void)
{
	return osc2_radio_state();
}

int opensc2_link_info(struct opensc2_link_info *info)
{
	if (!osc2.initialised) {
		return -EACCES;
	}
	osc2_radio_info(info);
	return 0;
}
