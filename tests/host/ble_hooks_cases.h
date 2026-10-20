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

/* A stand-in for ble.c's test ops: a connected slot 2 on identity 3. */
static uint8_t ble_hook_op, ble_hook_args[4], ble_hook_alen;

static uint8_t fake_ble_op(uint8_t op, const uint8_t *args, uint8_t alen, uint8_t *result,
			   uint8_t *rlen)
{
	struct osc2_ble_status st;

	ble_hook_op = op;
	ble_hook_alen = alen;
	memcpy(ble_hook_args, args, alen < sizeof(ble_hook_args) ? alen : sizeof(ble_hook_args));
	switch (op) {
	case OSC2_TEST_OP_BLE_STATUS:
		memset(&st, 0, sizeof(st));
		st.enabled = st.connected = st.active = st.running = true;
		st.form = OSC2_BLE_ADV_NONE;
		st.slot = 1;
		st.id = 3;
		st.bonded = 0x08;
		st.identities = 5;
		st.security = 2;
		st.mtu = 67;
		st.interval_us = 7500;
		osc2_ble_pack_status(&st, result);
		*rlen = OSC2_BLE_STATUS_LEN;
		return OSC2_TEST_OK;
	case OSC2_TEST_OP_BLE_DROP:
		return args[0] == 0 ? OSC2_TEST_BAD_ARG : OSC2_TEST_OK;
	case OSC2_TEST_OP_BLE_WINDOW:
		return OSC2_TEST_OK;
	default:
		return OSC2_TEST_BAD_OP;
	}
}

/* The Bluetooth ops 0x60 to 0x62: answered by the library's Bluetooth code, BAD_OP in a build
 * without it; armed only; their argument lengths. And the Bluetooth ring entries of phase 4. */
OPENSC2_TEST(hooks_ble_ops)
{
	const uint8_t reason = 0x08, window[4] = { 0xD0, 0x07, 0, 0 };   /* 2000 ms */
	const uint8_t two[2] = { 1, 2 };
	int calls;

	stub_reset();
	/* Unarmed: the console answers NOT_ARMED, as for every op but ARM and STATUS. */
	CHECK_EQ(con_op(OSC2_TEST_OP_BLE_STATUS, NULL, 0), OSC2_TEST_NOT_ARMED);
	CHECK_EQ(arm_on(false, 0x0B1E0060, 0, 0), OSC2_TEST_OK);
	/* A build without Bluetooth. */
	CHECK_EQ(con_op(OSC2_TEST_OP_BLE_STATUS, NULL, 0), OSC2_TEST_BAD_OP);
	CHECK_EQ(hans_len, 4);
	CHECK_EQ(con_op(OSC2_TEST_OP_BLE_DROP, &reason, 1), OSC2_TEST_BAD_OP);
	/* With Bluetooth. */
	hk.ble_hook = fake_ble_op;
	CHECK_EQ(con_op(OSC2_TEST_OP_BLE_STATUS, NULL, 0), OSC2_TEST_OK);
	CHECK_EQ(hans_len, 4 + OSC2_BLE_STATUS_LEN);
	CHECK_EQ(RES[0], 0x01 | 0x04 | 0x08 | 0x20);   /* enabled, connected, active, running */
	CHECK_EQ(RES[2], 1);
	CHECK_EQ(RES[3], 3);
	CHECK_EQ(RES[4], 0x08);
	CHECK_EQ(opensc2_get_le16(&RES[10]), 67);
	CHECK_EQ(opensc2_get_le32(&RES[12]), 7500);
	CHECK_EQ(con_op(OSC2_TEST_OP_BLE_DROP, &reason, 1), OSC2_TEST_OK);
	CHECK_EQ(ble_hook_op, OSC2_TEST_OP_BLE_DROP);
	CHECK_EQ(ble_hook_args[0], 0x08);
	CHECK_EQ(con_op(OSC2_TEST_OP_BLE_DROP, &(uint8_t){ 0 }, 1), OSC2_TEST_BAD_ARG);
	CHECK_EQ(con_op(OSC2_TEST_OP_BLE_WINDOW, window, 4), OSC2_TEST_OK);
	CHECK_EQ(ble_hook_op, OSC2_TEST_OP_BLE_WINDOW);
	CHECK_EQ(ble_hook_alen, 4);
	CHECK_EQ(opensc2_get_le32(ble_hook_args), 2000);
	/* Lengths: checked before the library is asked. */
	calls = hk.ble_op_calls;
	CHECK_EQ(con_op(OSC2_TEST_OP_BLE_STATUS, two, 1), OSC2_TEST_BAD_LEN);
	CHECK_EQ(con_op(OSC2_TEST_OP_BLE_DROP, two, 2), OSC2_TEST_BAD_LEN);
	CHECK_EQ(con_op(OSC2_TEST_OP_BLE_WINDOW, two, 2), OSC2_TEST_BAD_LEN);
	CHECK_EQ(hk.ble_op_calls, calls);
	/* The same ops on the Bluetooth link's own feature report. */
	{
		uint8_t req[OSC2_TEST_REQUEST_MAX], rec[63] = { 0 }, out[63];
		uint8_t n = make_req(req, OSC2_TEST_OP_BLE_STATUS, NULL, 0);

		rec[0] = OSC2_TEST_OPCODE;
		rec[1] = n;
		memcpy(&rec[2], req, n);
		CHECK_EQ(osc2_command_set(OSC2_LINK_BLE, rec, sizeof(rec)), 0);
		CHECK_EQ(osc2_command_get_strict(OSC2_LINK_BLE, out), 63);
		CHECK_EQ(out[1], 4 + OSC2_BLE_STATUS_LEN);
		CHECK_EQ(out[2 + 3], OSC2_TEST_OK);
		CHECK_EQ(out[2 + 4 + 3], 3);   /* the identity */
	}

	/* The phase 4 entries: advertising, its end, identities, bonds, wireless off, a disconnect
	 * and what followed. Same layout as every Bluetooth entry. */
	CHECK_EQ(con_op(OSC2_TEST_OP_RING_CLEAR, NULL, 0), OSC2_TEST_OK);
	osc2_test_note_ble(OSC2_TEST_BLE_ADV, OSC2_BLE_ADV_DIRECTED, 0, 0x00000100);
	osc2_test_note_ble(OSC2_TEST_BLE_ADV_END, OSC2_BLE_ADV_END_FAILED, OSC2_HCI_ADV_TIMEOUT, 1);
	osc2_test_note_ble(OSC2_TEST_BLE_IDENTITY, OSC2_TEST_BLE_ID_RESET, 0, 2);
	osc2_test_note_ble(OSC2_TEST_BLE_BOND, OSC2_TEST_BLE_BOND_NEW_HOST, 0, 0x0102);
	osc2_test_note_ble(OSC2_TEST_BLE_DISCONNECT, OSC2_HCI_REMOTE_USER_TERM,
			   OSC2_BLE_AFTER_WIRELESS_OFF, OSC2_HCI_REMOTE_USER_TERM);
	osc2_test_note_ble(OSC2_TEST_BLE_OFF, OSC2_BLE_OFF_HOST, OSC2_HCI_REMOTE_USER_TERM, 0);
	read_ring(0);
	CHECK_EQ(count_kind(OSC2_TEST_KIND_BLE), 6);
	if (n_ents >= 6) {
		char text[128];
		int i = find_kind(OSC2_TEST_KIND_BLE, 0);

		CHECK(i >= 0);
		if (i >= 0) {
			CHECK_EQ(ents[i].d[0], OSC2_TEST_BLE_ADV);
			CHECK_EQ(ents[i].d[1], OSC2_BLE_ADV_DIRECTED);
			CHECK_EQ(opensc2_get_le32(&ents[i].d[4]), 0x100);
			osc2_test_fmt_entry(text, sizeof(text), 0, ents[i].raw,
					    (size_t)(7 + ents[i].len));
			CHECK(strstr(text, " ble BLE event=8 a=2 b=0 c=256") != NULL);
		}
		i = find_kind(OSC2_TEST_KIND_BLE, i + 1);
		CHECK(i >= 0);
		if (i >= 0) {
			CHECK_EQ(ents[i].d[0], OSC2_TEST_BLE_ADV_END);
			CHECK_EQ(opensc2_get_le16(&ents[i].d[2]), 0x3C);
		}
	}
	CHECK_EQ(hk.violations, 0);
}
