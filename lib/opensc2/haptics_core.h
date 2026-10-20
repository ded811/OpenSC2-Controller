/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * haptics_core.h: the haptics synthesiser's engine (CONFIG_OPENSC2_HAPTICS_SYNTH) as pure C: the
 * four channels on two sequencers, their generators, the command queues, the PCM stream rings and
 * the rendering of blocks. No kernel, no locks, nothing that waits: haptics.c runs it on Zephyr
 * (one thread per sequencer, a mutex around every call), the host tests and the firmware emulator
 * run it as it is.
 *
 * It follows the protocol documentation's description of the real controller's sequencers (not
 * public yet; docs/protocol.md lists the pages and the choices made where it is silent):
 *
 *   sequencer   channels                     rate      block        samples out
 *   0 pads      0 left pad, 1 right pad      4000 Hz   32 frames    unsigned 8-bit, silence 128
 *   1 grips     2 left grip, 3 right grip    8000 Hz   64 frames    signed 16-bit
 *
 * A frame is two samples, the left channel's first. The channel gain is the master gain (controller
 * setting 76), negated for the left pad and the right grip, as on the real controller. Each
 * channel runs up to eight generators at once, one of each type; their samples, each scaled by its
 * own gain, are summed, scaled by the channel gain and clipped. The first block after an idle
 * sequencer starts begins with 4 ms of silence. The pads refuse the rumble generator, the grips
 * the two clicks.
 */
#ifndef OPENSC2_HAPTICS_CORE_H_
#define OPENSC2_HAPTICS_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "haptics_dsp.h"

#if defined(CONFIG_OPENSC2_HAPTICS_QUEUE_DEPTH)
#define OSC2_HAP_QUEUE_MAX CONFIG_OPENSC2_HAPTICS_QUEUE_DEPTH
#else
#define OSC2_HAP_QUEUE_MAX 20      /* commands a sequencer holds, as on the real controller */
#endif
#define OSC2_HAP_EVENTS_MAX 24     /* events a sequencer keeps until the glue takes them */
#define OSC2_HAP_RING_SIZE 512     /* bytes of a channel's stream ring */
#define OSC2_HAP_CHANNELS 4
#define OSC2_HAP_PATHS 2
#define OSC2_HAP_PADS 0
#define OSC2_HAP_GRIPS 1
#define OSC2_HAP_PAD_RATE 4000
#define OSC2_HAP_GRIP_RATE 8000
#define OSC2_HAP_PAD_FRAMES 32
#define OSC2_HAP_GRIP_FRAMES 64
#define OSC2_HAP_BLOCK_MAX_BYTES (OSC2_HAP_GRIP_FRAMES * 2 * 2)   /* a grip block */
#define OSC2_HAP_LCG_SEED 5323u    /* the rumble's burst gains start here at boot */

/* Channels: OPENSC2_HAPTIC_* bit numbers. */
#define OSC2_HAP_CH_LEFT_PAD 0
#define OSC2_HAP_CH_RIGHT_PAD 1
#define OSC2_HAP_CH_LEFT_GRIP 2
#define OSC2_HAP_CH_RIGHT_GRIP 3

static inline uint8_t osc2_hap_path_of(uint8_t channel)
{
	return channel >> 1;
}

/* The channel ids of input report 0x44: left grip 0, right grip 1, left pad 3, right pad 4. */
uint8_t osc2_hap_status_id(uint8_t channel);

/* Generator types, numbered as the real controller numbers them. */
enum osc2_hap_gen_type {
	OSC2_HAP_GEN_CLICK_A = 0,
	OSC2_HAP_GEN_CLICK_B = 1,
	OSC2_HAP_GEN_PULSE = 2,
	OSC2_HAP_GEN_TONE = 3,
	OSC2_HAP_GEN_RUMBLE = 4,
	OSC2_HAP_GEN_SWEEP = 5,
	OSC2_HAP_GEN_STREAM = 6,
	OSC2_HAP_GEN_CLIP = 7,
	OSC2_HAP_GEN_COUNT = 8,
	OSC2_HAP_GEN_ALL = 9,          /* with a stop: every type, a stop-all */
};

/* The flags of input report 0x44 (page 8's flow control). */
#define OSC2_HAP_FLAG_DROPPED   0x01u   /* a write did not fit; the bytes that fit were kept */
#define OSC2_HAP_FLAG_STOPPED   0x02u   /* the stream ran dry (or was stopped) */
#define OSC2_HAP_FLAG_LOW       0x04u   /* playing, less than the water mark left: send more */
#define OSC2_HAP_FLAG_HIGH      0x08u   /* a write left less than the water mark free: pause */
#define OSC2_HAP_FLAG_NEED_8K   0x10u   /* an 8 kHz format asked for on a 4 kHz (pad) channel */
#define OSC2_HAP_FLAG_FORMAT    0x20u   /* the format was set */
#define OSC2_HAP_FLAG_BUSY      0x40u   /* the format cannot change while the stream plays */

/* ---- commands ---------------------------------------------------------------------------------- */

enum osc2_hap_op {
	OSC2_HAP_OP_START = 0,         /* start generator `type` on the channel */
	OSC2_HAP_OP_STOP = 1,          /* stop `type` (OSC2_HAP_GEN_ALL: every generator) */
	OSC2_HAP_OP_FORMAT = 2,        /* set the channel's stream format to `format` */
};

#define OSC2_HAP_CMD_INVERT  0x01u     /* tone, rumble, sweep: start the sine downwards */

struct osc2_hap_cmd {
	uint8_t op;                    /* enum osc2_hap_op */
	uint8_t channel;               /* 0..3 */
	uint8_t type;                  /* enum osc2_hap_gen_type */
	int8_t gain_db;                /* the generator's gain (pulse trains play at full scale) */
	uint8_t flags;                 /* OSC2_HAP_CMD_* */
	uint8_t format;                /* FORMAT: the format byte; CLIP: the clip's format */
	uint16_t duration_ms;          /* tone, rumble, sweep; 0 for a tone: until replaced */
	union {
		struct {
			uint16_t on_us, off_us, count;
		} pulse;
		struct {
			uint32_t freq_mhz;
			uint32_t lfo_mhz;      /* 0: no modulation */
			uint8_t depth;         /* percent, 0..255 */
		} tone;
		struct {
			uint32_t freq_mhz;
			uint32_t burst_mhz;    /* bursts a second, in mHz (at most 120 Hz) */
		} rumble;
		struct {
			uint16_t f0_hz, f1_hz;
		} sweep;
		struct {
			const uint8_t *data;   /* stays valid while it plays */
			uint16_t samples;
		} clip;
	};
};

/* ---- events: what the glue sends (0x44) or writes into the test hooks' ring -------------------- */

enum osc2_hap_ev_kind {
	OSC2_HAP_EV_STATUS = 1,        /* input report 0x44 to send: `flags` for the channel */
	OSC2_HAP_EV_GEN = 2,           /* a generator: `what` OSC2_HAP_GEN_EV_*, params a..d */
	OSC2_HAP_EV_STREAM = 3,        /* the stream: `what` OSC2_HAP_STREAM_EV_*, `a` its fill */
};

enum osc2_hap_gen_ev {
	OSC2_HAP_GEN_EV_START = 1,     /* started (or restarted over a running one) */
	OSC2_HAP_GEN_EV_STOP = 2,      /* stopped by a command */
	OSC2_HAP_GEN_EV_END = 3,       /* reached its end */
	OSC2_HAP_GEN_EV_REFUSED = 4,   /* this sequencer does not play the type */
	OSC2_HAP_GEN_EV_LOST = 5,      /* the queue was full: the command is lost */
	OSC2_HAP_GEN_EV_IGNORED = 6,   /* parameters that play nothing (0 Hz, an empty period) */
};

enum osc2_hap_stream_ev {
	OSC2_HAP_STREAM_EV_QUEUED = 1, /* a write found enough audio: a start is queued */
	OSC2_HAP_STREAM_EV_START = 2,  /* the stream generator runs */
	OSC2_HAP_STREAM_EV_DRY = 3,    /* it ran dry */
	OSC2_HAP_STREAM_EV_EMPTIED = 4,/* a stop emptied the ring */
	OSC2_HAP_STREAM_EV_FORMAT = 5, /* a format command: `flags` what it answered */
};

struct osc2_hap_event {
	uint8_t kind;                  /* enum osc2_hap_ev_kind */
	uint8_t channel;               /* 0..3 (osc2_hap_status_id() for a 0x44) */
	uint8_t what;
	uint8_t type;                  /* the generator type (GEN) */
	int8_t gain_db;
	uint16_t a, b, c, d;           /* GEN: by type (osc2_hap_cmd_params()); STREAM: a = fill */
	uint32_t flags;                /* STATUS and STREAM: 0x44 flags */
};

/* ---- the engine's state ----------------------------------------------------------------------- */

struct osc2_hap_gen {
	bool on;
	bool invert;
	bool endless;                  /* a tone of duration 0 */
	uint32_t gain_q16;
	uint32_t left;                 /* samples left (unless endless) */
	union {
		struct {
			uint32_t phase, step, lfo_phase, lfo_step;
			int32_t depth_q15;     /* depth / 200 in Q15; 0: no modulation */
		} tone;
		struct {
			uint64_t phase;        /* since the burst began; two cycles are played */
			uint32_t step;
			uint32_t pos, slot;    /* samples into the burst slot, its length */
			uint32_t burst_q16;    /* this burst's gain */
			bool started;          /* the first burst has played (later ones are random) */
		} rumble;
		struct {
			uint32_t phase, step0;
			int64_t slope;         /* log2(f1 / f0) / samples, Q32; 0: equal frequencies */
			uint32_t n, total;
			uint32_t fade_in, fade_out;
		} sweep;
		struct {
			uint32_t pos, period, on;
			uint32_t periods_left;
		} pulse;
		struct {
			const uint8_t *data;
			uint32_t samples, next;   /* clip samples, the next one to take */
			uint8_t enc, ratio, step;
			int32_t a, b;
		} clip;
		struct {
			uint8_t enc, ratio, step;
			int32_t a, b;
		} stream;
	};
};

struct osc2_hap_stream {
	uint8_t buf[OSC2_HAP_RING_SIZE];
	uint16_t head, count;          /* the oldest byte, bytes held */
	uint8_t format;                /* in effect (0..11) */
	bool format_set;               /* a format was set: the stream can start */
	bool playing;                  /* the stream generator runs */
	bool start_queued;
	bool low_armed;                /* flag 0x04 may go out (after an 0x08) */
};

struct osc2_hap_channel {
	struct osc2_hap_gen gen[OSC2_HAP_GEN_COUNT];
	struct osc2_hap_stream stream;
};

struct osc2_hap_seq {
	uint8_t path;
	uint16_t rate;
	uint8_t frames;                /* per block */
	uint8_t lead_in;               /* silent frames at the start of a first block */
	uint8_t refused;               /* bit per generator type this sequencer does not play */
	uint8_t negated;               /* the local channel (0 left, 1 right) whose gain is negated */
	struct osc2_hap_channel ch[2];
	struct osc2_hap_cmd queue[OSC2_HAP_QUEUE_MAX];
	uint8_t q_head, q_count;
	struct osc2_hap_event ev[OSC2_HAP_EVENTS_MAX];
	uint8_t ev_count;
	/* counters since the engine was initialised */
	uint32_t blocks;               /* blocks rendered */
	uint32_t lost;                 /* commands lost to a full queue */
	uint32_t ev_lost;              /* events the glue did not take in time */
};

struct osc2_hap {
	struct osc2_hap_seq seq[OSC2_HAP_PATHS];
	uint32_t lcg;                  /* the rumble's burst gains */
	int16_t master_db;             /* controller setting 76 (the glue refreshes it) */
	bool notes;                    /* record GEN and STREAM events (the test hooks are armed) */
};

/* ---- the engine -------------------------------------------------------------------------------- */

/* Both sequencers idle, every stream without a format, the rumble's random burst gains at their
 * boot seed, the master gain at -3 dB (setting 76's default). */
void osc2_hap_init(struct osc2_hap *h);

/* Restart the rumble's random burst gains from `seed` (the test hooks, for byte-exact blocks). */
void osc2_hap_seed(struct osc2_hap *h, uint32_t seed);

/* Everything stops at once, the queues and stream rings empty, the formats forgotten (the test
 * hooks' reset; no events). */
void osc2_hap_reset(struct osc2_hap *h);

/* Queue a command for its channel's sequencer, as the real controller does: a type the sequencer
 * does not play is turned away (a REFUSED event), and a full queue loses the command (LOST).
 * Returns whether it was queued. */
bool osc2_hap_queue(struct osc2_hap *h, const struct osc2_hap_cmd *cmd);

/* Apply every queued command of sequencer `path` (between blocks). */
void osc2_hap_apply(struct osc2_hap *h, uint8_t path);

/* A command is waiting in the queue of `path`. */
bool osc2_hap_pending(const struct osc2_hap *h, uint8_t path);

/* A generator plays on one of the channels of `path`. */
bool osc2_hap_busy(const struct osc2_hap *h, uint8_t path);

/* Bit per channel of `path` (bit 0 left) and generator type: what plays, for a status. */
uint16_t osc2_hap_gen_mask(const struct osc2_hap *h, uint8_t channel);

/* Render the next block of `path` into `out` (32 frames of two uint8_t for the pads, 64 frames of
 * two int16_t for the grips); `first`: the first block after the sequencer was idle, which begins
 * with the lead-in. Returns the block's size in bytes. */
size_t osc2_hap_render(struct osc2_hap *h, uint8_t path, bool first, void *out);

/* A stream write (output reports 0x87, 0x88, 0x89) to `channel`, from the path that received it:
 * the bytes that fit go into the ring; a start is queued once the ring already held more than
 * 16 ms of audio on a grip (8 ms on a pad) before this write and a format is set. Returns the 0x44
 * flags the write itself reports (0x01 or 0x08, or 0). */
uint32_t osc2_hap_stream_write(struct osc2_hap *h, uint8_t channel, const uint8_t *data,
			       size_t len);

/* Bytes in the ring of `channel`, and its format (0xFF: none set). */
uint16_t osc2_hap_stream_fill(const struct osc2_hap *h, uint8_t channel);
uint8_t osc2_hap_stream_format(const struct osc2_hap *h, uint8_t channel);

/* Take the events of `path` (oldest first) into `out`; returns how many. */
uint8_t osc2_hap_take_events(struct osc2_hap *h, uint8_t path, struct osc2_hap_event *out,
			     uint8_t max);

/* The water mark and the start threshold of a format on `path`, in bytes. */
uint16_t osc2_hap_water_mark(uint8_t format);
uint16_t osc2_hap_start_threshold(uint8_t path, uint8_t format);

/* A command's parameters as the four u16 of a GEN event (and of the test hooks' ring entry):
 * pulse on us, off us, count, period samples; tone Hz, ms, LFO cHz, depth (bit 15 inverted);
 * rumble Hz, ms, burst cHz, bit 15 inverted; sweep start Hz, end Hz, ms, bit 15 inverted; clicks
 * and clips samples, format. */
void osc2_hap_cmd_params(const struct osc2_hap_cmd *cmd, uint16_t p[4]);

/* The largest |sample| of a block, Q15 (pads: |u8 - 128| * 32767 / 127). */
uint16_t osc2_hap_block_peak(uint8_t path, const void *block);

#endif /* OPENSC2_HAPTICS_CORE_H_ */
