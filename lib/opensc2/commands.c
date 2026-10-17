/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * commands.c: the vendor command channel ("feature report 1") as it reaches the controller over
 * the puck's radio link: a SET runs a command, a GET answers the command the last SET named
 * (with no SET before it, a GET answers 0x83, the attributes). The commands and their formats
 * are in docs/api.md; anything not handled here goes to the application's vendor_command callback.
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#if defined(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif
#include "internal.h"

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

#define KEY_FACTORY_RESET      0xEFC1A917u   /* 0x86 */
#define KEY_BOOTLOADER         0xECAABAC0u   /* 0x95 */
#define KEY_REBOOT_KEEP_LINK   0xA427AF52u   /* 0x95 */
#define KEY_TURN_OFF           0xABDF9710u   /* 0x95 */
#define POWER_DELAY_MS         20            /* so the answer can still go out */

/* The request the last SET stored, which selects what the next GET answers. */
static struct {
	uint8_t opcode;
	uint8_t len;
	uint8_t payload[OPENSC2_ANSWER_MAX];
} last;

void osc2_commands_reset(void)
{
	memset(&last, 0, sizeof(last));
	last.opcode = 0x83;
}

static uint32_t get_u32(const uint8_t *payload, uint8_t len)
{
	uint8_t b[4] = { 0 };

	memcpy(b, payload, MIN(len, 4));
	return opensc2_get_le32(b);
}

/* ---- settings by name (0xED, 0xEE, 0xEF, 0xF0) ----------------------------------------------- */

/* Keys of the real controller this library has an equivalent for. */
enum named_key {
	KEY_NONE = -1,
	KEY_BOND_1,           /* esb/bond */
	KEY_BOND_2,           /* esb/bond_2 */
	KEY_TRANSPORT,        /* user/wireless_transport: 2 = bond 1, 3 = bond 2 */
	KEY_CHIME,            /* user/haptic_boot_level */
#if defined(CONFIG_OPENSC2_IDENTITY_VENDOR_WRITE)
	KEY_ID_UNIT,          /* opensc2/unit_serial */
	KEY_ID_BOARD,         /* opensc2/board_serial */
	KEY_ID_GIT,           /* opensc2/git_revision */
	KEY_ID_REV,           /* opensc2/board_revision, u32 */
#endif
};

static enum named_key find_key(const uint8_t *payload, uint8_t len, uint8_t *name_len)
{
	static const char *const names[] = {
		"esb/bond", "esb/bond_2", "user/wireless_transport", "user/haptic_boot_level",
#if defined(CONFIG_OPENSC2_IDENTITY_VENDOR_WRITE)
		"opensc2/unit_serial", "opensc2/board_serial", "opensc2/git_revision",
		"opensc2/board_revision",
#endif
	};
	const uint8_t *nul = memchr(payload, 0, len);

	if (!nul) {
		return KEY_NONE;
	}
	*name_len = (uint8_t)(nul - payload) + 1;
	for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
		if (strcmp((const char *)payload, names[i]) == 0) {
			return (enum named_key)i;
		}
	}
	return KEY_NONE;
}

/* Set a key in RAM (0xEE). Bonds wake the radio at once, as on the real controller. */
static int key_set(enum named_key key, const uint8_t *value, uint8_t len)
{
	switch (key) {
	case KEY_BOND_1:
	case KEY_BOND_2: {
		struct opensc2_bond bond = { 0 };
		uint8_t slot = key == KEY_BOND_1 ? 0 : 1;
		k_spinlock_key_t lk;

		if (len != 0 && len != OPENSC2_BOND_LEN) {
			osc2_radio_post(OSC2_EV_BOND);
			return -EINVAL;
		}
		if (len) {
			opensc2_bond_unpack(value, &bond);
		}
		lk = k_spin_lock(&osc2.lock);
		osc2.bonds[slot] = opensc2_bond_valid(&bond) ? bond : (struct opensc2_bond){ 0 };
		k_spin_unlock(&osc2.lock, lk);
		osc2_radio_post(OSC2_EV_BOND);
		return 0;
	}
	case KEY_TRANSPORT:
		if (len == 1 && (value[0] == 2 || value[0] == 3)) {
			osc2.slot = value[0] - 2;
			osc2_radio_post(OSC2_EV_BOND);
		}
		return 0;   /* 0 and 1 select Bluetooth, which this library does not have */
	case KEY_CHIME:
		if (len > 1) {
			return -EINVAL;
		}
		osc2.chime = len ? value[0] : 2;   /* deleting the key brings back the default */
		return 0;
#if defined(CONFIG_OPENSC2_IDENTITY_VENDOR_WRITE)
	case KEY_ID_UNIT:
	case KEY_ID_BOARD:
	case KEY_ID_GIT:
	case KEY_ID_REV: {
		struct opensc2_identity id = { 0 };
		char text[OPENSC2_SERIAL_LEN + 1] = { 0 };

		memcpy(text, value, MIN(len, OPENSC2_SERIAL_LEN));
		if (key == KEY_ID_UNIT) {
			memcpy(id.unit_serial, text, sizeof(id.unit_serial));
		} else if (key == KEY_ID_BOARD) {
			memcpy(id.board_serial, text, sizeof(id.board_serial));
		} else if (key == KEY_ID_GIT) {
			memcpy(id.git_revision, text, sizeof(id.git_revision) - 1);
		} else {
			id.board_revision = get_u32(value, len);
		}
		return opensc2_identity_store(&id);
	}
#endif
	default:
		return -ENOENT;
	}
}

static int key_save(enum named_key key)
{
	switch (key) {
	case KEY_BOND_1:
		return osc2_storage_save_bond(0);
	case KEY_BOND_2:
		return osc2_storage_save_bond(1);
	case KEY_TRANSPORT:
		return osc2_storage_save_slot();
	case KEY_CHIME:
		return osc2_storage_save_chime();
	case KEY_NONE:
		return -ENOENT;
	default:
		return 0;   /* identity keys are stored when they are set */
	}
}

static int key_read(enum named_key key, uint8_t *out, uint8_t *out_len)
{
	k_spinlock_key_t lk;

	switch (key) {
	case KEY_BOND_1:
	case KEY_BOND_2:
		lk = k_spin_lock(&osc2.lock);
		opensc2_bond_pack(&osc2.bonds[key == KEY_BOND_1 ? 0 : 1], out);
		k_spin_unlock(&osc2.lock, lk);
		*out_len = OPENSC2_BOND_LEN;
		return 0;
	case KEY_TRANSPORT:
		out[0] = 2 + osc2.slot;
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

	if (!osc2.cb || !osc2.cb->vendor_command) {
		return -ENOTSUP;
	}
	*answer_len = 0;
	rc = osc2.cb->vendor_command(&cmd, answer, answer_len, osc2.user_data);
	*answer_len = MIN(*answer_len, OPENSC2_ANSWER_MAX);
	return rc;
}

/* ---- SET ------------------------------------------------------------------------------------- */

static int set_settings(const uint8_t *payload, uint8_t len)
{
	bool led = false;

	if (len % 3) {
		return -EINVAL;
	}
	for (uint8_t i = 0; i < len; i += 3) {
		uint8_t number = payload[i];
		int16_t value = (int16_t)opensc2_get_le16(&payload[i + 1]);

		if (number >= OPENSC2_SETTINGS_COUNT) {
			LOG_DBG("Setting %u does not exist", number);
			continue;
		}
		osc2.settings[number] = value;
		if (number == OPENSC2_SETTING_LIZARD_MODE) {
			osc2_watchdog_kick();   /* Steam repeats "lizard off" to show it is there */
		}
		if (number == OPENSC2_SETTING_LED_OVERRIDE || number == OPENSC2_SETTING_LED_BRIGHTNESS) {
			k_spinlock_key_t lk = k_spin_lock(&osc2.lock);

			osc2.led.override = osc2.settings[OPENSC2_SETTING_LED_OVERRIDE] != 0;
			osc2.led.brightness =
				(uint8_t)CLAMP(osc2.settings[OPENSC2_SETTING_LED_BRIGHTNESS], 0, 100);
			k_spin_unlock(&osc2.lock, lk);
			led = true;
		}
		if (osc2.cb && osc2.cb->setting_changed) {
			osc2.cb->setting_changed(number, value, osc2.user_data);
		}
	}
	if (led) {
		osc2_notify_led();
	}
	return 0;
}

static int run_set(uint8_t opcode, const uint8_t *payload, uint8_t len)
{
	switch (opcode) {
	/* Commands that only answer: the SET just selects them for the GET. */
	case 0x83: case 0xAE: case 0xF2: case 0xA1: case 0xBE: case 0xE9: case 0xDB: case 0x89:
	case 0xED:
		return 0;

	case 0x87:   /* ID_SET_SETTINGS_VALUES */
		return set_settings(payload, len);
	case 0x8E:   /* ID_LOAD_DEFAULT_SETTINGS_VALUES */
		osc2_settings_restore_defaults();
		if (osc2.cb && osc2.cb->setting_changed) {
			osc2.cb->setting_changed(OPENSC2_SETTINGS_ALL, 0, osc2.user_data);
		}
		osc2_notify_led();
		return 0;
	case 0x81:   /* ID_CLEAR_DIGITAL_MAPPINGS: lizard mode off */
	case 0x85:   /* ID_SET_DEFAULT_DIGITAL_MAPPINGS: lizard mode on */
		osc2.settings[OPENSC2_SETTING_LIZARD_MODE] = opcode == 0x85;
		osc2_watchdog_kick();
		if (osc2.cb && osc2.cb->setting_changed) {
			osc2.cb->setting_changed(OPENSC2_SETTING_LIZARD_MODE,
						 osc2.settings[OPENSC2_SETTING_LIZARD_MODE],
						 osc2.user_data);
		}
		return 0;

	case 0xC5: { /* ID_SET_LED_COLOR: four percentages R, G, B, W */
		uint8_t c[4] = { 0 };
		k_spinlock_key_t lk;

		memcpy(c, payload, MIN(len, 4));
		lk = k_spin_lock(&osc2.lock);
		osc2.led.red = MIN(c[0], 100);
		osc2.led.green = MIN(c[1], 100);
		osc2.led.blue = MIN(c[2], 100);
		osc2.led.white = MIN(c[3], 100);
		k_spin_unlock(&osc2.lock, lk);
		osc2_notify_led();
		return 0;
	}
	case 0xDC:   /* ID_SET_USER_STORE: type 1 is the chime level */
		if (len >= 2 && payload[0] == 1) {
			osc2.chime = payload[1];
			return osc2_storage_save_chime();
		}
		return -EINVAL;

	case 0xEE:   /* set a setting by name, in RAM */
	case 0xEF:   /* save it */
	case 0xF0: { /* delete it */
		uint8_t name_len = 0;
		enum named_key key = find_key(payload, len, &name_len);
		uint8_t answer[OPENSC2_ANSWER_MAX];
		uint8_t answer_len;
		int rc;

		if (key == KEY_NONE) {
			rc = app_command(opcode, payload, len, false, answer, &answer_len);
			return rc == -ENOTSUP ? -ENOENT : rc;
		}
		if (opcode == 0xEE) {
			return key_set(key, payload + name_len, len - name_len);
		}
		if (opcode == 0xF0) {
			rc = key_set(key, NULL, 0);
			if (rc) {
				return rc;
			}
		}
		return key_save(key);
	}
	case 0xA2: { /* write the bond of the main puck (slot 1) */
		struct opensc2_bond bond = { 0 };

		if (len != OPENSC2_BOND_LEN) {
			return -EINVAL;
		}
		opensc2_bond_unpack(payload, &bond);
		return opensc2_bond_set(0, &bond);
	}

	case 0x86:   /* factory reset */
		if (get_u32(payload, len) != KEY_FACTORY_RESET) {
			LOG_INF("Ignoring a factory reset without its key");
			return 0;
		}
		LOG_INF("Factory reset");
		memset(osc2.bonds, 0, sizeof(osc2.bonds));
		osc2.slot = 0;
		osc2.chime = 2;
		(void)osc2_storage_factory_reset();
		osc2_radio_post(OSC2_EV_BOND);
		osc2_schedule_power(OPENSC2_POWER_FACTORY_RESET, 100);
		return 0;
	case 0x90:   /* ID_REBOOT_INTO_ISP */
		osc2_schedule_power(OPENSC2_POWER_BOOTLOADER, POWER_DELAY_MS);
		return 0;
	case 0x95: { /* ID_FIRMWARE_UPDATE_REBOOT */
		uint32_t k = get_u32(payload, len);

		osc2_schedule_power(k == KEY_BOOTLOADER ? OPENSC2_POWER_BOOTLOADER :
				    k == KEY_REBOOT_KEEP_LINK ? OPENSC2_POWER_REBOOT_KEEP_LINK :
				    k == KEY_TURN_OFF ? OPENSC2_POWER_TURN_OFF :
				    OPENSC2_POWER_REBOOT, POWER_DELAY_MS);
		return 0;
	}
	case 0x9F:   /* ID_TURN_OFF */
		osc2_schedule_power(OPENSC2_POWER_TURN_OFF, POWER_DELAY_MS);
		return 0;

	default: {
		uint8_t answer[OPENSC2_ANSWER_MAX];
		uint8_t answer_len;

		return app_command(opcode, payload, len, false, answer, &answer_len);
	}
	}
}

int32_t osc2_command_set(const uint8_t *rec, uint8_t rec_len)
{
	uint8_t opcode, len;
	int rc;

	if (rec_len < 2) {
		return -EINVAL;
	}
	opcode = rec[0];
	len = rec[1];
	if (len > rec_len - 2 || len > OPENSC2_ANSWER_MAX) {
		return -EINVAL;
	}
	rc = run_set(opcode, &rec[2], len);
	LOG_DBG("SET 0x%02x (%u bytes): %d", opcode, len, rc);
	if (rc == 0) {
		last.opcode = opcode;
		last.len = len;
		memcpy(last.payload, &rec[2], len);
	}
	return rc;
}

/* ---- GET ------------------------------------------------------------------------------------- */

static int run_get(uint8_t opcode, const uint8_t *payload, uint8_t len, uint8_t *out,
		   uint8_t *out_len)
{
	struct opensc2_identity id;
	uint8_t sel = len ? payload[0] : 0;

	opensc2_identity_get(&id);
	switch (opcode) {
	case 0x83:   /* ID_GET_ATTRIBUTES_VALUES */
		*out_len = (uint8_t)opensc2_encode_attributes(&id, OSC2_REPORT_INTERVAL_US, out);
		return 0;
	case 0xAE:   /* ID_GET_STRING_ATTRIBUTE */
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
			*out_len = 34;   /* no crash record */
		} else if (sel == 2) {
			opensc2_put_le32(&out[1], (uint32_t)k_uptime_get());
			out[5] = osc2.last_disconnect_reason;
			*out_len = 6;
		} else {
			*out_len = 0;
		}
		return 0;
	case 0xA1:   /* device info: reset cause and chip ID */
		memset(out, 0, OPENSC2_ANSWER_MAX);
		*out_len = 0;
		if (sel == 1) {
			uint32_t cause = 0;

			out[0] = 1;
#if defined(CONFIG_HWINFO)
			(void)hwinfo_get_reset_cause(&cause);
			(void)hwinfo_get_device_id(&out[5], 8);
#endif
			opensc2_put_le32(&out[1], cause);
			*out_len = 21;
		}
		return 0;
	case 0xBE: { /* ID_GET_BATTERY_DATA */
		struct opensc2_battery b;
		k_spinlock_key_t lk = k_spin_lock(&osc2.lock);

		b = osc2.battery;
		k_spin_unlock(&osc2.lock, lk);
		*out_len = (uint8_t)opensc2_encode_battery_data(&b, out);
		return 0;
	}
	case 0xE9: { /* ID_GET_LED_COLOR */
		struct opensc2_led led;

		opensc2_led_get(&led);
		out[0] = led.red;
		out[1] = led.green;
		out[2] = led.blue;
		out[3] = led.white;
		*out_len = 4;
		return 0;
	}
	case 0xDB:   /* ID_GET_USER_STORE */
		out[0] = 1;
		out[1] = osc2.chime;
		*out_len = 2;
		return 0;
	case 0x89:   /* ID_GET_SETTINGS_VALUES */
		if (sel >= OPENSC2_SETTINGS_COUNT) {
			return -EINVAL;
		}
		out[0] = sel;
		opensc2_put_le16(&out[1], (uint16_t)osc2.settings[sel]);
		*out_len = 3;
		return 0;
	case 0xED: { /* read a setting by name */
		uint8_t name_len;
		enum named_key key = find_key(payload, len, &name_len);

		*out_len = 0;
		if (key != KEY_NONE && key_read(key, out, out_len) == 0) {
			return 0;
		}
		if (app_command(opcode, payload, len, true, out, out_len) != 0) {
			*out_len = 0;   /* unknown key: an empty answer */
		}
		return 0;
	}
	default:
		return app_command(opcode, payload, len, true, out, out_len);
	}
}

uint8_t osc2_command_get(uint8_t opcode, uint8_t out[63])
{
	const uint8_t *payload = NULL;
	uint8_t len = 0, data_len = 0;
	int rc;

	if (opcode == last.opcode) {
		payload = last.payload;
		len = last.len;
	}
	memset(out, 0, 63);
	out[0] = opcode;
	rc = run_get(opcode, payload ? payload : last.payload, len, &out[2], &data_len);
	if (rc) {
		LOG_DBG("GET 0x%02x failed (%d)", opcode, rc);
		out[1] = 0;
		return 2;
	}
	out[1] = data_len;
	return 2 + data_len;
}
