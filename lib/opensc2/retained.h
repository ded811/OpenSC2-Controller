/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * retained.h: the layout of the RAM area that start-up does not clear. It survives a soft reset (a
 * reboot command, the reset pin, the watchdog) but not a power loss. Layout version 3 (OpenSC2
 * 0.2.0 had version 1: no crash slot, no system words; 2 had no kept battery level), in the
 * devicetree region chosen as
 * opensc2,retained-ram:
 *   - with CONFIG_OPENSC2_CRASH_RECORD, first the crash record's slot, OSC2_CRASH_SLOT_SIZE (44)
 *     bytes with a magic word and a CRC-32 of its own (crash_rules.h; crash.c, linked before
 *     usblog.c, which takes the rest of the region);
 *   - then struct osc2_retained: a header with a CRC, so a boot can tell a kept area from random
 *     power-on RAM; the summary of the last wired session (counters and why it ended); the
 *     warm-restart mark (outside the CRC: it carries its own complement); the system machine's six
 *     test words (outside the CRC, each group with a magic word of its own: a boot-time injection
 *     and the last shutdown's end, test_sys.h); the battery level the fuel gauge keeps over a
 *     warm reboot (version 3; outside the CRC, behind a magic word of its own);
 *   - then a ring of text lines, the USB session log.
 * A boot that finds another version, or a header that does not fit the area, sets the area up
 * empty; the crash slot checks itself. Pure logic on a caller's buffer; the host tests build it.
 */
#ifndef OPENSC2_RETAINED_H_
#define OPENSC2_RETAINED_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OSC2_RET_MAGIC    0x3243534Fu   /* "OSC2" */
#define OSC2_RET_VERSION  3           /* the crash slot before the header, the system words,
				       * the kept battery level */
#define OSC2_RET_SYS_WORDS 6
#define OSC2_RET_SYS_BOOT_WORDS 3   /* the first of them: the boot-time injection */
#define OSC2_RET_END_LEN  32           /* "host: reboot into wireless" and the like */

struct osc2_session {
	uint32_t boot;           /* boot number of the session (0: none yet) */
	uint32_t requests;       /* feature requests */
	uint32_t stalls;         /* of which refused */
	uint32_t outputs;        /* output reports */
	uint32_t reports;        /* state reports sent */
	uint32_t dropped;        /* state reports not sent */
	char end[OSC2_RET_END_LEN];   /* why it ended; empty while it runs or after a crash */
};

struct osc2_retained {
	uint32_t magic;
	uint16_t version;
	uint16_t header_len;     /* sizeof(struct osc2_retained) */
	uint32_t size;           /* bytes of the ring after the header */
	uint32_t head;           /* next write offset in the ring */
	uint32_t used;           /* bytes of the ring in use (at most size) */
	uint32_t boots;          /* boots since the area was last set up */
	struct osc2_session session;
	uint32_t crc;            /* CRC-32 of everything above */
	uint32_t mark;           /* warm-restart mark word ... */
	uint32_t mark_inv;       /* ... and its complement */
	/* The test hooks' words for the system machine (test_sys.h): a boot-time injection for the
	 * next boot (read and cleared at boot, like the mark), then the last shutdown's end. */
	uint32_t sys[OSC2_RET_SYS_WORDS];
	/* The fuel gauge's level kept over a warm reboot: OSC2_RET_LEVEL_MAGIC, then the level. */
	uint32_t level_magic;
	uint32_t level;
};

#define OSC2_RET_LEVEL_MAGIC 0x4C564C42u   /* "BLVL" */

/* The ring follows the header in the same area. */
static inline uint8_t *osc2_ret_ring(struct osc2_retained *r)
{
	return (uint8_t *)(r + 1);
}

static inline const uint8_t *osc2_ret_ring_const(const struct osc2_retained *r)
{
	return (const uint8_t *)(r + 1);
}

uint32_t osc2_crc32(uint32_t crc, const void *data, size_t len);

/* Is the header intact for an area of `area_size` bytes? */
bool osc2_ret_valid(const struct osc2_retained *r, size_t area_size);
/* Set the area up empty (boot counter 0, no session, no mark). */
void osc2_ret_format(struct osc2_retained *r, size_t area_size);
/* Recompute the CRC after changing header fields. */
void osc2_ret_seal(struct osc2_retained *r);
/* Append `len` bytes to the ring, overwriting the oldest when it is full. Seals. */
void osc2_ret_append(struct osc2_retained *r, const char *text, size_t len);
/* Empty the ring and the session summary (the boot counter stays). Seals. */
void osc2_ret_clear(struct osc2_retained *r);
/* Keep `why` as the reason the session ended, cut to OSC2_RET_END_LEN - 1 characters. Seals. */
void osc2_ret_set_end(struct osc2_retained *r, const char *why);

/* Call `cb` for every complete line in the ring, oldest first, without the '\n'. When the ring
 * has wrapped, the partly overwritten oldest line is skipped. `line` has room for `max` bytes;
 * longer lines are cut. */
typedef void (*osc2_ret_line_cb)(const char *line, size_t len, void *ctx);
void osc2_ret_lines(const struct osc2_retained *r, char *line, size_t max, osc2_ret_line_cb cb,
		    void *ctx);

#endif /* OPENSC2_RETAINED_H_ */
