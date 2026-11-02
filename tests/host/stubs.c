/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: stand-ins for the parts of the library around the command channel (core.c,
 * storage.c, key_store.c, radio.c, report.c), so commands.c runs on a PC. Each records what it
 * was asked to do; the tests in commands_cases.h look at the record.
 */
#include <errno.h>
#include <string.h>
#include "internal.h"
#include "cal_store.h"
#include "cal_host.h"
#include "cmd_deferred.h"
#include "dock_link.h"
#include "imu_rules.h"
#include "dock_stub.h"
#include "haptics_glue.h"
#include "stubs.h"
#include "test_hooks.h"

struct osc2_ctx osc2;
struct stub_record stub;
int osc2_host_mutexes_held;   /* shim/zephyr/kernel.h's mutexes taken and not let go */

int64_t k_uptime_get(void)
{
	return stub.now_ms;
}

void opensc2_identity_get(struct opensc2_identity *identity)
{
	*identity = osc2.identity;
}

K_MUTEX_DEFINE(osc2_id_lock);   /* core.c's */

/* As core.c's: the configured identity, the stored fields over it. */
void osc2_identity_refresh(void)
{
	struct opensc2_identity id = osc2.base;

	osc2_id_apply(&id, &osc2.stored);
	osc2.identity = id;
}

int opensc2_identity_store(const struct opensc2_identity *identity)
{
	(void)identity;
	return 0;
}

void opensc2_led_get(struct opensc2_led *led)
{
	*led = osc2.led;
}

void osc2_notify_led(void)
{
	osc2_test_note_led(&osc2.led);   /* as core.c does */
	stub.led_notified++;
}

void osc2_radio_post(uint32_t events)
{
	stub.radio_events |= events;
}

int osc2_storage_save_bond(uint8_t slot)
{
	stub.saved_bonds |= 1u << slot;
	stub.stored_bonds[slot] = opensc2_bond_valid(&osc2.bonds[slot]) ? osc2.bonds[slot] :
									(struct opensc2_bond){ 0 };
	return stub.storage_error;
}

/* 0xA2's write (flash only; the bond in RAM stays) */
int osc2_storage_store_bond(uint8_t slot, const struct opensc2_bond *bond)
{
	stub.stored_bond_slot = slot;
	stub.stored_bond = *bond;
	stub.stored_bond_writes++;
	stub.stored_bonds[slot] = opensc2_bond_valid(bond) ? *bond : (struct opensc2_bond){ 0 };
	return stub.storage_error;
}

int osc2_storage_delete_bond(uint8_t slot)
{
	stub.deleted_bonds |= 1u << slot;
	if (stub.bond_delete_error == 0) {
		stub.stored_bonds[slot] = (struct opensc2_bond){ 0 };
	}
	return stub.bond_delete_error;
}

int osc2_storage_save_transport(uint8_t t)
{
	/* as storage.c: stored, and read back into the key */
	stub.saved_transport++;
	stub.stored_transport = t;
	osc2.transport_saved = t;
	osc2.transport_stored = true;
	osc2.transport = t;
	return 0;
}

int osc2_storage_delete_transport(void)
{
	stub.deleted_transport++;
	stub.stored_transport = OSC2_TRANSPORT_NONE;
	osc2.transport_saved = OSC2_TRANSPORT_NONE;
	osc2.transport_stored = false;
	return 0;
}

/* wireless.c's: USB while wired, else the running wireless transport's link. */
enum osc2_link osc2_mode_link(void)
{
	if (osc2_wired()) {
		return OSC2_LINK_USB;
	}
	return stub.ble_running ? OSC2_LINK_BLE : OSC2_LINK_RADIO;
}

/* ble.c's (wireless.c's without Bluetooth): the interval while Bluetooth is active, else 0. */
uint32_t osc2_ble_interval_us(void)
{
	return stub.link_active[OSC2_LINK_BLE] ? stub.ble_interval_us : 0;
}

bool osc2_ble_active(void)
{
	return stub.link_active[OSC2_LINK_BLE];
}

int osc2_storage_save_chime(void)
{
	return osc2_storage_store_chime(osc2.chime);
}

int osc2_storage_store_chime(uint8_t level)
{
	if (stub.storage_error) {
		return stub.storage_error;
	}
	stub.saved_chime++;
	stub.stored_chime = level;
	return 0;
}

int osc2_storage_factory_reset(void)
{
	stub.storage_factory_resets++;
	return 0;
}

/* Another Zephyr settings handler, as storage.c reaches it: one stand-in handler, "app/thing" (a
 * 2-byte value with a read function), and "app/wo" (set only); any other name has none. */
static bool own_subtree(const char *name)
{
	return strncmp(name, "opensc2", 7) == 0 && (name[7] == '\0' || name[7] == '/');
}

int osc2_storage_named_set(const char *name, const void *value, size_t len)
{
	if (own_subtree(name)) {
		return -EINVAL;
	}
	if (strcmp(name, "app/thing") == 0) {
		if (len == 0) {
			memset(stub.named_value, 0, sizeof(stub.named_value));
			return 0;
		}
		if (len != sizeof(stub.named_value)) {
			return -EINVAL;
		}
		memcpy(stub.named_value, value, len);
		return 0;
	}
	if (strcmp(name, "app/wo") == 0) {
		return len == 0 || len == 1 ? 0 : -EINVAL;
	}
	return -EINVAL;
}

int osc2_storage_named_get(const char *name, void *out, size_t size)
{
	if (own_subtree(name)) {
		return -EINVAL;
	}
	if (strcmp(name, "app/thing") == 0) {
		if (size < sizeof(stub.named_value)) {
			return -ENODEV;
		}
		memcpy(out, stub.named_value, sizeof(stub.named_value));
		return (int)sizeof(stub.named_value);
	}
	if (strcmp(name, "app/wo") == 0) {
		return -ENOTSUP;
	}
	return -EINVAL;
}

int osc2_storage_named_save(const char *name, const void *value, size_t len)
{
	ARG_UNUSED(value);
	if (own_subtree(name)) {
		return -EINVAL;
	}
	stub.named_saves++;
	stub.named_saved_len = (int)len;
	return 0;
}

int osc2_storage_named_delete(const char *name)
{
	if (own_subtree(name)) {
		return 0;
	}
	stub.named_deletes++;
	(void)osc2_storage_named_set(name, NULL, 0);
	return 0;
}

/* cmd_deferred.c's, recorded. The power request goes through osc2_schedule_power()'s stand-in 20
 * ms later (the emulator acts on it); a turn-off is dropped at once while wired, where the real
 * file drops it when it falls due. */
void osc2_cmd_power(enum opensc2_power_request request, const char *why)
{
	stub.cmd_power++;
	stub.cmd_power_last = request;
	if (request == OPENSC2_POWER_TURN_OFF && osc2_wired()) {
		return;
	}
	osc2_schedule_power(request, 20, why);
}

void osc2_cmd_factory_reset(void)
{
	stub.factory_resets++;
	(void)osc2_storage_factory_reset();
	(void)osc2_keys_factory_reset();
	osc2_schedule_power(OPENSC2_POWER_FACTORY_RESET, 110, "host: factory reset");
}

void osc2_cmd_bond_write(const struct opensc2_bond *bond)
{
	stub.bond_writes++;
	(void)osc2_storage_store_bond(0, bond);
}

/* key_store.c, on the pure blob store; saves and deletes are recorded, nothing is stored. */
static bool blob(uint8_t id)
{
	const struct osc2_key_info *k = osc2_key_by_id(id);

	return k && k->kind == OSC2_KEYK_BLOB;
}

void osc2_keys_boot(void)
{
	osc2_blob_defaults(&stub.blobs);
}

int osc2_keys_set(uint8_t id, const uint8_t *value, uint8_t len)
{
	return blob(id) ? osc2_blob_set(&stub.blobs, id, value, len) : -EINVAL;
}

int osc2_keys_save(uint8_t id)
{
	if (!blob(id)) {
		return -EINVAL;
	}
	stub.key_saves |= 1u << id;
	return 0;
}

int osc2_keys_delete(uint8_t id)
{
	if (!blob(id)) {
		return -EINVAL;
	}
	(void)osc2_blob_reset(&stub.blobs, id);
	stub.key_deletes |= 1u << id;
	return 0;
}

/* 0xF0's delete: as osc2_keys_delete(), noted apart (the charge window counts it as a load) */
int osc2_keys_delete_cmd(uint8_t id)
{
	int err = osc2_keys_delete(id);

	if (err == 0) {
		stub.key_cmd_deletes |= 1u << id;
	}
	return err;
}

int osc2_keys_get(uint8_t id, uint8_t *out, uint8_t *len)
{
	const uint8_t *v = blob(id) ? osc2_blob_get(&stub.blobs, id, len) : NULL;

	if (!v) {
		return -EINVAL;
	}
	memcpy(out, v, *len);
	return 0;
}

int osc2_keys_put(uint8_t id, const uint8_t *value, uint8_t len, bool save)
{
	int err = osc2_keys_set(id, value, len);

	return err || !save ? err : osc2_keys_save(id);
}

int osc2_keys_store(uint8_t id, const uint8_t *value, uint8_t len)
{
	const struct osc2_key_info *k = osc2_key_by_id(id);

	if (!blob(id) || len != k->size || !value) {
		return -EINVAL;
	}
	stub.key_saves |= 1u << id;
	return 0;
}

/* crash.c's 0xF2 selector 1, on the stand-in store: the save is recorded. */
uint8_t osc2_crash_read(uint8_t out[OSC2_CRASH_ANSWER_LEN])
{
	uint8_t record[OSC2_KEY_VALUE_MAX], len = 0;

	if (osc2_keys_get(OSC2_KEY_RESET_REASON, record, &len) != 0 || len != OSC2_CRASH_LEN) {
		memset(record, 0, OSC2_CRASH_LEN);
	}
	if (osc2_crash_answer(record, out)) {
		(void)osc2_keys_set(OSC2_KEY_RESET_REASON, record, OSC2_CRASH_LEN);
		(void)osc2_keys_save(OSC2_KEY_RESET_REASON);
	}
	return OSC2_CRASH_ANSWER_LEN;
}

int osc2_keys_factory_reset(void)
{
	stub.key_factory_resets++;
	for (uint8_t id = 1; id < OSC2_KEY_COUNT; id++) {
		const struct osc2_key_info *k = osc2_key_by_id(id);

		if (blob(id) && (k->flags & OSC2_KEYF_FACTORY)) {
			stub.key_deletes |= 1u << id;   /* flash only: the RAM value stays */
		}
	}
	return 0;
}

void osc2_schedule_power(enum opensc2_power_request request, uint32_t delay_ms, const char *why)
{
	stub.power_requests++;
	stub.power = request;
	stub.power_delay_ms = delay_ms;
	stub.power_why = why;
}

int opensc2_bond_set(uint8_t slot, const struct opensc2_bond *bond)
{
	osc2.bonds[slot] = opensc2_bond_valid(bond) ? *bond : (struct opensc2_bond){ 0 };
	return osc2_storage_save_bond(slot);
}

void osc2_settings_restore_defaults(bool keep_imu)
{
	int16_t imu[3] = { osc2.settings[48], osc2.settings[84], osc2.settings[85] };

	for (int i = 0; i < OPENSC2_SETTINGS_COUNT; i++) {
		osc2.settings[i] = opensc2_setting_default((uint8_t)i);
	}
	if (keep_imu) {
		osc2.settings[48] = imu[0];
		osc2.settings[84] = imu[1];
		osc2.settings[85] = imu[2];
	}
	osc2.led.override = false;   /* as core.c does */
	osc2.led.brightness = (uint8_t)osc2.settings[OPENSC2_SETTING_LED_BRIGHTNESS];
	stub.defaults_restored++;
	osc2_haptics_setting(OPENSC2_SETTINGS_ALL);   /* as core.c does */
}

void osc2_watchdog_arm(void)
{
	if (osc2.settings[OPENSC2_SETTING_STEAM_WATCHDOG] != 0) {
		stub.watchdog_armed++;
	}
}

void osc2_watchdog_cancel(void)
{
	stub.watchdog_cancelled++;
}

static void note(char what)
{
	if (stub.lizard_calls < sizeof(stub.lizard) - 1) {
		stub.lizard[stub.lizard_calls++] = what;
	}
}

#if !defined(OSC2_HOST_FWEMU)
/* (The firmware emulator, fwemu.c, has lizard mode itself: lizard.c with report.c's order.) */
void osc2_lizard_set(bool on)
{
	note(on ? 'E' : 'L');   /* enter, leave */
	osc2_test_note_lizard(on ? OSC2_TEST_LIZARD_ENTER : OSC2_TEST_LIZARD_LEAVE);
}

void osc2_lizard_switches(bool up)
{
	note(up ? 'U' : 'D');   /* switches up, down */
	osc2_test_note_lizard(up ? OSC2_TEST_LIZARD_UP : OSC2_TEST_LIZARD_DOWN);
}
#else
void osc2_stub_lizard_note(char what)
{
	note(what);
}
#endif

bool osc2_link_active(enum osc2_link link)
{
#if defined(CONFIG_OPENSC2_TEST_HOOKS)
	if (link == OSC2_LINK_TEST) {
		link = osc2_mode_link();   /* as core.c does */
	}
#endif
	if (link == OSC2_LINK_DOCK) {
		return false;   /* as core.c does: the dock never counts */
	}
	return stub.link_active[link];
}

/* Back to boot: defaults, a test identity, empty bonds, both stored requests reset. */
void stub_reset(void)
{
	memset(&stub, 0, sizeof(stub));
	memset(&osc2, 0, sizeof(osc2));
	osc2_settings_restore_defaults(false);
	stub.defaults_restored = 0;
	osc2_keys_boot();
	memcpy(osc2.identity.unit_serial, "OSC0123456789", 13);
	memcpy(osc2.identity.board_serial, "OSB0123456789", 13);
	memcpy(osc2.identity.git_revision, "opensc2-0.2", 11);
	osc2.identity.product_id = 0x1302;
	osc2.identity.board_revision = 0x48;
	osc2.identity.build_time = 0x6A628345;
	osc2.identity.bootloader_build_time = 0x68D2F92E;
	memcpy(osc2.identity.product_name, "Steam Controller", 16);
	osc2.chime = 2;
	osc2.transport = OSC2_TRANSPORT_DEFAULT;   /* as core.c starts it */
	osc2.transport_saved = OSC2_TRANSPORT_NONE;
	stub.stored_transport = OSC2_TRANSPORT_NONE;
	osc2.mode = OPENSC2_MODE_WIRED;
	stub.link_active[OSC2_LINK_USB] = true;
	for (int link = 0; link < OSC2_LINK_COUNT; link++) {
		osc2_commands_reset((enum osc2_link)link);
	}
	cal_host_erase_all();   /* a new board: no calibration stored */
	osc2_cal_reset();
	osc2_imu_reset();
	osc2_dock_reset();
	osc2_dock_offsets_reset();
	memset(&dock_stub, 0, sizeof(dock_stub));
#if defined(CONFIG_OPENSC2_TEST_HOOKS)
	hooks_reset();
#endif
}
