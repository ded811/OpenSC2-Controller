/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * The wire formats of the Steam Controller 2's 2.4 GHz link to the puck, as pure functions: no
 * kernel, no radio, no allocation. The library uses them; tools and tests can too. The formats
 * come from the published protocol documentation (see docs/protocol.md).
 *
 * All multi-byte values on the wire are little endian.
 */
#ifndef OPENSC2_CODEC_H_
#define OPENSC2_CODEC_H_

#include <opensc2/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Radio packets: the first byte of every ESB payload -------------------------------------- */

/* Puck to controller. */
#define OPENSC2_PKT_PAIRING_OFFER     0xE0  /* puck ID u32, puck serial (16 bytes) */
#define OPENSC2_PKT_CONNECT_REQUEST   0xE1  /* 18 bytes, no acknowledgement */
#define OPENSC2_PKT_HELLO             0xE2  /* also the pairing beacon */
#define OPENSC2_PKT_KEEPALIVE         0xE3  /* followed by records */
#define OPENSC2_PKT_CHANNEL_CHANGE    0xE4  /* 1 to 4 channels */
#define OPENSC2_PKT_TURN_OFF          0xE5
#define OPENSC2_PKT_HOST_ASLEEP       0xE6
#define OPENSC2_PKT_PROTOCOL_VERSION  0xE7  /* {a, b} */
/* Controller to puck, always inside an acknowledgement. */
#define OPENSC2_PKT_PAIRING_REQUEST   0xF0  /* temporary ID u32, unit serial (16 bytes) */
#define OPENSC2_PKT_APP_DATA          0xF1  /* records */
#define OPENSC2_PKT_DISCONNECTING     0xF2
#define OPENSC2_PKT_VERSION_ANSWER    0xF3  /* {version} */
#define OPENSC2_PKT_PAIRING_CONFIRM   0xF4  /* controller ID u32, unit serial (16 bytes) */
#define OPENSC2_PKT_IDENTITY          0xF5  /* the 57-byte identity record */

/* Records {length, type, data[length]} inside 0xE3 and 0xF1. */
#define OPENSC2_REC_SET_FEATURE   1   /* puck: {opcode, length, payload} */
#define OPENSC2_REC_SET_RESULT    2   /* controller: s32 return code */
#define OPENSC2_REC_GET_FEATURE   3   /* puck: {opcode} */
#define OPENSC2_REC_GET_ANSWER    4   /* controller: {opcode, length, data} */
#define OPENSC2_REC_OUTPUT        5   /* puck: output report, ID first */
#define OPENSC2_REC_INPUT         6   /* controller: input report, ID first */

/* Largest acknowledgement payload this library sends (the controller's own limit). */
#define OPENSC2_PAYLOAD_MAX       127
/* Largest answer to a vendor command: 63 bytes with opcode and length. */
#define OPENSC2_ANSWER_MAX        61

/* Radio constants of the link. */
#define OPENSC2_COMMON_CHANNEL    2     /* pairing and connection requests */
#define OPENSC2_PIPE0_PREFIX      0x10
extern const uint8_t opensc2_common_address[4];   /* "ibex" */
extern const uint8_t opensc2_pairing_address[4];  /* 91 A2 A7 93 */

/* ---- Input and output reports --------------------------------------------------------------- */

#define OPENSC2_RPT_MOUSE          0x40   /* 5 bytes: lizard mode */
#define OPENSC2_RPT_KEYBOARD       0x41   /* 8 bytes: lizard mode */
#define OPENSC2_RPT_STATE          0x42   /* 53 bytes */
#define OPENSC2_RPT_BATTERY        0x43   /* 14 bytes */
#define OPENSC2_RPT_HAPTIC_STATUS  0x44   /* 5 bytes */

#define OPENSC2_STATE_REPORT_LEN    54    /* with the ID */
#define OPENSC2_BATTERY_REPORT_LEN  15
#define OPENSC2_BATTERY_DATA_LEN    15    /* answer of vendor command 0xBE */
#define OPENSC2_HAPTIC_STATUS_LEN   6

/* Bytes after the ID of an input report the puck lets through, 0 for an unknown ID. */
uint8_t opensc2_input_report_size(uint8_t id);

/* Encode the state report 0x42 (ID included). The button word is taken as given; the trackpads'
 * `touched` and `clicked` flags are ORed into their bits. Returns OPENSC2_STATE_REPORT_LEN. */
size_t opensc2_encode_state(const struct opensc2_state *s, uint8_t seq,
			    uint8_t out[OPENSC2_STATE_REPORT_LEN]);
/* The reverse, for tests and tools. Pad touch and click flags are taken from the button word. */
void opensc2_decode_state(const uint8_t in[OPENSC2_STATE_REPORT_LEN], struct opensc2_state *s,
			  uint8_t *seq);

/* Battery report 0x43 (ID included), and the 15-byte answer of vendor command 0xBE. */
size_t opensc2_encode_battery(const struct opensc2_battery *b,
			      uint8_t out[OPENSC2_BATTERY_REPORT_LEN]);
size_t opensc2_encode_battery_data(const struct opensc2_battery *b,
				   uint8_t out[OPENSC2_BATTERY_DATA_LEN]);

/* Haptic stream status 0x44 (ID included). */
size_t opensc2_encode_haptic_status(uint8_t channel_id, uint32_t flags,
				    uint8_t out[OPENSC2_HAPTIC_STATUS_LEN]);

/* Decode output report `report` (ID first, `len` bytes in all) into up to two outputs; report
 * 0x88 carries samples for both grips and becomes two. Returns the number of outputs (0 for an ID
 * that does nothing, such as 0x8A), or -1 when the report is too short for its ID. PCM data points
 * into `report`. */
int opensc2_decode_output(const uint8_t *report, size_t len, struct opensc2_output out[2]);

/* Channels (OPENSC2_HAPTIC_*) a target byte selects in output report `report_id`. */
uint8_t opensc2_target_channels(uint8_t report_id, uint8_t target);

/* ---- Identity -------------------------------------------------------------------------------- */

/* Problems opensc2_identity_check() can report, as a bit mask (0 = valid). */
#define OPENSC2_ID_BAD_UNIT_SERIAL    OPENSC2_BIT(0)
#define OPENSC2_ID_BAD_BOARD_SERIAL   OPENSC2_BIT(1)
#define OPENSC2_ID_BAD_GIT_REVISION   OPENSC2_BIT(2)
#define OPENSC2_ID_BAD_PRODUCT_ID     OPENSC2_BIT(3)

/* A serial must be exactly 13 characters of 0-9 and A-Z, as the real controller requires. */
bool opensc2_serial_valid(const char *s);
/* Git revision: 1 to 12 printable ASCII characters. */
bool opensc2_git_revision_valid(const char *s);
uint32_t opensc2_identity_check(const struct opensc2_identity *id);

/* The 57-byte identity record: product ID, build time, bootloader build time, board revision
 * (u32 each), unit serial (14 bytes), PCBA serial (14), git revision (13). */
size_t opensc2_encode_identity(const struct opensc2_identity *id,
			       uint8_t out[OPENSC2_IDENTITY_RECORD_LEN]);

/* Answers of vendor commands 0x83 (six {tag u8, value u32} pairs, 30 bytes) and 0xAE (the tag,
 * then the string zero padded, 20 bytes). `interval_us` is attribute 11, the report interval. */
size_t opensc2_encode_attributes(const struct opensc2_identity *id, uint32_t interval_us,
				 uint8_t out[30]);
size_t opensc2_encode_string_attribute(const struct opensc2_identity *id, uint8_t tag,
				       uint8_t out[20]);

/* ---- Bonds ----------------------------------------------------------------------------------- */

void opensc2_bond_pack(const struct opensc2_bond *b, uint8_t out[OPENSC2_BOND_LEN]);
void opensc2_bond_unpack(const uint8_t in[OPENSC2_BOND_LEN], struct opensc2_bond *b);
/* A bond is valid when both IDs are non-zero. */
bool opensc2_bond_valid(const struct opensc2_bond *b);

/* ---- Packets of the link --------------------------------------------------------------------- */

struct opensc2_connect_request {
	uint32_t puck_id;
	uint32_t controller_id;
	uint8_t channel;
	uint8_t address[4];   /* the puck's own base address */
	uint8_t prefix;       /* the slot's pipe prefix */
};

/* Parse an 0xE1 connection request; false unless it is one. */
bool opensc2_parse_connect_request(const uint8_t *pkt, size_t len,
				   struct opensc2_connect_request *req);

/* Version from an 0xE7 packet: 0 or 1, or -1 when the controller must refuse it. */
int opensc2_protocol_version(const uint8_t *pkt, size_t len);

/* Channels of an 0xE4 packet into `channels` (0xFF for unused entries); returns 1..4, or 0 when
 * the packet carries none or a channel above 100. */
int opensc2_parse_channel_change(const uint8_t *pkt, size_t len, uint8_t channels[4]);

/* Pairing packets: 0xF0 / 0xF4 from the controller, 0xE0 from the puck, 21 bytes each. */
#define OPENSC2_PAIRING_PKT_LEN 21
size_t opensc2_build_pairing(uint8_t type, uint32_t id, const char *serial,
			     uint8_t out[OPENSC2_PAIRING_PKT_LEN]);
bool opensc2_parse_pairing_offer(const uint8_t *pkt, size_t len, uint32_t *puck_id,
				 char serial[17]);

/* Address bytes the radio cannot use well (they look like the preamble or idle). */
bool opensc2_address_byte_ok(uint8_t b);

/* Records. `*pos` starts at 1 (after the packet type) and is advanced by each call. Returns 1 with
 * a record, 0 at the end, -1 when the packet is malformed (stop reading it). */
struct opensc2_record {
	uint8_t type;
	uint8_t len;
	const uint8_t *data;
};
int opensc2_record_next(const uint8_t *pkt, size_t len, size_t *pos, struct opensc2_record *rec);

/* Building an acknowledgement payload. */
struct opensc2_payload {
	uint8_t len;
	uint8_t data[128];
};
void opensc2_payload_start(struct opensc2_payload *p, uint8_t type);
/* Append {len, type, data}; returns 0, or -1 when it would pass OPENSC2_PAYLOAD_MAX. */
int opensc2_payload_add_record(struct opensc2_payload *p, uint8_t type, const uint8_t *data,
			       uint8_t len);
/* Append raw bytes (for 0xF3 and 0xF5). */
int opensc2_payload_add_bytes(struct opensc2_payload *p, const uint8_t *data, size_t len);

/* ---- Little-endian helpers ------------------------------------------------------------------- */

static inline void opensc2_put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void opensc2_put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static inline uint16_t opensc2_get_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t opensc2_get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

#ifdef __cplusplus
}
#endif

#endif /* OPENSC2_CODEC_H_ */
