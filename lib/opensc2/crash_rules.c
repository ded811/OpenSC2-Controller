/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * crash_rules.c: the crash record (crash_rules.h). Pure logic.
 */
#include <string.h>
#include <opensc2/codec.h>
#include "crash_rules.h"
#include "retained.h"   /* osc2_crc32() */

void osc2_crash_record(uint8_t out[OSC2_CRASH_LEN], uint32_t build_time, const char *git,
		       uint32_t reason, uint32_t pc, uint32_t lr)
{
	size_t n = 0;

	memset(out, 0, OSC2_CRASH_LEN);
	out[OSC2_CRASH_AT_NEW] = 1;
	opensc2_put_le32(&out[OSC2_CRASH_AT_BUILD], build_time);
	while (git && n < OSC2_CRASH_GIT_LEN && git[n] != '\0') {
		out[OSC2_CRASH_AT_GIT + n] = (uint8_t)git[n];
		n++;
	}
	opensc2_put_le32(&out[OSC2_CRASH_AT_REASON], reason);
	opensc2_put_le32(&out[OSC2_CRASH_AT_PC], pc);
	opensc2_put_le32(&out[OSC2_CRASH_AT_LR], lr);
}

uint32_t osc2_crash_field(const uint8_t record[OSC2_CRASH_LEN], size_t at)
{
	return at + 4 <= OSC2_CRASH_LEN ? opensc2_get_le32(&record[at]) : 0;
}

void osc2_crash_slot_write(struct osc2_crash_slot *s, const uint8_t record[OSC2_CRASH_LEN])
{
	s->magic = 0;
	memcpy(s->record, record, OSC2_CRASH_LEN);
	memset(s->pad, 0, sizeof(s->pad));
	s->crc = osc2_crc32(0, s->record, OSC2_CRASH_LEN);
	s->magic = OSC2_CRASH_MAGIC;   /* last: a record cut short by a reset is no record */
}

bool osc2_crash_slot_valid(const struct osc2_crash_slot *s)
{
	return s->magic == OSC2_CRASH_MAGIC && s->crc == osc2_crc32(0, s->record, OSC2_CRASH_LEN);
}

void osc2_crash_slot_clear(struct osc2_crash_slot *s)
{
	memset(s, 0, sizeof(*s));
}

bool osc2_crash_should_save(const uint8_t stored[OSC2_CRASH_LEN],
			    const uint8_t fresh[OSC2_CRASH_LEN])
{
	return !(stored[OSC2_CRASH_AT_NEW] == 1 &&
		 memcmp(&stored[1], &fresh[1], OSC2_CRASH_LEN - 1) == 0);
}

bool osc2_crash_answer(uint8_t record[OSC2_CRASH_LEN], uint8_t out[OSC2_CRASH_ANSWER_LEN])
{
	out[0] = 1;
	memcpy(&out[1], record, OSC2_CRASH_LEN);
	if (record[OSC2_CRASH_AT_NEW] == 0) {
		return false;
	}
	record[OSC2_CRASH_AT_NEW] = 0;   /* seen */
	return true;
}

bool osc2_crash_empty(const uint8_t record[OSC2_CRASH_LEN])
{
	return osc2_crash_field(record, OSC2_CRASH_AT_BUILD) == 0;
}
