/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: the settings keys of 0xED..0xF0 (key_rules.c) and the values the library keeps
 * for them, through the command channel (commands.c with the stand-ins of stubs.c): the table of
 * keys, the blob values, the kept keys' rules, the calibration keys in the calibration store, the
 * factory reset. Uses proto_cases.h's proto_usb().
 */
#include <errno.h>
#include <string.h>
#include "internal.h"
#include "key_rules.h"
#include "keys.h"
#include "stubs.h"

/* `name` with its NUL, then `value`: a 0xED..0xF0 payload; returns its length. */
static uint8_t named(uint8_t *out, const char *name, const void *value, uint8_t len)
{
	size_t n = strlen(name) + 1;

	memcpy(out, name, n);
	if (len) {
		memcpy(&out[n], value, len);
	}
	return (uint8_t)(n + len);
}

/* A SET_REPORT(Feature 1) of a settings-by-name command, no GET: 0 or a negative errno. */
static int named_set(uint8_t opcode, const char *name, const void *value, uint8_t len)
{
	uint8_t frame[64] = { 1, opcode };

	frame[2] = named(&frame[3], name, value, len);
	return osc2_usb_feature_set(frame, sizeof(frame));
}

OPENSC2_TEST(keys_table)
{
	static const struct {
		const char *name;
		uint8_t id, kind, size, flags;
	} want[] = {
		{ "esb/bond", 1, OSC2_KEYK_BOND, 24, OSC2_KEYF_READ | OSC2_KEYF_FACTORY },
		{ "esb/bond_2", 2, OSC2_KEYK_BOND, 24, OSC2_KEYF_READ | OSC2_KEYF_FACTORY },
		{ "user/wireless_transport", 3, OSC2_KEYK_TRANSPORT, 1,
		  OSC2_KEYF_READ | OSC2_KEYF_FACTORY },
		{ "user/haptic_boot_level", 4, OSC2_KEYK_CHIME, 1, OSC2_KEYF_READ | OSC2_KEYF_FACTORY },
		{ "opensc2/unit_serial", 5, OSC2_KEYK_IDENTITY, 0, 0 },
		{ "opensc2/board_revision", 8, OSC2_KEYK_IDENTITY, 0, 0 },
		{ "user/keylock", 9, OSC2_KEYK_BLOB, 4, OSC2_KEYF_READ | OSC2_KEYF_FACTORY },
		{ "user/fully_charged_led_intensity", 10, OSC2_KEYK_BLOB, 1,
		  OSC2_KEYF_READ | OSC2_KEYF_FACTORY },
		{ "mte/charge_level", 11, OSC2_KEYK_BLOB, 8, OSC2_KEYF_READ | OSC2_KEYF_FACTORY },
		{ "debug/resetreason", 12, OSC2_KEYK_BLOB, 33, OSC2_KEYF_READ | OSC2_KEYF_FACTORY },
		{ "settings/haptics/haptic_master_gain_db", 13, OSC2_KEYK_BLOB, 2, 0 },
		{ "settings/haptics/amplifier_mode", 14, OSC2_KEYK_BLOB, 2, 0 },
		{ "settings/haptics/enabled", 15, OSC2_KEYK_BLOB, 2, 0 },
		{ "cal/joy_l", 16, OSC2_KEYK_CAL, 18, OSC2_KEYF_READ },
		{ "cal/trg_r", 19, OSC2_KEYK_CAL, 7, OSC2_KEYF_READ },
		{ "cal/prs_l", 20, OSC2_KEYK_CAL, 8, OSC2_KEYF_READ },
		{ "cal/touch_r", 23, OSC2_KEYK_CAL, 10, OSC2_KEYF_READ },
		{ "cal/rgbw_w", 27, OSC2_KEYK_CAL, 1, OSC2_KEYF_READ },
		{ "cal/volt_offset", 28, OSC2_KEYK_CAL, 2, OSC2_KEYF_READ },
		{ "cal/sensors/gyroscope/bias", 29, OSC2_KEYK_CAL, 12, 0 },
	};
	uint8_t p[61], name_len;

	for (size_t i = 0; i < ARRAY_SIZE(want); i++) {
		uint8_t n = named(p, want[i].name, (const uint8_t[]){ 1, 2 }, 2);
		const struct osc2_key_info *k = osc2_key_lookup(p, n, &name_len);

		CHECK(k != NULL);
		if (!k) {
			continue;
		}
		CHECK_EQ(name_len, strlen(want[i].name) + 1);
		CHECK_EQ(k->id, want[i].id);
		CHECK_EQ(k->kind, want[i].kind);
		CHECK_EQ(k->size, want[i].size);
		CHECK_EQ(k->flags, want[i].flags);
		CHECK(osc2_key_by_id(k->id) == k);
		CHECK((k->store != NULL) == (k->kind == OSC2_KEYK_BLOB));
		if (k->store) {
			CHECK(osc2_key_by_store(k->store) == k);
		}
	}
	/* Every number from 1 to 29 is a key; nothing else is. */
	for (int id = 1; id < OSC2_KEY_COUNT; id++) {
		CHECK(osc2_key_by_id((uint8_t)id) != NULL);
	}
	CHECK(osc2_key_by_id(0) == NULL);
	CHECK(osc2_key_by_id(OSC2_KEY_COUNT) == NULL);
	CHECK(osc2_key_by_store("bond0") == NULL);
	/* No NUL, a prefix, a longer name, a NUL inside: no key. */
	CHECK(osc2_key_lookup((const uint8_t *)"esb/bond", 8, &name_len) == NULL);
	CHECK(osc2_key_lookup(p, named(p, "esb/bon", NULL, 0), &name_len) == NULL);
	CHECK(osc2_key_lookup(p, named(p, "esb/bond_3", NULL, 0), &name_len) == NULL);
	CHECK(osc2_key_lookup(p, named(p, "cal/joy", NULL, 0), &name_len) == NULL);
	CHECK(osc2_key_lookup(p, 0, &name_len) == NULL);
	CHECK(osc2_key_lookup((const uint8_t *)"user\0keylock", 13, &name_len) == NULL);
}

OPENSC2_TEST(keys_blob_store)
{
	static const uint8_t blob_ids[] = { 9, 10, 11, 12, 13, 14, 15 };
	struct osc2_blob_store s;
	uint8_t len, value[OSC2_KEY_VALUE_MAX];
	const uint8_t *v;
	size_t total = 0;

	osc2_blob_defaults(&s);
	/* Defaults: zeros, and 3 (full brightness) for the fully-charged LED level. */
	v = osc2_blob_get(&s, OSC2_KEY_FULL_LED, &len);
	CHECK(v && len == 1 && v[0] == 3);
	v = osc2_blob_get(&s, OSC2_KEY_RESET_REASON, &len);
	CHECK(v && len == 33);
	CHECK(v && memcmp(v, (const uint8_t[33]){ 0 }, 33) == 0);
	/* Each holds exactly its size, without touching the others. */
	for (size_t i = 0; i < ARRAY_SIZE(blob_ids); i++) {
		const struct osc2_key_info *k = osc2_key_by_id(blob_ids[i]);

		memset(value, (int)(0x10 + i), sizeof(value));
		CHECK_EQ(osc2_blob_set(&s, blob_ids[i], value, k->size), 0);
		CHECK_EQ(osc2_blob_set(&s, blob_ids[i], value, (size_t)k->size + 1), -EINVAL);
		CHECK_EQ(osc2_blob_set(&s, blob_ids[i], value, (size_t)k->size - 1), -EINVAL);
		total += k->size;
	}
	CHECK_EQ(total, OSC2_BLOB_SPACE);   /* their sizes add up to the store */
	for (size_t i = 0; i < ARRAY_SIZE(blob_ids); i++) {
		const struct osc2_key_info *k = osc2_key_by_id(blob_ids[i]);

		v = osc2_blob_get(&s, blob_ids[i], &len);
		CHECK(v && len == k->size);
		for (uint8_t j = 0; v && j < len; j++) {
			CHECK_EQ(v[j], 0x10 + i);
		}
	}
	/* Back to the default, one at a time. */
	CHECK_EQ(osc2_blob_reset(&s, OSC2_KEY_FULL_LED), 0);
	v = osc2_blob_get(&s, OSC2_KEY_FULL_LED, &len);
	CHECK(v && v[0] == 3);
	CHECK_EQ(osc2_blob_reset(&s, OSC2_KEY_KEYLOCK), 0);
	v = osc2_blob_get(&s, OSC2_KEY_KEYLOCK, &len);
	CHECK(v && memcmp(v, (const uint8_t[4]){ 0 }, 4) == 0);
	v = osc2_blob_get(&s, OSC2_KEY_CHARGE_WINDOW, &len);
	CHECK(v && v[0] == 0x12);   /* untouched */
	/* Keys that are no blob. */
	CHECK(osc2_blob_get(&s, OSC2_KEY_BOND, &len) == NULL);
	CHECK(osc2_blob_get(&s, OSC2_KEY_CAL_JOY_L, &len) == NULL);
	CHECK_EQ(osc2_blob_set(&s, OSC2_KEY_CHIME, value, 1), -EINVAL);
	CHECK_EQ(osc2_blob_reset(&s, 0), -EINVAL);
}

OPENSC2_TEST(keys_cmd_kept)
{
	const uint8_t lock[4] = { 0xC3, 0x0A, 0xFB, 0x41 };   /* 0x41FB0AC3, the travel lock */
	const uint8_t window[8] = { 0x48, 0x0D, 0, 0, 0x18, 0x10, 0, 0 };
	uint8_t p[61], reply[64], len, v[OSC2_KEY_VALUE_MAX];

	stub_reset();
	/* Readable, nothing set: their whole size, zeros. */
	CHECK_EQ(proto_usb(0xED, p, named(p, "user/keylock", NULL, 0), reply), 4);
	CHECK(memcmp(&reply[3], (const uint8_t[4]){ 0 }, 4) == 0);
	CHECK_EQ(proto_usb(0xED, p, named(p, "mte/charge_level", NULL, 0), reply), 8);
	CHECK_EQ(proto_usb(0xED, p, named(p, "debug/resetreason", NULL, 0), reply), 33);
	CHECK(memcmp(&reply[3], (const uint8_t[33]){ 0 }, 33) == 0);
	/* 0xEE: exactly the size, in RAM only; 0xED reads it back. */
	CHECK(named_set(0xEE, "user/keylock", lock, 3) < 0);
	CHECK(named_set(0xEE, "user/keylock", lock, 0) < 0);
	CHECK(named_set(0xEE, "user/keylock", window, 5) < 0);
	CHECK_EQ(named_set(0xEE, "user/keylock", lock, 4), 0);
	CHECK_EQ(stub.key_saves, 0);
	CHECK_EQ(proto_usb(0xED, p, named(p, "user/keylock", NULL, 0), reply), 4);
	CHECK(memcmp(&reply[3], lock, 4) == 0);
	/* Through the radio too: a SET record. */
	{
		uint8_t rec[2 + 61] = { 0xEE };

		rec[1] = named(&rec[2], "mte/charge_level", window, 8);
		CHECK_EQ(osc2_command_set(OSC2_LINK_RADIO, rec, (uint8_t)(2 + rec[1])), 0);
	}
	CHECK_EQ(proto_usb(0xED, p, named(p, "mte/charge_level", NULL, 0), reply), 8);
	CHECK(memcmp(&reply[3], window, 8) == 0);
	/* 0xEF saves (the NUL must be last); 0xF0 deletes and the RAM value is 0 again. */
	CHECK(named_set(0xEF, "user/keylock", (const uint8_t[]){ 1 }, 1) < 0);
	CHECK_EQ(stub.key_saves, 0);
	CHECK_EQ(named_set(0xEF, "user/keylock", NULL, 0), 0);
	CHECK_EQ(stub.key_saves, 1u << OSC2_KEY_KEYLOCK);
	CHECK_EQ(named_set(0xF0, "user/keylock", NULL, 0), 0);
	CHECK_EQ(stub.key_deletes, 1u << OSC2_KEY_KEYLOCK);
	CHECK_EQ(proto_usb(0xED, p, named(p, "user/keylock", NULL, 0), reply), 4);
	CHECK(memcmp(&reply[3], (const uint8_t[4]){ 0 }, 4) == 0);
	/* The fully-charged LED level has a read function: 3 until set. The haptics keys have none:
	 * 0xED answers length 0, while 0xEE, 0xEF and 0xF0 work. */
	CHECK_EQ(proto_usb(0xED, p, named(p, "user/fully_charged_led_intensity", NULL, 0), reply), 1);
	CHECK_EQ(reply[3], 3);
	CHECK_EQ(named_set(0xEE, "user/fully_charged_led_intensity", (const uint8_t[]){ 1 }, 1), 0);
	CHECK_EQ(named_set(0xEE, "settings/haptics/haptic_master_gain_db",
			   (const uint8_t[]){ 0xFA, 0xFF }, 2), 0);
	CHECK(named_set(0xEE, "settings/haptics/enabled", (const uint8_t[]){ 1 }, 1) < 0);
	CHECK_EQ(proto_usb(0xED, p, named(p, "user/fully_charged_led_intensity", NULL, 0), reply), 1);
	CHECK_EQ(reply[3], 1);
	CHECK_EQ(proto_usb(0xED, p, named(p, "settings/haptics/amplifier_mode", NULL, 0), reply), 0);
	CHECK_EQ(osc2_keys_get(OSC2_KEY_FULL_LED, v, &len), 0);
	CHECK(len == 1 && v[0] == 1);
	CHECK_EQ(osc2_keys_get(OSC2_KEY_HAPTIC_GAIN, v, &len), 0);
	CHECK(len == 2 && (int16_t)opensc2_get_le16(v) == -6);
	CHECK_EQ(named_set(0xEF, "user/fully_charged_led_intensity", NULL, 0), 0);
	CHECK(stub.key_saves & (1u << OSC2_KEY_FULL_LED));
	/* The factory reset deletes the user, mte and debug keys and keeps settings/haptics. */
	{
		const uint8_t key[4] = { 0x17, 0xA9, 0xC1, 0xEF };

		CHECK_EQ(proto_usb(0x86, key, 4, reply), -1);   /* set-only: the GET stalls */
		CHECK_EQ(stub.key_factory_resets, 1);
		CHECK_EQ(osc2_keys_get(OSC2_KEY_FULL_LED, v, &len), 0);
		CHECK_EQ(v[0], 3);
		CHECK_EQ(osc2_keys_get(OSC2_KEY_CHARGE_WINDOW, v, &len), 0);
		CHECK(memcmp(v, (const uint8_t[8]){ 0 }, 8) == 0);
		CHECK_EQ(osc2_keys_get(OSC2_KEY_HAPTIC_GAIN, v, &len), 0);
		CHECK_EQ((int16_t)opensc2_get_le16(v), -6);
		CHECK(stub.key_deletes & (1u << OSC2_KEY_RESET_REASON));
		CHECK(stub.key_deletes & (1u << OSC2_KEY_CHARGE_WINDOW));
		CHECK(!(stub.key_deletes & (1u << OSC2_KEY_HAPTIC_GAIN)));
	}
}

/* An mte/charge_level value: the lower and the upper mV, u32 each. */
static void window(uint8_t out[8], uint32_t lower, uint32_t upper)
{
	opensc2_put_le32(&out[0], lower);
	opensc2_put_le32(&out[4], upper);
}

OPENSC2_TEST(keys_charge_window)
{
	static const struct {
		uint32_t lower, upper;
		bool valid;
	} cases[] = {
		{ 0, 0, true },             /* no window */
		{ 3400, 4120, true },       /* the widest */
		{ 3800, 4000, true },
		{ 4120, 3400, true },       /* the order is not checked */
		{ 3400, 0, true },          /* only the bounds and the step are */
		{ 3390, 4120, false },      /* below 3400 */
		{ 3400, 4130, false },      /* above 4120 */
		{ 3405, 4100, false },      /* not a multiple of 10 */
		{ 3400, 4115, false },
		{ 0, 4000, false },         /* one 0 is not "no window" */
		{ 0xFFFFFFFFu, 0xFFFFFFFFu, false },
	};
	uint8_t v[8], p[61], reply[64], len, kept[OSC2_KEY_VALUE_MAX];

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		window(v, cases[i].lower, cases[i].upper);
		CHECK_EQ(osc2_key_value_readable(OSC2_KEY_CHARGE_WINDOW, v, 8), cases[i].valid);
	}
	CHECK(!osc2_key_value_readable(OSC2_KEY_CHARGE_WINDOW, v, 7));
	/* Every other key reads whatever it holds. */
	memset(kept, 0xFF, sizeof(kept));
	CHECK(osc2_key_value_readable(OSC2_KEY_KEYLOCK, kept, 4));
	CHECK(osc2_key_value_readable(OSC2_KEY_RESET_REASON, kept, 33));

	/* Through the command channel: 0xEE keeps any 8 bytes, 0xED answers them only while they
	 * are a valid window, as on the real controller; the value itself stays as set. */
	stub_reset();
	window(v, 3405, 4100);
	CHECK_EQ(named_set(0xEE, "mte/charge_level", v, 8), 0);
	CHECK_EQ(proto_usb(0xED, p, named(p, "mte/charge_level", NULL, 0), reply), 0);
	CHECK_EQ(osc2_keys_get(OSC2_KEY_CHARGE_WINDOW, kept, &len), 0);
	CHECK(len == 8 && memcmp(kept, v, 8) == 0);
	window(v, 3800, 4000);
	CHECK_EQ(named_set(0xEE, "mte/charge_level", v, 8), 0);
	CHECK_EQ(proto_usb(0xED, p, named(p, "mte/charge_level", NULL, 0), reply), 8);
	CHECK(memcmp(&reply[3], v, 8) == 0);
}

OPENSC2_TEST(keys_cmd_cal)
{
	uint8_t p[61], reply[64], value[OSC2_KEY_VALUE_MAX];

	stub_reset();
	/* With the library's calibration (CONFIG_OPENSC2_CAL, as these binaries are built) every
	 * calibration key of the key table is the calibration store's (cal_store.c): exactly the
	 * table's size, read back by 0xED where the table marks it readable, and never one of
	 * keys.c's kept values. Without the store the keys go to vendor_command, so 0xED answers
	 * them empty and the others are refused (run.ps1 and run.sh compile that build too). */
	memset(value, 0x01, sizeof(value));   /* a format word that is not 0, a trim within 1..199 */
	for (uint8_t id = 1; id < OSC2_KEY_COUNT; id++) {
		const struct osc2_key_info *k = osc2_key_by_id(id);

		if (!k || k->kind != OSC2_KEYK_CAL) {
			continue;
		}
		CHECK(k->size + 1u <= sizeof(value));
		CHECK(named_set(0xEE, k->name, value, (uint8_t)(k->size + 1)) < 0);
		CHECK(named_set(0xEE, k->name, value, (uint8_t)(k->size - 1)) < 0);
		CHECK_EQ(named_set(0xEE, k->name, value, k->size), 0);   /* RAM only */
		CHECK_EQ(proto_usb(0xED, p, named(p, k->name, NULL, 0), reply),
			 (k->flags & OSC2_KEYF_READ) ? k->size : 0);
		CHECK(named_set(0xEF, k->name, "x", 1) < 0);   /* the NUL must be the last byte */
		CHECK(named_set(0xF0, k->name, "x", 1) < 0);
		CHECK_EQ(named_set(0xF0, k->name, NULL, 0), 0);   /* zeros again */
	}
	CHECK_EQ(stub.key_saves | stub.key_deletes, 0);
	/* The identity keys are names like any other without CONFIG_OPENSC2_IDENTITY_VENDOR_WRITE. */
	CHECK(named_set(0xEE, "opensc2/unit_serial", "OSC2ABCDEF012", 13) < 0);
	CHECK_EQ(proto_usb(0xED, p, named(p, "opensc2/unit_serial", NULL, 0), reply), 0);
}
