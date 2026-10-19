/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * id_rules.h: the identity a board stores over its configured one (which fields, and their
 * values), and how the shell reads one field and its value. Pure logic, no kernel (the host tests
 * build it).
 */
#ifndef OPENSC2_ID_RULES_H_
#define OPENSC2_ID_RULES_H_

#include <stdint.h>
#include <opensc2/types.h>

/* The fields of an identity. */
#define OSC2_ID_UNIT      OPENSC2_BIT(0)
#define OSC2_ID_BOARD     OPENSC2_BIT(1)
#define OSC2_ID_GIT       OPENSC2_BIT(2)
#define OSC2_ID_PID       OPENSC2_BIT(3)
#define OSC2_ID_REV       OPENSC2_BIT(4)
#define OSC2_ID_BUILD     OPENSC2_BIT(5)
#define OSC2_ID_BL_BUILD  OPENSC2_BIT(6)
#define OSC2_ID_NAME      OPENSC2_BIT(7)   /* the USB product name */
#define OSC2_ID_ALL       0xFF
/* The fields of the settings key opensc2/id (its layout has no room for the name, which has a key
 * of its own, opensc2/name). */
#define OSC2_ID_BLOB_FIELDS 0x7F

/* A build time at or below this makes Steam's updater force an update (its configuration names
 * this value), into a bootloader a board running this library does not have. */
#define OSC2_BUILD_TIME_FLOOR 0x6A18D057u

/* What the board stores: the fields in `mask`, with their values in `id`. A field in the mask
 * counts whatever its value, 0 and an empty string included. */
struct osc2_id_override {
	uint8_t mask;
	struct opensc2_identity id;
};

/* The fields of `id` that are set: strings that are not empty, numbers that are not 0 (how
 * opensc2_init()'s and opensc2_identity_store()'s identities are read). */
uint8_t osc2_id_fields_set(const struct opensc2_identity *id);

/* Copy the fields in `fields` from `from` into `to`, whatever their values. */
void osc2_id_copy(struct opensc2_identity *to, const struct opensc2_identity *from,
		  uint8_t fields);

/* Store the fields in `fields` of `value` in `o`, whatever their values; the other stored fields
 * stay. */
void osc2_id_override_set(struct osc2_id_override *o, uint8_t fields,
			  const struct opensc2_identity *value);

/* The stored fields of `o` replace those of `id` (the configured identity): the one in use. */
void osc2_id_apply(struct opensc2_identity *id, const struct osc2_id_override *o);

/* The shell's name of a field (unit, board, git, pid, rev, build, blbuild, name): its OSC2_ID_*
 * bit, or 0 for any other name. */
uint8_t osc2_id_field(const char *name);

/* Read `text` as the value of the one field `field` (an OSC2_ID_* bit) into `out`; only that field
 * is written. Serials: exactly 13 characters, 0-9 and A-Z. The git revision: 1 to 12 printable
 * characters. The product name: 1 to 31 printable ASCII characters, spaces inside only (the shell
 * hands over a quoted name as one argument). Numbers: "0x" and 1 to 8 hex digits, or a lone "0",
 * so that a value typed without its 0x or with a stray character is refused instead of being read
 * as another number; the product ID at most 0xFFFF. 0, or -EINVAL. */
int osc2_id_parse(uint8_t field, const char *text, struct opensc2_identity *out);

#endif /* OPENSC2_ID_RULES_H_ */
