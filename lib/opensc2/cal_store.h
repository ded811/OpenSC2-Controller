/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * cal_store.h: the library's calibration (CONFIG_OPENSC2_CAL), as the rest of the library calls it:
 * the cal/ keys behind the by-name commands, the calibration commands, the raw inputs processed at
 * every state report and the pad rules after the test hooks' injection. Not a public interface.
 * Without CONFIG_OPENSC2_CAL each call is an inline that keeps the library's earlier behaviour.
 */
#ifndef OPENSC2_CAL_STORE_H_
#define OPENSC2_CAL_STORE_H_

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <opensc2/cal.h>

#if defined(CONFIG_OPENSC2_CAL)

/* The by-name commands 0xEE, 0xEF and 0xF0 on a cal/ key (payload: name, NUL, value). -ENOENT when
 * the name is not one: the caller goes on with its own keys. */
int osc2_cal_by_name(uint8_t opcode, const uint8_t *payload, uint8_t len);
/* 0xED on a cal/ key: 0 with the answer (length 0 when the key has no value to show), or -ENOENT
 * when the name is not one. */
int osc2_cal_read_by_name(const uint8_t *payload, uint8_t len, uint8_t *out, uint8_t *out_len);
/* 0xD8, 0xC0, 0xC3, 0xE2 and 0xFE (`avail` bytes follow the length byte; the real controller reads
 * its bytes whatever the length says). 0 or a negative errno; -ENOTSUP for another opcode. */
int osc2_cal_command(uint8_t opcode, const uint8_t *payload, uint8_t len, size_t avail);
/* Every state report, before the test hooks' injection: the raw inputs into `s`, the recordings
 * fed with the frame's samples. */
void osc2_cal_frame(struct opensc2_state *s);
/* Every state report, after the injection: the pad rules (rotation, the stabiliser). */
void osc2_cal_rules(struct opensc2_state *s);
/* A pad's pressure key as RAM has it, and the count of its loads (for the trackpad frames). */
uint32_t osc2_cal_pressure_key(int side, struct opensc2_cal_pressure *out);
/* A trackpad frame's raw pressure word for the factory pressure recording (0xC3). */
void osc2_cal_rec_pressure_frame(int side, int16_t raw);
/* How many times the sticks and triggers were set up so far (each set-up clears the triggers'
 * digital bits: a key load, setting 68, the dock). */
uint32_t osc2_cal_setups(void);
/* Did this frame's raw trigger of `side` (0, 1) decide its digital bit, and is `value` still the
 * value it gave? Then *bit is that bit. */
bool osc2_cal_trigger_decided(int side, uint16_t value, bool *bit);
/* The hardware test hooks' CAL_STATUS (docs/test-protocol.md); byte 6, the injection's inputs, is
 * the hooks' own. */
void osc2_cal_status(uint8_t out[16]);

/* cal_store.c, for the glue: a key found in storage at boot ("joy_l" for opensc2/cal/joy_l), and
 * the deferred flash work (saves and the erase), run from the system work queue. */
int osc2_cal_load(const char *key, const void *value, size_t len);
void osc2_cal_flush(void);
/* Back to boot (the host tests). */
void osc2_cal_reset(void);

struct shell;
/* "opensc2 cal ..." (cal_shell.c). */
int osc2_cal_shell(const struct shell *sh, size_t argc, char **argv);

/* cal_flash.c: the glue (the host tests have a stand-in). A name is a key's own ("cal/joy_l"); it is
 * stored as opensc2/cal/joy_l. osc2_cal_flush_wait() returns once the deferred work asked for so
 * far (an erase, saves) is done; never call it with a lock held that that work needs. */
void osc2_cal_defer(void);
void osc2_cal_flush_wait(void);
int osc2_cal_flash_write(const char *name, const void *value, size_t len);
int osc2_cal_flash_delete(const char *name);
/* Load every stored cal/ key into RAM again (osc2_cal_load() for each). */
void osc2_cal_flash_reload(void);

#else /* !CONFIG_OPENSC2_CAL */

static inline int osc2_cal_by_name(uint8_t opcode, const uint8_t *payload, uint8_t len)
{
	(void)opcode;
	(void)payload;
	(void)len;
	return -ENOENT;
}

static inline int osc2_cal_read_by_name(const uint8_t *payload, uint8_t len, uint8_t *out,
					uint8_t *out_len)
{
	(void)payload;
	(void)len;
	(void)out;
	(void)out_len;
	return -ENOENT;
}

static inline int osc2_cal_command(uint8_t opcode, const uint8_t *payload, uint8_t len,
				   size_t avail)
{
	(void)avail;
	if (opcode == 0xE2) {
		/* Nothing to persist: side 0 or 1 is acknowledged. */
		return len && payload[0] > 1 ? -EINVAL : 0;
	}
	return -ENOTSUP;
}

static inline void osc2_cal_frame(struct opensc2_state *s)
{
	(void)s;
}

static inline void osc2_cal_rules(struct opensc2_state *s)
{
	(void)s;
}

static inline uint32_t osc2_cal_setups(void)
{
	return 0;
}

static inline bool osc2_cal_trigger_decided(int side, uint16_t value, bool *bit)
{
	(void)side;
	(void)value;
	(void)bit;
	return false;
}

#endif /* CONFIG_OPENSC2_CAL */

#endif /* OPENSC2_CAL_STORE_H_ */
