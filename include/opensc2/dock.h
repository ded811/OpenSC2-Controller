/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * The dock's frames: the slow serial link a Steam Controller 2 and Valve's puck run over the
 * puck's charging pads while the controller rests on it. Pure functions with no kernel, like the
 * radio formats in <opensc2/codec.h>: the library's dock link uses them, and so can tools, tests
 * and a fake puck. The format comes from the protocol documentation (not public yet;
 * docs/protocol.md lists the pages).
 *
 * Every message, in both directions, is one 73-byte frame:
 *
 *   [0]       start byte: 0x5A from the puck, 0xAC from the controller (neither side checks it)
 *   [1]       request type, copied into the answer
 *   [2..3]    unused (0)
 *   [4..7]    result, a signed 32-bit value in answers, little endian
 *   [8..70]   payload, 63 bytes
 *   [71..72]  CRC-16/KERMIT of bytes 0 to 70, little endian
 *
 * The puck asks, the controller answers, one frame at a time, at 115200 baud, 8N1, no flow
 * control (one frame takes about 6.3 ms on the wire).
 */
#ifndef OPENSC2_DOCK_H_
#define OPENSC2_DOCK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OPENSC2_DOCK_FRAME_LEN         73
#define OPENSC2_DOCK_TYPE_OFF          1
#define OPENSC2_DOCK_RESULT_OFF        4
#define OPENSC2_DOCK_PAYLOAD_OFF       8
#define OPENSC2_DOCK_PAYLOAD_LEN       63
#define OPENSC2_DOCK_CRC_OFF           71

#define OPENSC2_DOCK_START_PUCK        0x5A
#define OPENSC2_DOCK_START_CONTROLLER  0xAC

/* Request types (byte 1). */
#define OPENSC2_DOCK_PROBE   0   /* answer: the 57-byte identity record, as radio packet 0xF5 */
#define OPENSC2_DOCK_GET     1   /* answer: {opcode, length, data} of the command last selected on
				  * the dock; result 63, or a negative error */
#define OPENSC2_DOCK_SET     2   /* payload {opcode, length, data}: run the vendor command; result
				  * 0, or a negative error */
#define OPENSC2_DOCK_INPUT   3   /* answer: the latest state report 0x42 without its ID (53 bytes);
				  * result 0 */

/* The link: 115200 baud, 8N1, no flow control; a silence of 64 ms inside a frame ends it. */
#define OPENSC2_DOCK_BAUD             115200
#define OPENSC2_DOCK_GAP_TIMEOUT_MS   64
/* Attribute 11 of vendor command 0x83 while the dock is the controller's current link, in µs. */
#define OPENSC2_DOCK_INTERVAL_US      64000

/* CRC-16/KERMIT: polynomial 0x1021 processed bit-reversed (0x8408), input and output reflected,
 * no final XOR; `crc` is the value so far (0 to start). Its check value for "123456789" is
 * 0x2189. Zephyr's crc16_ccitt(0, ...) computes the same. */
uint16_t opensc2_dock_crc16(uint16_t crc, const uint8_t *data, size_t len);

enum opensc2_dock_check {
	OPENSC2_DOCK_FRAME_OK = 0,
	OPENSC2_DOCK_FRAME_BAD_SIZE = 1,   /* not exactly 73 bytes */
	OPENSC2_DOCK_FRAME_BAD_CRC = 2,    /* the CRC over all 73 bytes is not 0 */
};

/* Check a received frame of `len` bytes the way the controller does: the length, then the CRC run
 * over all 73 bytes, stored CRC included, which must come out 0. */
enum opensc2_dock_check opensc2_dock_frame_check(const uint8_t *frame, size_t len);

/* Make `frame` ready to send: its start byte at [0] and the CRC of bytes 0 to 70 at [71..72]. */
void opensc2_dock_frame_seal(uint8_t frame[OPENSC2_DOCK_FRAME_LEN], uint8_t start);

/* A request as the puck builds it: start byte 0x5A, the type, zeros at [2..7], `len` payload
 * bytes (at most 63; the rest zero) and the CRC. Returns OPENSC2_DOCK_FRAME_LEN, or 0 when `len`
 * is above 63. */
size_t opensc2_dock_build_request(uint8_t type, const uint8_t *payload, size_t len,
				  uint8_t out[OPENSC2_DOCK_FRAME_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* OPENSC2_DOCK_H_ */
