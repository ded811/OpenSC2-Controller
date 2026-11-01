/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * haptics_core.c: the haptics synthesiser's engine (haptics_core.h). Pure C: integers only, no
 * kernel, no allocation.
 *
 * The generators follow the protocol documentation's description of the real controller's,
 * including what its arithmetic does at the edges a host can reach: a sweep whose formula divides
 * by zero plays a full-scale push, a pulse period shorter than a sample plays a constant -1 for
 * 65,536 samples, a tone above the channel's rate turns into clipped noise, 16-bit counts wrap,
 * and a stream that runs dry with half a sample left loses that byte. docs/protocol.md lists what
 * still differs and why.
 */
#include <string.h>
#include "haptics_core.h"

#define BIT8(n) ((uint8_t)(1u << (n)))
#define LFO_START_PHASE (-OSC2_HAP_CYCLE / 8)  /* -pi/4: at full depth the modulation starts low */
#define STATUS_RESERVE 8                       /* event slots kept for 0x44 statuses */
#define LN2_Q30 744261118LL                    /* ln 2, Q30 */

enum sweep_mode {
	SWEEP_NORMAL = 0,      /* f0 to f1, exponentially */
	SWEEP_NAN = 1,         /* every sample not a number: f0 = 0, f0 = f1, a wrapped rising one */
	SWEEP_FIRST_NAN = 2,   /* the first sample not a number, then silence: f1 = 0 */
	SWEEP_CONSTANT = 3,    /* the first not a number, then a constant: a wrapped falling one */
};

uint8_t osc2_hap_status_id(uint8_t channel)
{
	static const uint8_t ids[OSC2_HAP_CHANNELS] = { 3, 4, 0, 1 };

	return ids[channel & 3u];
}

/* A rate in mHz as the 0.01 Hz of a ring entry's u16 (0xFFFF from 655.35 Hz up). */
static uint16_t centi_hz(uint32_t mhz)
{
	return mhz / 10u > 0xFFFFu ? 0xFFFFu : (uint16_t)(mhz / 10u);
}

/* ---- events ----------------------------------------------------------------------------------- */

static void push_event(struct osc2_hap_seq *s, const struct osc2_hap_event *e)
{
	uint8_t room = e->kind == OSC2_HAP_EV_STATUS || e->kind == OSC2_HAP_EV_AMP ?
		       OSC2_HAP_EVENTS_MAX : OSC2_HAP_EVENTS_MAX - STATUS_RESERVE;

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

/* A stop's 0x02 (`what` 1: the reply to a stop, as the test hooks note it). */
static void stop_reply(struct osc2_hap_seq *s, uint8_t channel)
{
	struct osc2_hap_event e = {
		.kind = OSC2_HAP_EV_STATUS, .channel = channel, .what = 1,
		.flags = OSC2_HAP_FLAG_STOPPED,
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
	int32_t gain[2] = { s->ch[0].gain_q16, s->ch[1].gain_q16 };
	bool held = s->amp_held;

	memset(s, 0, sizeof(*s));
	s->blocks = blocks;
	s->lost = lost;
	s->ev_lost = ev_lost;
	s->ch[0].gain_q16 = gain[0];
	s->ch[1].gain_q16 = gain[1];
	s->amp_held = held;
	s->path = path;
	if (path == OSC2_HAP_PADS) {
		s->rate = OSC2_HAP_PAD_RATE;
		s->frames = OSC2_HAP_PAD_FRAMES;
		s->lead_in = 16;
		s->release = 1;
		s->refused = BIT8(OSC2_HAP_GEN_RUMBLE);
		s->negated = 0;                    /* the left pad */
	} else {
		s->rate = OSC2_HAP_GRIP_RATE;
		s->frames = OSC2_HAP_GRIP_FRAMES;
		s->lead_in = 32;
		s->release = 4;
		s->refused = BIT8(OSC2_HAP_GEN_CLICK_A) | BIT8(OSC2_HAP_GEN_CLICK_B);
		s->negated = 1;                    /* the right grip */
	}
}

void osc2_hap_set_click(struct osc2_hap *h, uint8_t click, const struct opensc2_haptics_sound *sound)
{
	static const struct opensc2_haptics_sound own[2] = {
		{ (const uint8_t *)osc2_hap_click_a, OSC2_HAP_CLICK_A_LEN, 5 },
		{ (const uint8_t *)osc2_hap_click_b, OSC2_HAP_CLICK_B_LEN, 5 },
	};

	click &= 1u;
	h->click[click] = sound ? *sound : own[click];
}

void osc2_hap_init(struct osc2_hap *h)
{
	memset(h, 0, sizeof(*h));
	seq_setup(&h->seq[OSC2_HAP_PADS], OSC2_HAP_PADS);
	seq_setup(&h->seq[OSC2_HAP_GRIPS], OSC2_HAP_GRIPS);
	h->lcg = OSC2_HAP_LCG_SEED;
	osc2_hap_set_click(h, 0, NULL);
	osc2_hap_set_click(h, 1, NULL);
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

static int32_t channel_gain(const struct osc2_hap_seq *s, uint8_t local, int8_t db)
{
	int32_t g = (int32_t)osc2_hap_gain_q16_db(db);

	return local == s->negated ? -g : g;
}

void osc2_hap_set_gains(struct osc2_hap *h, int8_t db)
{
	for (uint8_t p = 0; p < OSC2_HAP_PATHS; p++) {
		for (uint8_t c = 0; c < 2; c++) {
			h->seq[p].ch[c].gain_q16 = channel_gain(&h->seq[p], c, db);
		}
	}
}

/* ---- formats ---------------------------------------------------------------------------------- */

/* The real controller's divisor of a format byte: 1 for the 8 kHz formats, 2, 4 and 8 for the
 * lower rates, and 8 for every byte from 12 up. */
static uint8_t fmt_div(uint8_t format)
{
	switch (format) {
	case 0: case 4: case 8: return 1;
	case 1: case 5: case 9: return 2;
	case 2: case 6: case 10: return 4;
	default: return 8;
	}
}

static uint8_t fmt_bytes(uint8_t format)
{
	return format < 4u ? 2u : 1u;
}

uint16_t osc2_hap_water_mark(uint8_t format)
{
	/* 128 samples at 8 kHz (a quarter of the ring; one block and a frame were it larger), in the
	 * format's bytes and rate: 16 ms of audio. */
	return (uint16_t)(128u * fmt_bytes(format) / fmt_div(format));
}

uint16_t osc2_hap_start_threshold(uint8_t path, uint8_t format)
{
	/* Two blocks' worth of the format's bytes: 16 ms of audio on a grip, 8 ms on a pad. */
	uint16_t frames = path == OSC2_HAP_PADS ? OSC2_HAP_PAD_FRAMES : OSC2_HAP_GRIP_FRAMES;

	return (uint16_t)(frames * fmt_bytes(format) * 2u / fmt_div(format));
}

/* Output samples per sample of a format's rate on a channel of `rate`, as the real controller's
 * integer division gives it (an 8 kHz recording on a 4 kHz pad: 0, one sample each). Bytes 12 and
 * up: none (the caller keeps what it had). */
static bool fmt_step(uint8_t format, uint16_t rate, uint8_t *step)
{
	if (format >= 12u) {
		return false;
	}
	*step = (uint8_t)(rate / (8000u >> (format % 4u)));
	return true;
}

/* ---- the queue -------------------------------------------------------------------------------- */

void osc2_hap_cmd_params(const struct osc2_hap_cmd *cmd, uint16_t p[4])
{
	uint16_t inv = (cmd->flags & OSC2_HAP_CMD_INVERT) ? 0x8000u : 0u;

	memset(p, 0, 4 * sizeof(p[0]));
	if (cmd->op == OSC2_HAP_OP_FORMAT || cmd->op == OSC2_HAP_OP_AMP) {
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
	case OSC2_HAP_GEN_CLICK_B:
		/* the click's own length and format are those of the sound in use (start()) */
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
	if ((cmd->op == OSC2_HAP_OP_START || cmd->op == OSC2_HAP_OP_STOP) &&
	    cmd->type < OSC2_HAP_GEN_COUNT && (s->refused & BIT8(cmd->type))) {
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

	return s->ch[0].mix != 0 || s->ch[1].mix != 0;
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

/* ---- the interpolation values ----------------------------------------------------------------- */

static void fifo_put(struct osc2_hap_fifo *f, int32_t v)
{
	if (f->count < OSC2_HAP_FIFO_MAX) {
		f->v[(f->head + f->count) % OSC2_HAP_FIFO_MAX] = v;
		f->count++;
	}
}

static int32_t fifo_get(struct osc2_hap_fifo *f)
{
	int32_t v;

	if (f->count == 0) {
		return 0;
	}
	v = f->v[f->head];
	f->head = (uint8_t)((f->head + 1u) % OSC2_HAP_FIFO_MAX);
	f->count--;
	return v;
}

/* The n - 1 evenly spaced steps from a towards b (a itself is already queued). */
static void fifo_steps(struct osc2_hap_fifo *f, uint8_t n, int32_t a, int32_t b)
{
	for (uint8_t i = 1; i < n; i++) {
		fifo_put(f, a + (int32_t)(((int64_t)(b - a) * i) / n));
	}
}

/* ---- applying commands ------------------------------------------------------------------------ */

static void empty_ring(struct osc2_hap_stream *st)
{
	st->head = 0;
	st->count = 0;
}

static void stop_type(struct osc2_hap *h, struct osc2_hap_seq *s, uint8_t local, uint8_t type)
{
	struct osc2_hap_channel *ch = &s->ch[local];
	uint8_t channel = (uint8_t)(s->path * 2u + local);

	if (type == OSC2_HAP_GEN_STREAM) {
		/* Stopping the stream empties its ring and reports 0x02, whether or not it played
		 * (Steam's two all-zero 0x81 reports get their 0x44 replies this way). */
		empty_ring(&ch->stream);
		stop_reply(s, channel);
		note_stream(h, s, channel, OSC2_HAP_STREAM_EV_EMPTIED, 0);
	}
	if (ch->gen[type].on) {
		note_gen(h, s, channel, type, OSC2_HAP_GEN_EV_STOP, 0, NULL);
	}
	/* A stopped generator leaves the mix at once. */
	ch->gen[type].on = false;
	ch->mix &= (uint8_t)~BIT8(type);
}

static void set_format(struct osc2_hap *h, struct osc2_hap_seq *s, uint8_t local, uint8_t format)
{
	struct osc2_hap_channel *ch = &s->ch[local];
	struct osc2_hap_stream *st = &ch->stream;
	uint8_t channel = (uint8_t)(s->path * 2u + local);
	uint32_t flags;

	if ((format == 0 || format == 4 || format == 8) && s->rate != 8000u) {
		flags = OSC2_HAP_FLAG_NEED_8K;      /* 8 kHz formats only on an 8 kHz channel */
	} else if (ch->gen[OSC2_HAP_GEN_STREAM].on) {
		flags = OSC2_HAP_FLAG_BUSY;
	} else {
		/* Any byte is taken, 12 and above too: the water mark and the start point follow
		 * from it, the sample size and rate only when the stream next starts (and for 12 and
		 * up not at all). The ring is not emptied; no 0x04 before the next 0x08. */
		st->format = format;
		st->format_set = true;
		st->water = osc2_hap_water_mark(format);
		st->start_space = (uint16_t)(OSC2_HAP_RING_SIZE -
					     osc2_hap_start_threshold(s->path, format));
		st->need_data = true;
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
	uint32_t per_ms = s->rate / 1000u;
	uint16_t p[4];

	osc2_hap_cmd_params(cmd, p);
	switch (cmd->type) {
	case OSC2_HAP_GEN_CLICK_A:
	case OSC2_HAP_GEN_CLICK_B:
		/* The click's sound, one sample per output sample (a click never interpolates). */
		g->gain_q16 = (int32_t)osc2_hap_gain_q16_db(cmd->gain_db);
		g->clip.sound = h->click[cmd->type == OSC2_HAP_GEN_CLICK_B];
		g->clip.pos = 0;
		p[0] = g->clip.sound.samples;
		p[1] = g->clip.sound.format;
		break;
	case OSC2_HAP_GEN_CLIP:
		g->gain_q16 = (int32_t)osc2_hap_gain_q16_db(cmd->gain_db);
		g->clip.sound = (struct opensc2_haptics_sound){ cmd->clip.data, cmd->clip.samples,
							 cmd->format };
		g->clip.pos = 0;
		(void)fmt_step(cmd->format, s->rate, &g->clip.step);   /* 12 and up: as it was */
		break;
	case OSC2_HAP_GEN_PULSE: {
		/* Whole samples, truncated, in 16 bits: a period under one sample is 0 samples long,
		 * which only the 16-bit position's wrap ends (65,536 samples of -1). */
		uint32_t sample_us = 1000000u / s->rate;          /* 250 on a pad, 125 on a grip */

		g->gain_q16 = OSC2_HAP_ONE_Q16;                    /* full scale: no gain of its own */
		g->pulse.repeats = cmd->pulse.count ? (uint16_t)(cmd->pulse.count - 1u) : 0u;
		g->pulse.period = (uint16_t)(((uint32_t)cmd->pulse.on_us + cmd->pulse.off_us) /
					     sample_us);
		g->pulse.on = (uint16_t)(cmd->pulse.on_us / sample_us);
		g->pulse.pos = 0;
		break;
	}
	case OSC2_HAP_GEN_TONE:
		g->gain_q16 = (int32_t)osc2_hap_gain_q16_db(cmd->gain_db);
		if (!g->on) {
			/* A tone over a running one keeps its phases: notes join without a click. */
			g->tone.phase = 0;
			g->tone.lfo_phase = LFO_START_PHASE;
		}
		g->tone.step = osc2_hap_phase_step((cmd->flags & OSC2_HAP_CMD_INVERT) ?
						   -(int64_t)cmd->tone.freq_mhz :
						   (int64_t)cmd->tone.freq_mhz, s->rate);
		g->tone.lfo = cmd->tone.lfo_mhz != 0;
		g->tone.lfo_step = g->tone.lfo ? osc2_hap_phase_step(cmd->tone.lfo_mhz, s->rate) : 0;
		g->tone.depth_q15 = g->tone.lfo ?
				    (int32_t)((cmd->tone.depth * (uint32_t)OSC2_HAP_ONE_Q15 + 100u) /
					      200u) : 0;
		g->tone.len = cmd->duration_ms * per_ms;            /* 0: 2^32 samples */
		g->tone.pos = 0;
		break;
	case OSC2_HAP_GEN_RUMBLE:
		g->gain_q16 = (int32_t)osc2_hap_gain_q16_db(cmd->gain_db);
		g->rumble.step = osc2_hap_phase_step((cmd->flags & OSC2_HAP_CMD_INVERT) ?
						     -(int64_t)cmd->rumble.freq_mhz :
						     (int64_t)cmd->rumble.freq_mhz, s->rate);
		/* The rate's samples per burst, truncated; a burst rate of 0 never brings another */
		g->rumble.period = cmd->rumble.burst_mhz ?
				   (uint32_t)((uint64_t)s->rate * 1000u / cmd->rumble.burst_mhz) :
				   UINT32_MAX;
		if (!g->on) {
			/* A fresh start: the first burst at 0 dB. Over a running rumble the bursts
			 * go on where they are. */
			g->rumble.burst_q16 = OSC2_HAP_ONE_Q16;
			g->rumble.phase = 0;
			g->rumble.count = 0;
			g->rumble.cycles = 0;
		}
		g->rumble.len = (uint16_t)(cmd->duration_ms * per_ms);
		g->rumble.pos = 0;
		break;
	case OSC2_HAP_GEN_SWEEP: {
		uint32_t full = (uint32_t)cmd->duration_ms * s->rate / 1000u;
		uint16_t f0 = cmd->sweep.f0_hz, f1 = cmd->sweep.f1_hz;
		int32_t fo;

		g->gain_q16 = (int32_t)osc2_hap_gain_q16_db(cmd->gain_db);
		if (cmd->flags & OSC2_HAP_CMD_INVERT) {
			g->gain_q16 = -g->gain_q16;
		}
		/* Every sweep starts afresh; its count of samples is 16-bit. */
		g->sweep.pos = 0;
		g->sweep.len = (uint16_t)full;
		g->sweep.fade_in = (uint16_t)(s->rate * 8u / 100u);          /* 80 ms */
		fo = (int32_t)g->sweep.len - (int32_t)(s->rate / 200u);       /* 5 ms before the end */
		g->sweep.fade_out = (uint16_t)(fo < 0 ? 0 : fo);
		g->sweep.phase = 0;
		if (f0 == 0 || f0 == f1 || (g->sweep.len == 0 && f1 > f0)) {
			/* The formula divides by zero all along: not a number, a full-scale push. */
			g->sweep.mode = SWEEP_NAN;
		} else if (f1 == 0 || (g->sweep.len == 0 && cmd->duration_ms == 0)) {
			g->sweep.mode = SWEEP_FIRST_NAN;
		} else if (g->sweep.len == 0) {
			/* A falling sweep whose count wrapped to 0: after the first sample a constant
			 * sin(T 2 pi f0 / ln(f0 / f1)), which the fade-out's arithmetic then grows. */
			int64_t ln_q24 = (((osc2_hap_log2_q30(f0) - osc2_hap_log2_q30(f1)) >> 6) *
					  LN2_Q30) >> 30;
			uint64_t den = 1000u * (uint64_t)(ln_q24 > 0 ? ln_q24 : 1);
			uint64_t num = ((uint64_t)cmd->duration_ms * f0) << 24;
			uint64_t frac = ((num % den) << 24) / den;

			g->sweep.mode = SWEEP_CONSTANT;
			g->sweep.value = (int32_t)osc2_hap_sinp((int64_t)(frac << 8));
		} else {
			/* f(n) = f0 (f1 / f0)^(n / len): the duration's own rate over the 16-bit
			 * count, so a wrapped count sweeps faster and higher. The ratio to 30 bits:
			 * a small error in it moves the end frequency and the whole phase after it. */
			int64_t ratio = (osc2_hap_log2_q30(f1) - osc2_hap_log2_q30(f0)) * 4;
			int64_t half = (int64_t)g->sweep.len / 2;

			g->sweep.mode = SWEEP_NORMAL;
			g->sweep.step0 = (((uint64_t)f0 << 32) / s->rate) * full / g->sweep.len;
			g->sweep.slope = (ratio >= 0 ? ratio + half : ratio - half) /
					 (int64_t)g->sweep.len;
		}
		break;
	}
	case OSC2_HAP_GEN_STREAM: {
		struct osc2_hap_stream *st = &ch->stream;
		uint8_t step;

		/* The sample size and rate of the format byte set last (12 and up: as they were). */
		if (fmt_step(st->format, s->rate, &step)) {
			g->stream.bytes = fmt_bytes(st->format);
			g->stream.step = step;
		}
		note_stream(h, s, channel, OSC2_HAP_STREAM_EV_START, 0);
		break;
	}
	default:
		return;
	}
	g->on = true;
	g->release = s->release;
	ch->mix |= BIT8(cmd->type);
	note_gen(h, s, channel, cmd->type, OSC2_HAP_GEN_EV_START, cmd->gain_db, p);
}

static void amp_mode(struct osc2_hap_seq *s, uint8_t mode)
{
	struct osc2_hap_event e = { .kind = OSC2_HAP_EV_AMP, .channel = s->path * 2u };

	if (mode > 1u) {
		return;                           /* any other value: nothing */
	}
	/* 1 raises the enable at once and holds it; 0 drops it at once, even in the middle of a
	 * sound, and lets playback drive it again. */
	s->amp_held = mode == 1u;
	e.what = mode;
	push_event(s, &e);
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
	case OSC2_HAP_OP_GAIN:
		s->ch[local].gain_q16 = channel_gain(s, local, cmd->gain_db);
		break;
	case OSC2_HAP_OP_AMP:
		amp_mode(s, cmd->format);
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

/* ---- the generators, one sample each (gain included) ------------------------------------------ */

static void end_gen(const struct osc2_hap *h, struct osc2_hap_seq *s, uint8_t local, uint8_t type)
{
	s->ch[local].gen[type].on = false;    /* it stays in the mix for its release samples */
	note_gen(h, s, (uint8_t)(s->path * 2u + local), type, OSC2_HAP_GEN_EV_END, 0, NULL);
}

/* One step of a phase, wrapped by one cycle at most (`both`: downwards too). */
static int64_t advance(int64_t phase, int64_t step, bool both)
{
	int64_t ph = phase + step;

	if (ph > OSC2_HAP_CYCLE) {
		ph -= OSC2_HAP_CYCLE;
	} else if (both && ph < -OSC2_HAP_CYCLE) {
		ph += OSC2_HAP_CYCLE;
	}
	return ph;
}

static int64_t tone_sample(struct osc2_hap_gen *g, bool *ended)
{
	int64_t v = osc2_hap_mul_q16(osc2_hap_sinp(g->tone.phase), g->gain_q16);

	g->tone.phase = advance(g->tone.phase, g->tone.step, true);
	if (g->tone.lfo) {
		/* (1 - d) + d sin(LFO): 100 % swings the amplitude between 0 and 1. */
		int64_t m = OSC2_HAP_ONE_Q15 - g->tone.depth_q15 +
			    osc2_hap_mul_q15(g->tone.depth_q15, osc2_hap_sinp(g->tone.lfo_phase));

		g->tone.lfo_phase = advance(g->tone.lfo_phase, g->tone.lfo_step, false);
		v = osc2_hap_mul_q15(v, m);
	}
	*ended = ++g->tone.pos == g->tone.len;
	return v;
}

static uint32_t lcg_next(struct osc2_hap *h)
{
	h->lcg = h->lcg * 8253729u + 2396403u;
	return h->lcg % 32767u;
}

static int64_t rumble_sample(struct osc2_hap *h, struct osc2_hap_gen *g, bool *ended)
{
	int64_t ph, v = 0;

	if (g->rumble.count++ > g->rumble.period) {
		/* A new burst: the sine from phase 0 at a random -4 to +3 dB. */
		g->rumble.count = 0;
		g->rumble.cycles = 0;
		g->rumble.phase = 0;
		g->rumble.burst_q16 = osc2_hap_gain_q16_db(3 - (int32_t)(lcg_next(h) & 7u));
	}
	/* The phase moves before the sample is taken: a burst's first sample is not 0. */
	ph = g->rumble.phase + g->rumble.step;
	g->rumble.phase = ph;
	if (ph > OSC2_HAP_CYCLE) {
		g->rumble.phase = ph - OSC2_HAP_CYCLE;
		g->rumble.cycles++;
	} else if (ph < -OSC2_HAP_CYCLE) {
		g->rumble.phase = ph + OSC2_HAP_CYCLE;
		g->rumble.cycles++;
	}
	if (g->rumble.cycles < 2) {
		v = osc2_hap_mul_q16(osc2_hap_mul_q16(osc2_hap_sinp(g->rumble.phase), g->gain_q16),
				     (int32_t)g->rumble.burst_q16);
	}
	*ended = g->rumble.pos++ == g->rumble.len;
	return v;
}

static int64_t sweep_sample(struct osc2_hap_gen *g, bool *ended)
{
	uint16_t pos = g->sweep.pos;
	int64_t v;

	switch (g->sweep.mode) {
	case SWEEP_NORMAL:
		v = osc2_hap_sinp(g->sweep.phase);
		/* The real phase is f0 T (exp(k n / len) - 1) / k cycles (k = ln(f1 / f0)); its
		 * growth from sample n to n + 1 is the frequency f0 (f1 / f0)^(n / len) taken half a
		 * sample later (to within (k / len)^2 / 24), the exponent rounded to nearest: summed
		 * step by step it stays with the closed form to the end. */
		g->sweep.phase += (uint32_t)osc2_hap_scale_exp2(
			g->sweep.step0,
			(int32_t)((g->sweep.slope * (2 * (int64_t)pos + 1) + 65536) >> 17));
		break;
	case SWEEP_FIRST_NAN:
		v = pos == 0 ? OSC2_HAP_NAN : 0;
		break;
	case SWEEP_CONSTANT:
		v = pos == 0 ? OSC2_HAP_NAN : g->sweep.value;
		break;
	default:
		v = OSC2_HAP_NAN;
		break;
	}
	if (v != OSC2_HAP_NAN) {
		/* A linear fade-in over 80 ms and fade-out over the last 5 ms, multiplied where they
		 * meet; the fade-out's arithmetic as the real controller's (to 0 at the last sample,
		 * from the start for a sweep under 5 ms, growing for one whose count wrapped to 0). */
		if (pos < g->sweep.fade_in) {
			v = v * pos / g->sweep.fade_in;
		}
		if (pos >= g->sweep.fade_out) {
			int32_t den = (int32_t)g->sweep.len - g->sweep.fade_out - 1;

			v = den == 0 ? OSC2_HAP_NAN :
			    osc2_hap_sat(v * (den - (int32_t)(pos - g->sweep.fade_out)) / den);
		}
	}
	g->sweep.pos = (uint16_t)(pos + 1u);
	*ended = g->sweep.pos == g->sweep.len;
	return osc2_hap_mul_q16(v, g->gain_q16);
}

static int64_t pulse_sample(struct osc2_hap_gen *g, bool *ended)
{
	int64_t v = g->pulse.on <= g->pulse.pos ? -OSC2_HAP_ONE_Q15 : OSC2_HAP_ONE_Q15;

	g->pulse.pos = (uint16_t)(g->pulse.pos + 1u);
	if (g->pulse.pos == g->pulse.period) {
		if (g->pulse.repeats) {
			g->pulse.repeats--;
			g->pulse.pos = 0;
		} else {
			*ended = true;
		}
	}
	return v;
}

/* The clip's sample at `pos` with its gain, as the real controller reads a stored recording:
 * formats 0..3 signed 16-bit, 8..11 mu-law, everything else signed 8-bit. */
static int32_t clip_read(const struct osc2_hap_gen *g, uint16_t pos)
{
	const struct opensc2_haptics_sound *snd = &g->clip.sound;
	uint8_t enc = snd->format < 4u ? OSC2_HAP_ENC_S16LE :
		      (snd->format >= 8u && snd->format < 12u) ? OSC2_HAP_ENC_MULAW : OSC2_HAP_ENC_S8;
	const uint8_t *p = snd->data + (size_t)pos * osc2_hap_enc_bytes(enc);

	return (int32_t)osc2_hap_mul_q16(osc2_hap_pcm_sample(p, enc), g->gain_q16);
}

static int64_t clip_sample(struct osc2_hap_gen *g, struct osc2_hap_fifo *f, bool *ended)
{
	if (f->count == 0) {
		/* Take the next sample; to interpolate, look at the one after it. The last one plays
		 * once, without steps, and ends the clip; past the end nothing plays. */
		if (!g->clip.sound.data || g->clip.pos >= g->clip.sound.samples) {
			*ended = true;
		} else {
			int32_t a = clip_read(g, g->clip.pos++);

			fifo_put(f, a);
			if (g->clip.step > 1u) {
				if (g->clip.pos < g->clip.sound.samples) {
					fifo_steps(f, g->clip.step, a, clip_read(g, g->clip.pos));
				} else {
					*ended = true;
				}
			}
		}
	}
	return fifo_get(f);
}

static int64_t stream_sample(struct osc2_hap *h, struct osc2_hap_seq *s, uint8_t local,
			     struct osc2_hap_gen *g, bool *ended)
{
	struct osc2_hap_channel *ch = &s->ch[local];
	struct osc2_hap_stream *st = &ch->stream;
	struct osc2_hap_fifo *f = &ch->fifo[OSC2_HAP_GEN_STREAM];
	uint8_t channel = (uint8_t)(s->path * 2u + local);
	uint8_t bytes = g->stream.bytes;
	uint32_t flags = 0;
	uint8_t raw[2] = { 0, 0 };
	uint16_t n;
	bool dry = false;

	if (f->count != 0) {
		return fifo_get(f);
	}
	/* Take a sample: what there is of it (a lone byte of a 16-bit sample is taken and lost). */
	n = st->count < bytes ? st->count : bytes;
	for (uint16_t i = 0; i < n; i++) {
		raw[i] = st->buf[st->head];
		st->head = (uint16_t)((st->head + 1u) % OSC2_HAP_RING_SIZE);
	}
	st->count = (uint16_t)(st->count - n);
	if (n != bytes) {
		dry = true;
	} else {
		/* Two bytes are s16le; one is mu-law for the mu-law formats, else s8 (also after a
		 * mu-law format, for a byte of 12 and up). No bytes at all (a stream started with a
		 * byte of 12 and up before any other): a silent value, without end. */
		uint8_t enc = n == 2u ? OSC2_HAP_ENC_S16LE :
			      (uint8_t)(st->format - 8u) <= 3u ? OSC2_HAP_ENC_MULAW : OSC2_HAP_ENC_S8;
		int32_t cur = osc2_hap_pcm_sample(raw, enc);

		if (st->count < st->water && !st->need_data) {
			st->need_data = true;
			flags = OSC2_HAP_FLAG_LOW;
		}
		fifo_put(f, cur);
		if (g->stream.step > 1u) {
			/* The next sample, looked at, not taken: when it has not arrived, the current
			 * one plays once, without its steps. */
			if (st->count < bytes) {
				dry = true;
			} else {
				uint8_t nx[2] = { st->buf[st->head],
						  st->buf[(st->head + 1u) % OSC2_HAP_RING_SIZE] };

				fifo_steps(f, g->stream.step, cur, osc2_hap_pcm_sample(nx, enc));
			}
		}
		if (!dry && flags) {
			status(s, channel, flags);
		}
	}
	if (dry) {
		/* The only combined flags: 0x06. */
		*ended = true;
		status(s, channel, flags | OSC2_HAP_FLAG_STOPPED);
		note_stream(h, s, channel, OSC2_HAP_STREAM_EV_DRY, flags | OSC2_HAP_FLAG_STOPPED);
	}
	return fifo_get(f);
}

/* One sample of a channel: every generator in its mix with its gain, summed; a generator that
 * ended by itself counts down its release samples first. */
static int64_t channel_sample(struct osc2_hap *h, struct osc2_hap_seq *s, uint8_t local)
{
	struct osc2_hap_channel *ch = &s->ch[local];
	int64_t sum = 0;
	bool nan = false;

	for (uint8_t t = 0; t < OSC2_HAP_GEN_COUNT; t++) {
		struct osc2_hap_gen *g = &ch->gen[t];
		bool ended = false;
		int64_t v;

		if (!(ch->mix & BIT8(t))) {
			continue;
		}
		if (!g->on) {
			if (--g->release == 0) {
				ch->mix &= (uint8_t)~BIT8(t);
			}
			continue;
		}
		switch (t) {
		case OSC2_HAP_GEN_CLICK_A:
		case OSC2_HAP_GEN_CLICK_B:
		case OSC2_HAP_GEN_CLIP:
			v = clip_sample(g, &ch->fifo[t], &ended);
			break;
		case OSC2_HAP_GEN_PULSE:
			v = pulse_sample(g, &ended);
			break;
		case OSC2_HAP_GEN_TONE:
			v = tone_sample(g, &ended);
			break;
		case OSC2_HAP_GEN_RUMBLE:
			v = rumble_sample(h, g, &ended);
			break;
		case OSC2_HAP_GEN_SWEEP:
			v = sweep_sample(g, &ended);
			break;
		case OSC2_HAP_GEN_STREAM:
			v = stream_sample(h, s, local, g, &ended);
			break;
		default:
			v = 0;
			break;
		}
		if (v == OSC2_HAP_NAN) {
			nan = true;
		} else {
			sum = osc2_hap_sat(sum + v);
		}
		if (ended) {
			end_gen(h, s, local, t);
		}
	}
	return nan ? OSC2_HAP_NAN : sum;
}

static void put_sample(uint8_t path, void *out, uint8_t frame, uint8_t c, int32_t v)
{
	if (path == OSC2_HAP_PADS) {
		((uint8_t *)out)[frame * 2u + c] = osc2_hap_to_u8(v);
	} else {
		uint16_t w = (uint16_t)osc2_hap_to_s16(v);

		/* little endian, whatever the CPU */
		((uint8_t *)out)[frame * 4u + c * 2u] = (uint8_t)w;
		((uint8_t *)out)[frame * 4u + c * 2u + 1u] = (uint8_t)(w >> 8);
	}
}

size_t osc2_hap_render(struct osc2_hap *h, uint8_t path, bool first, void *out)
{
	struct osc2_hap_seq *s = &h->seq[path];
	uint8_t from = first ? s->lead_in : 0u;

	/* Channel by channel, as the real sequencer renders a block: the negated one (the left
	 * pad, the right grip) first, which orders the 0x44 reports both make in one block. */
	for (uint8_t k = 0; k < 2; k++) {
		uint8_t c = k == 0 ? s->negated : (uint8_t)(1u - s->negated);

		for (uint8_t f = 0; f < s->frames; f++) {
			int32_t v = 0;

			if (f >= from) {
				int64_t sum = channel_sample(h, s, c);

				/* The sum times the channel gain, clipped; not a number plays +1. */
				v = osc2_hap_clip(sum == OSC2_HAP_NAN ? OSC2_HAP_NAN :
						  osc2_hap_mul_q16(sum, s->ch[c].gain_q16));
			}
			put_sample(path, out, f, c, v);
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
	uint32_t space;
	size_t take;
	uint32_t flags = 0;

	if (channel >= OSC2_HAP_CHANNELS) {
		return 0;
	}
	s = &h->seq[osc2_hap_path_of(channel)];
	st = &s->ch[channel & 1u].stream;
	space = OSC2_HAP_RING_SIZE - st->count;
	if (!s->ch[channel & 1u].gen[OSC2_HAP_GEN_STREAM].on && st->start_space > space) {
		/* Enough audio was already there: a start, from each such write until one is
		 * applied (none before a format: the start point is 0 until then). */
		struct osc2_hap_cmd cmd = {
			.op = OSC2_HAP_OP_START, .channel = channel, .type = OSC2_HAP_GEN_STREAM,
		};

		if (osc2_hap_queue(h, &cmd)) {
			note_stream(h, s, channel, OSC2_HAP_STREAM_EV_QUEUED, 0);
		}
	}
	if (len <= space && space - len < st->water) {
		/* Less than the water mark left free (never before a format: the mark is 0). */
		st->need_data = false;
		flags = OSC2_HAP_FLAG_HIGH;
	}
	take = len < space ? len : space;
	for (size_t i = 0; i < take; i++) {
		st->buf[(st->head + st->count + i) % OSC2_HAP_RING_SIZE] = data[i];
	}
	st->count = (uint16_t)(st->count + take);
	if (take < len) {
		flags |= OSC2_HAP_FLAG_DROPPED;     /* the bytes that fit are kept */
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
