/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * The wire formats of the Steam Controller 2's 2.4 GHz link to the puck and of its USB device, as
 * pure functions: no kernel, no radio, no allocation. The library uses them; tools and tests can
 * too. The formats come from the protocol documentation the same authors wrote (to be published in
 * steam-controller-2-reverse-engineered, not public yet; docs/protocol.md sums it up and lists the
 * pages). Three details of the report descriptor come directly from the same authors' study of the
 * controller's firmware: the sizes of its mouse and keyboard collections, the form of its vendor
 * items and the order of its reports (lib/opensc2/usb_desc.c and docs/protocol.md, gap 13, name
 * the source of each detail).
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

/* Battery report 0x43 (ID included), and the 15-byte answer of vendor command 0xBE: temperature,
 * battery, system and input mV, current and input current, the level (at most 100), 1 while the
 * charge state is "charging" (else 0), and the charge state without a 0x80 flag. */
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

/* Channels (OPENSC2_HAPTIC_*) a target byte selects in output report `report_id`, the real
 * controller's way: 0x81 swaps the exact targets 0 and 1 (0 is the right pad there), while 0x80
 * and 0x81 go through the common table with bit 7 cleared (the left, the right pad). */
uint8_t opensc2_target_channels(uint8_t report_id, uint8_t target);

/* ---- Identity -------------------------------------------------------------------------------- */

/* Problems opensc2_identity_check() can report, as a bit mask (0 = valid). */
#define OPENSC2_ID_BAD_UNIT_SERIAL    OPENSC2_BIT(0)
#define OPENSC2_ID_BAD_BOARD_SERIAL   OPENSC2_BIT(1)
#define OPENSC2_ID_BAD_GIT_REVISION   OPENSC2_BIT(2)
#define OPENSC2_ID_BAD_PRODUCT_ID     OPENSC2_BIT(3)
#define OPENSC2_ID_BAD_PRODUCT_NAME   OPENSC2_BIT(4)

/* A serial must be exactly 13 characters of 0-9 and A-Z, as the real controller requires. */
bool opensc2_serial_valid(const char *s);
/* Git revision: 1 to 12 printable ASCII characters. */
bool opensc2_git_revision_valid(const char *s);
/* The USB product name: 1 to OPENSC2_PRODUCT_NAME_MAX (31) printable ASCII characters (0x20 to
 * 0x7E, the only ones a USB string descriptor of the stack may carry), spaces allowed inside but
 * not at either end. */
bool opensc2_product_name_valid(const char *s);
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

/* ---- USB: the wired Steam Controller 2 -------------------------------------------------------
 * Over its cable the controller is one full-speed device 28DE:1302 with one HID interface: a mouse
 * collection (report 0x40), a keyboard collection (0x41) and the vendor collection with every other
 * report, in that order, so the vendor one is Windows' third collection ("Col03"). Feature report
 * 1 carries the same vendor commands as the radio link.
 */

#define OPENSC2_USB_VID              0x28DE
#define OPENSC2_USB_PID              0x1302

/* The report descriptor: 67 bytes of mouse, 41 of keyboard, 264 of vendor collection. */
#define OPENSC2_HID_REPORT_DESC_LEN  372
extern const uint8_t opensc2_hid_report_desc[OPENSC2_HID_REPORT_DESC_LEN];

/* Feature report 1: 64 bytes with the ID, `01 <opcode> <n> <payload>`. */
#define OPENSC2_USB_FEATURE_ID       1
#define OPENSC2_USB_FEATURE_LEN      64

/* The two forms of the real controller's USB device (its July and October 2026 firmware), as the
 * protocol documentation gives them (not public yet; docs/protocol.md, gap 14, has both). They
 * differ only in these fields; everything else (descriptors, reports, commands) is the same. */
struct opensc2_usb_profile {
	uint16_t bcd_usb;
	uint8_t device_class;
	uint8_t device_subclass;
	uint8_t device_protocol;
	uint16_t bcd_device;
	bool bos_usb20_ext;      /* a BOS descriptor with a USB 2.0 extension (no LPM) */
	uint8_t get_reply_len;   /* bytes of a GET_REPORT(Feature 1) reply, ID included */
};

/* The July 2026 controller firmware, the form Steam was seen pairing over the cable: USB 2.00, no
 * BOS, class EF/02/01, release 3.07, 64-byte GET replies. */
extern const struct opensc2_usb_profile opensc2_usb_profile_july;
/* The October 2026 firmware: USB 2.01 with a USB 2.0 extension, class 00/00/00, release 4.04,
 * 63-byte GET replies. */
extern const struct opensc2_usb_profile opensc2_usb_profile_october;

/* The descriptors a host should read: the 18-byte device descriptor of a profile (strings 1, 2
 * and 3, one configuration), the 41-byte configuration descriptor (configuration, interface 0, HID,
 * interrupt IN 0x81 and OUT 0x01 of 64 bytes every 1 ms, bus powered with remote wakeup, 500 mA)
 * and the 9-byte HID descriptor. Each returns its length. */
size_t opensc2_usb_device_descriptor(const struct opensc2_usb_profile *p, uint8_t out[18]);
size_t opensc2_usb_config_descriptor(uint8_t out[41]);
size_t opensc2_usb_hid_descriptor(uint8_t out[9]);

/* Why a SET_REPORT(Feature) data stage is refused (opensc2_usb_feature_check()). */
enum opensc2_usb_set_error {
	OPENSC2_USB_SET_OK = 0,
	OPENSC2_USB_SET_LENGTH = 1,   /* not exactly 64 bytes */
	OPENSC2_USB_SET_ID = 2,       /* byte 0 is neither 1 nor 2 */
	OPENSC2_USB_SET_N = 3,        /* the payload length byte is above 61 */
};

/* Check the data stage of SET_REPORT(Feature 1). On success *rec points at {opcode, n, payload}
 * (63 bytes) and OPENSC2_USB_SET_OK is returned. */
enum opensc2_usb_set_error opensc2_usb_feature_check(const uint8_t *data, size_t len,
						      const uint8_t **rec);

/* Build a GET_REPORT(Feature 1) reply: `01 <opcode> <n> <answer>`, zeros after the answer,
 * `reply_len` bytes (64 or 63) but never more than the host's wLength. Returns the bytes written. */
size_t opensc2_usb_feature_reply(uint8_t opcode, const uint8_t *answer, uint8_t n,
				 size_t reply_len, size_t wlength, uint8_t *out);

/* A short description of a control request for logs, for example
 * "80 06 0303 0409 0202 GET_DESCRIPTOR string 3". */
void opensc2_usb_setup_text(const uint8_t setup[8], char *buf, size_t size);

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
