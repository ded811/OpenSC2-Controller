/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: stand-ins for the parts of the library around the command channel (core.c,
 * storage.c, keys.c, radio.c, report.c), so commands.c runs on a PC. Each records what it was asked
 * to do; the tests in commands_cases.h look at the record.
 */
#include <errno.h>
#include <string.h>
#include "internal.h"
#include "stubs.h"
#include "test_hooks.h"

struct osc2_ctx osc2;
struct stub_record stub;

int64_t k_uptime_get(void)
{
	return stub.now_ms;
}

void opensc2_identity_get(struct opensc2_identity *identity)
{
	*identity = osc2.identity;
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
	return 0;
}

int osc2_storage_save_slot(void)
{
	stub.saved_slot++;
	return 0;
}

int osc2_storage_save_chime(void)
{
	stub.saved_chime++;
	return 0;
}

int osc2_storage_factory_reset(void)
{
	return 0;
}

/* keys.c, on the pure blob store; saves and deletes are recorded, nothing is stored. */
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

int osc2_keys_factory_reset(void)
{
	stub.key_factory_resets++;
	for (uint8_t id = 1; id < OSC2_KEY_COUNT; id++) {
		const struct osc2_key_info *k = osc2_key_by_id(id);

		if (blob(id) && (k->flags & OSC2_KEYF_FACTORY)) {
			(void)osc2_keys_delete(id);
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
		link = osc2_wired() ? OSC2_LINK_USB : OSC2_LINK_RADIO;   /* as core.c does */
	}
#endif
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
	osc2.mode = OPENSC2_MODE_WIRED;
	stub.link_active[OSC2_LINK_USB] = true;
	for (int link = 0; link < OSC2_LINK_COUNT; link++) {
		osc2_commands_reset((enum osc2_link)link);
	}
#if defined(CONFIG_OPENSC2_TEST_HOOKS)
	hooks_reset();
#endif
}
