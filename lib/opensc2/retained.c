/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * retained.c: the retained RAM area (see retained.h). Pure logic.
 */
#include <stddef.h>
#include <string.h>
#include "retained.h"

uint32_t osc2_crc32(uint32_t crc, const void *data, size_t len)
{
	const uint8_t *p = data;

	crc = ~crc;
	while (len--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++) {
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
		}
	}
	return ~crc;
}

static uint32_t header_crc(const struct osc2_retained *r)
{
	return osc2_crc32(0, r, offsetof(struct osc2_retained, crc));
}

bool osc2_ret_valid(const struct osc2_retained *r, size_t area_size)
{
	if (area_size < sizeof(*r)) {
		return false;
	}
	return r->magic == OSC2_RET_MAGIC && r->version == OSC2_RET_VERSION &&
	       r->header_len == sizeof(*r) && r->size == area_size - sizeof(*r) &&
	       (r->size == 0 || r->head < r->size) && r->used <= r->size &&
	       r->crc == header_crc(r);
}

#define LEVEL_MAGIC 0x4C564C00u   /* "LVL" and the level in the low byte */

uint32_t osc2_ret_level_word(uint8_t level)
{
	return level <= 100 ? LEVEL_MAGIC | level : 0;
}

uint8_t osc2_ret_level_from(uint32_t word, uint32_t inverse)
{
	if (word != (uint32_t)~inverse || (word & 0xFFFFFF00u) != LEVEL_MAGIC ||
	    (word & 0xFFu) > 100) {
		return OSC2_RET_NO_LEVEL;
	}
	return (uint8_t)(word & 0xFFu);
}

void osc2_ret_seal(struct osc2_retained *r)
{
	r->crc = header_crc(r);
}

void osc2_ret_format(struct osc2_retained *r, size_t area_size)
{
	memset(r, 0, sizeof(*r));
	r->magic = OSC2_RET_MAGIC;
	r->version = OSC2_RET_VERSION;
	r->header_len = sizeof(*r);
	r->size = (uint32_t)(area_size - sizeof(*r));
	r->mark_inv = ~0u;   /* no mark: the complement does not match */
	osc2_ret_seal(r);
}

void osc2_ret_append(struct osc2_retained *r, const char *text, size_t len)
{
	uint8_t *ring = osc2_ret_ring(r);

	if (r->size == 0) {
		return;
	}
	if (len > r->size) {   /* only the end of an oversized text fits */
		text += len - r->size;
		len = r->size;
	}
	while (len) {
		size_t chunk = r->size - r->head;

		if (chunk > len) {
			chunk = len;
		}
		memcpy(&ring[r->head], text, chunk);
		r->head = (uint32_t)((r->head + chunk) % r->size);
		r->used = (uint32_t)(r->used + chunk > r->size ? r->size : r->used + chunk);
		text += chunk;
		len -= chunk;
	}
	osc2_ret_seal(r);
}

void osc2_ret_clear(struct osc2_retained *r)
{
	r->head = 0;
	r->used = 0;
	memset(&r->session, 0, sizeof(r->session));
	osc2_ret_seal(r);
}

void osc2_ret_set_end(struct osc2_retained *r, const char *why)
{
	size_t n = 0;

	memset(r->session.end, 0, sizeof(r->session.end));
	while (n + 1 < sizeof(r->session.end) && why[n] != '\0') {
		r->session.end[n] = why[n];
		n++;
	}
	osc2_ret_seal(r);
}

void osc2_ret_lines(const struct osc2_retained *r, char *line, size_t max, osc2_ret_line_cb cb,
		    void *ctx)
{
	const uint8_t *ring = osc2_ret_ring_const(r);
	size_t start, n = 0;
	bool skipping;

	if (r->size == 0 || r->used == 0 || max == 0) {
		return;
	}
	start = (r->head + r->size - r->used) % r->size;
	/* A full ring has overwritten the start of its oldest line: skip to the next one. */
	skipping = r->used == r->size;
	for (size_t i = 0; i < r->used; i++) {
		char c = (char)ring[(start + i) % r->size];

		if (skipping) {
			skipping = c != '\n';
			continue;
		}
		if (c == '\n') {
			cb(line, n, ctx);
			n = 0;
			continue;
		}
		if (n < max) {
			line[n++] = c;
		}
	}
}
