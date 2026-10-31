/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: the puck link's rules (radio_rules.c) and the bond keys as the real controller
 * keeps them (commands.c with the stand-ins of stubs.c): what a control event does in each state,
 * the connection request a bond takes, the first disconnect reason, the channel walk, the two
 * answers that wait for a state report, the state report's slot and lead, the output record's
 * length, and 0xEE, 0xEF, 0xF0 of esb/bond and esb/bond_2 and 0xA2. Included by run_tests.c after
 * commands_cases.h (its helpers).
 */
#include "radio_rules.h"

/* A connection request {0xE1, puck ID, controller ID, channel u32, address, prefix}. */
static size_t radio_request(uint8_t out[24], uint32_t puck, uint32_t controller, uint8_t channel)
{
	memset(out, 0, 24);
	out[0] = OPENSC2_PKT_CONNECT_REQUEST;
	opensc2_put_le32(&out[1], puck);
	opensc2_put_le32(&out[5], controller);
	out[9] = channel;
	memcpy(&out[13], "\x11\x22\x33\x44", 4);
	out[17] = 0x5A;
	return 18;
}

OPENSC2_TEST(radio_control_by_state)
{
	uint8_t next = 0xAA, reason = 0xAA;

	/* A stop ends every state with reason 2. */
	for (uint8_t st = OPENSC2_LINK_NO_BOND; st <= OPENSC2_LINK_CONNECTED; st++) {
		CHECK(osc2_radio_control(st, OSC2_RCTL_STOP | OSC2_RCTL_BOND, false, &next, &reason));
		CHECK_EQ(next, OPENSC2_LINK_OFF);
		CHECK_EQ(reason, OPENSC2_REASON_DISABLED);
	}
	/* Waiting for a bond: any bond write listens, whatever it wrote. */
	CHECK(osc2_radio_control(OPENSC2_LINK_NO_BOND, OSC2_RCTL_BOND, false, &next, &reason));
	CHECK_EQ(next, OPENSC2_LINK_LISTENING);
	CHECK_EQ(reason, OPENSC2_REASON_BOND_CHANGED);
	/* Listening: a bond write (either slot) starts listening again. */
	CHECK(osc2_radio_control(OPENSC2_LINK_LISTENING, OSC2_RCTL_BOND, false, &next, &reason));
	CHECK_EQ(next, OPENSC2_LINK_LISTENING);
	CHECK_EQ(reason, OPENSC2_REASON_BOND_CHANGED);
	/* Connected: a bond write is ignored; START and CANCEL do nothing. */
	CHECK(!osc2_radio_control(OPENSC2_LINK_CONNECTED, OSC2_RCTL_BOND, false, &next, &reason));
	CHECK(!osc2_radio_control(OPENSC2_LINK_CONNECTED, OSC2_RCTL_START | OSC2_RCTL_CANCEL, false,
				  &next, &reason));
	CHECK(!osc2_radio_control(OPENSC2_LINK_LISTENING, OSC2_RCTL_START | OSC2_RCTL_CANCEL, false,
				  &next, &reason));
	/* OpenSC2's own: another slot chosen at run time leaves the state for that slot ... */
	CHECK(osc2_radio_control(OPENSC2_LINK_CONNECTED, OSC2_RCTL_SLOT, true, &next, &reason));
	CHECK_EQ(next, OSC2_RADIO_BOND_OR_NOT);
	CHECK_EQ(reason, OPENSC2_REASON_DISABLED);
	CHECK(osc2_radio_control(OPENSC2_LINK_LISTENING, OSC2_RCTL_SLOT, true, &next, &reason));
	CHECK_EQ(next, OSC2_RADIO_BOND_OR_NOT);
	CHECK_EQ(reason, OPENSC2_REASON_BOND_CHANGED);
	/* ... and the same slot chosen again changes nothing. */
	CHECK(!osc2_radio_control(OPENSC2_LINK_CONNECTED, OSC2_RCTL_SLOT, false, &next, &reason));
	CHECK(!osc2_radio_control(OPENSC2_LINK_NO_BOND, OSC2_RCTL_SLOT, false, &next, &reason));
	/* Pairing (OpenSC2's): from a connection with reason 2; cancelled back to the slot. */
	CHECK(osc2_radio_control(OPENSC2_LINK_CONNECTED, OSC2_RCTL_PAIR, false, &next, &reason));
	CHECK_EQ(next, OPENSC2_LINK_PAIRING);
	CHECK_EQ(reason, OPENSC2_REASON_DISABLED);
	CHECK(osc2_radio_control(OPENSC2_LINK_NO_BOND, OSC2_RCTL_PAIR, false, &next, &reason));
	CHECK_EQ(next, OPENSC2_LINK_PAIRING);
	CHECK(osc2_radio_control(OPENSC2_LINK_PAIRING, OSC2_RCTL_CANCEL, false, &next, &reason));
	CHECK_EQ(next, OSC2_RADIO_BOND_OR_NOT);
	CHECK_EQ(reason, OPENSC2_REASON_PAIRING_FAILED);
	CHECK(!osc2_radio_control(OPENSC2_LINK_PAIRING, OSC2_RCTL_BOND | OSC2_RCTL_PAIR, false,
				  &next, &reason));
}

OPENSC2_TEST(radio_request_and_reason)
{
	struct opensc2_connect_request req;
	uint8_t pkt[24];
	size_t n = radio_request(pkt, 0x01020304, 0x0A0B0C0D, 40);

	CHECK(osc2_radio_request_matches(pkt, n, 0x01020304, 0x0A0B0C0D, &req));
	CHECK_EQ(req.channel, 40);
	CHECK_EQ(req.prefix, 0x5A);
	/* Exactly 18 bytes, both IDs. */
	CHECK(!osc2_radio_request_matches(pkt, n + 1, 0x01020304, 0x0A0B0C0D, &req));
	CHECK(!osc2_radio_request_matches(pkt, n - 1, 0x01020304, 0x0A0B0C0D, &req));
	CHECK(!osc2_radio_request_matches(pkt, n, 0x01020304, 0x0A0B0C0E, &req));
	CHECK(!osc2_radio_request_matches(pkt, n, 0x01020305, 0x0A0B0C0D, &req));
	pkt[0] = OPENSC2_PKT_HELLO;
	CHECK(!osc2_radio_request_matches(pkt, n, 0x01020304, 0x0A0B0C0D, &req));
	/* An empty bond listens for IDs 0 and 0, as the real controller does. */
	n = radio_request(pkt, 0, 0, 40);
	CHECK(osc2_radio_request_matches(pkt, n, 0, 0, &req));

	/* The first reason of a connection stays. */
	CHECK_EQ(osc2_radio_reason_first(0, 2), 2);
	CHECK_EQ(osc2_radio_reason_first(3, 2), 3);   /* a stop after the puck's turn-off */
	CHECK_EQ(osc2_radio_reason_first(4, 1), 4);   /* a lost link after a refused version */
	CHECK_EQ(osc2_radio_reason_first(0, 1), 1);
}

OPENSC2_TEST(radio_channel_walk)
{
	const uint8_t one[4] = { 40, 0xFF, 0xFF, 0xFF };
	const uint8_t three[4] = { 40, 2, 0xFF, 80 };
	const uint8_t none[4] = { 0xFF, 0xFF, 0xFF, 0xFF };

	/* One channel: it is listened to again (13 ms then). */
	CHECK_EQ(osc2_radio_walk_next(one, 0), 0);
	/* The others in order, 0xFF skipped, round to the first. */
	CHECK_EQ(osc2_radio_walk_next(three, 0), 1);
	CHECK_EQ(osc2_radio_walk_next(three, 1), 3);
	CHECK_EQ(osc2_radio_walk_next(three, 3), 0);
	CHECK_EQ(osc2_radio_walk_next(none, 0), -1);
}

OPENSC2_TEST(radio_answers_wait_one_each)
{
	struct osc2_radio_answers a;
	uint8_t data[63];
	uint8_t len = 0xAA;
	const uint8_t get1[4] = { 0xED, 2, 0x11, 0x22 };
	const uint8_t get2[2] = { 0x83, 0 };

	osc2_radio_answers_clear(&a);
	CHECK_EQ(osc2_radio_answers_next(&a, data, &len), 0);
	CHECK_EQ(len, 0);
	/* A SET result, then a GET answer: the GET answer goes first, one per report. */
	CHECK(osc2_radio_answers_put_set(&a, -22));
	CHECK(osc2_radio_answers_put_get(&a, get1, sizeof(get1)));
	/* A newer one of each kind is dropped while one waits. */
	CHECK(!osc2_radio_answers_put_set(&a, 0));
	CHECK(!osc2_radio_answers_put_get(&a, get2, sizeof(get2)));
	CHECK_EQ(osc2_radio_answers_next(&a, data, &len), OPENSC2_REC_GET_ANSWER);
	CHECK_EQ(len, 4);
	CHECK(memcmp(data, get1, 4) == 0);
	/* Not sent (the queue was full): still there. */
	CHECK_EQ(osc2_radio_answers_next(&a, data, &len), OPENSC2_REC_GET_ANSWER);
	osc2_radio_answers_sent(&a, OPENSC2_REC_GET_ANSWER);
	CHECK_EQ(osc2_radio_answers_next(&a, data, &len), OPENSC2_REC_SET_RESULT);
	CHECK_EQ(len, 4);
	CHECK_EQ((int32_t)opensc2_get_le32(data), -22);
	osc2_radio_answers_sent(&a, OPENSC2_REC_SET_RESULT);
	CHECK_EQ(osc2_radio_answers_next(&a, data, &len), 0);
	/* Room again. */
	CHECK(osc2_radio_answers_put_set(&a, 0));
}

OPENSC2_TEST(radio_slot_lead)
{
	struct osc2_slot_lead l;

	/* Before 65 reports were timed: the initial lead (81 > 68: built at once). */
	osc2_slot_lead_init(&l, 81);
	CHECK(osc2_slot_lead_wait(&l, 1000 + 68, 1000) <= 0);
	/* The current window's longest counts as soon as it is longer than the last window's. */
	osc2_slot_lead_done(&l, 100);
	CHECK_EQ(osc2_slot_lead_wait(&l, 1000 + 68, 1000), 68 - 100);
	/* After 65 reports their longest is the lead. */
	osc2_slot_lead_init(&l, 81);
	for (int i = 0; i < OSC2_SLOT_LEAD_FRAMES; i++) {
		osc2_slot_lead_done(&l, i == 10 ? 9 : 4);
	}
	CHECK_EQ(l.worst, 9);
	CHECK_EQ(l.peak, 0);
	CHECK_EQ(osc2_slot_lead_wait(&l, 1000 + 68, 1000), 68 - 9);
	CHECK_EQ(osc2_slot_lead_wait(&l, 1000 + 68, 1030), 68 - 9 - 30);
	osc2_slot_lead_done(&l, 12);
	CHECK_EQ(osc2_slot_lead_wait(&l, 1000 + 68, 1000), 68 - 12);
}

OPENSC2_TEST(radio_output_record_length)
{
	/* A keep-alive {0xE3, record {len 3, type 5, 0x80 a b}, record {len 1, type 3, x}}. */
	const uint8_t pkt[] = { OPENSC2_PKT_KEEPALIVE, 3, OPENSC2_REC_OUTPUT, 0x80, 0x0A, 0x0B,
				1, OPENSC2_REC_GET_FEATURE, 0x77 };
	uint8_t out[OPENSC2_PAYLOAD_MAX + 1];
	struct opensc2_record rec;
	size_t pos = 1;

	CHECK_EQ(opensc2_record_next(pkt, sizeof(pkt), &pos, &rec), 1);
	/* The record's length, and the byte after the report with it. */
	CHECK_EQ(osc2_radio_output_record(pkt, sizeof(pkt), &rec, out), 4);
	CHECK_EQ(out[0], 0x80);
	CHECK_EQ(out[2], 0x0B);
	CHECK_EQ(out[3], 1);   /* the next record's length byte */
	/* At the end of the packet the extra byte is 0. */
	CHECK_EQ(osc2_radio_output_record(pkt, 6, &rec, out), 4);
	CHECK_EQ(out[3], 0);
	rec.len = 0;
	CHECK_EQ(osc2_radio_output_record(pkt, sizeof(pkt), &rec, out), 0);
}

/* The bond keys as the real controller keeps them. */
OPENSC2_TEST(radio_bond_keys)
{
	uint8_t key_and_bond[9 + OPENSC2_BOND_LEN];
	struct opensc2_bond bond = { .puck_id = 0x01020304, .controller_id = 0 };
	uint8_t reply[64];

	stub_reset();
	memcpy(bond.puck_serial, "PUCK000000001", 13);
	memcpy(key_and_bond, key_bond, sizeof(key_bond));

	/* A bond with one ID 0 is kept as written, reads back empty and cannot be saved
	 * (-ENODEV: the key has no value). */
	opensc2_bond_pack(&bond, &key_and_bond[9]);
	CHECK_EQ(usb_set(0xEE, key_and_bond, sizeof(key_and_bond)), 0);
	CHECK_EQ(osc2.bonds[0].puck_id, 0x01020304u);
	CHECK_EQ(osc2.bonds[0].controller_id, 0);
	CHECK(stub.radio_events & OSC2_EV_BOND);
	CHECK_EQ(usb_set(0xED, key_bond, sizeof(key_bond)), 0);
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK_EQ(reply[2], 0);
	CHECK_EQ(usb_set(0xEF, key_bond, sizeof(key_bond)), -ENODEV);
	CHECK_EQ(stub.saved_bonds, 0);
	/* A refused length: -EINVAL, and the radio still hears a bond write. */
	stub.radio_events = 0;
	CHECK_EQ(usb_set(0xEE, key_and_bond, sizeof(key_and_bond) - 1), -EINVAL);
	CHECK(stub.radio_events & OSC2_EV_BOND);
	CHECK_EQ(osc2.bonds[0].puck_id, 0x01020304u);

	/* 0xF0: deleted first; only then is the RAM bond emptied (and the radio told). */
	bond.controller_id = 0x0A0B0C0D;
	osc2.bonds[0] = bond;
	stub.radio_events = 0;
	stub.bond_delete_error = -EIO;
	CHECK_EQ(usb_set(0xF0, key_bond, sizeof(key_bond)), -EIO);
	CHECK_EQ(osc2.bonds[0].puck_id, 0x01020304u);
	CHECK_EQ(stub.radio_events, 0);
	stub.bond_delete_error = 0;
	CHECK_EQ(usb_set(0xF0, key_bond, sizeof(key_bond)), 0);
	CHECK_EQ(stub.deleted_bonds, 1);
	CHECK_EQ(osc2.bonds[0].puck_id, 0);
	CHECK(stub.radio_events & OSC2_EV_BOND);
}

/* 0xA2: written to flash only (from a work item), for the next boot; the bond in RAM and the
 * radio are not touched. */
OPENSC2_TEST(radio_bond_a2_counts_from_the_next_boot)
{
	struct opensc2_bond bond = { .puck_id = 0x01020304, .controller_id = 0x0A0B0C0D };
	uint8_t packed[OPENSC2_BOND_LEN];

	stub_reset();
	memcpy(bond.puck_serial, "PUCK000000001", 13);
	opensc2_bond_pack(&bond, packed);
	CHECK_EQ(usb_set(0xA2, packed, sizeof(packed)), 0);
	CHECK_EQ(stub.stored_bond_writes, 1);
	CHECK_EQ(stub.stored_bond_slot, 0);
	CHECK_EQ(stub.stored_bond.puck_id, 0x01020304u);
	CHECK_EQ(stub.stored_bond.controller_id, 0x0A0B0C0Du);
	CHECK_EQ(osc2.bonds[0].puck_id, 0);   /* RAM as it was */
	CHECK_EQ(stub.radio_events, 0);
	CHECK_EQ(stub.saved_bonds, 0);
	/* An invalid one is handed on as it is (the store deletes the stored bond). */
	memset(packed, 0, sizeof(packed));
	CHECK_EQ(usb_set(0xA2, packed, sizeof(packed)), 0);
	CHECK_EQ(stub.stored_bond_writes, 2);
	CHECK(!opensc2_bond_valid(&stub.stored_bond));
	/* Exactly 24 bytes. */
	CHECK_EQ(usb_set(0xA2, packed, 23), -EINVAL);
	CHECK_EQ(stub.stored_bond_writes, 2);
}
