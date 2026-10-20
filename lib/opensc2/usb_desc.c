/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * usb_desc.c: what the wired Steam Controller 2 looks like on USB, as pure data and functions (no
 * Zephyr headers, so the host tests build it too): the HID report descriptor, the two device
 * profiles, the descriptors a host reads, and the framing of feature report 1.
 *
 * Where each detail of the report descriptor comes from (docs/protocol.md, gap 13, says the same):
 *  - the USB page of the protocol documentation the same authors wrote (not public yet): 372 bytes;
 *    the three collections in this order (mouse, keyboard, vendor); every report's ID, direction
 *    and size; the vendor collection's item rule (every field a byte, with a usage equal to its
 *    report ID);
 *  - Counterpuck's published wire-formats.md: the fields of the mouse and keyboard reports, the
 *    vendor fields' range (8 bits, 0..255) and the feature reports' usage 1;
 *  - the same authors' study of the controller's firmware: the sizes of the mouse and keyboard
 *    collections (67 and 41 bytes; that documentation gives them too, for the puck's slot
 *    interfaces, whose descriptor it calls the controller's), the form of the vendor items (every
 *    input and output report states its ID, logical minimum 0, a two-byte logical maximum of 255,
 *    report size 8, its count, its usage and its main item: 15 bytes; the two feature reports
 *    state only ID, count, usage 1 and main item: 8 bytes) and the order of the vendor reports
 *    (these two not in that documentation yet).
 * No source gives every byte of the mouse and keyboard collections: they follow the usual boot
 * layouts at those sizes, and the two items marked "a guess" below are what makes the sizes fit.
 */
#include <stdio.h>
#include <string.h>
#include <opensc2/codec.h>

/* One vendor input or output report: `n` bytes after the ID. */
#define VENDOR_INPUT(id, n)                                                                 \
	0x85, (id), 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, (n), 0x09, (id), 0x81, 0x02
#define VENDOR_OUTPUT(id, n)                                                                \
	0x85, (id), 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, (n), 0x09, (id), 0x91, 0x02
/* A feature report of 63 bytes after the ID, inheriting range and size. */
#define VENDOR_FEATURE(id) 0x85, (id), 0x95, 0x3F, 0x09, 0x01, 0xB1, 0x02

const uint8_t opensc2_hid_report_desc[OPENSC2_HID_REPORT_DESC_LEN] = {
	/* ---- 1. Mouse, report 0x40: buttons 1 and 2, X, Y, wheel, horizontal pan (67 bytes) */
	0x05, 0x01,          /* Usage Page (Generic Desktop) */
	0x09, 0x02,          /* Usage (Mouse) */
	0xA1, 0x01,          /* Collection (Application) */
	0x85, 0x40,          /*   Report ID (0x40) */
	0x09, 0x01,          /*   Usage (Pointer) */
	0xA1, 0x00,          /*   Collection (Physical) */
	0x05, 0x09,          /*     Usage Page (Button) */
	0x19, 0x01,          /*     Usage Minimum (1) */
	0x29, 0x02,          /*     Usage Maximum (2) */
	0x15, 0x00,          /*     Logical Minimum (0) */
	0x25, 0x01,          /*     Logical Maximum (1) */
	0x95, 0x02,          /*     Report Count (2) */
	0x75, 0x01,          /*     Report Size (1) */
	0x81, 0x02,          /*     Input (Data, Variable, Absolute): the two buttons */
	0x95, 0x01,          /*     Report Count (1) */
	0x75, 0x06,          /*     Report Size (6) */
	0x81, 0x01,          /*     Input (Constant): padding */
	0x05, 0x01,          /*     Usage Page (Generic Desktop) */
	0x09, 0x30,          /*     Usage (X) */
	0x09, 0x31,          /*     Usage (Y) */
	0x15, 0x81,          /*     Logical Minimum (-127) */
	0x25, 0x7F,          /*     Logical Maximum (127) */
	0x75, 0x08,          /*     Report Size (8) */
	0x95, 0x02,          /*     Report Count (2) */
	0x81, 0x06,          /*     Input (Data, Variable, Relative): X, Y */
	0x09, 0x38,          /*     Usage (Wheel) */
	0x95, 0x01,          /*     Report Count (1) */
	0x81, 0x06,          /*     Input (Data, Variable, Relative): wheel */
	0x05, 0x0C,          /*     Usage Page (Consumer) */
	0x0A, 0x38, 0x02,    /*     Usage (AC Pan) */
	0x95, 0x01,          /*     Report Count (1), repeated: a guess (see the top) */
	0x81, 0x06,          /*     Input (Data, Variable, Relative): horizontal pan */
	0xC0,                /*   End Collection */
	0xC0,                /* End Collection */

	/* ---- 2. Keyboard, report 0x41: modifiers, a reserved byte, six keys; no LEDs (41 bytes) */
	0x05, 0x01,          /* Usage Page (Generic Desktop) */
	0x09, 0x06,          /* Usage (Keyboard) */
	0xA1, 0x01,          /* Collection (Application) */
	0x85, 0x41,          /*   Report ID (0x41) */
	0x05, 0x07,          /*   Usage Page (Keyboard/Keypad) */
	0x19, 0xE0,          /*   Usage Minimum (Left Control) */
	0x29, 0xE7,          /*   Usage Maximum (Right GUI) */
	0x15, 0x00,          /*   Logical Minimum (0) */
	0x25, 0x01,          /*   Logical Maximum (1) */
	0x75, 0x01,          /*   Report Size (1) */
	0x95, 0x08,          /*   Report Count (8) */
	0x81, 0x02,          /*   Input (Data, Variable, Absolute): modifiers */
	0x81, 0x01,          /*   Input (Constant): reserved byte */
	0x95, 0x06,          /*   Report Count (6) */
	0x75, 0x08,          /*   Report Size (8) */
	0x15, 0x00,          /*   Logical Minimum (0), restated: a guess (see the top) */
	0x25, 0x65,          /*   Logical Maximum (0x65) */
	0x19, 0x00,          /*   Usage Minimum (0) */
	0x29, 0x65,          /*   Usage Maximum (0x65) */
	0x81, 0x00,          /*   Input (Data, Array, Absolute): six key codes */
	0xC0,                /* End Collection */

	/* ---- 3. The vendor collection: every other report (264 bytes) */
	0x06, 0x00, 0xFF,    /* Usage Page (Vendor 0xFF00) */
	0x09, 0x01,          /* Usage (1) */
	0xA1, 0x01,          /* Collection (Application) */
	VENDOR_INPUT(0x42, 53),    /* state */
	VENDOR_INPUT(0x44, 5),     /* haptic stream status */
	VENDOR_INPUT(0x79, 1),     /* declared, never sent */
	VENDOR_INPUT(0x43, 14),    /* battery */
	VENDOR_INPUT(0x7B, 12),    /* declared, never sent */
	VENDOR_INPUT(0x45, 45),    /* the Bluetooth state report, never sent on USB */
	VENDOR_OUTPUT(0x80, 9),    /* rumble */
	VENDOR_OUTPUT(0x81, 7),    /* pulse */
	VENDOR_OUTPUT(0x82, 3),    /* command */
	VENDOR_OUTPUT(0x83, 9),    /* tone */
	VENDOR_OUTPUT(0x84, 8),    /* sweep */
	VENDOR_OUTPUT(0x85, 3),    /* script */
	VENDOR_OUTPUT(0x86, 3),    /* PCM stream control */
	VENDOR_OUTPUT(0x87, 63),   /* PCM data */
	VENDOR_OUTPUT(0x89, 63),   /* PCM data with a length */
	VENDOR_OUTPUT(0x88, 63),   /* PCM data for both grips */
	VENDOR_FEATURE(0x01),      /* the vendor command channel */
	VENDOR_FEATURE(0x02),      /* declared; refused on USB */
	0xC0,                /* End Collection */
};

/* ---- profiles and descriptors ------------------------------------------------------------- */

const struct opensc2_usb_profile opensc2_usb_profile_july = {
	.bcd_usb = 0x0200,
	.device_class = 0xEF,
	.device_subclass = 0x02,
	.device_protocol = 0x01,
	.bcd_device = 0x0307,
	.bos_usb20_ext = false,
	.get_reply_len = 64,
};

const struct opensc2_usb_profile opensc2_usb_profile_october = {
	.bcd_usb = 0x0201,
	.device_class = 0x00,
	.device_subclass = 0x00,
	.device_protocol = 0x00,
	.bcd_device = 0x0404,
	.bos_usb20_ext = true,
	.get_reply_len = 63,
};

size_t opensc2_usb_device_descriptor(const struct opensc2_usb_profile *p, uint8_t out[18])
{
	out[0] = 18;                       /* bLength */
	out[1] = 0x01;                     /* DEVICE */
	opensc2_put_le16(&out[2], p->bcd_usb);
	out[4] = p->device_class;
	out[5] = p->device_subclass;
	out[6] = p->device_protocol;
	out[7] = 64;                       /* bMaxPacketSize0 */
	opensc2_put_le16(&out[8], OPENSC2_USB_VID);
	opensc2_put_le16(&out[10], OPENSC2_USB_PID);
	opensc2_put_le16(&out[12], p->bcd_device);
	out[14] = 1;                       /* iManufacturer */
	out[15] = 2;                       /* iProduct */
	out[16] = 3;                       /* iSerialNumber */
	out[17] = 1;                       /* bNumConfigurations */
	return 18;
}

size_t opensc2_usb_hid_descriptor(uint8_t out[9])
{
	out[0] = 9;                        /* bLength */
	out[1] = 0x21;                     /* HID */
	opensc2_put_le16(&out[2], 0x0111); /* bcdHID 1.11 */
	out[4] = 0;                        /* bCountryCode */
	out[5] = 1;                        /* bNumDescriptors */
	out[6] = 0x22;                     /* REPORT */
	opensc2_put_le16(&out[7], OPENSC2_HID_REPORT_DESC_LEN);
	return 9;
}

size_t opensc2_usb_config_descriptor(uint8_t out[41])
{
	static const uint8_t head[18] = {
		/* configuration: 41 bytes, one interface, value 1, no string, bus powered with
		 * remote wakeup, 500 mA */
		0x09, 0x02, 0x29, 0x00, 0x01, 0x01, 0x00, 0xA0, 0xFA,
		/* interface 0, alternate 0, two endpoints, HID, no subclass or protocol, no string */
		0x09, 0x04, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,
	};
	static const uint8_t endpoints[14] = {
		0x07, 0x05, 0x81, 0x03, 0x40, 0x00, 0x01,   /* interrupt IN 0x81, 64 bytes, 1 ms */
		0x07, 0x05, 0x01, 0x03, 0x40, 0x00, 0x01,   /* interrupt OUT 0x01, 64 bytes, 1 ms */
	};

	memcpy(out, head, sizeof(head));
	opensc2_usb_hid_descriptor(&out[18]);
	memcpy(&out[27], endpoints, sizeof(endpoints));
	return 41;
}

/* ---- feature report 1 ---------------------------------------------------------------------- */

enum opensc2_usb_set_error opensc2_usb_feature_check(const uint8_t *data, size_t len,
						      const uint8_t **rec)
{
	if (len != OPENSC2_USB_FEATURE_LEN) {
		return OPENSC2_USB_SET_LENGTH;
	}
	/* The controller's command dispatcher takes report IDs 1 and 2 from the data; on USB the
	 * request's wValue has already said 1. */
	if (data[0] != 1 && data[0] != 2) {
		return OPENSC2_USB_SET_ID;
	}
	if (data[2] > OPENSC2_ANSWER_MAX) {
		return OPENSC2_USB_SET_N;
	}
	*rec = &data[1];
	return OPENSC2_USB_SET_OK;
}

size_t opensc2_usb_feature_reply(uint8_t opcode, const uint8_t *answer, uint8_t n,
				 size_t reply_len, size_t wlength, uint8_t *out)
{
	uint8_t full[OPENSC2_USB_FEATURE_LEN];
	size_t len = reply_len < wlength ? reply_len : wlength;

	if (len > sizeof(full)) {
		len = sizeof(full);
	}
	if (n > OPENSC2_ANSWER_MAX) {
		n = OPENSC2_ANSWER_MAX;
	}
	memset(full, 0, sizeof(full));
	full[0] = OPENSC2_USB_FEATURE_ID;
	full[1] = opcode;
	full[2] = n;
	if (n) {
		memcpy(&full[3], answer, n);
	}
	memcpy(out, full, len);
	return len;
}

/* ---- log text for a setup packet ----------------------------------------------------------- */

static const char *descriptor_name(uint8_t type)
{
	switch (type) {
	case 0x01: return "device";
	case 0x02: return "configuration";
	case 0x03: return "string";
	case 0x06: return "device qualifier";
	case 0x07: return "other speed";
	case 0x0F: return "BOS";
	case 0x21: return "HID";
	case 0x22: return "report";
	default:   return "type";
	}
}

void opensc2_usb_setup_text(const uint8_t setup[8], char *buf, size_t size)
{
	uint8_t type = setup[0], request = setup[1];
	uint16_t value = opensc2_get_le16(&setup[2]);
	uint16_t windex = opensc2_get_le16(&setup[4]);
	uint16_t length = opensc2_get_le16(&setup[6]);
	char what[40];

	what[0] = '\0';
	if ((type & 0x60) == 0x00) {   /* standard */
		switch (request) {
		case 0x00: snprintf(what, sizeof(what), "GET_STATUS"); break;
		case 0x01: snprintf(what, sizeof(what), "CLEAR_FEATURE %u", value); break;
		case 0x03: snprintf(what, sizeof(what), "SET_FEATURE %u", value); break;
		case 0x05: snprintf(what, sizeof(what), "SET_ADDRESS %u", value); break;
		case 0x06:
			snprintf(what, sizeof(what), "GET_DESCRIPTOR %s %u",
				 descriptor_name((uint8_t)(value >> 8)), value & 0xFF);
			break;
		case 0x08: snprintf(what, sizeof(what), "GET_CONFIGURATION"); break;
		case 0x09: snprintf(what, sizeof(what), "SET_CONFIGURATION %u", value); break;
		case 0x0A: snprintf(what, sizeof(what), "GET_INTERFACE"); break;
		case 0x0B: snprintf(what, sizeof(what), "SET_INTERFACE %u", value); break;
		default: break;
		}
	} else if ((type & 0x60) == 0x20) {   /* class (HID) */
		static const char *const report_types[4] = { "?", "input", "output", "feature" };

		switch (request) {
		case 0x01:
			snprintf(what, sizeof(what), "GET_REPORT %s %u",
				 report_types[(value >> 8) & 3], value & 0xFF);
			break;
		case 0x02: snprintf(what, sizeof(what), "GET_IDLE %u", value & 0xFF); break;
		case 0x03: snprintf(what, sizeof(what), "GET_PROTOCOL"); break;
		case 0x09:
			snprintf(what, sizeof(what), "SET_REPORT %s %u",
				 report_types[(value >> 8) & 3], value & 0xFF);
			break;
		case 0x0A:
			snprintf(what, sizeof(what), "SET_IDLE %u ms, report %u", (value >> 8) * 4u,
				 value & 0xFF);
			break;
		case 0x0B: snprintf(what, sizeof(what), "SET_PROTOCOL %u", value); break;
		default: break;
		}
	} else if ((type & 0x60) == 0x40) {
		snprintf(what, sizeof(what), "vendor request");
	}
	snprintf(buf, size, "%02X %02X %04X %04X %04X %s", type, request, value, windex, length,
		 what);
}
