/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * dock_codec.c: the dock's 73-byte frames and their CRC (<opensc2/dock.h>). Pure C: the host
 * tests build it, and tests/vectors/hwtest/dock.txt holds the vectors both sides check.
 */
#include <string.h>
#include <opensc2/dock.h>

uint16_t opensc2_dock_crc16(uint16_t crc, const uint8_t *data, size_t len)
{
	/* Reflected: the low bit first, the polynomial 0x1021 mirrored to 0x8408. */
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int bit = 0; bit < 8; bit++) {
			crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0x8408u) : (uint16_t)(crc >> 1);
		}
	}
	return crc;
}

enum opensc2_dock_check opensc2_dock_frame_check(const uint8_t *frame, size_t len)
{
	if (len != OPENSC2_DOCK_FRAME_LEN) {
		return OPENSC2_DOCK_FRAME_BAD_SIZE;
	}
	/* With the CRC stored low byte first, the CRC over the whole frame is 0 when it matches. */
	return opensc2_dock_crc16(0, frame, OPENSC2_DOCK_FRAME_LEN) == 0 ? OPENSC2_DOCK_FRAME_OK :
									    OPENSC2_DOCK_FRAME_BAD_CRC;
}

void opensc2_dock_frame_seal(uint8_t frame[OPENSC2_DOCK_FRAME_LEN], uint8_t start)
{
	uint16_t crc;

	frame[0] = start;
	crc = opensc2_dock_crc16(0, frame, OPENSC2_DOCK_CRC_OFF);
	frame[OPENSC2_DOCK_CRC_OFF] = (uint8_t)crc;
	frame[OPENSC2_DOCK_CRC_OFF + 1] = (uint8_t)(crc >> 8);
}

size_t opensc2_dock_build_request(uint8_t type, const uint8_t *payload, size_t len,
				  uint8_t out[OPENSC2_DOCK_FRAME_LEN])
{
	if (len > OPENSC2_DOCK_PAYLOAD_LEN) {
		return 0;
	}
	memset(out, 0, OPENSC2_DOCK_FRAME_LEN);
	out[OPENSC2_DOCK_TYPE_OFF] = type;
	if (len) {
		memcpy(&out[OPENSC2_DOCK_PAYLOAD_OFF], payload, len);
	}
	opensc2_dock_frame_seal(out, OPENSC2_DOCK_START_PUCK);
	return OPENSC2_DOCK_FRAME_LEN;
}
