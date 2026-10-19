/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * id_rules.c: the stored identity, field by field (see id_rules.h). The configured identity
 * (Kconfig and opensc2_init()) is the base; a stored override replaces the fields in its mask, so
 * the shell can also store a 0 (a bootloader build time of 0, say) over a configured value. The
 * USB product name is one more such field. Pure logic.
 */
#include <errno.h>
#include <string.h>
#include <opensc2/codec.h>
#include "id_rules.h"

uint8_t osc2_id_fields_set(const struct opensc2_identity *id)
{
	uint8_t fields = 0;

	if (id->unit_serial[0]) {
		fields |= OSC2_ID_UNIT;
	}
	if (id->board_serial[0]) {
		fields |= OSC2_ID_BOARD;
	}
	if (id->git_revision[0]) {
		fields |= OSC2_ID_GIT;
	}
	if (id->product_id) {
		fields |= OSC2_ID_PID;
	}
	if (id->board_revision) {
		fields |= OSC2_ID_REV;
	}
	if (id->build_time) {
		fields |= OSC2_ID_BUILD;
	}
	if (id->bootloader_build_time) {
		fields |= OSC2_ID_BL_BUILD;
	}
	if (id->product_name[0]) {
		fields |= OSC2_ID_NAME;
	}
	return fields;
}

void osc2_id_copy(struct opensc2_identity *to, const struct opensc2_identity *from,
		  uint8_t fields)
{
	if (fields & OSC2_ID_UNIT) {
		memcpy(to->unit_serial, from->unit_serial, sizeof(to->unit_serial));
	}
	if (fields & OSC2_ID_BOARD) {
		memcpy(to->board_serial, from->board_serial, sizeof(to->board_serial));
	}
	if (fields & OSC2_ID_GIT) {
		memcpy(to->git_revision, from->git_revision, sizeof(to->git_revision));
	}
	if (fields & OSC2_ID_PID) {
		to->product_id = from->product_id;
	}
	if (fields & OSC2_ID_REV) {
		to->board_revision = from->board_revision;
	}
	if (fields & OSC2_ID_BUILD) {
		to->build_time = from->build_time;
	}
	if (fields & OSC2_ID_BL_BUILD) {
		to->bootloader_build_time = from->bootloader_build_time;
	}
	if (fields & OSC2_ID_NAME) {
		memcpy(to->product_name, from->product_name, sizeof(to->product_name));
	}
}

void osc2_id_override_set(struct osc2_id_override *o, uint8_t fields,
			  const struct opensc2_identity *value)
{
	fields &= OSC2_ID_ALL;
	osc2_id_copy(&o->id, value, fields);
	o->mask |= fields;
}

void osc2_id_apply(struct opensc2_identity *id, const struct osc2_id_override *o)
{
	osc2_id_copy(id, &o->id, o->mask);
}

uint8_t osc2_id_field(const char *name)
{
	static const struct {
		const char *name;
		uint8_t field;
	} names[] = {
		{ "unit", OSC2_ID_UNIT }, { "board", OSC2_ID_BOARD }, { "git", OSC2_ID_GIT },
		{ "pid", OSC2_ID_PID }, { "rev", OSC2_ID_REV }, { "build", OSC2_ID_BUILD },
		{ "blbuild", OSC2_ID_BL_BUILD }, { "name", OSC2_ID_NAME },
	};

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (strcmp(name, names[i].name) == 0) {
			return names[i].field;
		}
	}
	return 0;
}

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

/* "0x" and 1 to 8 hex digits, or "0"; nothing before or after. */
static int parse_hex(const char *text, uint32_t *value)
{
	uint32_t v = 0;
	size_t digits = 0;

	if (text[0] == '0' && text[1] == '\0') {
		*value = 0;
		return 0;
	}
	if (text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) {
		return -EINVAL;
	}
	for (const char *p = &text[2]; *p != '\0'; p++) {
		int d = hex_digit(*p);

		if (d < 0 || digits == 8) {
			return -EINVAL;
		}
		v = (v << 4) | (uint32_t)d;
		digits++;
	}
	if (digits == 0) {
		return -EINVAL;
	}
	*value = v;
	return 0;
}

int osc2_id_parse(uint8_t field, const char *text, struct opensc2_identity *out)
{
	uint32_t v = 0;

	switch (field) {
	case OSC2_ID_UNIT:
	case OSC2_ID_BOARD:
		if (!opensc2_serial_valid(text)) {
			return -EINVAL;
		}
		memcpy(field == OSC2_ID_UNIT ? out->unit_serial : out->board_serial, text,
		       OPENSC2_SERIAL_LEN + 1);
		return 0;
	case OSC2_ID_GIT:
		if (!opensc2_git_revision_valid(text)) {
			return -EINVAL;
		}
		memset(out->git_revision, 0, sizeof(out->git_revision));
		memcpy(out->git_revision, text, strlen(text));
		return 0;
	case OSC2_ID_NAME:
		if (!opensc2_product_name_valid(text)) {
			return -EINVAL;
		}
		memset(out->product_name, 0, sizeof(out->product_name));
		memcpy(out->product_name, text, strlen(text));
		return 0;
	case OSC2_ID_PID:
	case OSC2_ID_REV:
	case OSC2_ID_BUILD:
	case OSC2_ID_BL_BUILD:
		if (parse_hex(text, &v) != 0 || (field == OSC2_ID_PID && v > 0xFFFF)) {
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}
	if (field == OSC2_ID_PID) {
		out->product_id = (uint16_t)v;
	} else if (field == OSC2_ID_REV) {
		out->board_revision = v;
	} else if (field == OSC2_ID_BUILD) {
		out->build_time = v;
	} else {
		out->bootloader_build_time = v;
	}
	return 0;
}
