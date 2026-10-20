/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * key_rules.c: the settings keys of 0xED..0xF0 and the blob values (key_rules.h). Pure logic. The
 * keys, their sizes, which ones 0xED reads and which ones the factory reset deletes follow the
 * protocol documentation (not public yet; docs/protocol.md lists the pages).
 */
#include <errno.h>
#include <string.h>
#include "key_rules.h"

#define R OSC2_KEYF_READ
#define F OSC2_KEYF_FACTORY

static const struct osc2_key_info keys[] = {
	{ "esb/bond", OSC2_KEY_BOND, OSC2_KEYK_BOND, 24, R | F, NULL },
	{ "esb/bond_2", OSC2_KEY_BOND_2, OSC2_KEYK_BOND, 24, R | F, NULL },
	{ "user/wireless_transport", OSC2_KEY_TRANSPORT, OSC2_KEYK_TRANSPORT, 1, R | F, NULL },
	{ "user/haptic_boot_level", OSC2_KEY_CHIME, OSC2_KEYK_CHIME, 1, R | F, NULL },
	/* OpenSC2's own: written with CONFIG_OPENSC2_IDENTITY_VENDOR_WRITE, never read by 0xED */
	{ "opensc2/unit_serial", OSC2_KEY_ID_UNIT, OSC2_KEYK_IDENTITY, 0, 0, NULL },
	{ "opensc2/board_serial", OSC2_KEY_ID_BOARD, OSC2_KEYK_IDENTITY, 0, 0, NULL },
	{ "opensc2/git_revision", OSC2_KEY_ID_GIT, OSC2_KEYK_IDENTITY, 0, 0, NULL },
	{ "opensc2/board_revision", OSC2_KEY_ID_REV, OSC2_KEYK_IDENTITY, 0, 0, NULL },
	/* kept by the library: the travel lock, the fully-charged LED level, the factory charge
	 * window, the crash record, the haptics settings */
	{ "user/keylock", OSC2_KEY_KEYLOCK, OSC2_KEYK_BLOB, 4, R | F, "keylock" },
	{ "user/fully_charged_led_intensity", OSC2_KEY_FULL_LED, OSC2_KEYK_BLOB, 1, F, "fcled" },
	{ "mte/charge_level", OSC2_KEY_CHARGE_WINDOW, OSC2_KEYK_BLOB, 8, R | F, "mte" },
	{ "debug/resetreason", OSC2_KEY_RESET_REASON, OSC2_KEYK_BLOB, 33, R | F, "crash" },
	{ "settings/haptics/haptic_master_gain_db", OSC2_KEY_HAPTIC_GAIN, OSC2_KEYK_BLOB, 2, 0,
	  "hgain" },
	{ "settings/haptics/amplifier_mode", OSC2_KEY_HAPTIC_AMP, OSC2_KEYK_BLOB, 2, 0, "hamp" },
	{ "settings/haptics/enabled", OSC2_KEY_HAPTIC_ENABLED, OSC2_KEYK_BLOB, 2, 0, "hen" },
	/* calibration: the factory reset keeps it; 0xED reads all but the gyro bias */
	{ "cal/joy_l", OSC2_KEY_CAL_JOY_L, OSC2_KEYK_CAL, 18, R, NULL },
	{ "cal/joy_r", OSC2_KEY_CAL_JOY_R, OSC2_KEYK_CAL, 18, R, NULL },
	{ "cal/trg_l", OSC2_KEY_CAL_TRG_L, OSC2_KEYK_CAL, 7, R, NULL },
	{ "cal/trg_r", OSC2_KEY_CAL_TRG_R, OSC2_KEYK_CAL, 7, R, NULL },
	{ "cal/prs_l", OSC2_KEY_CAL_PRS_L, OSC2_KEYK_CAL, 8, R, NULL },
	{ "cal/prs_r", OSC2_KEY_CAL_PRS_R, OSC2_KEYK_CAL, 8, R, NULL },
	{ "cal/touch_l", OSC2_KEY_CAL_TOUCH_L, OSC2_KEYK_CAL, 10, R, NULL },
	{ "cal/touch_r", OSC2_KEY_CAL_TOUCH_R, OSC2_KEYK_CAL, 10, R, NULL },
	{ "cal/rgbw_r", OSC2_KEY_CAL_RGBW_R, OSC2_KEYK_CAL, 1, R, NULL },
	{ "cal/rgbw_g", OSC2_KEY_CAL_RGBW_G, OSC2_KEYK_CAL, 1, R, NULL },
	{ "cal/rgbw_b", OSC2_KEY_CAL_RGBW_B, OSC2_KEYK_CAL, 1, R, NULL },
	{ "cal/rgbw_w", OSC2_KEY_CAL_RGBW_W, OSC2_KEYK_CAL, 1, R, NULL },
	{ "cal/volt_offset", OSC2_KEY_CAL_VOLT_OFFSET, OSC2_KEYK_CAL, 2, R, NULL },
	{ "cal/sensors/gyroscope/bias", OSC2_KEY_CAL_GYRO_BIAS, OSC2_KEYK_CAL, 12, 0, NULL },
};

#undef R
#undef F

_Static_assert(sizeof(keys) / sizeof(keys[0]) == OSC2_KEY_COUNT - 1, "one row per key");

const struct osc2_key_info *osc2_key_lookup(const uint8_t *payload, size_t len, uint8_t *name_len)
{
	const uint8_t *nul = len ? memchr(payload, 0, len) : NULL;
	size_t name;

	if (!nul) {
		return NULL;
	}
	name = (size_t)(nul - payload);
	if (name_len) {
		*name_len = (uint8_t)(name + 1);
	}
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		if (strlen(keys[i].name) == name && memcmp(payload, keys[i].name, name) == 0) {
			return &keys[i];
		}
	}
	return NULL;
}

const struct osc2_key_info *osc2_key_by_id(uint8_t id)
{
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		if (keys[i].id == id) {
			return &keys[i];
		}
	}
	return NULL;
}

const struct osc2_key_info *osc2_key_by_store(const char *store)
{
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		if (keys[i].store && strcmp(keys[i].store, store) == 0) {
			return &keys[i];
		}
	}
	return NULL;
}

/* ---- the blob values ---------------------------------------------------------------------- */

/* Where blob `id`'s value starts in the store (-1: no blob). */
static int blob_offset(uint8_t id)
{
	int at = 0;

	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		if (keys[i].kind != OSC2_KEYK_BLOB) {
			continue;
		}
		if (keys[i].id == id) {
			return at;
		}
		at += keys[i].size;
	}
	return -1;
}

int osc2_blob_reset(struct osc2_blob_store *s, uint8_t id)
{
	const struct osc2_key_info *k = osc2_key_by_id(id);
	int at = blob_offset(id);

	if (!k || at < 0 || at + k->size > (int)sizeof(s->value)) {
		return -EINVAL;
	}
	memset(&s->value[at], 0, k->size);
	if (id == OSC2_KEY_FULL_LED) {
		s->value[at] = 3;   /* full brightness, the real controller's value without the key */
	}
	return 0;
}

void osc2_blob_defaults(struct osc2_blob_store *s)
{
	memset(s, 0, sizeof(*s));
	(void)osc2_blob_reset(s, OSC2_KEY_FULL_LED);
}

int osc2_blob_set(struct osc2_blob_store *s, uint8_t id, const uint8_t *value, size_t len)
{
	const struct osc2_key_info *k = osc2_key_by_id(id);
	int at = blob_offset(id);

	if (!k || at < 0 || len != k->size || at + k->size > (int)sizeof(s->value)) {
		return -EINVAL;
	}
	memcpy(&s->value[at], value, len);
	return 0;
}

const uint8_t *osc2_blob_get(const struct osc2_blob_store *s, uint8_t id, uint8_t *len)
{
	const struct osc2_key_info *k = osc2_key_by_id(id);
	int at = blob_offset(id);

	if (!k || at < 0 || at + k->size > (int)sizeof(s->value)) {
		return NULL;
	}
	if (len) {
		*len = k->size;
	}
	return &s->value[at];
}
