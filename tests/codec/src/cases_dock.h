/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Test cases for the dock's pure parts: the frame codec (dock_codec.c) and the dock's rules
 * (dock_rules.c: the answer to a frame, the pilot check, the supply rule, the docked states).
 * Written once, run two ways, like cases.h: a Zephyr ztest suite (tests/codec) and the host tests
 * (tests/host, which also run tests/vectors/hwtest/dock.txt through the same functions). The
 * values are the protocol documentation's (not public yet; docs/protocol.md lists its pages).
 */
#include <string.h>
#include <opensc2/codec.h>
#include <opensc2/dock.h>
#include "dock_rules.h"

/* ---- the frames ------------------------------------------------------------------------------ */

OPENSC2_TEST(dock_crc)
{
	static const uint8_t check[] = "123456789";
	uint8_t frame[OPENSC2_DOCK_FRAME_LEN] = { 0x5A, 0x00 };

	CHECK_EQ(opensc2_dock_crc16(0, check, 9), 0x2189);
	/* in two parts: the running value carries over */
	CHECK_EQ(opensc2_dock_crc16(opensc2_dock_crc16(0, check, 4), &check[4], 5), 0x2189);
	CHECK_EQ(opensc2_dock_crc16(0, check, 0), 0);
	/* the documentation's two examples */
	CHECK_EQ(opensc2_dock_crc16(0, frame, 71), 0x9BD0);
	frame[1] = 0x03;
	CHECK_EQ(opensc2_dock_crc16(0, frame, 71), 0x3BA7);
}

OPENSC2_TEST(dock_frames)
{
	static const uint8_t payload[3] = { 0x83, 0x00, 0x00 };
	uint8_t f[OPENSC2_DOCK_FRAME_LEN];

	CHECK_EQ(opensc2_dock_build_request(OPENSC2_DOCK_PROBE, NULL, 0, f), 73);
	CHECK_EQ(f[0], 0x5A);
	CHECK_EQ(f[1], 0);
	CHECK_EQ(f[71], 0xD0);   /* low byte first */
	CHECK_EQ(f[72], 0x9B);
	CHECK_EQ(opensc2_dock_frame_check(f, 73), OPENSC2_DOCK_FRAME_OK);
	CHECK_EQ(opensc2_dock_frame_check(f, 72), OPENSC2_DOCK_FRAME_BAD_SIZE);
	CHECK_EQ(opensc2_dock_frame_check(f, 0), OPENSC2_DOCK_FRAME_BAD_SIZE);
	f[40] ^= 0x10;
	CHECK_EQ(opensc2_dock_frame_check(f, 73), OPENSC2_DOCK_FRAME_BAD_CRC);
	f[40] ^= 0x10;
	f[0] = 0xAC;   /* the start byte is covered by the CRC (and checked by nobody) */
	CHECK_EQ(opensc2_dock_frame_check(f, 73), OPENSC2_DOCK_FRAME_BAD_CRC);
	opensc2_dock_frame_seal(f, 0xAC);
	CHECK_EQ(opensc2_dock_frame_check(f, 73), OPENSC2_DOCK_FRAME_OK);

	CHECK_EQ(opensc2_dock_build_request(OPENSC2_DOCK_SET, payload, sizeof(payload), f), 73);
	CHECK_EQ(f[1], 2);
	CHECK_EQ(f[8], 0x83);
	CHECK_EQ(f[11], 0);
	CHECK_EQ(opensc2_dock_frame_check(f, 73), OPENSC2_DOCK_FRAME_OK);
	CHECK_EQ(opensc2_dock_build_request(OPENSC2_DOCK_SET, f, 64, f), 0);   /* too long */
}

/* ---- one frame in, one answer out ------------------------------------------------------------ */

struct fake_dock {
	int identity, set, get, input;
	uint8_t set_rec[OPENSC2_DOCK_PAYLOAD_LEN];
	int32_t set_rc, get_rc;
};

static void fake_identity(void *ctx, uint8_t out[OPENSC2_IDENTITY_RECORD_LEN])
{
	struct fake_dock *f = ctx;

	f->identity++;
	for (int i = 0; i < OPENSC2_IDENTITY_RECORD_LEN; i++) {
		out[i] = (uint8_t)(0x10 + i);
	}
}

static int32_t fake_set(void *ctx, const uint8_t rec[OPENSC2_DOCK_PAYLOAD_LEN])
{
	struct fake_dock *f = ctx;

	f->set++;
	memcpy(f->set_rec, rec, OPENSC2_DOCK_PAYLOAD_LEN);
	return f->set_rc;
}

static int32_t fake_get(void *ctx, uint8_t out[OPENSC2_DOCK_PAYLOAD_LEN])
{
	struct fake_dock *f = ctx;

	f->get++;
	out[0] = 0x89;
	out[1] = 3;
	out[2] = 45;
	out[3] = 50;
	return f->get_rc;
}

static void fake_input(void *ctx, uint8_t out[OPENSC2_STATE_REPORT_LEN - 1])
{
	struct fake_dock *f = ctx;

	f->input++;
	memset(out, 0x42, OPENSC2_STATE_REPORT_LEN - 1);
}

static const struct osc2_dock_ops fake_ops = {
	.identity = fake_identity,
	.set = fake_set,
	.get = fake_get,
	.input = fake_input,
};

static bool all_zero(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (p[i]) {
			return false;
		}
	}
	return true;
}

OPENSC2_TEST(dock_answers)
{
	struct fake_dock f;
	uint8_t req[OPENSC2_DOCK_FRAME_LEN], ans[OPENSC2_DOCK_FRAME_LEN];
	static const uint8_t set87[] = { 0x87, 3, 45, 50, 0 };

	/* a probe: the identity record, type and result 0, the rest zero, 0xAC, a good CRC */
	memset(&f, 0, sizeof(f));
	memset(ans, 0xEE, sizeof(ans));
	(void)opensc2_dock_build_request(OPENSC2_DOCK_PROBE, NULL, 0, req);
	CHECK_EQ(osc2_dock_answer(req, 73, ans, &fake_ops, &f), OSC2_DOCK_ANSWERED);
	CHECK_EQ(f.identity, 1);
	CHECK_EQ(ans[0], 0xAC);
	CHECK_EQ(ans[1], 0);
	CHECK(all_zero(&ans[2], 6));
	CHECK_EQ(ans[8], 0x10);
	CHECK_EQ(ans[8 + 56], 0x10 + 56);
	CHECK(all_zero(&ans[8 + 57], 6));
	CHECK_EQ(opensc2_dock_frame_check(ans, 73), OPENSC2_DOCK_FRAME_OK);

	/* a SET: the 63 payload bytes go to the command channel; its result; no payload back */
	memset(&f, 0, sizeof(f));
	f.set_rc = -22;
	(void)opensc2_dock_build_request(OPENSC2_DOCK_SET, set87, sizeof(set87), req);
	CHECK_EQ(osc2_dock_answer(req, 73, ans, &fake_ops, &f), OSC2_DOCK_ANSWERED);
	CHECK_EQ(f.set, 1);
	CHECK(memcmp(f.set_rec, set87, sizeof(set87)) == 0);
	CHECK_EQ(ans[1], 2);
	CHECK_EQ((int32_t)opensc2_get_le32(&ans[4]), -22);
	CHECK(all_zero(&ans[8], 63));

	/* a GET: {opcode, length, data} and 63 */
	memset(&f, 0, sizeof(f));
	f.get_rc = OSC2_DOCK_RESULT_GET_OK;
	(void)opensc2_dock_build_request(OPENSC2_DOCK_GET, set87, sizeof(set87), req);
	CHECK_EQ(osc2_dock_answer(req, 73, ans, &fake_ops, &f), OSC2_DOCK_ANSWERED);
	CHECK_EQ(f.get, 1);
	CHECK_EQ(f.set, 0);   /* a GET's payload is ignored */
	CHECK_EQ(ans[1], 1);
	CHECK_EQ(opensc2_get_le32(&ans[4]), 63);
	CHECK_EQ(ans[8], 0x89);
	CHECK_EQ(ans[9], 3);

	/* GET input: the 53 bytes, result 0 */
	memset(&f, 0, sizeof(f));
	(void)opensc2_dock_build_request(OPENSC2_DOCK_INPUT, NULL, 0, req);
	CHECK_EQ(osc2_dock_answer(req, 73, ans, &fake_ops, &f), OSC2_DOCK_ANSWERED);
	CHECK_EQ(f.input, 1);
	CHECK_EQ(ans[1], 3);
	CHECK_EQ(opensc2_get_le32(&ans[4]), 0);
	CHECK_EQ(ans[8], 0x42);
	CHECK_EQ(ans[8 + 52], 0x42);
	CHECK(all_zero(&ans[8 + 53], 10));

	/* another type: its number back, -22, nothing asked of the library */
	memset(&f, 0, sizeof(f));
	(void)opensc2_dock_build_request(4, NULL, 0, req);
	CHECK_EQ(osc2_dock_answer(req, 73, ans, &fake_ops, &f), OSC2_DOCK_ANSWERED);
	CHECK_EQ(f.identity + f.set + f.get + f.input, 0);
	CHECK_EQ(ans[1], 4);
	CHECK_EQ((int32_t)opensc2_get_le32(&ans[4]), -22);

	/* the start byte and bytes 2 to 7 of a request are ignored */
	(void)opensc2_dock_build_request(OPENSC2_DOCK_INPUT, NULL, 0, req);
	req[0] = 0x00;
	req[2] = 0xFF;
	req[7] = 0x80;
	opensc2_dock_frame_seal(req, 0x11);
	CHECK_EQ(osc2_dock_answer(req, 73, ans, &fake_ops, &f), OSC2_DOCK_ANSWERED);
	CHECK_EQ(ans[0], 0xAC);
	CHECK_EQ(ans[1], 3);

	/* a short frame or a bad CRC: no answer, nothing asked, `ans` left alone */
	memset(&f, 0, sizeof(f));
	memset(ans, 0xEE, sizeof(ans));
	(void)opensc2_dock_build_request(OPENSC2_DOCK_PROBE, NULL, 0, req);
	CHECK_EQ(osc2_dock_answer(req, 72, ans, &fake_ops, &f), OSC2_DOCK_BAD_SIZE);
	req[30] = 1;
	CHECK_EQ(osc2_dock_answer(req, 73, ans, &fake_ops, &f), OSC2_DOCK_BAD_CRC);
	CHECK_EQ(f.identity, 0);
	CHECK_EQ(ans[0], 0xEE);
}

/* ---- the pilot, the supply, the docked states ------------------------------------------------ */

OPENSC2_TEST(dock_pilot)
{
	int32_t in[3] = { 2500, 1500, 3300 };
	int32_t one[3] = { 2000, -1, 3301 };
	uint8_t inside = 0xFF;

	CHECK_EQ(osc2_dock_pilot_mv(2276), 2000);
	CHECK_EQ(osc2_dock_pilot_mv(4095), 3599);
	CHECK_EQ(osc2_dock_pilot_mv(-5), -1);
	CHECK(osc2_dock_pilot_in_window(2000));
	CHECK(osc2_dock_pilot_in_window(3300));
	CHECK(!osc2_dock_pilot_in_window(1999));
	CHECK(!osc2_dock_pilot_in_window(3301));
	CHECK(!osc2_dock_pilot_in_window(-1));
	CHECK(osc2_dock_pilot_present(in, &inside));
	CHECK_EQ(inside, 0x05);
	CHECK(!osc2_dock_pilot_present(one, &inside));
	CHECK_EQ(inside, 0x01);
	CHECK(!osc2_dock_pilot_present(one, NULL));
}

OPENSC2_TEST(dock_supply)
{
	struct osc2_dock_power_rule r;
	struct osc2_dock_power_in none = { 0 }, usb = { .usb = true }, dock = { .plus = true,
										 .pilot = true };

	memset(&r, 0, sizeof(r));
	/* the first reading posts, also "nothing" */
	CHECK_EQ(osc2_dock_power_step(&r, &none), OSC2_DOCK_SUPPLY_NONE);
	CHECK_EQ(osc2_dock_power_step(&r, &none), -1);
	CHECK_EQ(osc2_dock_power_step(&r, &dock), OSC2_DOCK_SUPPLY_DOCK);
	CHECK_EQ(osc2_dock_power_step(&r, &dock), -1);
	/* from the dock to a cable with power all along: nothing, it stays the dock */
	CHECK_EQ(osc2_dock_power_step(&r, &usb), -1);
	CHECK_EQ(r.kind, OSC2_DOCK_SUPPLY_DOCK);
	CHECK_EQ(osc2_dock_power_step(&r, &none), OSC2_DOCK_SUPPLY_NONE);
	CHECK_EQ(osc2_dock_power_step(&r, &usb), OSC2_DOCK_SUPPLY_USB);
	dock.pilot = false;
	CHECK_EQ(osc2_dock_supply_kind(&dock), OSC2_DOCK_SUPPLY_USB);   /* + without the pilot */
}

OPENSC2_TEST(dock_states)
{
	struct osc2_dock_sm sm;
	struct osc2_dock_step st;

	/* a cold power-on on the dock, a chord, its release, turned off again, lifted, back */
	osc2_dock_sm_init(&sm);
	osc2_dock_sm_step(&sm, OSC2_DOCK_EV_POWER_DOCK, 0, &st);
	CHECK(!st.handled);   /* outside the dock states: the caller enters them */
	osc2_dock_sm_enter(&sm, OSC2_DOCK_AT_POWER_ON, false, &st);
	CHECK_EQ(st.to, OSC2_DOCK_RADIO_OFF);
	CHECK_EQ(st.actions, OSC2_DOCK_DO_KEYLOCK_CLEAR | OSC2_DOCK_DO_LED_CHARGING);
	osc2_dock_sm_step(&sm, OSC2_DOCK_EV_REBOOT, 0, &st);
	CHECK(!st.handled);   /* a plain reboot does nothing on the dock with the radio off */
	osc2_dock_sm_step(&sm, OSC2_DOCK_EV_CHORD, OSC2_DOCK_CHORD_PUCK1, &st);
	CHECK_EQ(st.to, OSC2_DOCK_CHORD);
	CHECK(st.actions & OSC2_DOCK_DO_TIMER_4S);
	osc2_dock_sm_step(&sm, OSC2_DOCK_EV_CHORD_UP, OSC2_DOCK_CHORD_BT1, &st);
	CHECK(!st.handled);   /* not its chord */
	osc2_dock_sm_step(&sm, OSC2_DOCK_EV_CHORD_UP, OSC2_DOCK_CHORD_PUCK1, &st);
	CHECK_EQ(st.to, OSC2_DOCK_LINK_UP);
	CHECK(st.actions & OSC2_DOCK_DO_START_LINK);
	CHECK(!(st.actions & OSC2_DOCK_DO_PAIRING));
	osc2_dock_sm_step(&sm, OSC2_DOCK_EV_TURN_OFF, 0, &st);
	CHECK_EQ(st.to, OSC2_DOCK_RADIO_OFF);
	CHECK(st.actions & OSC2_DOCK_DO_CHIME_OFF);
	CHECK(st.actions & OSC2_DOCK_DO_STOP_LINK);
	CHECK(!(st.actions & OSC2_DOCK_DO_SHUTDOWN));   /* never a power-off on the dock */
	osc2_dock_sm_step(&sm, OSC2_DOCK_EV_POWER_GONE, 0, &st);
	CHECK_EQ(st.to, OSC2_DOCK_LIFTED);
	CHECK(st.actions & OSC2_DOCK_DO_TIMER_5S);
	osc2_dock_sm_step(&sm, OSC2_DOCK_EV_POWER_DOCK, 0, &st);
	CHECK_EQ(st.to, OSC2_DOCK_RADIO_OFF);
	/* lifted off for good: 5 s, then off */
	osc2_dock_sm_step(&sm, OSC2_DOCK_EV_POWER_GONE, 0, &st);
	osc2_dock_sm_step(&sm, OSC2_DOCK_EV_TIMER, 0, &st);
	CHECK_EQ(st.to, OSC2_DOCK_AWAY);
	CHECK_EQ(st.actions, OSC2_DOCK_DO_TIMER_STOP | OSC2_DOCK_DO_SHUTDOWN);

	/* placed on the dock with the link up, STEAM held 4 s on the dock: pairing flag */
	osc2_dock_sm_init(&sm);
	osc2_dock_sm_enter(&sm, OSC2_DOCK_ON_BATTERY, false, &st);
	CHECK_EQ(st.to, OSC2_DOCK_LINK_UP);
	osc2_dock_sm_step(&sm, OSC2_DOCK_EV_POWER_GONE, 0, &st);
	CHECK_EQ(st.to, OSC2_DOCK_AWAY);
	CHECK_EQ(st.actions, OSC2_DOCK_DO_TO_BATTERY);
	osc2_dock_sm_enter(&sm, OSC2_DOCK_AT_POWER_ON, true, &st);   /* a warm boot on the dock */
	CHECK_EQ(st.to, OSC2_DOCK_LINK_UP);
	CHECK(st.actions & OSC2_DOCK_DO_START_LINK);
	CHECK(!(st.actions & OSC2_DOCK_DO_CUE));
}
