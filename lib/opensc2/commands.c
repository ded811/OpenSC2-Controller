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
 *
 * As on the real controller (the protocol documentation):
 *   - a request is {opcode, length, payload}; its payload is read from a 61-byte buffer, zero
 *     padded after what the link carried, so a command reads its bytes whatever the length says;
 *   - a successful SET copies its length + 2 bytes over the link's stored request;
 *   - a GET writes the stored request's length + 2 bytes over the start of its reply buffer, then
 *     the answer over them: the bytes after the answer are the request's, then the buffer's (on
 *     USB the rest of the last SET_REPORT the board received, as the USB stack's buffer is
 *     reused; over Bluetooth the rest of the value last written; zeros on the other links). A
 *     command reads its parameters there, so 0xED's name
 *     sent without its NUL runs on into those bytes;
 *   - an opcode nobody knows is -EINVAL (a STALL on USB), a GET of a command that only acts
 *     -EOPNOTSUPP, the errors a handler gives otherwise its own.
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
#include "cmd_deferred.h"
#include "dock_link.h"
#include "imu_rules.h"
#include "key_store.h"
#include "proto_rules.h"
#include "system.h"
#include "haptics_glue.h"
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
#define CMD_LEN                63            /* {opcode, length, payload[61]} */

/* The request the last successful SET left on a link, which selects what its next GET answers:
 * {opcode, length, payload}; only its first length + 2 bytes count. */
static uint8_t stored[OSC2_LINK_COUNT][CMD_LEN];

/* USB: the data of the last SET_REPORT(Feature) the board received, which a GET's reply shows
 * after the stored request (the USB stack hands the GET the buffer it used). */
static uint8_t usb_last_set[OPENSC2_USB_FEATURE_LEN];

/* One command at a time, whichever link it came on: the dock runs commands in a thread of its own
 * beside the radio's (or USB's), and the handlers share the settings, the bonds and the identity.
 * The real controller has no such lock; nothing it does can be told apart from the outside. The
 * test channel's own requests (opcode 0x7E) run outside it: they take the hooks' request lock
 * first and may then run a command (HOST_SET, the dock frame op), so this lock always comes
 * second. A mutex, so a command that calls back into this file from the same thread is fine. */
static K_MUTEX_DEFINE(command_lock);

void osc2_commands_reset(enum osc2_link link)
{
	memset(stored[link], 0, sizeof(stored[link]));
	stored[link][0] = 0x83;
	if (link == OSC2_LINK_USB) {
		memset(usb_last_set, 0, sizeof(usb_last_set));
	}
}

/* A little-endian u32 from the first bytes of `p` (`avail` of them exist), zero padded. */
static uint32_t get_u32(const uint8_t *p, size_t avail)
{
	uint8_t b[4] = { 0 };

	memcpy(b, p, MIN(avail, sizeof(b)));
	return opensc2_get_le32(b);
}

/* ---- settings by name (0xED, 0xEE, 0xEF, 0xF0) -----------------------------------------------
 * The real controller hands the name to Zephyr's settings handlers: a name no handler has is
 * -EINVAL, a handler without a read function cannot be read (0xED length 0) nor saved (-ENOTSUP),
 * a read function with nothing valid to give fails the save with -ENODEV, a value of the wrong
 * size is -EINVAL. Here a name goes, in this order, to the library's keys (key_rules.c: every key
 * of the real controller, and OpenSC2's identity keys), the calibration store, the application's
 * vendor_command, and any other Zephyr settings handler of the build (Bluetooth's "bt" keys, the
 * application's own handlers; never the library's "opensc2" subtree).
 */
enum named_key {
	KEY_NONE = OSC2_KEY_UNKNOWN,
	KEY_BOND_1 = OSC2_KEY_BOND,           /* esb/bond */
	KEY_BOND_2 = OSC2_KEY_BOND_2,         /* esb/bond_2 */
	KEY_TRANSPORT = OSC2_KEY_TRANSPORT,   /* user/wireless_transport */
	KEY_CHIME = OSC2_KEY_CHIME,           /* user/haptic_boot_level */
	KEY_ID_UNIT = OSC2_KEY_ID_UNIT,       /* opensc2/unit_serial */
	KEY_ID_BOARD = OSC2_KEY_ID_BOARD,     /* opensc2/board_serial */
	KEY_ID_GIT = OSC2_KEY_ID_GIT,         /* opensc2/git_revision */
	KEY_ID_REV = OSC2_KEY_ID_REV,         /* opensc2/board_revision, u32 */
};

/* The length of the name a by-name payload starts with: up to its NUL, at most `max`. */
static size_t name_len_of(const uint8_t *payload, size_t max)
{
	const uint8_t *nul = memchr(payload, 0, max);

	return nul ? (size_t)(nul - payload) : max;
}

/* The library's key named by the `name_len` bytes at `name` (a NUL follows them), or NULL (the
 * calibration store's, the application's or another handler's, or no key). */
static const struct osc2_key_info *find_key(const uint8_t *name, size_t name_len)
{
	const struct osc2_key_info *k = osc2_key_lookup(name, name_len + 1, NULL);

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

	if (len == 0) {
		return -EINVAL;   /* no value: refused before any key hears of it */
	}
	if (info->kind == OSC2_KEYK_BLOB) {
		int err = osc2_keys_set(info->id, value, len);

		/* The haptics keys reach the channels at once, as the real controller's handlers
		 * do, past settings 75 and 76 (0x89 still reads the old numbers). */
		if (err == 0 && (info->id == OSC2_KEY_HAPTIC_GAIN || info->id == OSC2_KEY_HAPTIC_AMP)) {
			err = osc2_haptics_key(info->id == OSC2_KEY_HAPTIC_GAIN ? 0 : 1,
					       (int16_t)opensc2_get_le16(value));
		}
		return err;
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
		/* Exactly the key named, as sent: a revision of 0 is stored. */
		return osc2_identity_store_fields(field, &id);
	}
#endif
	default:
		return -EINVAL;
	}
}

/* 0xEF: write the RAM value to flash, as the real controller saves what the key's read function
 * gives: -ENOTSUP for a key without one (the settings/haptics keys), -ENODEV when it has nothing
 * valid to give (an empty bond, an invalid charge window); nothing is written then. */
static int key_save(const struct osc2_key_info *info)
{
	enum named_key key = (enum named_key)info->id;

	if (info->kind == OSC2_KEYK_BLOB) {
		uint8_t value[OSC2_KEY_VALUE_MAX];
		uint8_t len = 0;

		if (!(info->flags & OSC2_KEYF_READ)) {
			return -ENOTSUP;
		}
		if (osc2_keys_get(info->id, value, &len) != 0 ||
		    !osc2_key_value_readable(info->id, value, len)) {
			return -ENODEV;
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

/* 0xF0: delete from flash and, once that worked, give the RAM value what its handler gives a
 * deleted key: an empty bond (the radio hears it), the main puck (2), chime level 2, a kept key's
 * default. */
static int key_delete(const struct osc2_key_info *info)
{
	enum named_key key = (enum named_key)info->id;
	int err;

	if (info->kind == OSC2_KEYK_BLOB) {
		return osc2_keys_delete_cmd(info->id);
	}
	switch (key) {
	case KEY_BOND_1:
	case KEY_BOND_2: {
		k_spinlock_key_t lk;

		err = osc2_storage_delete_bond(bond_slot_of(key));
		if (err) {
			return err;
		}
		lk = k_spin_lock(&osc2.lock);
		memset(&osc2.bonds[bond_slot_of(key)], 0, sizeof(osc2.bonds[0]));
		k_spin_unlock(&osc2.lock, lk);
		osc2_radio_post(OSC2_EV_BOND);
		return 0;
	}
	case KEY_TRANSPORT:
		/* The key back at 2; the running link carries on. */
		err = osc2_storage_delete_transport();
		if (err == 0) {
			osc2.transport = OSC2_TRANSPORT_DEFAULT;
		}
		return err;
	case KEY_CHIME:
		err = osc2_storage_store_chime(2);
		if (err == 0) {
			osc2.chime = 2;
		}
		return err;
	default:
		return 0;
	}
}

/* 0xED: the key's RAM value into `out`; length 0 for an empty or invalid bond and for a key the
 * real controller has no read function for (the haptics settings). A blob reads its whole value,
 * zeros when nothing was set; the charge window only while it is valid (key_rules.h). */
static void key_read(const struct osc2_key_info *info, uint8_t *out, uint8_t *out_len)
{
	enum named_key key = (enum named_key)info->id;
	k_spinlock_key_t lk;
	struct opensc2_bond bond;

	*out_len = 0;
	if (!(info->flags & OSC2_KEYF_READ)) {
		return;
	}
	if (info->kind == OSC2_KEYK_BLOB) {
		uint8_t value[OSC2_KEY_VALUE_MAX];
		uint8_t len = 0;

		if (osc2_keys_get(info->id, value, &len) == 0 &&
		    osc2_key_value_readable(info->id, value, len)) {
			memcpy(out, value, len);
			*out_len = len;
		}
		return;
	}
	switch (key) {
	case KEY_BOND_1:
	case KEY_BOND_2:
		lk = k_spin_lock(&osc2.lock);
		bond = osc2.bonds[bond_slot_of(key)];
		k_spin_unlock(&osc2.lock, lk);
		if (opensc2_bond_valid(&bond)) {
			opensc2_bond_pack(&bond, out);
			*out_len = OPENSC2_BOND_LEN;
		}
		return;
	case KEY_TRANSPORT:
		out[0] = osc2.transport;
		*out_len = 1;
		return;
	case KEY_CHIME:
		out[0] = osc2.chime;
		*out_len = 1;
		return;
	default:
		return;
	}
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

/* An opcode nobody knows, once the application said -ENOTSUP (or has no callback): -EINVAL, the
 * real controller's answer. */
static int unknown_opcode(int rc)
{
	return rc == -ENOTSUP ? -EINVAL : rc;
}

/* 0xEE, 0xEF and 0xF0 for a name that is none of the library's keys: the calibration store's,
 * then the application's, then another settings handler's. `payload` holds the name, its NUL
 * and, for 0xEE, the value (`len` bytes in all); the caller has checked the form. */
static int named_other(uint8_t opcode, const uint8_t *payload, uint8_t len, size_t name_len)
{
	char name[OPENSC2_ANSWER_MAX + 1];
	uint8_t answer[OPENSC2_ANSWER_MAX];
	uint8_t answer_len;
	int rc = osc2_imu_by_name(opcode, payload, len, &answer_len);   /* the IMU's (set only)? */

	if (rc != -ENOENT) {
		return rc;
	}
	rc = osc2_cal_by_name(opcode, payload, len);   /* a cal/ key? */
	if (rc != -ENOENT) {
		return rc;
	}
	rc = app_command(opcode, payload, len, false, answer, &answer_len);
	if (rc != -ENOTSUP) {
		return rc;
	}
	memcpy(name, payload, name_len);
	name[name_len] = '\0';
	if (opcode == 0xEE) {
		return osc2_storage_named_set(name, payload + name_len + 1, len - name_len - 1);
	}
	if (opcode == 0xEF) {
		int n = osc2_storage_named_get(name, answer, sizeof(answer));

		return n > 0 ? osc2_storage_named_save(name, answer, (size_t)n) : n;
	}
	return osc2_storage_named_delete(name);
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
		osc2_haptics_setting(number);   /* 75, 76: to the haptic channels */
		/* After any lizard mode reports the write caused. */
		osc2_test_note_setting(number, v);
		if (osc2.cb && osc2.cb->setting_changed) {
			osc2.cb->setting_changed(number, v, osc2.user_data);
		}
	}
}

/* 0x87: length / 3 triplets {number, s16 value}; bytes after the last whole triplet are ignored. */
static int set_settings(const uint8_t *payload, uint8_t len)
{
	bool led = false;

	for (uint8_t i = 0; i + 3 <= len; i += 3) {
		uint8_t number = payload[i];

		int16_t value = (int16_t)opensc2_get_le16(&payload[i + 1]);
		int rc;

		if (number >= OPENSC2_SETTINGS_COUNT) {
			LOG_DBG("Setting %u does not exist", number);
			continue;
		}
		/* 48, 84 and 85 go to the IMU at every write (imu.c); the sensor's error for 85 ends
		 * the command, as on the real controller */
		rc = osc2_imu_setting(number, value);
		if (rc) {
			if (led) {
				osc2_notify_led();   /* the LED settings before it took effect */
			}
			return rc;
		}
		write_setting(number, value, &led);
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
	/* Every accepted 0x81, as the real controller logs it. */
	LOG_INF("Host turned the mouse and keyboard mappings off");
	osc2.settings[OPENSC2_SETTING_LIZARD_MODE] = 0;
	if (old != 0) {
		/* The switches down and setting 9 off in one go: the real controller's 0x81 lowers
		 * the switches first, and the keyboard takes both up together. */
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

	/* As sent: on the real controller a channel above 100 % is brighter, up to full duty. */
	osc2.led.red = rgbw[0];
	osc2.led.green = rgbw[1];
	osc2.led.blue = rgbw[2];
	osc2.led.white = rgbw[3];
	k_spin_unlock(&osc2.lock, lk);
	osc2_notify_led();
}

/* ---- SET ------------------------------------------------------------------------------------- */

/* `payload` has OPENSC2_ANSWER_MAX bytes (the request's `len`, then zeros or what the link carried
 * after it): handlers read their bytes whatever the length says, as the real controller's do. */
static int run_set(enum osc2_link link, uint8_t opcode, const uint8_t *payload, uint8_t len)
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

	case 0xC5:   /* ID_SET_LED_COLOR: four percentages R, G, B, W, as sent */
		osc2_led_color_write(payload);
		return 0;
	case 0xDC:   /* ID_SET_USER_STORE: type 1 is the chime level, saved, then in use */
		if (payload[0] == 1 && osc2_storage_store_chime(payload[1]) == 0) {
			osc2.chime = payload[1];
		}
		return 0;   /* other types, and a save that failed, are acknowledged */
	case 0xE2: { /* persist the calibration of side 0 or 1 */
		uint8_t answer[OPENSC2_ANSWER_MAX];
		uint8_t answer_len;
		int rc = osc2_cal_command(opcode, payload, len, OPENSC2_ANSWER_MAX);   /* cal/ keys */

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
		int rc = osc2_cal_command(opcode, payload, len, OPENSC2_ANSWER_MAX);

		return rc != -ENOTSUP ? rc :
		       unknown_opcode(app_command(opcode, payload, len, false, answer,
						  &answer_len));
	}

	case 0xEE: { /* set a setting by name, in RAM: key NUL value */
		size_t name_len = name_len_of(payload, len);
		const struct osc2_key_info *key;

		if (name_len == 0 || name_len + 1 >= len) {
			return -EINVAL;   /* no name, or no value */
		}
		key = find_key(payload, name_len);
		return key ? key_set(key, payload + name_len + 1, (uint8_t)(len - name_len - 1)) :
			     named_other(opcode, payload, len, name_len);
	}
	case 0xEF:   /* save it: key NUL, the NUL the last byte */
	case 0xF0: { /* delete it */
		size_t name_len = name_len_of(payload, len);
		const struct osc2_key_info *key;

		if (len == 0 || name_len != (size_t)len - 1) {
			return -EINVAL;
		}
		key = find_key(payload, name_len);
		if (!key) {
			return named_other(opcode, payload, len, name_len);
		}
		return opcode == 0xEF ? key_save(key) : key_delete(key);
	}
	case 0xA2: { /* the main puck's bond, stored for the next boot */
		struct opensc2_bond bond = { 0 };

		if (len != OPENSC2_BOND_LEN) {
			return -EINVAL;
		}
		opensc2_bond_unpack(payload, &bond);
		osc2_cmd_bond_write(&bond);
		return 0;
	}

	case 0x86:   /* factory reset: only with its key, else acknowledged and ignored */
		if (opensc2_get_le32(payload) == KEY_FACTORY_RESET) {
			osc2_cmd_factory_reset();
		} else {
			LOG_INF("Ignoring a factory reset without its key");
		}
		return 0;
	case 0x90:   /* ID_REBOOT_INTO_ISP */
		osc2_cmd_power(OPENSC2_POWER_BOOTLOADER, "host: bootloader");
		return 0;
	case 0x95: { /* ID_FIRMWARE_UPDATE_REBOOT: the key is the first four payload bytes */
		uint32_t k = opensc2_get_le32(payload);

		if (k == KEY_BOOTLOADER) {
			osc2_cmd_power(OPENSC2_POWER_BOOTLOADER, "host: bootloader");
		} else if (k == KEY_REBOOT_KEEP_LINK) {
			osc2_cmd_power(OPENSC2_POWER_REBOOT_KEEP_LINK, "host: reboot into wireless");
		} else if (k == KEY_TURN_OFF) {
			osc2_cmd_power(OPENSC2_POWER_TURN_OFF, "host: turn off");
		} else {
			osc2_cmd_power(OPENSC2_POWER_REBOOT, "host: reboot");
		}
		return 0;
	}
	case 0x9F:   /* ID_TURN_OFF (a wired controller ignores it) */
		osc2_cmd_power(OPENSC2_POWER_TURN_OFF, "host: turn off");
		return 0;

	default: {
		uint8_t answer[OPENSC2_ANSWER_MAX];
		uint8_t answer_len;

		/* The dock never carries the test channel: a 0x7E there is an opcode nobody knows,
		 * and it never reaches the application, which a 0x7E would do outside the command
		 * lock (osc2_command_set()). */
		if (opcode == OSC2_TEST_OPCODE && link == OSC2_LINK_DOCK) {
			return -EINVAL;
		}
		/* The hardware test channel (CONFIG_OPENSC2_TEST_HOOKS) claims a keyed 0x7E. */
		if (osc2_test_claim(link, opcode, payload, len)) {
			return 0;
		}
		return unknown_opcode(app_command(opcode, payload, len, false, answer,
						  &answer_len));
	}
	}
}

/* The test hooks' record of a SET (never of the test channel itself, which would flood it). */
static void note_set(enum osc2_link link, const uint8_t *rec, size_t avail, int rc)
{
	if (rec[0] == OSC2_TEST_OPCODE) {
		return;
	}
	osc2_test_count(rc == 0 ? OSC2_TEST_CNT_VENDOR_SET_OK : OSC2_TEST_CNT_VENDOR_SET_ERR);
	osc2_test_note_vendor(link, false, rec[0], rec[1], rc, &rec[2], rec[1], avail);
}

int32_t osc2_command_set(enum osc2_link link, const uint8_t *rec, uint8_t rec_len)
{
	uint8_t cmd[CMD_LEN] = { 0 };
	size_t avail = rec_len >= 2 ? (size_t)rec_len - 2 : 0;
	bool locked;
	uint8_t ctx;
	int rc;

	/* What the link carried, zero padded to a whole request. */
	memcpy(cmd, rec, MIN((size_t)rec_len, sizeof(cmd)));
	if (cmd[1] > OPENSC2_ANSWER_MAX) {
		note_set(link, cmd, avail, -EINVAL);
		return -EINVAL;   /* longer than the stored request can hold */
	}
	locked = cmd[0] != OSC2_TEST_OPCODE;   /* the test channel locks on its own (above) */
	if (locked) {
		(void)k_mutex_lock(&command_lock, K_FOREVER);
	}
	ctx = osc2_test_ctx_enter(link);
	rc = run_set(link, cmd[0], &cmd[2], cmd[1]);
	LOG_DBG("SET 0x%02x (%u bytes): %d", cmd[0], cmd[1], rc);
	if (rc == 0) {
		memcpy(stored[link], cmd, (size_t)cmd[1] + 2);
		osc2_test_stored(link);
	}
	note_set(link, cmd, avail, rc);
	osc2_test_ctx_leave(ctx);
	if (locked) {
		(void)k_mutex_unlock(&command_lock);
	}
	return rc;
}

uint8_t osc2_command_stored(enum osc2_link link, uint8_t rec[2 + OPENSC2_ANSWER_MAX])
{
	uint8_t n = (uint8_t)(stored[link][1] + 2);

	memcpy(rec, stored[link], n);
	return n;
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

/* 0xED: `data` holds the stored request's payload, the name; its value is written over it. A name
 * without a NUL in the 61 bytes, or one nobody has, answers length 0; an empty name is refused. */
static int read_by_name(enum osc2_link link, uint8_t *data, uint8_t *n)
{
	uint8_t name[OPENSC2_ANSWER_MAX + 1];
	size_t name_len = name_len_of(data, OPENSC2_ANSWER_MAX);
	const struct osc2_key_info *key;

	ARG_UNUSED(link);
	*n = 0;
	if (name_len == 0) {
		return -EINVAL;
	}
	if (name_len == OPENSC2_ANSWER_MAX) {
		return 0;
	}
	memcpy(name, data, name_len + 1);
	key = find_key(name, name_len);
	if (key) {
		key_read(key, data, n);
		return 0;
	}
	if (osc2_imu_by_name(0xED, name, (uint8_t)(name_len + 1), n) == 0) {
		return 0;   /* the IMU's keys are set only: length 0 */
	}
	if (osc2_cal_read_by_name(name, (uint8_t)(name_len + 1), data, n) == 0) {
		return 0;
	}
	if (app_command(0xED, name, (uint8_t)(name_len + 1), true, data, n) == 0) {
		return 0;
	}
	*n = 0;
	{
		int got = osc2_storage_named_get((const char *)name, data, OPENSC2_ANSWER_MAX);

		*n = got > 0 ? (uint8_t)MIN(got, OPENSC2_ANSWER_MAX) : 0;
	}
	return 0;
}

/* Answer the GET of `opcode` in place: `data` (OPENSC2_ANSWER_MAX bytes) holds the stored
 * request's payload and gets the answer, `*n` its length. Only the answer's bytes are written. */
static int run_get(enum osc2_link link, uint8_t opcode, uint8_t len, uint8_t *data, uint8_t *n)
{
	struct opensc2_identity id;
	uint8_t sel = data[0];   /* read whatever the request's length was */

	*n = 0;
	opensc2_identity_get(&id);
	switch (opcode) {
	case 0x83:   /* ID_GET_ATTRIBUTES_VALUES: attribute 11 is the current link's interval */
		*n = (uint8_t)opensc2_encode_attributes(&id, link_interval_us(), data);
		return 0;
	case 0xAE:   /* ID_GET_STRING_ATTRIBUTE: the tag, then 20 characters, NUL padded */
		*n = (uint8_t)opensc2_encode_string_attribute(&id, sel, data);
		data[20] = 0;   /* the twentieth character's place (no string is that long) */
		return 0;
	case 0xF2:   /* device info: the selector, then its blob */
		if (sel == 0) {
			/* build time, hardware ID, git revision (16), unit serial (16) */
			opensc2_put_le32(&data[1], id.build_time);
			opensc2_put_le32(&data[5], id.board_revision);
			memset(&data[9], 0, 32);
			memcpy(&data[9], id.git_revision, strlen(id.git_revision));
			memcpy(&data[25], id.unit_serial, OPENSC2_SERIAL_LEN);
			*n = 41;
		} else if (sel == 1) {
			/* the crash record as it is (zeros without one); a new one is then marked seen */
			*n = osc2_crash_read(data);
		} else if (sel == 2) {
			/* uptime in ms, the last radio disconnect reason (u32) */
			opensc2_put_le32(&data[1], (uint32_t)k_uptime_get());
			opensc2_put_le32(&data[5], osc2.last_disconnect_reason);
			*n = 9;
		}
		return 0;
	case 0xA1: { /* device info: the reset causes and the chip ID for selector 1, else zeros */
		uint32_t cause = 0;

		memset(&data[1], 0, 17);
		if (sel != 1) {
			data[0] = 0;
			*n = 18;
			return 0;
		}
#if defined(CONFIG_HWINFO)
		(void)hwinfo_get_device_id(&data[2], 16);
		/* never cleared, so the bits of several resets add up, as on the real one; a cause
		 * that cannot be read refuses the request */
		{
			int err = hwinfo_get_reset_cause(&cause);

			if (err == -ENOSYS) {
				cause = 0;   /* no reset causes on this chip: none to report */
			} else if (err) {
				return err;
			}
		}
#endif
		data[1] = osc2_reset_flags(cause);
		*n = 18;
		return 0;
	}
	case 0xBE: { /* ID_GET_BATTERY_DATA */
		struct opensc2_battery b;
		k_spinlock_key_t lk = k_spin_lock(&osc2.lock);

		b = osc2.battery;
		k_spin_unlock(&osc2.lock, lk);
		osc2_test_battery(&b);
		*n = (uint8_t)opensc2_encode_battery_data(&b, data);
		return 0;
	}
	case 0xE9: { /* ID_GET_LED_COLOR: as the real controller reads it back (proto_rules.h) */
		struct opensc2_led led;

		opensc2_led_get(&led);
		data[0] = osc2_led_readback(led.red);
		data[1] = osc2_led_readback(led.green);
		data[2] = osc2_led_readback(led.blue);
		data[3] = osc2_led_readback(led.white);
		*n = 4;
		return 0;
	}
	case 0xDB:   /* ID_GET_USER_STORE */
		data[0] = 1;
		data[1] = osc2.chime;
		*n = 2;
		return 0;
	case 0x89:   /* ID_GET_SETTINGS_VALUES */
		/* As the real controller answers: a number above 85 reads 0, and 48, 84 and 85 their
		 * defaults (its IMU keeps those) */
		opensc2_put_le16(&data[1], sel < OPENSC2_SETTINGS_COUNT ?
				 (uint16_t)osc2_setting_answer(sel, osc2.settings[sel]) : 0);
		*n = 3;
		return 0;
	case 0xED:   /* read a setting by name */
		return read_by_name(link, data, n);
	/* Commands that only act: nothing to answer. */
	case 0x81: case 0x85: case 0x86: case 0x87: case 0x8E: case 0x90: case 0x95: case 0x9F:
	case 0xA2: case 0xC0: case 0xC3: case 0xC5: case 0xD8: case 0xDC: case 0xE2: case 0xEE:
	case 0xEF: case 0xF0: case 0xFE:
		return -EOPNOTSUPP;
	default: {
		uint8_t payload[OPENSC2_ANSWER_MAX];

		/* The answer the hardware test channel holds for this link's claimed 0x7E. */
		if (opcode == OSC2_TEST_OPCODE && osc2_test_answer(link, data, n)) {
			return 0;
		}
		memcpy(payload, data, sizeof(payload));
		return unknown_opcode(app_command(opcode, payload, len, true, data, n));
	}
	}
}

/* The test hooks' record of a GET (not of the test channel's). */
static void note_get(enum osc2_link link, const uint8_t *req, int rc, uint8_t n)
{
	if (req[0] == OSC2_TEST_OPCODE) {
		return;
	}
	osc2_test_count(rc == 0 ? OSC2_TEST_CNT_VENDOR_GET_OK : OSC2_TEST_CNT_VENDOR_GET_ERR);
	osc2_test_note_vendor(link, true, req[0], n, rc, &req[2], req[1], req[1]);
}

/* The GET of `link` into `reply` (CMD_LEN bytes, holding what the link's buffer held): the stored
 * request copied over its start, then the answer over that. 0 or the command's error. */
static int get_into(enum osc2_link link, uint8_t reply[CMD_LEN])
{
	uint8_t req[CMD_LEN];
	bool locked = stored[link][0] != OSC2_TEST_OPCODE;   /* the test channel's held answer */
	uint8_t n = 0;
	uint8_t ctx;
	int rc;

	if (locked) {
		(void)k_mutex_lock(&command_lock, K_FOREVER);
	}
	ctx = osc2_test_ctx_enter(link);
	memcpy(req, stored[link], sizeof(req));
	memcpy(reply, req, (size_t)req[1] + 2);
	rc = run_get(link, req[0], req[1], &reply[2], &n);
	note_get(link, req, rc, rc ? 0 : n);
	osc2_test_ctx_leave(ctx);
	if (locked) {
		(void)k_mutex_unlock(&command_lock);
	}
	reply[1] = rc ? 0 : n;
	return rc;
}

uint8_t osc2_command_get_rc(enum osc2_link link, uint8_t out[63], int *rc)
{
	memset(out, 0, CMD_LEN);
	*rc = get_into(link, out);
	if (*rc) {
		LOG_DBG("GET 0x%02x failed (%d)", out[0], *rc);
		return 2;
	}
	return (uint8_t)(2 + out[1]);
}

int osc2_command_get_in_place(enum osc2_link link, uint8_t out[63])
{
	uint8_t req[CMD_LEN];
	uint8_t before[CMD_LEN];
	bool locked = stored[link][0] != OSC2_TEST_OPCODE;   /* the test channel's held answer */
	uint8_t n = 0;
	uint8_t ctx;
	int rc;

	if (locked) {
		(void)k_mutex_lock(&command_lock, K_FOREVER);
	}
	ctx = osc2_test_ctx_enter(link);
	memcpy(req, stored[link], sizeof(req));
	memcpy(before, out, sizeof(before));
	/* The stored request over the previous answer, then the command's answer over that. */
	memcpy(out, req, (size_t)req[1] + 2);
	rc = run_get(link, req[0], req[1], &out[2], &n);
	note_get(link, req, rc, rc ? 0 : n);
	osc2_test_ctx_leave(ctx);
	if (locked) {
		(void)k_mutex_unlock(&command_lock);
	}
	if (rc == -EOPNOTSUPP || rc == -ENOTSUP) {
		memcpy(out, before, sizeof(before));   /* nothing to read: nothing was copied */
	} else if (rc == 0) {
		out[1] = n;
	}
	return rc;
}

uint8_t osc2_command_get(enum osc2_link link, uint8_t out[63])
{
	int rc;

	return osc2_command_get_rc(link, out, &rc);
}

int osc2_command_get_strict(enum osc2_link link, uint8_t out[63])
{
	/* `out` holds the link's buffer as it is (Bluetooth: the value last written), so the bytes
	 * after the request and its answer are that buffer's, as on the real controller. */
	int rc = get_into(link, out);

	if (rc) {
		LOG_DBG("GET 0x%02x failed (%d)", out[0], rc);
		return rc < 0 ? rc : -EIO;
	}
	return 63;
}

/* ---- USB: feature report 1 ------------------------------------------------------------------- */

int osc2_usb_feature_set(const uint8_t *data, uint16_t len)
{
	const uint8_t *rec = NULL;

	/* The stack's buffer as the next GET finds it. */
	memset(usb_last_set, 0, sizeof(usb_last_set));
	memcpy(usb_last_set, data, MIN((size_t)len, sizeof(usb_last_set)));
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
	uint8_t reply[OPENSC2_USB_FEATURE_LEN];
	size_t len;
	int rc;

	/* The report ID, then the request and its answer over what the last SET left. */
	memcpy(reply, usb_last_set, sizeof(reply));
	reply[0] = OPENSC2_USB_FEATURE_ID;
	rc = get_into(OSC2_LINK_USB, &reply[1]);
	*opcode = reply[1];
	*n = reply[2];
	if (rc) {
		return rc < 0 ? rc : -EIO;
	}
	/* The test channel (OpenSC2's own opcode, answered only while the hooks are armed) always
	 * gets the whole report: its answers may use all 61 bytes, which the October form's 63-byte
	 * replies would cut. */
	if (*opcode == OSC2_TEST_OPCODE) {
		reply_len = OPENSC2_USB_FEATURE_LEN;
	}
	len = MIN(MIN((size_t)reply_len, (size_t)wlength), sizeof(reply));
	memcpy(out, reply, len);
	return (int)len;
}

/* ---- the same changes from the board (opensc2.h) -------------------------------------------- */

int opensc2_setting_set(uint8_t number, int16_t value)
{
	if (!osc2.initialised) {
		return -EACCES;
	}
	if (number >= OPENSC2_SETTINGS_COUNT) {
		return -EINVAL;
	}
	(void)k_mutex_lock(&command_lock, K_FOREVER);
	osc2_settings_write(number, value);
	(void)k_mutex_unlock(&command_lock);
	return 0;
}

int opensc2_led_set_color(uint8_t red, uint8_t green, uint8_t blue, uint8_t white)
{
	const uint8_t rgbw[4] = { red, green, blue, white };

	if (!osc2.initialised) {
		return -EACCES;
	}
	(void)k_mutex_lock(&command_lock, K_FOREVER);
	osc2_led_color_write(rgbw);
	(void)k_mutex_unlock(&command_lock);
	return 0;
}

int opensc2_chime_level_set(uint8_t level)
{
	int err;

	if (!osc2.initialised) {
		return -EACCES;
	}
	(void)k_mutex_lock(&command_lock, K_FOREVER);
	err = osc2_storage_store_chime(level);
	if (err == 0) {
		osc2.chime = level;
	}
	(void)k_mutex_unlock(&command_lock);
	return err;
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
