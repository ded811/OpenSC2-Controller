/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only, in the binary with CONFIG_OPENSC2_TEST_HOOKS (after hooks_cases.h, whose
 * helpers it uses): the test channel on the Bluetooth link (link 4), its held answer of its own,
 * and the Bluetooth ring entries. Included by run_tests.c.
 */

OPENSC2_TEST(hooks_ble_link)
{
	uint8_t req[OSC2_TEST_REQUEST_MAX], rec[63] = { 0 }, out[63], reply[64];
	const uint8_t mark[4] = { 9, 8, 7, 6 };
	uint8_t n;
	int i;

	stub_reset();
	CHECK_EQ(arm_on(true, 0x0B1E0B1E, 0, 0), OSC2_TEST_OK);
	/* A keyed MARK written to Bluetooth's feature report: claimed on link 4, its answer held for
	 * Bluetooth's reads (the strict GET, a whole 63-byte value). */
	n = make_req(req, OSC2_TEST_OP_MARK, mark, sizeof(mark));
	rec[0] = OSC2_TEST_OPCODE;
	rec[1] = n;
	memcpy(&rec[2], req, n);
	CHECK_EQ(osc2_command_set(OSC2_LINK_BLE, rec, sizeof(rec)), 0);
	for (int read = 0; read < 2; read++) {   /* read twice: held, not run again */
		CHECK_EQ(osc2_command_get_strict(OSC2_LINK_BLE, out), 63);
		CHECK_EQ(out[0], OSC2_TEST_OPCODE);
		CHECK_EQ(out[1], 8);                     /* version, ID, op, status, the sequence u32 */
		CHECK_EQ(out[2 + 2], OSC2_TEST_OP_MARK);
		CHECK_EQ(out[2 + 3], OSC2_TEST_OK);
	}
	/* USB's held answer is its own: still the ARM. */
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK_EQ(reply[1], OSC2_TEST_OPCODE);
	CHECK_EQ(reply[3 + 2], OSC2_TEST_OP_ARM);
	/* The MARK's ring entry names the transport of its request: Bluetooth. */
	read_ring(0);
	i = find_kind(OSC2_TEST_KIND_MARK, 0);
	CHECK(i >= 0);
	if (i >= 0) {
		CHECK_EQ(ents[i].link, OSC2_TEST_LINK_BLE);
		CHECK(memcmp(ents[i].d, mark, sizeof(mark)) == 0);
	}
	/* Another SET on Bluetooth replaces its stored request and drops the held answer. */
	memset(rec, 0, sizeof(rec));
	rec[0] = 0x83;
	CHECK_EQ(osc2_command_set(OSC2_LINK_BLE, rec, sizeof(rec)), 0);
	CHECK_EQ(osc2_command_get_strict(OSC2_LINK_BLE, out), 63);
	CHECK_EQ(out[0], 0x83);
	rec[0] = OSC2_TEST_OPCODE;   /* 0x7E with nothing held: the application's, refused */
	CHECK(osc2_command_set(OSC2_LINK_BLE, rec, sizeof(rec)) < 0);

	/* A Bluetooth event as the ring and the console show it. */
	osc2_test_note_ble(OSC2_TEST_BLE_GATE, 1, 67, 7500);
	read_ring(0);
	i = find_kind(OSC2_TEST_KIND_BLE, 0);
	CHECK(i >= 0);
	if (i >= 0) {
		char text[128];

		CHECK_EQ(ents[i].link, OSC2_TEST_LINK_BLE);
		CHECK_EQ(ents[i].len, 8);
		CHECK_EQ(ents[i].d[0], OSC2_TEST_BLE_GATE);
		CHECK_EQ(opensc2_get_le16(&ents[i].d[2]), 67);
		CHECK_EQ(opensc2_get_le32(&ents[i].d[4]), 7500);
		osc2_test_fmt_entry(text, sizeof(text), 7, ents[i].raw, (size_t)(7 + ents[i].len));
		CHECK(strstr(text, " ble BLE event=5 a=1 b=67 c=7500") != NULL);
	}
	CHECK_EQ(hk.violations, 0);
}
