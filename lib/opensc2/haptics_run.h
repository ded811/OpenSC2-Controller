/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * haptics_run.h: the haptics synthesiser (CONFIG_OPENSC2_HAPTICS_SYNTH) around its engine, as
 * pure C: the decoded output reports fed in, the four script players and the drop sounds on a
 * millisecond clock, one block of a sequencer at a time, the test hooks' captures and ops, and an
 * outbox of what has to go out (0x44 reports, entries for the test hooks' ring). Not thread safe:
 * haptics.c holds one mutex around every call on Zephyr and runs the sequencers in their threads;
 * the firmware emulator (tests/host) calls it from its one thread with a virtual clock.
 */
#ifndef OPENSC2_HAPTICS_RUN_H_
#define OPENSC2_HAPTICS_RUN_H_

#include <stdbool.h>
#include <stdint.h>
#include <opensc2/types.h>
#include "haptics_core.h"
#include "haptics_rules.h"

/* Entries of the test hooks' ring (docs/test-protocol.md; the kinds 0x60..0x63 are this module's). */
#define OSC2_HAP_RING_GEN     0x60   /* channel, type, what, gain s8, a, b, c, d u16 */
#define OSC2_HAP_RING_BLOCK   0x61   /* path, index u32, CRC-32 u32, peak u16, flags */
#define OSC2_HAP_RING_STREAM  0x62   /* channel, what, fill u16, flags u32 */
#define OSC2_HAP_RING_SCRIPT  0x63   /* what, channel, id, gain s8, extra u16 */

/* HAP_STREAM entries' `what` beyond the engine's (enum osc2_hap_stream_ev). */
#define OSC2_HAP_STREAM_EV_WRITE   6     /* a write: flags = what it reported, fill after it */
#define OSC2_HAP_STREAM_EV_STATUS  7     /* a 0x44 the sequencer sent: flags */

/* HAP_BLOCK flags. */
#define OSC2_HAP_BLOCK_FIRST 0x01        /* the first block after the sequencer was idle */
#define OSC2_HAP_BLOCK_LAST  0x02        /* nothing plays after it: the output drains */

/* HAP_SCRIPT entries' `what`. */
enum osc2_hap_script_ev {
	OSC2_HAP_SCRIPT_EV_START = 1,    /* a script started on a channel: id, gain */
	OSC2_HAP_SCRIPT_EV_REFUSED = 2,  /* busy, or an id outside 1..17 */
	OSC2_HAP_SCRIPT_EV_STEP = 3,     /* a step ran: extra = its index */
	OSC2_HAP_SCRIPT_EV_DONE = 4,     /* the channel's player is free again */
	OSC2_HAP_SCRIPT_EV_CUE = 5,      /* a cue: id = cue, channel = level, gain, extra = script */
	OSC2_HAP_SCRIPT_EV_FALL = 6,     /* a free fall: extra 1 taken, 0 ignored (not armed) */
	OSC2_HAP_SCRIPT_EV_DROP = 7,     /* the drop sound due: id = script (0 none), extra = byte */
	OSC2_HAP_SCRIPT_EV_REARM = 8,    /* free falls count again */
};

/* What has to go out after a call: a 0x44 report or a ring entry. */
#define OSC2_HAP_OUT_STATUS 1            /* data: channel id, flags u32 */
#define OSC2_HAP_OUT_NOTE   2            /* data: a ring entry's data of kind `kind` */
#define OSC2_HAP_OUTBOX 40

struct osc2_hap_out_item {
	uint8_t what;
	uint8_t kind;
	uint8_t len;
	uint8_t data[12];
};

struct osc2_hap_rt {
	struct osc2_hap engine;
	struct osc2_hap_player player[OSC2_HAP_CHANNELS];
	struct osc2_hap_drop drop;
	bool notes;                      /* write ring entries (the test hooks are armed) */
	uint8_t capture[OSC2_HAP_PATHS]; /* blocks still to report a CRC of */
	uint32_t index[OSC2_HAP_PATHS];  /* blocks since the sequencer started from idle */
	int16_t forced_random;           /* the next drop's random byte (the test hooks), or -1 */
	uint32_t underruns[OSC2_HAP_PATHS];
	struct osc2_hap_out_item outbox[OSC2_HAP_OUTBOX];
	uint8_t out_count;
	uint32_t out_lost;
};

/* Everything idle: the engine (osc2_hap_init()), the players, the drop sounds armed. */
void osc2_hap_rt_init(struct osc2_hap_rt *rt);

/* Decoded output reports from the host, `n` of them (two for 0x88: the right grip's go in first,
 * as on the real controller). Queues the commands, writes the stream bytes (their 0x44 flags go
 * into the outbox at once), starts the scripts. `setting27`: the rumble gain offset. Returns the
 * paths that got commands (bit 0 pads, bit 1 grips). */
uint8_t osc2_hap_rt_feed(struct osc2_hap_rt *rt, const struct opensc2_output *out, int n,
			 int16_t setting27, uint32_t now_ms);

/* The scripts and drop sounds due at `now_ms`; `random8` is the system's random byte for a drop
 * (when the test hooks forced none). Returns the paths that got commands. */
uint8_t osc2_hap_rt_poll(struct osc2_hap_rt *rt, uint32_t now_ms, uint8_t random8);

/* Milliseconds until a script step, a drop sound or a re-arm is due (0: now), or -1. */
int32_t osc2_hap_rt_next(const struct osc2_hap_rt *rt, uint32_t now_ms);

/* Start a script on the channels of `channels` (OPENSC2_HAPTIC_* bits), as report 0x85 does;
 * returns how many started. */
uint8_t osc2_hap_rt_script(struct osc2_hap_rt *rt, uint8_t channels, uint8_t id, int8_t gain_db,
			   uint32_t now_ms);

/* A cue on both grips at chime level `level`: the number of grips it started on (a busy grip's
 * player refuses it), or -1 when the cue plays nothing at that level. */
int osc2_hap_rt_cue(struct osc2_hap_rt *rt, uint8_t cue, uint8_t level, uint32_t now_ms);

/* A free fall (from the IMU): true when it was taken (the drop sounds were armed). */
bool osc2_hap_rt_fall(struct osc2_hap_rt *rt, uint32_t now_ms);

/* Queue a command (the public API's clicks, stops and clips). */
bool osc2_hap_rt_queue(struct osc2_hap_rt *rt, const struct osc2_hap_cmd *cmd);

/* One block of `path`: the queued commands applied, the block rendered into `out` (its CRC and
 * peak reported while a capture runs), the events into the outbox. `first`: the first block of a
 * playback. Returns the block's bytes; `*busy` whether anything plays after it. */
size_t osc2_hap_rt_block(struct osc2_hap_rt *rt, uint8_t path, bool first, void *out, bool *busy);

/* Apply the queued commands of an idle `path` (formats and stops are answered then too); returns
 * whether something plays now. */
bool osc2_hap_rt_idle(struct osc2_hap_rt *rt, uint8_t path);

/* Take what has to go out, oldest first. */
uint8_t osc2_hap_rt_take(struct osc2_hap_rt *rt, struct osc2_hap_out_item *out, uint8_t max);

/* ---- the test hooks' ops 0x80..0x83 (docs/test-protocol.md) ---- */

#define OSC2_HAP_OP_RESET   0x80   /* seed u32: everything stops, the rumble's random gains reseeded */
#define OSC2_HAP_OP_CAPTURE 0x81   /* path u8 (0, 1, 2 both), blocks u8: CRCs of the next blocks */
#define OSC2_HAP_OP_STATUS  0x82   /* the synthesiser's state, 38 bytes */
#define OSC2_HAP_OP_PLAY    0x83   /* what u8, then its arguments; result s32 */

/* HAP_PLAY's `what`. */
#define OSC2_HAP_PLAY_CUE    0     /* cue u8, level u8 */
#define OSC2_HAP_PLAY_CLICK  1     /* channels u8, click u8 (1 A, 2 B), gain s8 */
#define OSC2_HAP_PLAY_SCRIPT 2     /* channels u8, id u8, gain s8 */
#define OSC2_HAP_PLAY_FALL   3     /* the random byte the drop sound will use, u8 */
#define OSC2_HAP_PLAY_STOP   4     /* channels u8 */

#define OSC2_HAP_STATUS_LEN 38

/* What the glue adds to the status: per path whether an output of the application's is set,
 * and the amplifier mode in effect. */
struct osc2_hap_rt_glue {
	uint8_t outputs;                 /* bit per path: an application output is set */
	uint8_t amp_mode;                /* controller setting 75 */
	uint8_t features;                /* bit 0: drop sounds built */
};

/* Run one op; returns the test channel's status (0 OK, 1 BAD_OP, 2 BAD_LEN, 4 BAD_ARG) and fills
 * `result` / `*rlen`. `*wake` gets the paths that got commands; `*schedule` is set when a script
 * or drop sound may have become due. */
uint8_t osc2_hap_rt_test_op(struct osc2_hap_rt *rt, uint8_t op, const uint8_t *args, uint8_t alen,
			    uint32_t now_ms, const struct osc2_hap_rt_glue *glue, uint8_t *result,
			    uint8_t *rlen, uint8_t *wake, bool *schedule);

#endif /* OPENSC2_HAPTICS_RUN_H_ */
