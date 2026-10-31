/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: the crash record (crash_rules.c): its layout, the RAM slot a reset keeps, the
 * rule that keeps a crash at every boot from writing flash at every boot, and vendor command 0xF2
 * selector 1 through the command channel (commands.c with the stand-ins of stubs.c), which answers
 * the record and then marks it seen. Uses proto_cases.h's proto_usb().
 */
#include <string.h>
#include "crash_rules.h"
#include "key_store.h"
#include "stubs.h"
#include "test_core.h"

OPENSC2_TEST(crash_record_layout)
{
	uint8_t r[OSC2_CRASH_LEN];

	osc2_crash_record(r, 0x6AD6E287u, "osc2-0.3.0", 3, 0x00031234u, 0x00030ABCu);
	CHECK_EQ(r[0], 1);   /* new */
	CHECK_EQ(opensc2_get_le32(&r[1]), 0x6AD6E287u);
	CHECK(memcmp(&r[5], "osc2-0.3.0", 10) == 0);
	CHECK(memcmp(&r[15], (const uint8_t[6]){ 0 }, 6) == 0);   /* zero padded to 16 */
	CHECK_EQ(opensc2_get_le32(&r[21]), 3);
	CHECK_EQ(opensc2_get_le32(&r[25]), 0x00031234u);
	CHECK_EQ(opensc2_get_le32(&r[29]), 0x00030ABCu);
	CHECK_EQ(osc2_crash_field(r, OSC2_CRASH_AT_REASON), 3);
	CHECK_EQ(osc2_crash_field(r, OSC2_CRASH_AT_LR), 0x00030ABCu);
	CHECK_EQ(osc2_crash_field(r, 30), 0);   /* past the end */
	CHECK(!osc2_crash_empty(r));
	/* A revision of 16 characters or more fills the field, without a NUL. */
	osc2_crash_record(r, 1, "0123456789abcdefXYZ", 0, 0, 0);
	CHECK(memcmp(&r[5], "0123456789abcdef", 16) == 0);
	CHECK_EQ(opensc2_get_le32(&r[21]), 0);
	osc2_crash_record(r, 1, NULL, 0, 0, 0);
	CHECK(memcmp(&r[5], (const uint8_t[16]){ 0 }, 16) == 0);
	memset(r, 0, sizeof(r));
	CHECK(osc2_crash_empty(r));
}

OPENSC2_TEST(crash_slot)
{
	struct osc2_crash_slot s;
	uint8_t r[OSC2_CRASH_LEN];

	CHECK_EQ(sizeof(s), 44);   /* crash.h's OSC2_CRASH_SLOT_SIZE with CONFIG_OPENSC2_CRASH_RECORD */
	osc2_crash_record(r, 0x6AD6E287u, "osc2", 0, 0x1234, 0x5678);
	memset(&s, 0xA5, sizeof(s));            /* power-on RAM, roughly */
	CHECK(!osc2_crash_slot_valid(&s));
	osc2_crash_slot_write(&s, r);
	CHECK(osc2_crash_slot_valid(&s));
	CHECK(memcmp(s.record, r, sizeof(r)) == 0);
	s.record[25] ^= 1;                      /* one bit off: not a record */
	CHECK(!osc2_crash_slot_valid(&s));
	s.record[25] ^= 1;
	s.magic = 0;                            /* cut short before the magic */
	CHECK(!osc2_crash_slot_valid(&s));
	osc2_crash_slot_write(&s, r);
	osc2_crash_slot_clear(&s);
	CHECK(!osc2_crash_slot_valid(&s));
}

OPENSC2_TEST(crash_save_rule)
{
	uint8_t stored[OSC2_CRASH_LEN] = { 0 }, fresh[OSC2_CRASH_LEN];

	osc2_crash_record(fresh, 0x6AD6E287u, "osc2", 4, 0x1234, 0x5678);
	/* Nothing stored, or another crash, or the same one already read: saved. */
	CHECK(osc2_crash_should_save(stored, fresh));
	osc2_crash_record(stored, 0x6AD6E287u, "osc2", 4, 0x1234, 0x9999);
	CHECK(osc2_crash_should_save(stored, fresh));
	memcpy(stored, fresh, sizeof(stored));
	stored[0] = 0;
	CHECK(osc2_crash_should_save(stored, fresh));
	/* The same crash, still unread: no flash write (a crash at every boot). */
	stored[0] = 1;
	CHECK(!osc2_crash_should_save(stored, fresh));
}

OPENSC2_TEST(crash_answer)
{
	uint8_t r[OSC2_CRASH_LEN], out[OSC2_CRASH_ANSWER_LEN];

	osc2_crash_record(r, 0x6AD6E287u, "osc2", 0, 0x1234, 0x5678);
	CHECK(osc2_crash_answer(r, out));       /* new: answered as it was, then seen */
	CHECK_EQ(out[0], 1);
	CHECK_EQ(out[1], 1);
	CHECK_EQ(r[0], 0);
	CHECK(memcmp(&out[2], &r[1], OSC2_CRASH_LEN - 1) == 0);
	CHECK(!osc2_crash_answer(r, out));      /* seen: no change, nothing to save */
	CHECK_EQ(out[1], 0);
	memset(r, 0, sizeof(r));
	CHECK(!osc2_crash_answer(r, out));      /* none: the selector and 33 zeros */
	CHECK(memcmp(&out[1], (const uint8_t[OSC2_CRASH_LEN]){ 0 }, OSC2_CRASH_LEN) == 0);
}

OPENSC2_TEST(crash_cmd_f2)
{
	const uint8_t sel1 = 1;
	uint8_t r[OSC2_CRASH_LEN], reply[64];
	uint8_t name[18];

	stub_reset();
	memcpy(name, "debug/resetreason", 18);
	/* No crash ever: the selector and 33 zero bytes, nothing written. */
	CHECK_EQ(proto_usb(0xF2, &sel1, 1, reply), 34);
	CHECK_EQ(reply[3], 1);
	CHECK(memcmp(&reply[4], (const uint8_t[OSC2_CRASH_LEN]){ 0 }, OSC2_CRASH_LEN) == 0);
	CHECK_EQ(stub.key_saves, 0);
	/* A new record (as the boot stores it): answered with its flag, then marked seen and saved. */
	osc2_crash_record(r, 0x6AD6E287u, "osc2-0.3.0", 3, 0x31234, 0x30ABC);
	CHECK_EQ(osc2_keys_put(OSC2_KEY_RESET_REASON, r, sizeof(r), false), 0);
	CHECK_EQ(proto_usb(0xF2, &sel1, 1, reply), 34);
	CHECK(memcmp(&reply[4], r, OSC2_CRASH_LEN) == 0);
	CHECK_EQ(reply[4], 1);
	CHECK_EQ(stub.key_saves, 1u << OSC2_KEY_RESET_REASON);
	/* Again: seen, and no second flash write. */
	stub.key_saves = 0;
	CHECK_EQ(proto_usb(0xF2, &sel1, 1, reply), 34);
	CHECK_EQ(reply[4], 0);
	CHECK(memcmp(&reply[5], &r[1], OSC2_CRASH_LEN - 1) == 0);
	CHECK_EQ(stub.key_saves, 0);
	/* 0xED reads the same record, and the factory reset deletes it from flash; the record in RAM
	 * stays until the reboot that follows, as on the real controller. */
	CHECK_EQ(proto_usb(0xED, name, sizeof(name), reply), 33);
	CHECK_EQ(reply[3], 0);
	CHECK(memcmp(&reply[4], &r[1], OSC2_CRASH_LEN - 1) == 0);
	CHECK_EQ(proto_usb(0x86, (const uint8_t[]){ 0x17, 0xA9, 0xC1, 0xEF }, 4, reply), -1);
	CHECK(stub.key_deletes & (1u << OSC2_KEY_RESET_REASON));
	CHECK_EQ(proto_usb(0xF2, &sel1, 1, reply), 34);
	CHECK(memcmp(&reply[5], &r[1], OSC2_CRASH_LEN - 1) == 0);
	/* The host emulation never asks for selector 1: its GET writes flash. */
	CHECK(!osc2_test_host_set_allowed(0xF2, &sel1, 1));
	CHECK(osc2_test_host_set_allowed(0xF2, (const uint8_t[]){ 0 }, 1));
	CHECK(osc2_test_host_set_allowed(0xF2, (const uint8_t[]){ 2 }, 1));
	CHECK(osc2_test_host_set_allowed(0xF2, NULL, 0));
}
