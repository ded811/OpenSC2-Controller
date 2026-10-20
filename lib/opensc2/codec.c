/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * codec.c: the wire formats as pure functions (see include/opensc2/codec.h and docs/protocol.md).
 * No Zephyr headers here, so this file also builds on a PC for the host tests.
 */
#include <string.h>
#include <opensc2/codec.h>

const uint8_t opensc2_common_address[4] = { 'i', 'b', 'e', 'x' };
const uint8_t opensc2_pairing_address[4] = { 0x91, 0xA2, 0xA7, 0x93 };

static int16_t clamp_axis(int16_t v)
{
	return v < -OPENSC2_AXIS_MAX ? -OPENSC2_AXIS_MAX : v;
}

static uint16_t clamp_trigger(uint16_t v)
{
	return v > OPENSC2_TRIGGER_MAX ? OPENSC2_TRIGGER_MAX : v;
}

uint8_t opensc2_input_report_size(uint8_t id)
{
	switch (id) {
	case 0x40: return 5;
	case 0x41: return 8;
	case 0x42: return 53;
	case 0x43: return 14;
	case 0x44: return 5;
	case 0x45: return 45;
	default:   return 0;
	}
}

/* ---- state report 0x42 ---------------------------------------------------------------------- */

static const uint32_t pad_touch_bit[2] = { OPENSC2_TOUCH_LEFT_PAD, OPENSC2_TOUCH_RIGHT_PAD };
static const uint32_t pad_click_bit[2] = { OPENSC2_CLICK_LEFT_PAD, OPENSC2_CLICK_RIGHT_PAD };

size_t opensc2_encode_state(const struct opensc2_state *s, uint8_t seq,
			    uint8_t out[OPENSC2_STATE_REPORT_LEN])
{
	uint32_t buttons = s->buttons;
	uint8_t *p;

	for (int side = 0; side < 2; side++) {
		if (s->pad[side].touched) {
			buttons |= pad_touch_bit[side];
		}
		if (s->pad[side].clicked) {
			buttons |= pad_click_bit[side];
		}
	}

	out[0] = OPENSC2_RPT_STATE;
	out[1] = seq;
	opensc2_put_le32(&out[2], buttons);
	opensc2_put_le16(&out[6], clamp_trigger(s->trigger[OPENSC2_LEFT]));
	opensc2_put_le16(&out[8], clamp_trigger(s->trigger[OPENSC2_RIGHT]));
	opensc2_put_le16(&out[10], (uint16_t)clamp_axis(s->stick[OPENSC2_LEFT].x));
	opensc2_put_le16(&out[12], (uint16_t)clamp_axis(s->stick[OPENSC2_LEFT].y));
	opensc2_put_le16(&out[14], (uint16_t)clamp_axis(s->stick[OPENSC2_RIGHT].x));
	opensc2_put_le16(&out[16], (uint16_t)clamp_axis(s->stick[OPENSC2_RIGHT].y));

	p = &out[18];
	for (int side = 0; side < 2; side++) {
		const struct opensc2_trackpad *pad = &s->pad[side];
		/* An untouched pad reports its position as 0, 0, as the real controller does. */
		bool touched = (buttons & pad_touch_bit[side]) != 0;

		opensc2_put_le16(&p[0], touched ? (uint16_t)clamp_axis(pad->x) : 0);
		opensc2_put_le16(&p[2], touched ? (uint16_t)clamp_axis(pad->y) : 0);
		opensc2_put_le16(&p[4], clamp_trigger(pad->pressure));
		p += 6;
	}

	opensc2_put_le32(&out[30], s->imu.timestamp_us);
	for (int i = 0; i < 3; i++) {
		opensc2_put_le16(&out[34 + 2 * i], (uint16_t)s->imu.accel[i]);
		opensc2_put_le16(&out[40 + 2 * i], (uint16_t)s->imu.gyro[i]);
	}
	for (int i = 0; i < 4; i++) {
		opensc2_put_le16(&out[46 + 2 * i], (uint16_t)s->imu.quat[i]);
	}
	return OPENSC2_STATE_REPORT_LEN;
}

void opensc2_decode_state(const uint8_t in[OPENSC2_STATE_REPORT_LEN], struct opensc2_state *s,
			  uint8_t *seq)
{
	const uint8_t *p = &in[18];

	memset(s, 0, sizeof(*s));
	if (seq) {
		*seq = in[1];
	}
	s->buttons = opensc2_get_le32(&in[2]);
	s->trigger[OPENSC2_LEFT] = opensc2_get_le16(&in[6]);
	s->trigger[OPENSC2_RIGHT] = opensc2_get_le16(&in[8]);
	s->stick[OPENSC2_LEFT].x = (int16_t)opensc2_get_le16(&in[10]);
	s->stick[OPENSC2_LEFT].y = (int16_t)opensc2_get_le16(&in[12]);
	s->stick[OPENSC2_RIGHT].x = (int16_t)opensc2_get_le16(&in[14]);
	s->stick[OPENSC2_RIGHT].y = (int16_t)opensc2_get_le16(&in[16]);
	for (int side = 0; side < 2; side++) {
		s->pad[side].x = (int16_t)opensc2_get_le16(&p[0]);
		s->pad[side].y = (int16_t)opensc2_get_le16(&p[2]);
		s->pad[side].pressure = opensc2_get_le16(&p[4]);
		s->pad[side].touched = (s->buttons & pad_touch_bit[side]) != 0;
		s->pad[side].clicked = (s->buttons & pad_click_bit[side]) != 0;
		p += 6;
	}
	s->imu.timestamp_us = opensc2_get_le32(&in[30]);
	for (int i = 0; i < 3; i++) {
		s->imu.accel[i] = (int16_t)opensc2_get_le16(&in[34 + 2 * i]);
		s->imu.gyro[i] = (int16_t)opensc2_get_le16(&in[40 + 2 * i]);
	}
	for (int i = 0; i < 4; i++) {
		s->imu.quat[i] = (int16_t)opensc2_get_le16(&in[46 + 2 * i]);
	}
}

/* ---- battery and haptic status -------------------------------------------------------------- */

size_t opensc2_encode_battery(const struct opensc2_battery *b,
			      uint8_t out[OPENSC2_BATTERY_REPORT_LEN])
{
	out[0] = OPENSC2_RPT_BATTERY;
	out[1] = b->charge_state;
	out[2] = b->level > 100 ? 100 : b->level;
	opensc2_put_le16(&out[3], b->battery_mv);
	opensc2_put_le16(&out[5], b->system_mv);
	opensc2_put_le16(&out[7], b->input_mv);
	opensc2_put_le16(&out[9], (uint16_t)b->current_ma);
	opensc2_put_le16(&out[11], b->input_current_ma);
	opensc2_put_le16(&out[13], b->temperature);
	return OPENSC2_BATTERY_REPORT_LEN;
}

/* 0x43's charge state without the 0x80 flag the real controller adds while its supply check has
 * given up. */
#define CHARGE_STATE_MASK ((uint8_t)~OPENSC2_CHARGE_GAVE_UP)

size_t opensc2_encode_battery_data(const struct opensc2_battery *b,
				   uint8_t out[OPENSC2_BATTERY_DATA_LEN])
{
	uint8_t state = b->charge_state & CHARGE_STATE_MASK;

	opensc2_put_le16(&out[0], b->temperature);
	opensc2_put_le16(&out[2], b->battery_mv);
	opensc2_put_le16(&out[4], b->system_mv);
	opensc2_put_le16(&out[6], b->input_mv);
	opensc2_put_le16(&out[8], (uint16_t)b->current_ma);
	opensc2_put_le16(&out[10], b->input_current_ma);
	out[12] = b->level > 100 ? 100 : b->level;
	out[13] = state == OPENSC2_CHARGE_CHARGING ? 1 : 0;   /* 1 while charging */
	out[14] = state;
	return OPENSC2_BATTERY_DATA_LEN;
}

size_t opensc2_encode_haptic_status(uint8_t channel_id, uint32_t flags,
				    uint8_t out[OPENSC2_HAPTIC_STATUS_LEN])
{
	out[0] = OPENSC2_RPT_HAPTIC_STATUS;
	out[1] = channel_id;
	opensc2_put_le32(&out[2], flags);
	return OPENSC2_HAPTIC_STATUS_LEN;
}

/* ---- output reports -------------------------------------------------------------------------- */

uint8_t opensc2_target_channels(uint8_t report_id, uint8_t target)
{
	const uint8_t lp = OPENSC2_HAPTIC_LEFT_PAD, rp = OPENSC2_HAPTIC_RIGHT_PAD;
	const uint8_t lg = OPENSC2_HAPTIC_LEFT_GRIP, rg = OPENSC2_HAPTIC_RIGHT_GRIP;

	switch (report_id) {
	case 0x80:
		return lg | rg;              /* rumble always plays on the grips */
	case 0x86:
		/* Stream control has its own numbering. */
		switch (target) {
		case 0: return lg;
		case 1: return rg;
		case 2: return lg | rg;
		case 3: return lp;
		case 4: return rp;
		case 5: return lp | rp;
		case 0x40: return rg;
		case 0x80: return lg | rg;
		default: return 0;
		}
	case 0x87:
	case 0x89:
		/* Stream data, numbered differently again (target 1 and 0x40 select nothing, 4 is the
		 * right grip): this is what the real controller does. */
		switch (target) {
		case 0: return lg;
		case 2: return lg | rg;
		case 3: return lp;
		case 4: return rg;
		case 5: return lp | rp;
		case 0x80: return lg | rg;
		default: return 0;
		}
	case 0x88:
		return lg | rg;
	default:
		break;
	}

	switch (target & 0x7F) {
	case 0: return report_id == 0x81 ? rp : lp;   /* the pulse report swaps the two pads */
	case 1: return report_id == 0x81 ? lp : rp;
	case 2: case 6: return lp | rp;
	case 3: return lg;
	case 4: return rg;
	case 5: case 7: return lg | rg;
	default: return 0;
	}
}

static void haptic_start(struct opensc2_output *o, enum opensc2_haptic_kind kind, uint8_t id,
			 uint8_t target)
{
	memset(o, 0, sizeof(*o));
	o->type = OPENSC2_OUTPUT_HAPTIC;
	o->haptic.kind = kind;
	o->haptic.report_id = id;
	o->haptic.target = target;
	o->haptic.channels = opensc2_target_channels(id, target);
}

int opensc2_decode_output(const uint8_t *report, size_t len, struct opensc2_output out[2])
{
	const uint8_t *d = report + 1;
	size_t n;
	uint8_t id;

	if (len < 1) {
		return -1;
	}
	id = report[0];
	n = len - 1;

	switch (id) {
	case 0x80:
		/* type u8, intensity u16, left {speed u16, gain s8}, right {speed u16, gain s8}: the
		 * layout of SDL's MsgHapticRumble, which fills the report's 9 bytes. */
		if (n < 9) {
			return -1;
		}
		memset(&out[0], 0, sizeof(out[0]));
		out[0].type = OPENSC2_OUTPUT_RUMBLE;
		out[0].rumble.type = d[0];
		out[0].rumble.intensity = opensc2_get_le16(&d[1]);
		out[0].rumble.left_speed = opensc2_get_le16(&d[3]);
		out[0].rumble.left_gain_db = (int8_t)d[5];
		out[0].rumble.right_speed = opensc2_get_le16(&d[6]);
		out[0].rumble.right_gain_db = (int8_t)d[8];
		return 1;
	case 0x81:
		if (n < 7) {
			return -1;
		}
		haptic_start(&out[0], OPENSC2_HAPTIC_PULSE, id, d[0]);
		out[0].haptic.pulse.on_us = opensc2_get_le16(&d[1]);
		out[0].haptic.pulse.off_us = opensc2_get_le16(&d[3]);
		out[0].haptic.pulse.count = opensc2_get_le16(&d[5]);
		return 1;
	case 0x82:
		if (n < 3) {
			return -1;
		}
		haptic_start(&out[0], OPENSC2_HAPTIC_COMMAND, id, d[0]);
		out[0].haptic.command.command = d[1];
		out[0].haptic.command.gain_db = (int8_t)d[2];
		return 1;
	case 0x83:
		if (n < 9) {
			return -1;
		}
		haptic_start(&out[0], OPENSC2_HAPTIC_TONE, id, d[0]);
		out[0].haptic.tone.gain_db = (int8_t)d[1];
		out[0].haptic.tone.freq_hz = opensc2_get_le16(&d[2]);
		out[0].haptic.tone.duration_ms = opensc2_get_le16(&d[4]);
		out[0].haptic.tone.lfo_hz = opensc2_get_le16(&d[6]);
		out[0].haptic.tone.lfo_depth = d[8];
		return 1;
	case 0x84:
		if (n < 8) {
			return -1;
		}
		haptic_start(&out[0], OPENSC2_HAPTIC_SWEEP, id, d[0]);
		out[0].haptic.sweep.gain_db = (int8_t)d[1];
		out[0].haptic.sweep.duration_ms = opensc2_get_le16(&d[2]);
		out[0].haptic.sweep.start_hz = opensc2_get_le16(&d[4]);
		out[0].haptic.sweep.end_hz = opensc2_get_le16(&d[6]);
		out[0].haptic.sweep.antiphase = (d[0] & 0x7F) == 6 || (d[0] & 0x7F) == 7;
		return 1;
	case 0x85:
		if (n < 3) {
			return -1;
		}
		haptic_start(&out[0], OPENSC2_HAPTIC_SCRIPT, id, d[0]);
		out[0].haptic.script.id = d[1];
		out[0].haptic.script.gain_db = (int8_t)d[2];
		return 1;
	case 0x86: {
		uint8_t format;

		if (n < 2) {
			return -1;
		}
		format = n >= 3 ? d[2] : 0;
		haptic_start(&out[0], OPENSC2_HAPTIC_PCM_CONTROL, id, d[1]);
		out[0].haptic.pcm_control.operation = d[0];
		out[0].haptic.pcm_control.format = format;
		out[0].haptic.pcm_control.encoding =
			format / 4 <= OPENSC2_PCM_MULAW ? format / 4 : OPENSC2_PCM_UNKNOWN;
		out[0].haptic.pcm_control.rate_hz = (uint16_t)(8000u >> (format % 4));
		return 1;
	}
	case 0x87:
		if (n < 1) {
			return -1;
		}
		haptic_start(&out[0], OPENSC2_HAPTIC_PCM_DATA, id, d[0]);
		out[0].haptic.pcm.data = &d[1];
		out[0].haptic.pcm.len = (uint8_t)(n - 1);
		return 1;
	case 0x88: {
		uint8_t count;

		if (n < 1) {
			return -1;
		}
		count = d[0] > 31 ? 31 : d[0];
		if (n < (size_t)32 + count) {
			return -1;
		}
		haptic_start(&out[0], OPENSC2_HAPTIC_PCM_DATA, id, 0);
		out[0].haptic.channels = OPENSC2_HAPTIC_LEFT_GRIP;
		out[0].haptic.pcm.data = &d[1];
		out[0].haptic.pcm.len = count;
		haptic_start(&out[1], OPENSC2_HAPTIC_PCM_DATA, id, 0);
		out[1].haptic.channels = OPENSC2_HAPTIC_RIGHT_GRIP;
		out[1].haptic.pcm.data = &d[32];
		out[1].haptic.pcm.len = count;
		return 2;
	}
	case 0x89:
		if (n < 2 || n < (size_t)2 + d[0]) {
			return -1;
		}
		haptic_start(&out[0], OPENSC2_HAPTIC_PCM_DATA, id, d[1]);
		out[0].haptic.pcm.data = &d[2];
		out[0].haptic.pcm.len = d[0];
		return 1;
	default:
		return 0;   /* 0x8A and anything else: nothing to do */
	}
}

/* ---- identity -------------------------------------------------------------------------------- */

bool opensc2_serial_valid(const char *s)
{
	size_t i;

	for (i = 0; i < OPENSC2_SERIAL_LEN; i++) {
		char c = s[i];

		if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) {
			return false;
		}
	}
	return s[OPENSC2_SERIAL_LEN] == '\0';
}

bool opensc2_git_revision_valid(const char *s)
{
	size_t i;

	for (i = 0; i <= OPENSC2_GIT_REVISION_MAX && s[i] != '\0'; i++) {
		if (s[i] < 0x21 || s[i] > 0x7E) {
			return false;
		}
	}
	return i >= 1 && i <= OPENSC2_GIT_REVISION_MAX && s[i] == '\0';
}

bool opensc2_product_name_valid(const char *s)
{
	size_t i;

	for (i = 0; i <= OPENSC2_PRODUCT_NAME_MAX && s[i] != '\0'; i++) {
		if (s[i] < 0x20 || s[i] > 0x7E) {
			return false;
		}
	}
	if (i < 1 || i > OPENSC2_PRODUCT_NAME_MAX || s[i] != '\0') {
		return false;
	}
	return s[0] != ' ' && s[i - 1] != ' ';
}

uint32_t opensc2_identity_check(const struct opensc2_identity *id)
{
	uint32_t bad = 0;

	if (!opensc2_serial_valid(id->unit_serial)) {
		bad |= OPENSC2_ID_BAD_UNIT_SERIAL;
	}
	if (!opensc2_serial_valid(id->board_serial)) {
		bad |= OPENSC2_ID_BAD_BOARD_SERIAL;
	}
	if (!opensc2_git_revision_valid(id->git_revision)) {
		bad |= OPENSC2_ID_BAD_GIT_REVISION;
	}
	if (id->product_id == 0) {
		bad |= OPENSC2_ID_BAD_PRODUCT_ID;
	}
	if (!opensc2_product_name_valid(id->product_name)) {
		bad |= OPENSC2_ID_BAD_PRODUCT_NAME;
	}
	return bad;
}

/* Copy at most field - 1 characters of `s` and zero-fill the rest of the field. */
static void put_string(uint8_t *out, const char *s, size_t field)
{
	size_t n = 0;

	while (n < field - 1 && s[n] != '\0') {
		n++;
	}
	memset(out, 0, field);
	memcpy(out, s, n);
}

size_t opensc2_encode_identity(const struct opensc2_identity *id,
			       uint8_t out[OPENSC2_IDENTITY_RECORD_LEN])
{
	opensc2_put_le32(&out[0], id->product_id);
	opensc2_put_le32(&out[4], id->build_time);
	opensc2_put_le32(&out[8], id->bootloader_build_time);
	opensc2_put_le32(&out[12], id->board_revision);
	put_string(&out[16], id->unit_serial, 14);
	put_string(&out[30], id->board_serial, 14);
	put_string(&out[44], id->git_revision, 13);
	return OPENSC2_IDENTITY_RECORD_LEN;
}

size_t opensc2_encode_attributes(const struct opensc2_identity *id, uint32_t interval_us,
				 uint8_t out[30])
{
	const struct {
		uint8_t tag;
		uint32_t value;
	} attr[6] = {
		{ 1, id->product_id },
		{ 2, 0 },                          /* capabilities */
		{ 10, id->bootloader_build_time },
		{ 4, id->build_time },
		{ 9, id->board_revision },
		{ 11, interval_us },               /* report interval of this link */
	};

	for (int i = 0; i < 6; i++) {
		out[5 * i] = attr[i].tag;
		opensc2_put_le32(&out[5 * i + 1], attr[i].value);
	}
	return 30;
}

size_t opensc2_encode_string_attribute(const struct opensc2_identity *id, uint8_t tag,
				       uint8_t out[20])
{
	const char *text;

	switch (tag) {
	case 0:
	case 4:
		text = id->board_serial;
		break;
	case 1:
		text = id->unit_serial;
		break;
	case 3:
		text = id->git_revision;
		break;
	default:
		text = "NA";
		break;
	}
	out[0] = tag;
	put_string(&out[1], text, 19);
	return 20;
}

/* ---- bonds ----------------------------------------------------------------------------------- */

void opensc2_bond_pack(const struct opensc2_bond *b, uint8_t out[OPENSC2_BOND_LEN])
{
	opensc2_put_le32(&out[0], b->puck_id);
	opensc2_put_le32(&out[4], b->controller_id);
	memcpy(&out[8], b->puck_serial, 16);
}

void opensc2_bond_unpack(const uint8_t in[OPENSC2_BOND_LEN], struct opensc2_bond *b)
{
	b->puck_id = opensc2_get_le32(&in[0]);
	b->controller_id = opensc2_get_le32(&in[4]);
	memcpy(b->puck_serial, &in[8], 16);
}

bool opensc2_bond_valid(const struct opensc2_bond *b)
{
	return b->puck_id != 0 && b->controller_id != 0;
}

/* ---- link packets ---------------------------------------------------------------------------- */

bool opensc2_parse_connect_request(const uint8_t *pkt, size_t len,
				   struct opensc2_connect_request *req)
{
	uint32_t channel;

	if (len < 18 || pkt[0] != OPENSC2_PKT_CONNECT_REQUEST) {
		return false;
	}
	channel = opensc2_get_le32(&pkt[9]) & 0xFF;   /* only the low byte is used */
	if (channel > 100) {
		return false;
	}
	req->puck_id = opensc2_get_le32(&pkt[1]);
	req->controller_id = opensc2_get_le32(&pkt[5]);
	req->channel = (uint8_t)channel;
	memcpy(req->address, &pkt[13], 4);
	req->prefix = pkt[17];
	return true;
}

int opensc2_protocol_version(const uint8_t *pkt, size_t len)
{
	uint8_t a = len > 1 ? pkt[1] : 0;
	uint8_t b = len > 2 ? pkt[2] : 0;

	if (a == 0) {
		return b == 0 ? 0 : 1;
	}
	if (a == 1 && b != 0) {
		return 1;
	}
	return -1;   /* {1, 0} or a above 1: not a version this controller knows */
}

int opensc2_parse_channel_change(const uint8_t *pkt, size_t len, uint8_t channels[4])
{
	size_t count = len > 1 ? len - 1 : 0;

	if (count > 4) {
		count = 4;
	}
	memset(channels, 0xFF, 4);
	for (size_t i = 0; i < count; i++) {
		if (pkt[1 + i] > 100) {
			return 0;
		}
		channels[i] = pkt[1 + i];
	}
	return (int)count;
}

size_t opensc2_build_pairing(uint8_t type, uint32_t id, const char *serial,
			     uint8_t out[OPENSC2_PAIRING_PKT_LEN])
{
	out[0] = type;
	opensc2_put_le32(&out[1], id);
	put_string(&out[5], serial, 16);
	return OPENSC2_PAIRING_PKT_LEN;
}

bool opensc2_parse_pairing_offer(const uint8_t *pkt, size_t len, uint32_t *puck_id,
				 char serial[17])
{
	size_t n;

	if (len < 5 || pkt[0] != OPENSC2_PKT_PAIRING_OFFER) {
		return false;
	}
	*puck_id = opensc2_get_le32(&pkt[1]);
	n = len - 5 > 16 ? 16 : len - 5;
	memset(serial, 0, 17);
	memcpy(serial, &pkt[5], n);
	return *puck_id != 0;
}

bool opensc2_address_byte_ok(uint8_t b)
{
	return b != 0x00 && b != 0xAA && b != 0x55;
}

int opensc2_record_next(const uint8_t *pkt, size_t len, size_t *pos, struct opensc2_record *rec)
{
	size_t at = *pos;

	if (at + 2 > len) {
		return 0;
	}
	if (at + 2 + pkt[at] > len) {
		return -1;
	}
	rec->len = pkt[at];
	rec->type = pkt[at + 1];
	rec->data = &pkt[at + 2];
	*pos = at + 2 + rec->len;
	return 1;
}

void opensc2_payload_start(struct opensc2_payload *p, uint8_t type)
{
	p->data[0] = type;
	p->len = 1;
}

int opensc2_payload_add_record(struct opensc2_payload *p, uint8_t type, const uint8_t *data,
			       uint8_t len)
{
	if ((size_t)p->len + 2 + len > OPENSC2_PAYLOAD_MAX) {
		return -1;
	}
	p->data[p->len] = len;
	p->data[p->len + 1] = type;
	if (len) {
		memcpy(&p->data[p->len + 2], data, len);
	}
	p->len = (uint8_t)(p->len + 2 + len);
	return 0;
}

int opensc2_payload_add_bytes(struct opensc2_payload *p, const uint8_t *data, size_t len)
{
	if ((size_t)p->len + len > OPENSC2_PAYLOAD_MAX) {
		return -1;
	}
	memcpy(&p->data[p->len], data, len);
	p->len = (uint8_t)(p->len + len);
	return 0;
}
