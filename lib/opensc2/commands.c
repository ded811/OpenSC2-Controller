/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * commands.c: the vendor command channel ("feature report 1"), the same on every link:
 *   - through the puck's radio a SET record runs a command, and a GET record asks for the answer to
 *     the last SET, as on USB (the record's byte is not used: Valve's puck leaves it unset);
 *   - on USB a 64-byte SET_REPORT(Feature 1) runs a command, and GET_REPORT(Feature 1) answers the
 *     request the last successful SET stored.
 * Each link keeps its own stored request (0x83, the attributes, until a SET). A refused command is
 * a result code on the radio and a STALL on USB. The commands and their formats are in
 * docs/api.md; anything not handled here goes to the application's vendor_command callback.
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
#include "cal_store.h"
#include "crash.h"
#include "dock_link.h"
#include "key_store.h"
#include "proto_rules.h"
#include "system.h"
#include "test_hooks.h"

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

#if defined(CONFIG_HWINFO)
/* proto_rules.c restates these to stay free of Zephyr. */
BUILD_ASSERT(RESET_PIN == BIT(0) && RESET_SOFTWARE == BIT(1) && RESET_BROWNOUT == BIT(2) &&
	     RESET_POR == BIT(3) && RESET_WATCHDOG == BIT(4), "Zephyr's RESET_* bits changed");
#endif

#define KEY_FACTORY_RESET      0xEFC1A917u   /* 0x86 */
#define KEY_BOOTLOADER         0xECAABAC0u   /* 0x95 */
#define KEY_REBOOT_KEEP_LINK   0xA427AF52u   /* 0x95: come back in wireless mode */
#define KEY_TURN_OFF           0xABDF9710u   /* 0x95 */
#define POWER_DELAY_MS         20            /* so the answer can still go out */

/* The request the last successful SET stored on a link, which selects what its next GET answers. */
struct stored_request {
	uint8_t opcode;
	uint8_t len;
	uint8_t payload[OPENSC2_ANSWER_MAX];
};

static struct stored_request stored[OSC2_LINK_COUNT];

/* One command at a time, whichever link it came on: the dock runs commands in a thread of its own
 * beside the radio's (or USB's), and the handlers share the settings, the bonds and the identity.
 * The real controller has no such lock; nothing it does can be told apart from the outside. The
 * test channel's own requests (opcode 0x7E) run outside it: they take the hooks' request lock
 * first and may then run a command (HOST_SET, the dock frame op), so this lock always comes
 * second. A mutex, so a command that calls back into this file from the same thread is fine. */
static K_MUTEX_DEFINE(command_lock);

void osc2_commands_reset(enum osc2_link link)
{
	memset(&stored[link], 0, sizeof(stored[link]));
	stored[link].opcode = 0x83;
}

/* A little-endian u32 from the first bytes of `p` (`avail` of them exist), zero padded. */
static uint32_t get_u32(const uint8_t *p, size_t avail)
{
	uint8_t b[4] = { 0 };

	memcpy(b, p, MIN(avail, sizeof(b)));
	return opensc2_get_le32(b);
}

/* ---- settings by name (0xED, 0xEE, 0xEF, 0xF0) ----------------------------------------------- */

/* The keys (key_rules.c): every key of the real controller, with its size and whether 0xED reads
 * it, and OpenSC2's identity keys. A name that is not there, and a calibration key while the
 * library keeps no calibration, goes to the application's vendor_command (0xED answers length 0
 * without one, the others are refused), so a host cannot fill the storage partition with names of
 * its own. The blob keys are only kept (key_store.c). */
enum named_key {
	KEY_NONE = OSC2_KEY_UNKNOWN,
	KEY_BOND_1 = OSC2_KEY_BOND,           /* esb/bond */
	KEY_BOND_2 = OSC2_KEY_BOND_2,         /* esb/bond_2 */
	KEY_TRANSPORT = OSC2_KEY_TRANSPORT,   /* user/wireless_transport: 0..3 (transport_rules.h) */
	KEY_CHIME = OSC2_KEY_CHIME,           /* user/haptic_boot_level */
	KEY_ID_UNIT = OSC2_KEY_ID_UNIT,       /* opensc2/unit_serial */
	KEY_ID_BOARD = OSC2_KEY_ID_BOARD,     /* opensc2/board_serial */
	KEY_ID_GIT = OSC2_KEY_ID_GIT,         /* opensc2/git_revision */
	KEY_ID_REV = OSC2_KEY_ID_REV,         /* opensc2/board_revision, u32 */
};

/* The library's key a payload names, or NULL (the application's, or no key). */
static const struct osc2_key_info *find_key(const uint8_t *payload, uint8_t len, uint8_t *name_len)
{
	const struct osc2_key_info *k = osc2_key_lookup(payload, len, name_len);

	if (!k || k->kind == OSC2_KEYK_CAL) {
		return NULL;
	}
#if !defined(CONFIG_OPENSC2_IDENTITY_VENDOR_WRITE)
	if (k->kind == OSC2_KEYK_IDENTITY) {
		return NULL;   /* without the option an identity key is a name like any other */
	}
#endif
	return k;
}

static uint8_t bond_slot_of(enum named_key key)
{
	return key == KEY_BOND_1 ? 0 : 1;
}

/* 0xEE: set a key in RAM. Refused for an empty value; a bond must be exactly 24 bytes, and is
 * kept as written (one with an ID of 0 reads back empty and cannot be saved). Either way the radio
 * hears that a bond was written, as on the real controller (radio_rules.h says what that does in
 * each state). A blob must have exactly its key's size. */
static int key_set(const struct osc2_key_info *info, const uint8_t *value, uint8_t len)
{
	enum named_key key = (enum named_key)info->id;

	if (info->kind == OSC2_KEYK_BLOB) {
		return osc2_keys_set(info->id, value, len);
	}
	switch (key) {
	case KEY_BOND_1:
	case KEY_BOND_2: {
		struct opensc2_bond bond = { 0 };
		k_spinlock_key_t lk;

		if (len != OPENSC2_BOND_LEN) {
			osc2_radio_post(OSC2_EV_BOND);
			return -EINVAL;
		}
		opensc2_bond_unpack(value, &bond);
		lk = k_spin_lock(&osc2.lock);
		osc2.bonds[bond_slot_of(key)] = bond;
		k_spin_unlock(&osc2.lock, lk);
		osc2_radio_post(OSC2_EV_BOND);
		return 0;
	}
	case KEY_TRANSPORT:
		/* The key's value in RAM, any byte (0xEF stores it; the next boot runs it, as
		 * transport_rules.h reads it). The running link carries on, and the link coming up
		 * writes the running transport back into the key, as on the real controller. */
		if (len != 1) {
			return -EINVAL;
		}
		osc2.transport = value[0];
		return 0;
	case KEY_CHIME:
		if (len != 1) {
			return -EINVAL;
		}
		osc2.chime = value[0];
		return 0;
#if defined(CONFIG_OPENSC2_IDENTITY_VENDOR_WRITE)
	case KEY_ID_UNIT:
	case KEY_ID_BOARD:
	case KEY_ID_GIT:
	case KEY_ID_REV: {
		struct opensc2_identity id = { 0 };
		char text[OPENSC2_SERIAL_LEN + 1] = { 0 };
		uint8_t field;

		if (len == 0) {
			return -EINVAL;
		}
		memcpy(text, value, MIN(len, OPENSC2_SERIAL_LEN));
		if (key == KEY_ID_UNIT) {
			memcpy(id.unit_serial, text, sizeof(id.unit_serial));
			field = OSC2_ID_UNIT;
		} else if (key == KEY_ID_BOARD) {
			memcpy(id.board_serial, text, sizeof(id.board_serial));
			field = OSC2_ID_BOARD;
		} else if (key == KEY_ID_GIT) {
			memcpy(id.git_revision, text, sizeof(id.git_revision) - 1);
			field = OSC2_ID_GIT;
		} else {
			id.board_revision = get_u32(value, len);
			field = OSC2_ID_REV;
		}
		/* Exactly the key named, as sent: a revision of 0 is stored, an empty string refused. */
		return osc2_identity_store_fields(field, &id);
	}
#endif
	default:
		return -ENOENT;
	}
}

/* 0xEF: write the RAM value to flash; refused when there is no valid value (an empty bond). The
 * real controller saves what the key's read function gives, so it fails for a key without one
 * (the settings/haptics keys) and for a value the read function refuses (an invalid charge
 * window): nothing is written then. */
static int key_save(const struct osc2_key_info *info)
{
	enum named_key key = (enum named_key)info->id;

	if (info->kind == OSC2_KEYK_BLOB) {
		uint8_t value[OSC2_KEY_VALUE_MAX];
		uint8_t len = 0;

		if (!(info->flags & OSC2_KEYF_READ) || osc2_keys_get(info->id, value, &len) != 0 ||
		    !osc2_key_value_readable(info->id, value, len)) {
			return -ENOENT;
		}
		return osc2_keys_save(info->id);
	}
	switch (key) {
	case KEY_BOND_1:
	case KEY_BOND_2: {
		k_spinlock_key_t lk = k_spin_lock(&osc2.lock);
		bool valid = opensc2_bond_valid(&osc2.bonds[bond_slot_of(key)]);

		k_spin_unlock(&osc2.lock, lk);
		return valid ? osc2_storage_save_bond(bond_slot_of(key)) : -ENODEV;   /* no value */
	}
	case KEY_TRANSPORT:
		return osc2_storage_save_transport(osc2.transport);
	case KEY_CHIME:
		return osc2_storage_save_chime();
	default:
		return 0;   /* identity keys are stored when they are set */
	}
}

/* 0xF0: delete from flash and, once that worked, clear the RAM value (back to the default). */
static int key_delete(const struct osc2_key_info *info)
{
	enum named_key key = (enum named_key)info->id;

	if (info->kind == OSC2_KEYK_BLOB) {
		return osc2_keys_delete(info->id);
	}
	switch (key) {
	case KEY_BOND_1:
	case KEY_BOND_2: {
		k_spinlock_key_t lk;
		int err = osc2_storage_delete_bond(bond_slot_of(key));

		if (err) {
			return err;
		}
		lk = k_spin_lock(&osc2.lock);
		memset(&osc2.bonds[bond_slot_of(key)], 0, sizeof(osc2.bonds[0]));
		k_spin_unlock(&osc2.lock, lk);
		osc2_radio_post(OSC2_EV_BOND);
		return 0;
	}
	case KEY_TRANSPORT: {
		/* The key back at 2; the running link carries on. */
		int err = osc2_storage_delete_transport();

		if (err == 0) {
			osc2.transport = OSC2_TRANSPORT_DEFAULT;
		}
		return err;
	}
	case KEY_CHIME:
		osc2.chime = 2;
		return osc2_storage_save_chime();
	default:
		return 0;
	}
}

/* 0xED: the key's RAM value; an empty or invalid bond reads back with length 0, and so does a key
 * the real controller has no read function for (the fully-charged LED level, the haptics
 * settings). A blob reads its whole value, zeros when nothing was set; the charge window only
 * while it is valid (key_rules.h). */
static int key_read(const struct osc2_key_info *info, uint8_t *out, uint8_t *out_len)
{
	enum named_key key = (enum named_key)info->id;
	k_spinlock_key_t lk;
	struct opensc2_bond bond;

	if (!(info->flags & OSC2_KEYF_READ)) {
		*out_len = 0;
		return 0;
	}
	if (info->kind == OSC2_KEYK_BLOB) {
		int err = osc2_keys_get(info->id, out, out_len);

		if (err == 0 && !osc2_key_value_readable(info->id, out, *out_len)) {
			*out_len = 0;
		}
		return err;
	}
	switch (key) {
	case KEY_BOND_1:
	case KEY_BOND_2:
		lk = k_spin_lock(&osc2.lock);
		bond = osc2.bonds[bond_slot_of(key)];
		k_spin_unlock(&osc2.lock, lk);
		if (!opensc2_bond_valid(&bond)) {
			*out_len = 0;
			return 0;
		}
		opensc2_bond_pack(&bond, out);
		*out_len = OPENSC2_BOND_LEN;
		return 0;
	case KEY_TRANSPORT:
		out[0] = osc2.transport;
		*out_len = 1;
		return 0;
	case KEY_CHIME:
		out[0] = osc2.chime;
		*out_len = 1;
		return 0;
	default:
		return -ENOENT;
	}
}

/* ---- 0xA2 ------------------------------------------------------------------------------------ */

/* As on the real controller: the bond is written to flash from the system work queue (an invalid
 * one deletes the stored bond), the bond in RAM stays as it is, and the radio hears nothing; the
 * new bond counts from the next boot. */
static uint8_t a2_bond[OPENSC2_BOND_LEN];

static void a2_fn(struct k_work *work)
{
	struct opensc2_bond bond;

	ARG_UNUSED(work);
	opensc2_bond_unpack(a2_bond, &bond);
	if (osc2_storage_store_bond(0, &bond)) {
		LOG_WRN("0xA2: the bond could not be stored");
	}
}
static K_WORK_DEFINE(a2_work, a2_fn);

static void bond_write_later(const uint8_t *payload)
{
	memcpy(a2_bond, payload, sizeof(a2_bond));
	k_work_submit(&a2_work);
}

/* ---- the application's handler --------------------------------------------------------------- */

static int app_command(uint8_t opcode, const uint8_t *payload, uint8_t len, bool is_get,
		       uint8_t *answer, uint8_t *answer_len)
{
	struct opensc2_vendor_cmd cmd = {
		.opcode = opcode,
		.len = len,
		.payload = payload,
		.is_get = is_get,
	};
	int rc;

	*answer_len = 0;
	if (!osc2.cb || !osc2.cb->vendor_command) {
		return -ENOTSUP;
	}
	rc = osc2.cb->vendor_command(&cmd, answer, answer_len, osc2.user_data);
	*answer_len = MIN(*answer_len, OPENSC2_ANSWER_MAX);
	return rc;
}

/* ---- controller settings --------------------------------------------------------------------- */

/* One setting written by 0x87: clamped to its range; setting 9 (lizard mode) and 71 (the Steam
 * watchdog) act as on the real controller. Callbacks run only when the value changes. */
static void write_setting(uint8_t number, int16_t value, bool *led)
{
	int16_t old = osc2.settings[number];
	int16_t v = opensc2_setting_clamp(number, value);

	osc2.settings[number] = v;
	switch (number) {
	case OPENSC2_SETTING_LIZARD_MODE:
		if (v == 0) {
			osc2_watchdog_arm();   /* every write of 0 shows Steam is still there */
			if (old != 0) {
				osc2_lizard_set(false);
			}
		} else if (old == 0) {
			osc2_lizard_set(true);
		}
		break;
	case OPENSC2_SETTING_STEAM_WATCHDOG:
		if (old != v) {
			if (v) {
				osc2_watchdog_arm();
			} else {
				osc2_watchdog_cancel();
			}
		}
		break;
	case OPENSC2_SETTING_LED_OVERRIDE:
	case OPENSC2_SETTING_LED_BRIGHTNESS: {
		k_spinlock_key_t lk = k_spin_lock(&osc2.lock);

		osc2.led.override = osc2.settings[OPENSC2_SETTING_LED_OVERRIDE] != 0;
		osc2.led.brightness =
			(uint8_t)CLAMP(osc2.settings[OPENSC2_SETTING_LED_BRIGHTNESS], 0, 100);
		k_spin_unlock(&osc2.lock, lk);
		*led = true;
		break;
	}
	default:
		break;
	}
	if (old != v) {
		osc2_sys_setting(number);   /* setting 50, 77, the LED's */
		/* After any lizard mode reports the write caused. */
		osc2_test_note_setting(number, v);
		if (osc2.cb && osc2.cb->setting_changed) {
			osc2.cb->setting_changed(number, v, osc2.user_data);
		}
	}
}

static int set_settings(const uint8_t *payload, uint8_t len)
{
	bool led = false;

	if (len % 3) {
		return -EINVAL;
	}
	for (uint8_t i = 0; i < len; i += 3) {
		uint8_t number = payload[i];

		if (number >= OPENSC2_SETTINGS_COUNT) {
			LOG_DBG("Setting %u does not exist", number);
			continue;
		}
		write_setting(number, (int16_t)opensc2_get_le16(&payload[i + 1]), &led);
	}
	if (led) {
		osc2_notify_led();
	}
	return 0;
}

/* 0x8E: every setting back to its default; lizard mode comes back if it was off. */
static void load_defaults(void)
{
	int16_t old_lizard = osc2.settings[OPENSC2_SETTING_LIZARD_MODE];
	int16_t old_watchdog = osc2.settings[OPENSC2_SETTING_STEAM_WATCHDOG];

	osc2_settings_restore_defaults(true);
	if (old_watchdog == 0 && osc2.settings[OPENSC2_SETTING_STEAM_WATCHDOG] != 0) {
		osc2_watchdog_arm();
	}
	if (old_lizard == 0 && osc2.settings[OPENSC2_SETTING_LIZARD_MODE] != 0) {
		osc2_lizard_set(true);
	}
	osc2_test_note_setting(OPENSC2_SETTINGS_ALL, 0);
	if (osc2.cb && osc2.cb->setting_changed) {
		osc2.cb->setting_changed(OPENSC2_SETTINGS_ALL, 0, osc2.user_data);
	}
	osc2_notify_led();
}

/* 0x81: lizard mode off (keyboard first, then the mouse), and the watchdog re-armed. */
static int lizard_off(enum osc2_link link)
{
	int16_t old = osc2.settings[OPENSC2_SETTING_LIZARD_MODE];

	if (!osc2_link_active(link)) {
		return -EINVAL;
	}
	osc2.settings[OPENSC2_SETTING_LIZARD_MODE] = 0;
	if (old != 0) {
		osc2_lizard_set(false);   /* releases whatever lizard mode held */
	} else {
		osc2_lizard_switches(false);
	}
	osc2_watchdog_arm();
	if (old != 0) {
		osc2_test_note_setting(OPENSC2_SETTING_LIZARD_MODE, 0);
		if (osc2.cb && osc2.cb->setting_changed) {
			osc2.cb->setting_changed(OPENSC2_SETTING_LIZARD_MODE, 0, osc2.user_data);
		}
	}
	return 0;
}

void osc2_settings_write(uint8_t number, int16_t value)
{
	bool led = false;

	if (number >= OPENSC2_SETTINGS_COUNT) {
		return;
	}
	write_setting(number, value, &led);
	if (led) {
		osc2_notify_led();
	}
}

void osc2_led_color_write(const uint8_t rgbw[4])
{
	k_spinlock_key_t lk = k_spin_lock(&osc2.lock);

	osc2.led.red = MIN(rgbw[0], 100);
	osc2.led.green = MIN(rgbw[1], 100);
	osc2.led.blue = MIN(rgbw[2], 100);
	osc2.led.white = MIN(rgbw[3], 100);
	k_spin_unlock(&osc2.lock, lk);
	osc2_notify_led();
}

/* ---- SET ------------------------------------------------------------------------------------- */

/* `avail` bytes follow the opcode and length (at least `len`). */
static int run_set(enum osc2_link link, uint8_t opcode, const uint8_t *payload, uint8_t len,
		   size_t avail)
{
	switch (opcode) {
	/* Commands that only answer: the SET just selects them for the GET. */
	case 0x83: case 0xAE: case 0xF2: case 0xA1: case 0xBE: case 0xE9: case 0xDB: case 0x89:
	case 0xED:
		return 0;

	case 0x87:   /* ID_SET_SETTINGS_VALUES */
		return set_settings(payload, len);
	case 0x8E:   /* ID_LOAD_DEFAULT_SETTINGS_VALUES */
		load_defaults();
		return 0;
	case 0x81:   /* ID_CLEAR_DIGITAL_MAPPINGS: lizard mode off */
		if (link == OSC2_LINK_DOCK) {
			return 0;   /* from the dock: acknowledged, and nothing happens */
		}
		return lizard_off(link);
	case 0x85:   /* ID_SET_DEFAULT_DIGITAL_MAPPINGS: lizard mode's keyboard and buttons back */
		if (link == OSC2_LINK_DOCK) {
			return 0;
		}
		if (!osc2_link_active(link)) {
			return -EINVAL;
		}
		osc2_lizard_switches(true);
		return 0;

	case 0xC5: { /* ID_SET_LED_COLOR: four percentages R, G, B, W */
		uint8_t c[4] = { 0 };

		memcpy(c, payload, MIN(len, 4));
		osc2_led_color_write(c);
		return 0;
	}
	case 0xDC:   /* ID_SET_USER_STORE: type 1 is the chime level */
		if (len >= 2 && payload[0] == 1) {
			osc2.chime = payload[1];
			return osc2_storage_save_chime();
		}
		return -EINVAL;
	case 0xE2: { /* persist the calibration of side 0 or 1 */
		uint8_t answer[OPENSC2_ANSWER_MAX];
		uint8_t answer_len;
		int rc = osc2_cal_command(opcode, payload, len, avail);   /* the cal/ keys */

		if (rc) {
			return rc;
		}
		/* The application may keep a calibration of its own; with nothing to persist the
		 * real controller acknowledges, and Steam sends it in every session. */
		rc = app_command(opcode, payload, len, false, answer, &answer_len);
		return rc == -ENOTSUP ? 0 : rc;
	}
	case 0xD8: case 0xC0: case 0xC3: case 0xFE: { /* calibration recordings, erase */
		uint8_t answer[OPENSC2_ANSWER_MAX];
		uint8_t answer_len;
		int rc = osc2_cal_command(opcode, payload, len, avail);

		return rc != -ENOTSUP ? rc :
					app_command(opcode, payload, len, false, answer, &answer_len);
	}

	case 0xEE:   /* set a setting by name, in RAM */
	case 0xEF:   /* save it */
	case 0xF0: { /* delete it */
		uint8_t name_len = 0;
		const struct osc2_key_info *key = find_key(payload, len, &name_len);
		uint8_t answer[OPENSC2_ANSWER_MAX];
		uint8_t answer_len;
		int rc;

		if (!key) {
			rc = osc2_cal_by_name(opcode, payload, len);   /* a cal/ key? */
			if (rc != -ENOENT) {
				return rc;
			}
			rc = app_command(opcode, payload, len, false, answer, &answer_len);
			return rc == -ENOTSUP ? -ENOENT : rc;
		}
		if (opcode == 0xEE) {
			return key_set(key, payload + name_len, len - name_len);
		}
		if (name_len != len) {
			return -EINVAL;   /* the NUL must be the last payload byte */
		}
		return opcode == 0xEF ? key_save(key) : key_delete(key);
	}
	case 0xA2: /* store the main puck's bond (the first slot) for the next boot */
		if (len != OPENSC2_BOND_LEN) {
			return -EINVAL;
		}
		bond_write_later(payload);
		return 0;

	case 0x86:   /* factory reset */
		if (get_u32(payload, MIN(avail, (size_t)4)) != KEY_FACTORY_RESET) {
			LOG_INF("Ignoring a factory reset without its key");
			return 0;
		}
		LOG_INF("Factory reset");
		memset(osc2.bonds, 0, sizeof(osc2.bonds));
		osc2.slot = 0;
		osc2.transport = OSC2_TRANSPORT_DEFAULT;
		osc2.chime = 2;
		(void)osc2_storage_factory_reset();
		(void)osc2_keys_factory_reset();   /* the keylock, LED level, charge window, crash record */
		osc2_radio_post(OSC2_EV_BOND);
		osc2_schedule_power(OPENSC2_POWER_FACTORY_RESET, 100, "host: factory reset");
		return 0;
	case 0x90:   /* ID_REBOOT_INTO_ISP */
		osc2_schedule_power(OPENSC2_POWER_BOOTLOADER, POWER_DELAY_MS, "host: bootloader");
		return 0;
	case 0x95: { /* ID_FIRMWARE_UPDATE_REBOOT: the key is the first four payload bytes */
		uint32_t k = get_u32(payload, MIN(avail, (size_t)4));

		if (k == KEY_BOOTLOADER) {
			osc2_schedule_power(OPENSC2_POWER_BOOTLOADER, POWER_DELAY_MS,
					    "host: bootloader");
		} else if (k == KEY_REBOOT_KEEP_LINK) {
			osc2_schedule_power(OPENSC2_POWER_REBOOT_KEEP_LINK, POWER_DELAY_MS,
					    "host: reboot into wireless");
		} else if (k == KEY_TURN_OFF) {
			if (!osc2_wired()) {   /* a wired controller ignores turn-off */
				osc2_schedule_power(OPENSC2_POWER_TURN_OFF, POWER_DELAY_MS,
						    "host: turn off");
			}
		} else {
			osc2_schedule_power(OPENSC2_POWER_REBOOT, POWER_DELAY_MS, "host: reboot");
		}
		return 0;
	}
	case 0x9F:   /* ID_TURN_OFF */
		if (!osc2_wired()) {
			osc2_schedule_power(OPENSC2_POWER_TURN_OFF, POWER_DELAY_MS, "host: turn off");
		}
		return 0;

	default: {
		uint8_t answer[OPENSC2_ANSWER_MAX];
		uint8_t answer_len;

		/* The dock never carries the test channel: a 0x7E there is an opcode nobody knows
		 * (-22 on the dock), and it never reaches the application, which a 0x7E would do
		 * outside the command lock (osc2_command_set()). */
		if (opcode == OSC2_TEST_OPCODE && link == OSC2_LINK_DOCK) {
			return -EINVAL;
		}
		/* The hardware test channel (CONFIG_OPENSC2_TEST_HOOKS) claims a keyed 0x7E. */
		if (osc2_test_claim(link, opcode, payload, len)) {
			return 0;
		}
		return app_command(opcode, payload, len, false, answer, &answer_len);
	}
	}
}

/* The test hooks' record of a SET (never of the test channel itself, which would flood it). */
static void note_set(enum osc2_link link, const uint8_t *rec, uint8_t rec_len, int rc)
{
	uint8_t len = rec[1];

	if (rec[0] == OSC2_TEST_OPCODE) {
		return;
	}
	osc2_test_count(rc == 0 ? OSC2_TEST_CNT_VENDOR_SET_OK : OSC2_TEST_CNT_VENDOR_SET_ERR);
	osc2_test_note_vendor(link, false, rec[0], len, rc, &rec[2], len, (size_t)rec_len - 2);
}

int32_t osc2_command_set(enum osc2_link link, const uint8_t *rec, uint8_t rec_len)
{
	struct stored_request *r = &stored[link];
	uint8_t opcode, len, ctx;
	bool locked;
	int rc;

	if (rec_len < 2) {
		return -EINVAL;
	}
	opcode = rec[0];
	len = rec[1];
	if (len > rec_len - 2 || len > OPENSC2_ANSWER_MAX) {
		note_set(link, rec, rec_len, -EINVAL);
		return -EINVAL;
	}
	locked = opcode != OSC2_TEST_OPCODE;   /* the test channel locks on its own (above) */
	if (locked) {
		(void)k_mutex_lock(&command_lock, K_FOREVER);
	}
	ctx = osc2_test_ctx_enter(link);
	rc = run_set(link, opcode, &rec[2], len, (size_t)rec_len - 2);
	LOG_DBG("SET 0x%02x (%u bytes): %d", opcode, len, rc);
	if (rc == 0) {
		r->opcode = opcode;
		r->len = len;
		memset(r->payload, 0, sizeof(r->payload));
		memcpy(r->payload, &rec[2], len);
		osc2_test_stored(link);
	}
	note_set(link, rec, rec_len, rc);
	osc2_test_ctx_leave(ctx);
	if (locked) {
		(void)k_mutex_unlock(&command_lock);
	}
	return rc;
}

uint8_t osc2_command_stored(enum osc2_link link, uint8_t rec[2 + OPENSC2_ANSWER_MAX])
{
	const struct stored_request *r = &stored[link];

	rec[0] = r->opcode;
	rec[1] = r->len;
	memcpy(&rec[2], r->payload, r->len);
	return (uint8_t)(2 + r->len);
}

/* ---- GET ------------------------------------------------------------------------------------- */

/* Attribute 11 of 0x83: the report interval of the controller's current link in µs, whichever
 * link asks (the real controller keeps one): 64000 while the dock is the only link up
 * (dock_link.h); on Bluetooth the connection interval once it is the active transport (0 before,
 * as on the real controller); 4000 on USB and the puck's radio. */
static uint32_t link_interval_us(void)
{
	uint32_t us = osc2_mode_link() == OSC2_LINK_BLE ? osc2_ble_interval_us() :
							  OSC2_REPORT_INTERVAL_US;

	return osc2_dock_interval_us(us);
}

static int run_get(enum osc2_link link, uint8_t opcode, const uint8_t *payload, uint8_t len,
		   uint8_t *out, uint8_t *out_len)
{
	struct opensc2_identity id;
	uint8_t sel = len ? payload[0] : 0;

	*out_len = 0;
	opensc2_identity_get(&id);
	switch (opcode) {
	case 0x83:   /* ID_GET_ATTRIBUTES_VALUES: attribute 11 is the current link's interval */
		*out_len = (uint8_t)opensc2_encode_attributes(&id, link_interval_us(), out);
		return 0;
	case 0xAE:   /* ID_GET_STRING_ATTRIBUTE: only the tag byte counts, whatever the length */
		*out_len = (uint8_t)opensc2_encode_string_attribute(&id, sel, out);
		return 0;
	case 0xF2:   /* device info */
		memset(out, 0, OPENSC2_ANSWER_MAX);
		out[0] = sel;
		if (sel == 0) {
			/* build time, hardware ID, git revision (16), unit serial (16) */
			opensc2_put_le32(&out[1], id.build_time);
			opensc2_put_le32(&out[5], id.board_revision);
			memcpy(&out[9], id.git_revision, strlen(id.git_revision));
			memcpy(&out[25], id.unit_serial, OPENSC2_SERIAL_LEN);
			*out_len = 41;
		} else if (sel == 1) {
			/* the crash record as it is (zeros without one); a new one is then marked seen */
			*out_len = osc2_crash_read(out);
		} else if (sel == 2) {
			/* uptime in ms, the last radio disconnect reason (u32) */
			opensc2_put_le32(&out[1], (uint32_t)k_uptime_get());
			opensc2_put_le32(&out[5], osc2.last_disconnect_reason);
			*out_len = 9;
		}
		return 0;
	case 0xA1:   /* device info: 18 bytes, the reset causes and chip ID for selector 1 */
		memset(out, 0, OPENSC2_ANSWER_MAX);
		*out_len = 18;   /* any other selector: 0 and 17 zero bytes */
		if (sel == 1) {
			uint32_t cause = 0;

			out[0] = 1;
#if defined(CONFIG_HWINFO)
			/* never cleared, so the bits of several resets add up, as on the real one */
			(void)hwinfo_get_reset_cause(&cause);
			(void)hwinfo_get_device_id(&out[2], 8);
#endif
			out[1] = osc2_reset_flags(cause);
		}
		return 0;
	case 0xBE: { /* ID_GET_BATTERY_DATA */
		struct opensc2_battery b;
		k_spinlock_key_t lk = k_spin_lock(&osc2.lock);

		b = osc2.battery;
		k_spin_unlock(&osc2.lock, lk);
		osc2_test_battery(&b);
		*out_len = (uint8_t)opensc2_encode_battery_data(&b, out);
		return 0;
	}
	case 0xE9: { /* ID_GET_LED_COLOR: as the real controller reads it back (proto_rules.h) */
		struct opensc2_led led;

		opensc2_led_get(&led);
		out[0] = osc2_led_readback(led.red);
		out[1] = osc2_led_readback(led.green);
		out[2] = osc2_led_readback(led.blue);
		out[3] = osc2_led_readback(led.white);
		*out_len = 4;
		return 0;
	}
	case 0xDB:   /* ID_GET_USER_STORE */
		out[0] = 1;
		out[1] = osc2.chime;
		*out_len = 2;
		return 0;
	case 0x89:   /* ID_GET_SETTINGS_VALUES */
		out[0] = sel;
		/* As the real controller answers: a number above 85 reads 0, and 48, 84 and 85 their
		 * defaults (its IMU keeps those) */
		opensc2_put_le16(&out[1], sel < OPENSC2_SETTINGS_COUNT ?
				 (uint16_t)osc2_setting_answer(sel, osc2.settings[sel]) : 0);
		*out_len = 3;
		return 0;
	case 0xED: { /* read a setting by name: length 0 for a key without a value */
		uint8_t name_len;
		const struct osc2_key_info *key = find_key(payload, len, &name_len);

		if (key && key_read(key, out, out_len) == 0) {
			return 0;
		}
		if (osc2_cal_read_by_name(payload, len, out, out_len) == 0) {
			return 0;
		}
		if (app_command(opcode, payload, len, true, out, out_len) != 0) {
			*out_len = 0;
		}
		return 0;
	}
	/* Commands that only act: nothing to answer. */
	case 0x81: case 0x85: case 0x86: case 0x87: case 0x8E: case 0x90: case 0x95: case 0x9F:
	case 0xA2: case 0xC5: case 0xDC: case 0xE2: case 0xEE: case 0xEF: case 0xF0:
		return -ENOTSUP;
	default:
		/* The answer the hardware test channel holds for this link's claimed 0x7E. */
		if (opcode == OSC2_TEST_OPCODE && osc2_test_answer(link, out, out_len)) {
			return 0;
		}
		return app_command(opcode, payload, len, true, out, out_len);
	}
}

/* The test hooks' record of a GET (not of the test channel's). */
static void note_get(enum osc2_link link, const struct stored_request *r, int rc, uint8_t n)
{
	if (r->opcode == OSC2_TEST_OPCODE) {
		return;
	}
	osc2_test_count(rc == 0 ? OSC2_TEST_CNT_VENDOR_GET_OK : OSC2_TEST_CNT_VENDOR_GET_ERR);
	osc2_test_note_vendor(link, true, r->opcode, n, rc, r->payload, r->len, r->len);
}

uint8_t osc2_command_get_rc(enum osc2_link link, uint8_t out[63], int *rc)
{
	const struct stored_request *r = &stored[link];
	bool locked = r->opcode != OSC2_TEST_OPCODE;   /* the test channel's held answer */
	uint8_t data_len = 0;
	uint8_t ctx;

	if (locked) {
		(void)k_mutex_lock(&command_lock, K_FOREVER);
	}
	ctx = osc2_test_ctx_enter(link);
	memset(out, 0, 63);
	out[0] = r->opcode;
	*rc = run_get(link, r->opcode, r->payload, r->len, &out[2], &data_len);
	note_get(link, r, *rc, *rc ? 0 : data_len);
	osc2_test_ctx_leave(ctx);
	if (locked) {
		(void)k_mutex_unlock(&command_lock);
	}
	if (*rc) {
		LOG_DBG("GET 0x%02x failed (%d)", out[0], *rc);
		out[1] = 0;
		return 2;
	}
	out[1] = data_len;
	return 2 + data_len;
}

uint8_t osc2_command_get(enum osc2_link link, uint8_t out[63])
{
	int rc;

	return osc2_command_get_rc(link, out, &rc);
}

int osc2_command_get_strict(enum osc2_link link, uint8_t out[63])
{
	int rc;

	(void)osc2_command_get_rc(link, out, &rc);
	if (rc) {
		return rc < 0 ? rc : -EIO;
	}
	return 63;
}

/* ---- USB: feature report 1 ------------------------------------------------------------------- */

int osc2_usb_feature_set(const uint8_t *data, uint16_t len)
{
	const uint8_t *rec = NULL;

	switch (opensc2_usb_feature_check(data, len, &rec)) {
	case OPENSC2_USB_SET_OK:
		break;
	case OPENSC2_USB_SET_LENGTH:
		osc2_test_count(OSC2_TEST_CNT_USB_FRAME_ERR);
		return -EMSGSIZE;
	case OPENSC2_USB_SET_ID:
		osc2_test_count(OSC2_TEST_CNT_USB_FRAME_ERR);
		return -EBADMSG;
	default:
		osc2_test_count(OSC2_TEST_CNT_USB_FRAME_ERR);
		return -E2BIG;
	}
	return osc2_command_set(OSC2_LINK_USB, rec, OPENSC2_USB_FEATURE_LEN - 1);
}

int osc2_usb_feature_get(uint8_t *out, uint16_t wlength, uint8_t reply_len, uint8_t *opcode,
			 uint8_t *n)
{
	const struct stored_request *r = &stored[OSC2_LINK_USB];
	bool locked = r->opcode != OSC2_TEST_OPCODE;
	uint8_t answer[OPENSC2_ANSWER_MAX] = { 0 };
	uint8_t answer_len = 0;
	uint8_t ctx;
	int rc;

	if (locked) {
		(void)k_mutex_lock(&command_lock, K_FOREVER);
	}
	ctx = osc2_test_ctx_enter(OSC2_LINK_USB);
	rc = run_get(OSC2_LINK_USB, r->opcode, r->payload, r->len, answer, &answer_len);
	note_get(OSC2_LINK_USB, r, rc, rc ? 0 : answer_len);
	osc2_test_ctx_leave(ctx);
	if (locked) {
		(void)k_mutex_unlock(&command_lock);
	}
	*opcode = r->opcode;
	*n = answer_len;
	if (rc) {
		return rc < 0 ? rc : -EIO;
	}
	return (int)opensc2_usb_feature_reply(r->opcode, answer, answer_len, reply_len, wlength,
					      out);
}

/* ---- descriptions for the session log -------------------------------------------------------- */

void osc2_command_describe(const uint8_t *rec, uint8_t rec_len, char *buf, size_t size)
{
	uint8_t opcode, len;
	const uint8_t *p;
	size_t at = 0;

	if (size == 0) {
		return;
	}
	buf[0] = '\0';
	if (rec_len < 2) {
		return;
	}
	opcode = rec[0];
	len = MIN(rec[1], (uint8_t)(rec_len - 2));
	p = &rec[2];

	switch (opcode) {
	case 0x87:
		for (uint8_t i = 0; i + 2 < len && at + 16 < size; i += 3) {
			at += (size_t)snprintf(&buf[at], size - at, "{%u,%d}", p[i],
					       (int16_t)opensc2_get_le16(&p[i + 1]));
		}
		break;
	case 0x89: case 0xAE: case 0xF2: case 0xA1:
		if (len) {
			snprintf(buf, size, "%s %u", opcode == 0xAE ? "tag" : "sel", p[0]);
		}
		break;
	case 0xED: case 0xEE: case 0xEF: case 0xF0: {
		const uint8_t *nul = memchr(p, 0, len);
		size_t name = nul ? (size_t)(nul - p) : len;

		/* Key names only, never the values (bonds identify a pairing). */
		for (size_t i = 0; i < name && at + 2 < size && i < 32; i++) {
			buf[at++] = (p[i] >= 0x20 && p[i] < 0x7F) ? (char)p[i] : '?';
		}
		buf[at] = '\0';
		if (opcode == 0xEE && nul) {
			snprintf(&buf[at], size - at, " +%u bytes", (unsigned int)(len - name - 1));
		}
		break;
	}
	case 0x95: case 0x86: case 0xFE:
		snprintf(buf, size, "key %08X",
			 (unsigned int)get_u32(p, MIN((size_t)rec_len - 2, (size_t)4)));
		break;
	case 0xDC:
		if (len >= 2) {
			snprintf(buf, size, "type %u value %u", p[0], p[1]);
		}
		break;
	case 0xE2:
		if (len >= 2) {
			snprintf(buf, size, "side %u mask 0x%02X", p[0], p[1]);
		}
		break;
	case 0xC0: case 0xC3: case 0xD8:
		if (len > (opcode == 0xD8 ? 1 : 0)) {
			snprintf(buf, size, "step %u", p[opcode == 0xD8 ? 1 : 0]);
		}
		break;
	case 0xC5:
		if (len >= 4) {
			snprintf(buf, size, "rgbw %u %u %u %u", p[0], p[1], p[2], p[3]);
		}
		break;
	default:
		break;
	}
}
