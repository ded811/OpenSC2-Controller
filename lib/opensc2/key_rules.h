/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * key_rules.h: the settings keys of vendor commands 0xED (read), 0xEE (set in RAM), 0xEF (save) and
 * 0xF0 (delete), as pure logic the host tests build: every key the real controller answers, with
 * its size, whether 0xED can read it and whether the factory reset (0x86) deletes it, plus
 * OpenSC2's own identity keys. A name that is not in the table is not the library's: it goes to
 * the application's vendor_command, so a host cannot fill the storage partition with names of its
 * own.
 *
 * The keys of the "blob" kind are values the library only keeps: it stores, reads back and deletes
 * them as the real controller's settings commands do, and other parts of the library or the
 * application read them (osc2_keys_get()). Their RAM copies live in a struct osc2_blob_store.
 */
#ifndef OPENSC2_KEY_RULES_H_
#define OPENSC2_KEY_RULES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The keys, numbered as the test ring's VENDOR entries name them (docs/test-protocol.md). */
enum osc2_key_id {
	OSC2_KEY_UNKNOWN = 0,
	OSC2_KEY_BOND = 1,              /* esb/bond */
	OSC2_KEY_BOND_2 = 2,            /* esb/bond_2 */
	OSC2_KEY_TRANSPORT = 3,         /* user/wireless_transport */
	OSC2_KEY_CHIME = 4,             /* user/haptic_boot_level */
	OSC2_KEY_ID_UNIT = 5,           /* opensc2/unit_serial */
	OSC2_KEY_ID_BOARD = 6,          /* opensc2/board_serial */
	OSC2_KEY_ID_GIT = 7,            /* opensc2/git_revision */
	OSC2_KEY_ID_REV = 8,            /* opensc2/board_revision */
	OSC2_KEY_KEYLOCK = 9,           /* user/keylock */
	OSC2_KEY_FULL_LED = 10,         /* user/fully_charged_led_intensity */
	OSC2_KEY_CHARGE_WINDOW = 11,    /* mte/charge_level */
	OSC2_KEY_RESET_REASON = 12,     /* debug/resetreason */
	OSC2_KEY_HAPTIC_GAIN = 13,      /* settings/haptics/haptic_master_gain_db */
	OSC2_KEY_HAPTIC_AMP = 14,       /* settings/haptics/amplifier_mode */
	OSC2_KEY_HAPTIC_ENABLED = 15,   /* settings/haptics/enabled */
	OSC2_KEY_CAL_JOY_L = 16,        /* cal/joy_l */
	OSC2_KEY_CAL_JOY_R = 17,        /* cal/joy_r */
	OSC2_KEY_CAL_TRG_L = 18,        /* cal/trg_l */
	OSC2_KEY_CAL_TRG_R = 19,        /* cal/trg_r */
	OSC2_KEY_CAL_PRS_L = 20,        /* cal/prs_l */
	OSC2_KEY_CAL_PRS_R = 21,        /* cal/prs_r */
	OSC2_KEY_CAL_TOUCH_L = 22,      /* cal/touch_l */
	OSC2_KEY_CAL_TOUCH_R = 23,      /* cal/touch_r */
	OSC2_KEY_CAL_RGBW_R = 24,       /* cal/rgbw_r */
	OSC2_KEY_CAL_RGBW_G = 25,       /* cal/rgbw_g */
	OSC2_KEY_CAL_RGBW_B = 26,       /* cal/rgbw_b */
	OSC2_KEY_CAL_RGBW_W = 27,       /* cal/rgbw_w */
	OSC2_KEY_CAL_VOLT_OFFSET = 28,  /* cal/volt_offset */
	OSC2_KEY_CAL_GYRO_BIAS = 29,    /* cal/sensors/gyroscope/bias */
	OSC2_KEY_COUNT,
};

/* What handles a key. */
enum osc2_key_kind {
	OSC2_KEYK_BOND,       /* the two puck bonds (osc2.bonds) */
	OSC2_KEYK_TRANSPORT,  /* the selected bond slot (osc2.slot) */
	OSC2_KEYK_CHIME,      /* the power-on chime level (osc2.chime) */
	OSC2_KEYK_IDENTITY,   /* OpenSC2's identity, with CONFIG_OPENSC2_IDENTITY_VENDOR_WRITE */
	OSC2_KEYK_BLOB,       /* a value the library keeps (struct osc2_blob_store) */
	OSC2_KEYK_CAL,        /* calibration: the application's (vendor_command) for now */
};

#define OSC2_KEYF_READ     0x01   /* 0xED answers it (the real controller has a read function) */
#define OSC2_KEYF_FACTORY  0x02   /* the factory reset (0x86) deletes it */

struct osc2_key_info {
	const char *name;
	uint8_t id;      /* enum osc2_key_id */
	uint8_t kind;    /* enum osc2_key_kind */
	uint8_t size;    /* the value's length, which 0xEE must give exactly (0: the kind decides) */
	uint8_t flags;   /* OSC2_KEYF_* */
	const char *store;   /* a blob's settings name under "opensc2/k/"; NULL for the others */
};

/* The key a 0xED..0xF0 payload names: its name up to the first NUL (`*name_len` gets the name's
 * length with the NUL). NULL when there is no NUL or the name is not in the table. */
const struct osc2_key_info *osc2_key_lookup(const uint8_t *payload, size_t len, uint8_t *name_len);
/* By number (NULL for OSC2_KEY_UNKNOWN or out of range). */
const struct osc2_key_info *osc2_key_by_id(uint8_t id);
/* By a blob's settings name under "opensc2/k/" (NULL when no blob has it). */
const struct osc2_key_info *osc2_key_by_store(const char *store);

/* ---- the blob values ---------------------------------------------------------------------------
 * Each blob key has a value of exactly its size. Until one is set, and after 0xF0 deletes it, the
 * value is the real controller's: zeros, or 3 for user/fully_charged_led_intensity (full
 * brightness). */

#define OSC2_BLOB_SPACE  52     /* the values, one after the other */
#define OSC2_KEY_VALUE_MAX 33   /* the longest (debug/resetreason) */

struct osc2_blob_store {
	uint8_t value[OSC2_BLOB_SPACE];
};

/* Every blob at its default. */
void osc2_blob_defaults(struct osc2_blob_store *s);
/* Blob `id` back at its default. -EINVAL for a key that is no blob. */
int osc2_blob_reset(struct osc2_blob_store *s, uint8_t id);
/* Set blob `id` to `len` bytes: -EINVAL for a length other than its size or a key that is no
 * blob. */
int osc2_blob_set(struct osc2_blob_store *s, uint8_t id, const uint8_t *value, size_t len);
/* Blob `id`'s value (its size in `*len`), or NULL for a key that is no blob. */
const uint8_t *osc2_blob_get(const struct osc2_blob_store *s, uint8_t id, uint8_t *len);

#endif /* OPENSC2_KEY_RULES_H_ */
