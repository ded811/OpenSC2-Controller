/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only, in both binaries: the unarmed command channel, byte for byte. A fixed-seed
 * generator sends 2000 SET and GET requests on the USB and radio links (every opcode 0x00..0xFF,
 * random lengths, payloads and framing, the test channel's 0x7E with random keys, versions and
 * lengths, settings by name with real and made-up keys), never arming the hardware test hooks
 * (a request that would be a claimed ARM is skipped). Each request becomes one line of the
 * transcript run.ps1 / run.sh pass (--transcript): link, request, result, the stored request after
 * it, the GET answer. The scripts fail when the two binaries' transcripts differ by one byte: the
 * hooks, unarmed, change nothing a host can see.
 */
#include <stdio.h>
#include <string.h>
#include "internal.h"
#include "stubs.h"
#include "test_core.h"

static uint32_t diff_state;

static uint32_t diff_next(void)
{
	/* xorshift32 */
	diff_state ^= diff_state << 13;
	diff_state ^= diff_state >> 17;
	diff_state ^= diff_state << 5;
	return diff_state;
}

static void diff_hex(FILE *f, const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		fprintf(f, "%02x", (unsigned int)p[i]);
	}
}

OPENSC2_TEST(cmd_differential)
{
	static const char *const names[] = {
		"esb/bond", "esb/bond_2", "user/wireless_transport", "user/haptic_boot_level",
		"opensc2/unit_serial", "cal/joy_l",
	};
	/* The test channel's ops (ARM among them: such requests are skipped below). */
	static const uint8_t ops[] = { 0x01, 0x02, 0x03, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
				       0x20, 0x21, 0x22, 0x23, 0x24, 0x30, 0x40, 0x41, 0x42, 0x43,
				       0x44 };
	FILE *f = transcript_path ? fopen(transcript_path, "w") : NULL;
	int done = 0, skipped = 0;

	CHECK(transcript_path == NULL || f != NULL);
	stub_reset();
	diff_state = 0x05C2D1F7u;
	while (done < 2000) {
		uint8_t frame[66], rec[64], reply[64], answer[63], stored_rec[2 + OPENSC2_ANSWER_MAX];
		bool usb = (diff_next() & 1) != 0;
		bool get_only = (diff_next() % 8) == 0;
		/* Every opcode once, then random ones, one in eight the test channel's. */
		uint8_t opcode = done < 256 ? (uint8_t)done :
				 (diff_next() % 8) == 0 ? OSC2_TEST_OPCODE : (uint8_t)diff_next();
		uint8_t n = (uint8_t)(diff_next() % 64);
		uint8_t payload[64];
		int set_rc = 0, get_rc;
		uint8_t stored_len;

		for (size_t i = 0; i < sizeof(payload); i++) {
			payload[i] = (uint8_t)diff_next();
		}
		if (opcode == OSC2_TEST_OPCODE) {
			uint32_t pick = diff_next() % 4;

			if (pick != 2) {
				opensc2_put_le32(payload, OSC2_TEST_KEY);   /* the key, or (2) a random one */
			}
			payload[4] = pick == 1 ? 2 : 1;                     /* (1) version 2 */
			if (diff_next() & 1) {
				payload[6] = ops[diff_next() % sizeof(ops)];   /* a real op, or a random one */
			}
			if (pick == 3) {
				n = (uint8_t)(OSC2_TEST_HEADER_LEN + diff_next() % 55);
			}
			if (opensc2_get_le32(payload) == OSC2_TEST_KEY && payload[4] == OSC2_TEST_VERSION &&
			    payload[6] == OSC2_TEST_OP_ARM) {
				skipped++;
				continue;   /* a claimed ARM would arm the hooks: not part of this test */
			}
		}
		if (opcode >= 0xED && opcode <= 0xF0 && (diff_next() & 1)) {
			const char *name = names[diff_next() % (sizeof(names) / sizeof(names[0]))];
			size_t len = strlen(name) + 1;

			memcpy(payload, name, len);
			n = (uint8_t)(len + diff_next() % 30);
			if (n > 61) {
				n = 61;
			}
		}
		if ((diff_next() % 16) == 0) {
			stub.now_ms += diff_next() % 5000;
		}

		if (usb) {
			uint8_t id = (diff_next() % 16) == 0 ? (uint8_t)(1 + diff_next() % 3) : 1;
			uint16_t flen = (diff_next() % 16) == 0 ? (uint16_t)(63 + diff_next() % 3) : 64;
			uint8_t gop = 0, gn = 0;

			frame[0] = id;
			frame[1] = opcode;
			frame[2] = n;
			memcpy(&frame[3], payload, 63);
			if (!get_only) {
				set_rc = osc2_usb_feature_set(frame, flen);
			}
			memset(reply, 0xEE, sizeof(reply));
			get_rc = osc2_usb_feature_get(reply, 64, 64, &gop, &gn);
			stored_len = osc2_command_stored(OSC2_LINK_USB, stored_rec);
			if (f) {
				fprintf(f, "%04d U %s id=%u len=%u op=%02x n=%u req=", done,
					get_only ? "GET" : "SET", (unsigned int)id, (unsigned int)flen,
					(unsigned int)opcode, (unsigned int)n);
				diff_hex(f, payload, 61);
				fprintf(f, " rc=%d stored=", set_rc);
				diff_hex(f, stored_rec, stored_len);
				fprintf(f, " get=%d op=%02x n=%u ", get_rc, (unsigned int)gop,
					(unsigned int)gn);
				diff_hex(f, reply, get_rc > 0 ? (size_t)get_rc : 0);
				fprintf(f, "\n");
			}
		} else {
			uint8_t rec_len = (uint8_t)(2 + diff_next() % 62);
			uint8_t answer_len;

			rec[0] = opcode;
			rec[1] = n;
			memcpy(&rec[2], payload, 62);
			if (!get_only) {
				set_rc = osc2_command_set(OSC2_LINK_RADIO, rec, rec_len);
			}
			answer_len = osc2_command_get(OSC2_LINK_RADIO, answer);
			stored_len = osc2_command_stored(OSC2_LINK_RADIO, stored_rec);
			if (f) {
				fprintf(f, "%04d R %s len=%u op=%02x n=%u req=", done,
					get_only ? "GET" : "SET", (unsigned int)rec_len, (unsigned int)opcode,
					(unsigned int)n);
				diff_hex(f, payload, 61);
				fprintf(f, " rc=%d stored=", set_rc);
				diff_hex(f, stored_rec, stored_len);
				fprintf(f, " get=");
				diff_hex(f, answer, answer_len);
				fprintf(f, "\n");
			}
		}
		done++;
	}
	if (f) {
		fprintf(f, "requests %d, claimed-ARM requests skipped %d\n", done, skipped);
		fclose(f);
	}
	CHECK_EQ(done, 2000);
	CHECK(skipped < 100);
}
