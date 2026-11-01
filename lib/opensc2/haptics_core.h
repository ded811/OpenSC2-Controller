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
 * It follows the protocol documentation's description of the real controller's sequencers, rule
 * for rule, the accidents of the real arithmetic included (docs/protocol.md lists what stays
 * different and why):
 *
 *   sequencer   channels                     rate      block        samples out
 *   0 pads      0 left pad, 1 right pad      4000 Hz   32 frames    unsigned 8-bit, silence 128
 *   1 grips     2 left grip, 3 right grip    8000 Hz   64 frames    signed 16-bit
 *
 * A frame is two samples, the left channel's first. Each channel has its own gain, set by a
 * configure command (the master gain, controller setting 76, which the glue hands to all four
 * channels; negated for the left pad and the right grip) and 0, silence, until then. Each channel
 * runs up to eight generators at once, one of each type; their samples, each scaled by its own
 * gain, are summed, scaled by the channel gain and clipped. A generator that ends by itself stays
 * in the mix, silent, for 1 more sample on the pads and 4 on the grips. The first block after an
 * idle sequencer starts begins with 4 ms of silence. The pads refuse the rumble generator, the
 * grips the two clicks.
 */
#ifndef OPENSC2_HAPTICS_CORE_H_
#define OPENSC2_HAPTICS_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <opensc2/haptics.h>
#include "haptics_dsp.h"

#if defined(CONFIG_OPENSC2_HAPTICS_QUEUE_DEPTH)
#define OSC2_HAP_QUEUE_MAX CONFIG_OPENSC2_HAPTICS_QUEUE_DEPTH
#else
#define OSC2_HAP_QUEUE_MAX 20      /* commands a sequencer holds, as on the real controller */
#endif
#define OSC2_HAP_EVENTS_MAX 24     /* events a sequencer keeps until the glue takes them */
#define OSC2_HAP_RING_SIZE 512     /* bytes of a channel's stream ring */
#define OSC2_HAP_FIFO_MAX 8        /* interpolated values a clip or stream keeps ahead */
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

/* The flags of input report 0x44 (the protocol documentation's flow control). */
#define OSC2_HAP_FLAG_DROPPED   0x01u   /* a write did not fit; the bytes that fit were kept */
#define OSC2_HAP_FLAG_STOPPED   0x02u   /* the stream ran dry, or was stopped */
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
	OSC2_HAP_OP_GAIN = 3,          /* the channel gain: `gain_db` (the master gain) */
	OSC2_HAP_OP_AMP = 4,           /* the sequencer's amplifier mode: `format` 0 follow, 1 held */
};

#define OSC2_HAP_CMD_INVERT  0x01u     /* tone, rumble: a negative frequency; sweep: negated */

struct osc2_hap_cmd {
	uint8_t op;                    /* enum osc2_hap_op */
	uint8_t channel;               /* 0..3 */
	uint8_t type;                  /* enum osc2_hap_gen_type */
	int8_t gain_db;                /* the generator's gain (pulse trains play at full scale) */
	uint8_t flags;                 /* OSC2_HAP_CMD_* */
	uint8_t format;                /* FORMAT: the format byte; CLIP: the clip's format; AMP */
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
			uint32_t burst_mhz;    /* bursts a second, in mHz (0: one burst only) */
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
	OSC2_HAP_EV_STATUS = 1,        /* input report 0x44 to send: `flags` for the channel
					* (`what` 1: the 0x02 that answers a stop) */
	OSC2_HAP_EV_GEN = 2,           /* a generator: `what` OSC2_HAP_GEN_EV_*, params a..d */
	OSC2_HAP_EV_STREAM = 3,        /* the stream: `what` OSC2_HAP_STREAM_EV_*, `a` its fill */
	OSC2_HAP_EV_AMP = 4,           /* the amplifier mode was applied: `what` 0 follow, 1 held */
};

enum osc2_hap_gen_ev {
	OSC2_HAP_GEN_EV_START = 1,     /* started (or restarted over a running one) */
	OSC2_HAP_GEN_EV_STOP = 2,      /* stopped by a command */
	OSC2_HAP_GEN_EV_END = 3,       /* reached its end */
	OSC2_HAP_GEN_EV_REFUSED = 4,   /* this sequencer does not play the type */
	OSC2_HAP_GEN_EV_LOST = 5,      /* the queue was full: the command is lost */
	OSC2_HAP_GEN_EV_IGNORED = 6,   /* (no longer made: every start plays, as on the real one) */
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

/* Interpolated values a clip or the stream has computed ahead (Q15, gain included). As on the
 * real controller nothing empties it when the generator stops: what is left plays first at its
 * next start. */
struct osc2_hap_fifo {
	int32_t v[OSC2_HAP_FIFO_MAX];
	uint8_t head, count;
};

struct osc2_hap_gen {
	bool on;                       /* producing samples */
	uint8_t release;               /* samples it stays in the mix once it ended by itself */
	int32_t gain_q16;              /* its own gain (negative: a negated sweep) */
	union {
		struct {
			int64_t phase, step, lfo_phase, lfo_step;   /* Q32 cycles */
			int32_t depth_q15;     /* LFO depth / 200 */
			bool lfo;
			uint32_t len, pos;     /* samples; ends when pos reaches len (0: 2^32) */
		} tone;
		struct {
			int64_t phase, step;
			uint32_t period, count;  /* samples between bursts; since the last one */
			uint32_t burst_q16;      /* this burst's gain */
			uint32_t len, pos;       /* plays len + 1 samples */
			uint16_t cycles;         /* sine periods into this burst; two are played */
		} rumble;
		struct {
			uint8_t mode;            /* how the sweep's arithmetic comes out (haptics_core.c) */
			uint16_t pos, len;       /* 16-bit: len 0 runs 65,536 samples */
			uint16_t fade_in, fade_out;
			uint32_t phase;          /* Q32 cycles, modulo a cycle */
			uint64_t step0;          /* the first sample's step, Q32 cycles */
			int64_t slope;           /* log2(f1 / f0) per sample, Q32, rounded */
			int32_t value;           /* the constant sine of a wrapped falling sweep */
		} sweep;
		struct {
			uint16_t pos, repeats, period, on;   /* 16-bit, as on the real controller */
		} pulse;
		struct {
			struct opensc2_haptics_sound sound;
			uint16_t pos;
			uint8_t step;            /* output samples per clip sample (0, 1: no steps) */
		} clip;
		struct {
			uint8_t bytes;           /* per sample (0 until the stream first started) */
			uint8_t step;
		} stream;
	};
};

struct osc2_hap_stream {
	uint8_t buf[OSC2_HAP_RING_SIZE];
	uint16_t head, count;          /* the oldest byte, bytes held */
	uint8_t format;                /* the last format byte set (0 at boot), 12 and above too */
	bool format_set;               /* a format was set (for the status only) */
	uint16_t water;                /* the water mark, bytes (0 until a format is set) */
	uint16_t start_space;          /* a write starts the stream when less is free (0: never) */
	bool need_data;                /* 0x04 sent or not wanted: none until the next 0x08 */
};

struct osc2_hap_channel {
	struct osc2_hap_gen gen[OSC2_HAP_GEN_COUNT];
	uint8_t mix;                   /* bit per generator type still in the mix */
	int32_t gain_q16;              /* the channel gain, 0 until configured */
	struct osc2_hap_fifo fifo[OSC2_HAP_GEN_COUNT];
	struct osc2_hap_stream stream;
};

struct osc2_hap_seq {
	uint8_t path;
	uint16_t rate;
	uint8_t frames;                /* per block */
	uint8_t lead_in;               /* silent frames at the start of a first block */
	uint8_t release;               /* samples an ended generator stays in the mix */
	uint8_t refused;               /* bit per generator type this sequencer does not play */
	uint8_t negated;               /* the local channel (0 left, 1 right) whose gain is negated */
	bool amp_held;                 /* amplifier mode 1: the enable held on */
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
	struct opensc2_haptics_sound click[2];  /* clicks A and B (OpenSC2's own unless replaced) */
	bool notes;                    /* record GEN and STREAM events (the test hooks are armed) */
};

/* ---- the engine -------------------------------------------------------------------------------- */

/* Both sequencers idle, every stream without a format, every channel gain 0 (silent until the
 * master gain is configured), the rumble's random burst gains at their boot seed, OpenSC2's own
 * clicks. */
void osc2_hap_init(struct osc2_hap *h);

/* Restart the rumble's random burst gains from `seed` (the test hooks, for byte-exact blocks). */
void osc2_hap_seed(struct osc2_hap *h, uint32_t seed);

/* Everything stops at once, the queues, stream rings and interpolation values empty, the formats
 * forgotten (the test hooks' reset; no events). The channel gains, the amplifier modes and the
 * clicks stay. */
void osc2_hap_reset(struct osc2_hap *h);

/* Set every channel gain to `db` at once, negated where the channel is (the test hooks' reset and
 * the host tests; the glue configures them through the queue, as the real controller does). */
void osc2_hap_set_gains(struct osc2_hap *h, int8_t db);

/* Click A (0) or B (1) as `sound` from the next click on; NULL: OpenSC2's own. */
void osc2_hap_set_click(struct osc2_hap *h, uint8_t click, const struct opensc2_haptics_sound *sound);

/* Queue a command for its channel's sequencer, as the real controller does: a start or stop of a
 * type the sequencer does not play is turned away (a REFUSED event), and a full queue loses the
 * command (LOST). Returns whether it was queued. */
bool osc2_hap_queue(struct osc2_hap *h, const struct osc2_hap_cmd *cmd);

/* Apply every queued command of sequencer `path` (between blocks, or while it is idle). */
void osc2_hap_apply(struct osc2_hap *h, uint8_t path);

/* A command is waiting in the queue of `path`. */
bool osc2_hap_pending(const struct osc2_hap *h, uint8_t path);

/* A generator is in the mix of one of the channels of `path` (playing, or just ended). */
bool osc2_hap_busy(const struct osc2_hap *h, uint8_t path);

/* Bit per generator type that is producing samples on `channel`, for a status. */
uint16_t osc2_hap_gen_mask(const struct osc2_hap *h, uint8_t channel);

/* Render the next block of `path` into `out` (32 frames of two uint8_t for the pads, 64 frames of
 * two int16_t little endian for the grips); `first`: the first block after the sequencer was
 * idle, which begins with the lead-in. Returns the block's size in bytes. */
size_t osc2_hap_render(struct osc2_hap *h, uint8_t path, bool first, void *out);

/* A stream write (output reports 0x87, 0x88, 0x89) to `channel`, as the real controller takes
 * it: while the stream generator is not playing, a write that finds less room than the format's
 * start point queues a start (each such write, until the start is applied); a write that leaves
 * less than the water mark free reports 0x08 (and allows the next 0x04); the bytes that fit go in,
 * and 0x01 if some did not. Returns the 0x44 flags of the write itself (0, 0x01, 0x08). */
uint32_t osc2_hap_stream_write(struct osc2_hap *h, uint8_t channel, const uint8_t *data,
			       size_t len);

/* Bytes in the ring of `channel`, and its format (0xFF: none set since the reset). */
uint16_t osc2_hap_stream_fill(const struct osc2_hap *h, uint8_t channel);
uint8_t osc2_hap_stream_format(const struct osc2_hap *h, uint8_t channel);

/* Take the events of `path` (oldest first) into `out`; returns how many. */
uint8_t osc2_hap_take_events(struct osc2_hap *h, uint8_t path, struct osc2_hap_event *out,
			     uint8_t max);

/* The water mark and the start threshold (more than this held before a write starts the stream)
 * of a format on `path`, in bytes. */
uint16_t osc2_hap_water_mark(uint8_t format);
uint16_t osc2_hap_start_threshold(uint8_t path, uint8_t format);

/* A command's parameters as the four u16 of a GEN event (and of the test hooks' ring entry):
 * pulse on us, off us, count; tone Hz, ms, LFO cHz, depth (bit 15 inverted);
 * rumble Hz, ms, burst cHz, bit 15 inverted; sweep start Hz, end Hz, ms, bit 15 inverted; clicks
 * and clips samples, format. */
void osc2_hap_cmd_params(const struct osc2_hap_cmd *cmd, uint16_t p[4]);

/* The largest |sample| of a block, Q15 (pads: |u8 - 128| * 32767 / 127). */
uint16_t osc2_hap_block_peak(uint8_t path, const void *block);

#endif /* OPENSC2_HAPTICS_CORE_H_ */
