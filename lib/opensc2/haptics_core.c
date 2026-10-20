/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * haptics_core.c: the haptics synthesiser's engine (haptics_core.h). Pure C: integers only, no
 * kernel, no allocation.
 *
 * The generators follow the protocol documentation's description of the real controller's (not
 * public yet; docs/protocol.md lists the pages). Where it leaves a detail open, or describes an
 * accident of the real arithmetic that no host can observe, this file keeps to the simplest
 * correct behaviour and says so; docs/protocol.md's gap list has each choice.
 */
#include <string.h>
#include "haptics_core.h"

#define BIT8(n) ((uint8_t)(1u << (n)))
#define LFO_START_PHASE 0xE0000000u          /* -pi/4: at full depth the modulation starts low */
#define TWO_CYCLES ((uint64_t)2 << 32)       /* a rumble burst plays two periods of its sine */
#define SWEEP_FADE_IN_MS 80
#define SWEEP_FADE_OUT_MS 5
#define RUMBLE_MAX_BURST_MHZ 120000u         /* rates are capped at 120 Hz */
#define STATUS_RESERVE 8                     /* event slots kept for 0x44 statuses */

/* A rate in mHz as the 0.01 Hz of a ring entry's u16 (0xFFFF from 655.35 Hz up). */
static uint16_t centi_hz(uint32_t mhz)
{
	return mhz / 10u > 0xFFFFu ? 0xFFFFu : (uint16_t)(mhz / 10u);
}

uint8_t osc2_hap_status_id(uint8_t channel)
{
	static const uint8_t ids[OSC2_HAP_CHANNELS] = { 3, 4, 0, 1 };

	return ids[channel & 3u];
}

/* ---- events ----------------------------------------------------------------------------------- */

static void push_event(struct osc2_hap_seq *s, const struct osc2_hap_event *e)
{
	uint8_t room = e->kind == OSC2_HAP_EV_STATUS ? OSC2_HAP_EVENTS_MAX :
		       OSC2_HAP_EVENTS_MAX - STATUS_RESERVE;

	if (s->ev_count < room) {
		s->ev[s->ev_count++] = *e;
	} else {
		s->ev_lost++;
	}
}

static void status(struct osc2_hap_seq *s, uint8_t channel, uint32_t flags)
{
	struct osc2_hap_event e = {
		.kind = OSC2_HAP_EV_STATUS, .channel = channel, .flags = flags,
	};

	push_event(s, &e);
}

static void note_gen(const struct osc2_hap *h, struct osc2_hap_seq *s, uint8_t channel,
		     uint8_t type, uint8_t what, int8_t gain_db, const uint16_t *p)
{
	struct osc2_hap_event e = {
		.kind = OSC2_HAP_EV_GEN, .channel = channel, .what = what, .type = type,
		.gain_db = gain_db,
	};

	if (!h->notes) {
		return;
	}
	if (p) {
		e.a = p[0];
		e.b = p[1];
		e.c = p[2];
		e.d = p[3];
	}
	push_event(s, &e);
}

static void note_stream(const struct osc2_hap *h, struct osc2_hap_seq *s, uint8_t channel,
			uint8_t what, uint32_t flags)
{
	struct osc2_hap_event e = {
		.kind = OSC2_HAP_EV_STREAM, .channel = channel, .what = what, .flags = flags,
		.a = s->ch[channel & 1u].stream.count,
	};

	if (h->notes) {
		push_event(s, &e);
	}
}

uint8_t osc2_hap_take_events(struct osc2_hap *h, uint8_t path, struct osc2_hap_event *out,
			     uint8_t max)
{
	struct osc2_hap_seq *s = &h->seq[path];
	uint8_t n = s->ev_count < max ? s->ev_count : max;

	memcpy(out, s->ev, n * sizeof(out[0]));
	if (n < s->ev_count) {
		memmove(s->ev, &s->ev[n], (s->ev_count - n) * sizeof(s->ev[0]));
	}
	s->ev_count -= n;
	return n;
}

/* ---- set-up ----------------------------------------------------------------------------------- */

static void seq_setup(struct osc2_hap_seq *s, uint8_t path)
{
	uint32_t blocks = s->blocks, lost = s->lost, ev_lost = s->ev_lost;

	memset(s, 0, sizeof(*s));
	s->blocks = blocks;
	s->lost = lost;
	s->ev_lost = ev_lost;
	s->path = path;
	if (path == OSC2_HAP_PADS) {
		s->rate = OSC2_HAP_PAD_RATE;
		s->frames = OSC2_HAP_PAD_FRAMES;
		s->lead_in = 16;
		s->refused = BIT8(OSC2_HAP_GEN_RUMBLE);
		s->negated = 0;                    /* the left pad */
	} else {
		s->rate = OSC2_HAP_GRIP_RATE;
		s->frames = OSC2_HAP_GRIP_FRAMES;
		s->lead_in = 32;
		s->refused = BIT8(OSC2_HAP_GEN_CLICK_A) | BIT8(OSC2_HAP_GEN_CLICK_B);
		s->negated = 1;                    /* the right grip */
	}
}

void osc2_hap_init(struct osc2_hap *h)
{
	memset(h, 0, sizeof(*h));
	seq_setup(&h->seq[OSC2_HAP_PADS], OSC2_HAP_PADS);
	seq_setup(&h->seq[OSC2_HAP_GRIPS], OSC2_HAP_GRIPS);
	h->lcg = OSC2_HAP_LCG_SEED;
	h->master_db = -3;
}

void osc2_hap_seed(struct osc2_hap *h, uint32_t seed)
{
	h->lcg = seed;
}

void osc2_hap_reset(struct osc2_hap *h)
{
	seq_setup(&h->seq[OSC2_HAP_PADS], OSC2_HAP_PADS);
	seq_setup(&h->seq[OSC2_HAP_GRIPS], OSC2_HAP_GRIPS);
}

/* ---- formats ---------------------------------------------------------------------------------- */

static uint8_t fmt_enc(uint8_t format)
{
	return format / 4u;
}

static uint16_t fmt_rate(uint8_t format)
{
	return (uint16_t)(8000u >> (format % 4u));
}

uint16_t osc2_hap_water_mark(uint8_t format)
{
	/* 128 samples at 8 kHz, scaled to the format's rate and sample size: 16 ms of audio. */
	return (uint16_t)((128u >> (format % 4u)) * osc2_hap_enc_bytes(fmt_enc(format)));
}

uint16_t osc2_hap_start_threshold(uint8_t path, uint8_t format)
{
	/* More than 16 ms of audio on a grip, 8 ms on a pad. */
	uint16_t mark = osc2_hap_water_mark(format);

	return path == OSC2_HAP_PADS ? mark / 2u : mark;
}

/* Samples of a clip or stream per output sample: interpolated up to the channel's rate when it is
 * lower; a faster one is not slowed down (one sample per output sample). */
static uint8_t ratio_of(uint16_t channel_rate, uint16_t rate)
{
	return rate < channel_rate ? (uint8_t)(channel_rate / rate) : 1u;
}

/* ---- the queue -------------------------------------------------------------------------------- */

void osc2_hap_cmd_params(const struct osc2_hap_cmd *cmd, uint16_t p[4])
{
	uint16_t inv = (cmd->flags & OSC2_HAP_CMD_INVERT) ? 0x8000u : 0u;

	memset(p, 0, 4 * sizeof(p[0]));
	if (cmd->op == OSC2_HAP_OP_FORMAT) {
		p[0] = cmd->format;
		return;
	}
	if (cmd->op != OSC2_HAP_OP_START) {
		return;
	}
	switch (cmd->type) {
	case OSC2_HAP_GEN_PULSE:
		p[0] = cmd->pulse.on_us;
		p[1] = cmd->pulse.off_us;
		p[2] = cmd->pulse.count;
		break;
	case OSC2_HAP_GEN_TONE:
		p[0] = (uint16_t)(cmd->tone.freq_mhz / 1000u);
		p[1] = cmd->duration_ms;
		p[2] = centi_hz(cmd->tone.lfo_mhz);
		p[3] = (uint16_t)(cmd->tone.depth | inv);
		break;
	case OSC2_HAP_GEN_RUMBLE:
		p[0] = (uint16_t)(cmd->rumble.freq_mhz / 1000u);
		p[1] = cmd->duration_ms;
		p[2] = centi_hz(cmd->rumble.burst_mhz);
		p[3] = inv;
		break;
	case OSC2_HAP_GEN_SWEEP:
		p[0] = cmd->sweep.f0_hz;
		p[1] = cmd->sweep.f1_hz;
		p[2] = cmd->duration_ms;
		p[3] = inv;
		break;
	case OSC2_HAP_GEN_CLICK_A:
		p[0] = OSC2_HAP_CLICK_A_LEN;
		p[1] = 5;                         /* signed 8-bit at 4 kHz */
		break;
	case OSC2_HAP_GEN_CLICK_B:
		p[0] = OSC2_HAP_CLICK_B_LEN;
		p[1] = 5;
		break;
	case OSC2_HAP_GEN_CLIP:
		p[0] = cmd->clip.samples;
		p[1] = cmd->format;
		break;
	default:
		break;
	}
}

bool osc2_hap_queue(struct osc2_hap *h, const struct osc2_hap_cmd *cmd)
{
	struct osc2_hap_seq *s;
	uint16_t p[4];

	if (cmd->channel >= OSC2_HAP_CHANNELS) {
		return false;
	}
	s = &h->seq[osc2_hap_path_of(cmd->channel)];
	if (cmd->op != OSC2_HAP_OP_FORMAT && cmd->type < OSC2_HAP_GEN_COUNT &&
	    (s->refused & BIT8(cmd->type))) {
		/* Turned away before it is queued, as the real sequencer does. */
		osc2_hap_cmd_params(cmd, p);
		note_gen(h, s, cmd->channel, cmd->type, OSC2_HAP_GEN_EV_REFUSED, cmd->gain_db, p);
		return false;
	}
	if (s->q_count >= OSC2_HAP_QUEUE_MAX) {
		/* Queued without waiting: a full queue loses the command, and nobody retries it. */
		s->lost++;
		osc2_hap_cmd_params(cmd, p);
		note_gen(h, s, cmd->channel, cmd->type, OSC2_HAP_GEN_EV_LOST, cmd->gain_db, p);
		return false;
	}
	s->queue[(s->q_head + s->q_count) % OSC2_HAP_QUEUE_MAX] = *cmd;
	s->q_count++;
	return true;
}

bool osc2_hap_pending(const struct osc2_hap *h, uint8_t path)
{
	return h->seq[path].q_count != 0;
}

bool osc2_hap_busy(const struct osc2_hap *h, uint8_t path)
{
	const struct osc2_hap_seq *s = &h->seq[path];

	for (int c = 0; c < 2; c++) {
		for (int t = 0; t < OSC2_HAP_GEN_COUNT; t++) {
			if (s->ch[c].gen[t].on) {
				return true;
			}
		}
	}
	return false;
}

uint16_t osc2_hap_gen_mask(const struct osc2_hap *h, uint8_t channel)
{
	const struct osc2_hap_channel *ch = &h->seq[osc2_hap_path_of(channel)].ch[channel & 1u];
	uint16_t mask = 0;

	for (int t = 0; t < OSC2_HAP_GEN_COUNT; t++) {
		if (ch->gen[t].on) {
			mask |= (uint16_t)(1u << t);
		}
	}
	return mask;
}

/* ---- applying commands ------------------------------------------------------------------------ */

static void empty_stream(struct osc2_hap_stream *st)
{
	st->head = 0;
	st->count = 0;
	st->playing = false;
	st->start_queued = false;
}

static void stop_type(struct osc2_hap *h, struct osc2_hap_seq *s, uint8_t local, uint8_t type)
{
	struct osc2_hap_channel *ch = &s->ch[local];
	uint8_t channel = (uint8_t)(s->path * 2u + local);

	if (ch->gen[type].on) {
		ch->gen[type].on = false;
		note_gen(h, s, channel, type, OSC2_HAP_GEN_EV_STOP, 0, NULL);
	}
	if (type == OSC2_HAP_GEN_STREAM) {
		/* Stopping the stream empties its ring, whether or not it played. (The real controller
		 * also reports flag 0x02 then: the library's stop status replies, not this engine.) */
		empty_stream(&ch->stream);
		note_stream(h, s, channel, OSC2_HAP_STREAM_EV_EMPTIED, 0);
	}
}

static void set_format(struct osc2_hap *h, struct osc2_hap_seq *s, uint8_t local, uint8_t format)
{
	struct osc2_hap_stream *st = &s->ch[local].stream;
	uint8_t channel = (uint8_t)(s->path * 2u + local);
	uint32_t flags;

	if (format < 12u && format % 4u == 0 && s->rate < 8000u) {
		flags = OSC2_HAP_FLAG_NEED_8K;      /* 8 kHz formats only on an 8 kHz channel */
	} else if (st->playing) {
		flags = OSC2_HAP_FLAG_BUSY;
	} else {
		/* Formats 12 and above are reported as set but keep the sample size and rate the
		 * channel had (the real controller's behaviour; avoid them). */
		if (format < 12u) {
			st->format = format;
		}
		st->format_set = true;
		st->low_armed = false;              /* no 0x04 before the first 0x08 */
		flags = OSC2_HAP_FLAG_FORMAT;
	}
	status(s, channel, flags);
	note_stream(h, s, channel, OSC2_HAP_STREAM_EV_FORMAT, flags);
}

static void start(struct osc2_hap *h, struct osc2_hap_seq *s, uint8_t local,
		  const struct osc2_hap_cmd *cmd)
{
	struct osc2_hap_channel *ch = &s->ch[local];
	struct osc2_hap_gen *g = &ch->gen[cmd->type];
	uint8_t channel = (uint8_t)(s->path * 2u + local);
	bool was_on = g->on;
	uint16_t p[4];

	osc2_hap_cmd_params(cmd, p);
	switch (cmd->type) {
	case OSC2_HAP_GEN_CLICK_A:
	case OSC2_HAP_GEN_CLICK_B:
	case OSC2_HAP_GEN_CLIP: {
		uint8_t format = cmd->type == OSC2_HAP_GEN_CLIP ? cmd->format : 5u;
		const uint8_t *data = cmd->type == OSC2_HAP_GEN_CLICK_A ?
				      (const uint8_t *)osc2_hap_click_a :
				      cmd->type == OSC2_HAP_GEN_CLICK_B ?
				      (const uint8_t *)osc2_hap_click_b : cmd->clip.data;
		uint16_t samples = cmd->type == OSC2_HAP_GEN_CLICK_A ? OSC2_HAP_CLICK_A_LEN :
				   cmd->type == OSC2_HAP_GEN_CLICK_B ? OSC2_HAP_CLICK_B_LEN :
				   cmd->clip.samples;

		if (format >= 12u || !data || samples == 0) {
			/* Nothing to play: a clip already playing goes on, as for the other types. */
			note_gen(h, s, channel, cmd->type, OSC2_HAP_GEN_EV_IGNORED, cmd->gain_db, p);
			return;
		}
		memset(g, 0, sizeof(*g));
		g->clip.data = data;
		g->clip.samples = samples;
		g->clip.enc = fmt_enc(format);
		g->clip.ratio = ratio_of(s->rate, fmt_rate(format));
		g->gain_q16 = osc2_hap_gain_q16_db(cmd->gain_db);
		break;
	}
	case OSC2_HAP_GEN_PULSE: {
		uint32_t sample_us = 1000000u / s->rate;          /* 250 on a pad, 125 on a grip */
		uint32_t period = ((uint32_t)cmd->pulse.on_us + cmd->pulse.off_us) / sample_us;

		if (period == 0) {
			/* Shorter than a sample: nothing (the real arithmetic plays a constant -1 for
			 * 65,536 samples; no host can tell). */
			note_gen(h, s, channel, cmd->type, OSC2_HAP_GEN_EV_IGNORED, 0, p);
			return;
		}
		memset(g, 0, sizeof(*g));
		g->pulse.period = period;
		g->pulse.on = cmd->pulse.on_us / sample_us;
		g->pulse.periods_left = cmd->pulse.count ? cmd->pulse.count : 1u;
		g->gain_q16 = OSC2_HAP_ONE_Q16;                    /* full scale: no gain of its own */
		break;
	}
	case OSC2_HAP_GEN_TONE:
		if (!was_on) {
			memset(g, 0, sizeof(*g));
			g->tone.phase = 0;
			g->tone.lfo_phase = LFO_START_PHASE;
		}
		/* A tone started over a running one keeps its phases: notes join without a click. */
		g->tone.step = osc2_hap_phase_step(cmd->tone.freq_mhz, s->rate);
		g->tone.lfo_step = osc2_hap_phase_step(cmd->tone.lfo_mhz, s->rate);
		g->tone.depth_q15 = cmd->tone.lfo_mhz ?
				    (int32_t)((cmd->tone.depth * OSC2_HAP_ONE_Q15 + 100u) / 200u) : 0;
		g->endless = cmd->duration_ms == 0;
		g->left = (uint32_t)cmd->duration_ms * s->rate / 1000u;
		g->gain_q16 = osc2_hap_gain_q16_db(cmd->gain_db);
		break;
	case OSC2_HAP_GEN_RUMBLE: {
		uint32_t burst = cmd->rumble.burst_mhz < RUMBLE_MAX_BURST_MHZ ?
				 cmd->rumble.burst_mhz : RUMBLE_MAX_BURST_MHZ;

		if (burst == 0) {
			note_gen(h, s, channel, cmd->type, OSC2_HAP_GEN_EV_IGNORED, cmd->gain_db, p);
			return;
		}
		if (!was_on) {
			memset(g, 0, sizeof(*g));       /* a fresh start: the first burst at 0 dB */
		}
		/* Over a running rumble only the gain, frequency, rate and duration change: the
		 * bursts go on where they are. */
		g->rumble.step = osc2_hap_phase_step(cmd->rumble.freq_mhz, s->rate);
		g->rumble.slot = (uint32_t)s->rate * 1000u / burst + 2u;
		g->left = (uint32_t)cmd->duration_ms * s->rate / 1000u + 1u;
		g->gain_q16 = osc2_hap_gain_q16_db(cmd->gain_db);
		break;
	}
	case OSC2_HAP_GEN_SWEEP: {
		uint32_t total = (uint32_t)cmd->duration_ms * s->rate / 1000u;
		int32_t ratio;

		if (cmd->sweep.f0_hz == 0 || cmd->sweep.f1_hz == 0 || total == 0) {
			/* 0 Hz at either end has no exponential sweep (the real arithmetic turns the
			 * channel's mix into invalid numbers); nothing plays. */
			note_gen(h, s, channel, cmd->type, OSC2_HAP_GEN_EV_IGNORED, cmd->gain_db, p);
			return;
		}
		/* Equal frequencies play as a plain tone with the sweep's fades. */
		ratio = cmd->sweep.f0_hz == cmd->sweep.f1_hz ? 0 :
			osc2_hap_log2(cmd->sweep.f1_hz) - osc2_hap_log2(cmd->sweep.f0_hz);
		memset(g, 0, sizeof(*g));               /* every sweep starts afresh */
		g->sweep.step0 = osc2_hap_phase_step((uint32_t)cmd->sweep.f0_hz * 1000u, s->rate);
		/* log2(f1 / f0) per sample, Q32: e(n) = slope * n / 2^16 never overflows (n <= total). */
		g->sweep.slope = ((int64_t)ratio * 65536) / (int64_t)total;
		g->sweep.total = total;
		g->sweep.fade_in = SWEEP_FADE_IN_MS * s->rate / 1000u;
		g->sweep.fade_out = SWEEP_FADE_OUT_MS * s->rate / 1000u;
		g->gain_q16 = osc2_hap_gain_q16_db(cmd->gain_db);
		break;
	}
	case OSC2_HAP_GEN_STREAM: {
		struct osc2_hap_stream *st = &ch->stream;

		st->start_queued = false;
		if (!st->format_set) {
			note_gen(h, s, channel, cmd->type, OSC2_HAP_GEN_EV_IGNORED, 0, p);
			return;
		}
		memset(g, 0, sizeof(*g));
		g->stream.enc = fmt_enc(st->format);
		g->stream.ratio = ratio_of(s->rate, fmt_rate(st->format));
		g->gain_q16 = OSC2_HAP_ONE_Q16;
		st->playing = true;
		note_stream(h, s, channel, OSC2_HAP_STREAM_EV_START, 0);
		break;
	}
	default:
		return;
	}
	g->invert = (cmd->flags & OSC2_HAP_CMD_INVERT) != 0;
	g->on = true;
	note_gen(h, s, channel, cmd->type, OSC2_HAP_GEN_EV_START, cmd->gain_db, p);
}

static void apply_one(struct osc2_hap *h, struct osc2_hap_seq *s, const struct osc2_hap_cmd *cmd)
{
	uint8_t local = cmd->channel & 1u;

	switch (cmd->op) {
	case OSC2_HAP_OP_START:
		if (cmd->type < OSC2_HAP_GEN_COUNT) {
			start(h, s, local, cmd);
		}
		break;
	case OSC2_HAP_OP_STOP:
		if (cmd->type == OSC2_HAP_GEN_ALL) {
			for (uint8_t t = 0; t < OSC2_HAP_GEN_COUNT; t++) {
				if (!(s->refused & BIT8(t))) {
					stop_type(h, s, local, t);
				}
			}
		} else if (cmd->type < OSC2_HAP_GEN_COUNT) {
			stop_type(h, s, local, cmd->type);
		}
		break;
	case OSC2_HAP_OP_FORMAT:
		set_format(h, s, local, cmd->format);
		break;
	default:
		break;
	}
}

void osc2_hap_apply(struct osc2_hap *h, uint8_t path)
{
	struct osc2_hap_seq *s = &h->seq[path];

	while (s->q_count) {
		struct osc2_hap_cmd cmd = s->queue[s->q_head];

		s->q_head = (uint8_t)((s->q_head + 1u) % OSC2_HAP_QUEUE_MAX);
		s->q_count--;
		apply_one(h, s, &cmd);
	}
}

/* ---- the generators, one sample each ---------------------------------------------------------- */

static void end_gen(const struct osc2_hap *h, struct osc2_hap_seq *s, uint8_t local, uint8_t type)
{
	s->ch[local].gen[type].on = false;
	note_gen(h, s, (uint8_t)(s->path * 2u + local), type, OSC2_HAP_GEN_EV_END, 0, NULL);
}

static int32_t tone_sample(struct osc2_hap_gen *g)
{
	int32_t v = osc2_hap_sin(g->tone.phase);

	g->tone.phase += g->tone.step;
	if (g->tone.depth_q15) {
		/* (1 - d) + d * sin(LFO): 100 % swings the amplitude between 0 and 1. */
		int32_t m = OSC2_HAP_ONE_Q15 - g->tone.depth_q15 +
			    osc2_hap_mul_q15(g->tone.depth_q15, osc2_hap_sin(g->tone.lfo_phase));

		v = osc2_hap_mul_q15(v, m);
		g->tone.lfo_phase += g->tone.lfo_step;
	}
	return v;
}

static uint32_t lcg_next(struct osc2_hap *h)
{
	h->lcg = h->lcg * 8253729u + 2396403u;
	return h->lcg % 32767u;
}

static int32_t rumble_sample(struct osc2_hap *h, struct osc2_hap_gen *g)
{
	int32_t v = 0;

	if (g->rumble.pos >= g->rumble.slot) {
		g->rumble.pos = 0;
	}
	if (g->rumble.pos == 0) {
		/* A new burst: the sine from phase 0, the first after a fresh start at 0 dB, every
		 * later one a random -4 to +3 dB. */
		g->rumble.phase = 0;
		if (g->rumble.started) {
			g->rumble.burst_q16 = osc2_hap_gain_q16_db(3 - (int32_t)(lcg_next(h) % 8u));
		} else {
			g->rumble.started = true;
			g->rumble.burst_q16 = OSC2_HAP_ONE_Q16;
			/* The first burst of a fresh start counts one sample more: the second comes
			 * P + 1 samples after it, every later one P + 2 (the real controller's) */
			g->rumble.pos++;
		}
	}
	if (g->rumble.phase < TWO_CYCLES) {
		/* The phase moves before the sample is taken: a burst's first sample is not 0. */
		g->rumble.phase += g->rumble.step;
		if (g->rumble.phase < TWO_CYCLES) {
			v = osc2_hap_mul_q16(osc2_hap_sin((uint32_t)g->rumble.phase),
					     (int32_t)g->rumble.burst_q16);
		}
	}
	g->rumble.pos++;
	return v;
}

static int32_t sweep_sample(struct osc2_hap_gen *g)
{
	uint32_t n = g->sweep.n;
	int64_t env = OSC2_HAP_ONE_Q15;
	int32_t e = (int32_t)((g->sweep.slope * (int64_t)n) >> 16);
	int32_t v = osc2_hap_sin(g->sweep.phase);

	/* A linear fade-in over 80 ms and fade-out over the last 5 ms, multiplied where they meet. */
	if (n < g->sweep.fade_in) {
		env = env * n / g->sweep.fade_in;
	}
	if (g->sweep.total - n <= g->sweep.fade_out && g->sweep.fade_out) {
		env = env * (g->sweep.total - n) / g->sweep.fade_out;
	}
	v = (int32_t)((v * env) >> 15);
	/* The frequency at sample n is f0 * (f1 / f0)^(n / N). */
	g->sweep.phase += (uint32_t)osc2_hap_scale_exp2(g->sweep.step0, e);
	g->sweep.n = n + 1u;
	return v;
}

static int32_t pulse_sample(struct osc2_hap_gen *g, bool *ended)
{
	int32_t v = g->pulse.pos < g->pulse.on ? OSC2_HAP_FULL_Q15 : -OSC2_HAP_FULL_Q15;

	if (++g->pulse.pos >= g->pulse.period) {
		g->pulse.pos = 0;
		if (--g->pulse.periods_left == 0) {
			*ended = true;
		}
	}
	return v;
}

static int32_t clip_sample(struct osc2_hap_gen *g, bool *ended)
{
	uint8_t bytes = osc2_hap_enc_bytes(g->clip.enc);
	int32_t v;

	if (g->clip.step == 0) {
		const uint8_t *p = g->clip.data + (size_t)g->clip.next * bytes;

		g->clip.a = osc2_hap_pcm_sample(p, g->clip.enc);
		g->clip.next++;
		/* The last sample holds its value through its interpolated steps. */
		g->clip.b = g->clip.next < g->clip.samples ?
			    osc2_hap_pcm_sample(p + bytes, g->clip.enc) : g->clip.a;
	}
	v = g->clip.a + (g->clip.b - g->clip.a) * g->clip.step / g->clip.ratio;
	if (++g->clip.step >= g->clip.ratio) {
		g->clip.step = 0;
		if (g->clip.next >= g->clip.samples) {
			*ended = true;
		}
	}
	return v;
}

static int32_t ring_sample(const struct osc2_hap_stream *st, uint16_t offset, uint8_t enc)
{
	uint8_t b[2];

	b[0] = st->buf[(st->head + offset) % OSC2_HAP_RING_SIZE];
	b[1] = st->buf[(st->head + offset + 1u) % OSC2_HAP_RING_SIZE];
	return osc2_hap_pcm_sample(b, enc);
}

static int32_t stream_sample(struct osc2_hap_seq *s, uint8_t local, struct osc2_hap_gen *g,
			     bool *dry)
{
	struct osc2_hap_stream *st = &s->ch[local].stream;
	uint8_t channel = (uint8_t)(s->path * 2u + local);
	int32_t v;

	if (g->stream.step == 0) {
		uint8_t bytes = osc2_hap_enc_bytes(g->stream.enc);
		uint16_t need = (uint16_t)(bytes * (g->stream.ratio > 1u ? 2u : 1u));

		if (st->count < need) {
			/* Less than a whole sample, or the next one to interpolate towards has not
			 * arrived: the stream stops (nothing is taken, so a sample it only looked at
			 * plays first next time). The only combined flags: 0x06. */
			uint32_t flags = OSC2_HAP_FLAG_STOPPED |
					 (st->low_armed ? OSC2_HAP_FLAG_LOW : 0u);

			st->low_armed = false;
			st->playing = false;
			status(s, channel, flags);
			*dry = true;
			return 0;
		}
		g->stream.a = ring_sample(st, 0, g->stream.enc);
		st->head = (uint16_t)((st->head + bytes) % OSC2_HAP_RING_SIZE);
		st->count = (uint16_t)(st->count - bytes);
		if (g->stream.ratio > 1u) {
			g->stream.b = ring_sample(st, 0, g->stream.enc);   /* looked at, not taken */
		}
		if (st->low_armed && st->count < osc2_hap_water_mark(st->format)) {
			st->low_armed = false;
			status(s, channel, OSC2_HAP_FLAG_LOW);
		}
	}
	v = g->stream.ratio > 1u ?
	    g->stream.a + (g->stream.b - g->stream.a) * g->stream.step / g->stream.ratio :
	    g->stream.a;
	g->stream.step = (uint8_t)((g->stream.step + 1u) % g->stream.ratio);
	return v;
}

/* One sample of every generator of a channel, each with its gain, summed. */
static int64_t channel_sample(struct osc2_hap *h, struct osc2_hap_seq *s, uint8_t local)
{
	struct osc2_hap_channel *ch = &s->ch[local];
	int64_t sum = 0;

	for (uint8_t t = 0; t < OSC2_HAP_GEN_COUNT; t++) {
		struct osc2_hap_gen *g = &ch->gen[t];
		bool ended = false;
		int32_t v;

		if (!g->on) {
			continue;
		}
		switch (t) {
		case OSC2_HAP_GEN_CLICK_A:
		case OSC2_HAP_GEN_CLICK_B:
		case OSC2_HAP_GEN_CLIP:
			v = clip_sample(g, &ended);
			break;
		case OSC2_HAP_GEN_PULSE:
			v = pulse_sample(g, &ended);
			break;
		case OSC2_HAP_GEN_TONE:
			v = tone_sample(g);
			ended = !g->endless && --g->left == 0;
			break;
		case OSC2_HAP_GEN_RUMBLE:
			v = rumble_sample(h, g);
			ended = --g->left == 0;
			break;
		case OSC2_HAP_GEN_SWEEP:
			v = sweep_sample(g);
			ended = g->sweep.n >= g->sweep.total;
			break;
		case OSC2_HAP_GEN_STREAM: {
			bool dry = false;

			v = stream_sample(s, local, g, &dry);
			if (dry) {
				g->on = false;
				if (h->notes) {
					note_stream(h, s, (uint8_t)(s->path * 2u + local),
						    OSC2_HAP_STREAM_EV_DRY, OSC2_HAP_FLAG_STOPPED);
				}
				continue;
			}
			break;
		}
		default:
			v = 0;
			break;
		}
		if (g->invert) {
			v = -v;
		}
		sum += osc2_hap_mul_q16(v, (int32_t)g->gain_q16);
		if (ended) {
			end_gen(h, s, local, t);
		}
	}
	return sum;
}

size_t osc2_hap_render(struct osc2_hap *h, uint8_t path, bool first, void *out)
{
	struct osc2_hap_seq *s = &h->seq[path];
	int32_t master = (int32_t)osc2_hap_gain_q16_db(h->master_db);
	int32_t gain[2];
	uint8_t from = first ? s->lead_in : 0u;

	/* The channel gain: the master gain, negated for the left pad and the right grip. */
	gain[0] = s->negated == 0 ? -master : master;
	gain[1] = s->negated == 1 ? -master : master;
	for (uint8_t f = 0; f < s->frames; f++) {
		for (uint8_t c = 0; c < 2; c++) {
			int32_t v = 0;

			if (f >= from) {
				v = osc2_hap_clip((channel_sample(h, s, c) * gain[c]) >> 16);
			}
			if (path == OSC2_HAP_PADS) {
				((uint8_t *)out)[f * 2u + c] = osc2_hap_to_u8(v);
			} else {
				/* little endian, whatever the CPU */
				((uint8_t *)out)[f * 4u + c * 2u] = (uint8_t)((uint16_t)(int16_t)v);
				((uint8_t *)out)[f * 4u + c * 2u + 1u] =
					(uint8_t)((uint16_t)(int16_t)v >> 8);
			}
		}
	}
	s->blocks++;
	return path == OSC2_HAP_PADS ? (size_t)s->frames * 2u : (size_t)s->frames * 4u;
}

/* ---- streams ---------------------------------------------------------------------------------- */

uint32_t osc2_hap_stream_write(struct osc2_hap *h, uint8_t channel, const uint8_t *data,
			       size_t len)
{
	struct osc2_hap_seq *s;
	struct osc2_hap_stream *st;
	uint16_t before, space;
	size_t take;
	uint32_t flags = 0;

	if (channel >= OSC2_HAP_CHANNELS) {
		return 0;
	}
	s = &h->seq[osc2_hap_path_of(channel)];
	st = &s->ch[channel & 1u].stream;
	before = st->count;
	space = (uint16_t)(OSC2_HAP_RING_SIZE - st->count);
	take = len < space ? len : space;
	for (size_t i = 0; i < take; i++) {
		st->buf[(st->head + st->count + i) % OSC2_HAP_RING_SIZE] = data[i];
	}
	st->count = (uint16_t)(st->count + take);
	if (take < len) {
		/* Keeps the bytes that fit and drops the rest; never together with 0x08. */
		flags = OSC2_HAP_FLAG_DROPPED;
	} else if (OSC2_HAP_RING_SIZE - st->count < osc2_hap_water_mark(st->format)) {
		flags = OSC2_HAP_FLAG_HIGH;
		st->low_armed = true;
	}
	if (st->format_set && !st->playing && !st->start_queued &&
	    before > osc2_hap_start_threshold(s->path, st->format)) {
		struct osc2_hap_cmd cmd = {
			.op = OSC2_HAP_OP_START, .channel = channel, .type = OSC2_HAP_GEN_STREAM,
		};

		if (osc2_hap_queue(h, &cmd)) {
			st->start_queued = true;
			note_stream(h, s, channel, OSC2_HAP_STREAM_EV_QUEUED, 0);
		}
	}
	return flags;
}

uint16_t osc2_hap_stream_fill(const struct osc2_hap *h, uint8_t channel)
{
	return h->seq[osc2_hap_path_of(channel)].ch[channel & 1u].stream.count;
}

uint8_t osc2_hap_stream_format(const struct osc2_hap *h, uint8_t channel)
{
	const struct osc2_hap_stream *st = &h->seq[osc2_hap_path_of(channel)].ch[channel & 1u].stream;

	return st->format_set ? st->format : 0xFFu;
}

/* ---- blocks ----------------------------------------------------------------------------------- */

uint16_t osc2_hap_block_peak(uint8_t path, const void *block)
{
	const uint8_t *b = block;
	uint32_t peak = 0;

	if (path == OSC2_HAP_PADS) {
		for (int i = 0; i < OSC2_HAP_PAD_FRAMES * 2; i++) {
			uint32_t d = b[i] >= 128u ? b[i] - 128u : 128u - b[i];

			peak = d > peak ? d : peak;
		}
		peak = peak * OSC2_HAP_FULL_Q15 / 127u;
		return (uint16_t)(peak > OSC2_HAP_FULL_Q15 ? OSC2_HAP_FULL_Q15 : peak);
	}
	for (int i = 0; i < OSC2_HAP_GRIP_FRAMES * 2; i++) {
		int32_t v = (int16_t)(uint16_t)(b[i * 2] | (b[i * 2 + 1] << 8));
		uint32_t d = (uint32_t)(v < 0 ? -v : v);

		peak = d > peak ? d : peak;
	}
	return (uint16_t)peak;
}
