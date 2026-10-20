/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * haptics_run.c: the synthesiser around its engine (haptics_run.h). Pure C.
 */
#include <string.h>
#include "haptics_run.h"
#include "retained.h"   /* osc2_crc32() */

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, (uint16_t)v);
	put16(p + 2, (uint16_t)(v >> 16));
}

/* ---- the outbox ------------------------------------------------------------------------------- */

static void out_push(struct osc2_hap_rt *rt, uint8_t what, uint8_t kind, const uint8_t *data,
		     uint8_t len)
{
	struct osc2_hap_out_item *it;

	if (rt->out_count >= OSC2_HAP_OUTBOX) {
		rt->out_lost++;
		return;
	}
	it = &rt->outbox[rt->out_count++];
	it->what = what;
	it->kind = kind;
	it->len = len;
	memcpy(it->data, data, len);
}

static void note(struct osc2_hap_rt *rt, uint8_t kind, const uint8_t *data, uint8_t len)
{
	if (rt->notes) {
		out_push(rt, OSC2_HAP_OUT_NOTE, kind, data, len);
	}
}

static void note_stream(struct osc2_hap_rt *rt, uint8_t channel, uint8_t what, uint16_t fill,
			uint32_t flags)
{
	uint8_t d[8] = { channel, what };

	put16(&d[2], fill);
	put32(&d[4], flags);
	note(rt, OSC2_HAP_RING_STREAM, d, sizeof(d));
}

static void note_script(struct osc2_hap_rt *rt, uint8_t what, uint8_t channel, uint8_t id,
			int8_t gain_db, uint16_t extra)
{
	uint8_t d[6] = { what, channel, id, (uint8_t)gain_db };

	put16(&d[4], extra);
	note(rt, OSC2_HAP_RING_SCRIPT, d, sizeof(d));
}

static void status_out(struct osc2_hap_rt *rt, uint8_t channel, uint32_t flags)
{
	uint8_t d[5] = { osc2_hap_status_id(channel) };

	put32(&d[1], flags);
	out_push(rt, OSC2_HAP_OUT_STATUS, 0, d, sizeof(d));
	note_stream(rt, channel, OSC2_HAP_STREAM_EV_STATUS, osc2_hap_stream_fill(&rt->engine, channel),
		    flags);
}

/* The engine's events of `path`: 0x44 reports, and ring entries while the hooks are armed. */
static void drain(struct osc2_hap_rt *rt, uint8_t path)
{
	struct osc2_hap_event ev[OSC2_HAP_EVENTS_MAX];
	uint8_t n = osc2_hap_take_events(&rt->engine, path, ev, OSC2_HAP_EVENTS_MAX);

	for (uint8_t i = 0; i < n; i++) {
		const struct osc2_hap_event *e = &ev[i];

		if (e->kind == OSC2_HAP_EV_STATUS) {
			status_out(rt, e->channel, e->flags);
		} else if (e->kind == OSC2_HAP_EV_GEN) {
			uint8_t d[12] = { e->channel, e->type, e->what, (uint8_t)e->gain_db };

			put16(&d[4], e->a);
			put16(&d[6], e->b);
			put16(&d[8], e->c);
			put16(&d[10], e->d);
			note(rt, OSC2_HAP_RING_GEN, d, sizeof(d));
		} else if (e->kind == OSC2_HAP_EV_STREAM) {
			note_stream(rt, e->channel, e->what, e->a, e->flags);
		}
	}
}

static void sync(struct osc2_hap_rt *rt)
{
	rt->engine.notes = rt->notes;
}

uint8_t osc2_hap_rt_take(struct osc2_hap_rt *rt, struct osc2_hap_out_item *out, uint8_t max)
{
	uint8_t n = rt->out_count < max ? rt->out_count : max;

	memcpy(out, rt->outbox, n * sizeof(out[0]));
	if (n < rt->out_count) {
		memmove(rt->outbox, &rt->outbox[n], (rt->out_count - n) * sizeof(rt->outbox[0]));
	}
	rt->out_count = (uint8_t)(rt->out_count - n);
	return n;
}

/* ---- set-up ----------------------------------------------------------------------------------- */

static void players_init(struct osc2_hap_rt *rt)
{
	for (uint8_t c = 0; c < OSC2_HAP_CHANNELS; c++) {
		osc2_hap_player_init(&rt->player[c], c);
	}
}

void osc2_hap_rt_init(struct osc2_hap_rt *rt)
{
	memset(rt, 0, sizeof(*rt));
	osc2_hap_init(&rt->engine);
	players_init(rt);
	osc2_hap_drop_init(&rt->drop);
	rt->forced_random = -1;
}

/* ---- feeding ---------------------------------------------------------------------------------- */

static uint8_t path_bit(uint8_t channel)
{
	return (uint8_t)(1u << osc2_hap_path_of(channel));
}

static bool start_script(struct osc2_hap_rt *rt, uint8_t channel, uint8_t id, int8_t gain_db,
			 uint32_t now_ms)
{
	bool ok = osc2_hap_player_start(&rt->player[channel], id, gain_db, now_ms);

	/* A busy channel refuses ("already active"): the running script goes on. */
	note_script(rt, ok ? OSC2_HAP_SCRIPT_EV_START : OSC2_HAP_SCRIPT_EV_REFUSED, channel, id,
		    gain_db, 0);
	return ok;
}

bool osc2_hap_rt_queue(struct osc2_hap_rt *rt, const struct osc2_hap_cmd *cmd)
{
	bool ok;

	sync(rt);
	ok = osc2_hap_queue(&rt->engine, cmd);
	drain(rt, osc2_hap_path_of(cmd->channel & 3u));
	return ok;
}

uint8_t osc2_hap_rt_feed(struct osc2_hap_rt *rt, const struct opensc2_output *out, int n,
			 int16_t setting27, uint32_t now_ms)
{
	/* 0x88 carries both grips: the real controller writes the right grip's bytes first. */
	bool reverse = n == 2 && out[0].type == OPENSC2_OUTPUT_HAPTIC &&
		       out[0].haptic.report_id == 0x88;
	uint8_t wake = 0;

	sync(rt);
	for (int k = 0; k < n; k++) {
		const struct opensc2_output *o = &out[reverse ? n - 1 - k : k];
		struct osc2_hap_actions a;

		osc2_hap_actions(o, setting27, &a);
		for (uint8_t i = 0; i < a.n_cmd; i++) {
			if (osc2_hap_queue(&rt->engine, &a.cmd[i])) {
				wake |= path_bit(a.cmd[i].channel);
			}
		}
		for (uint8_t i = 0; i < a.n_write; i++) {
			uint8_t ch = a.write[i].channel;
			bool pending = osc2_hap_pending(&rt->engine, osc2_hap_path_of(ch));
			uint32_t flags = osc2_hap_stream_write(&rt->engine, ch, a.write[i].data,
							       a.write[i].len);

			note_stream(rt, ch, OSC2_HAP_STREAM_EV_WRITE, osc2_hap_stream_fill(&rt->engine, ch),
				    flags);
			if (flags) {
				/* flags of a write go out at once, from the path that received it */
				status_out(rt, ch, flags);
			}
			if (!pending && osc2_hap_pending(&rt->engine, osc2_hap_path_of(ch))) {
				wake |= path_bit(ch);   /* the write queued the stream's start */
			}
		}
		for (uint8_t i = 0; i < a.n_script; i++) {
			(void)start_script(rt, a.script[i].channel, a.script[i].id, a.script[i].gain_db,
					   now_ms);
		}
	}
	drain(rt, OSC2_HAP_PADS);
	drain(rt, OSC2_HAP_GRIPS);
	return wake;
}

uint8_t osc2_hap_rt_clicks(struct osc2_hap_rt *rt, const struct opensc2_haptic *h, uint8_t n)
{
	struct osc2_hap_actions a;
	uint8_t wake = 0;

	sync(rt);
	osc2_hap_library_clicks(h, n, &a);
	for (uint8_t i = 0; i < a.n_cmd; i++) {
		if (osc2_hap_queue(&rt->engine, &a.cmd[i])) {
			wake |= path_bit(a.cmd[i].channel);
		}
	}
	drain(rt, OSC2_HAP_PADS);
	return wake;
}

uint8_t osc2_hap_rt_script(struct osc2_hap_rt *rt, uint8_t channels, uint8_t id, int8_t gain_db,
			   uint32_t now_ms)
{
	uint8_t started = 0;

	sync(rt);
	for (uint8_t c = 0; c < OSC2_HAP_CHANNELS; c++) {
		if (channels & (1u << c)) {
			if (start_script(rt, c, id, gain_db, now_ms)) {
				started++;
			}
		}
	}
	return started;
}

int osc2_hap_rt_cue(struct osc2_hap_rt *rt, uint8_t cue, uint8_t level, uint32_t now_ms)
{
	uint8_t script = 0;
	int8_t gain = 0;
	bool plays = osc2_hap_cue(cue, level, &script, &gain);

	sync(rt);
	note_script(rt, OSC2_HAP_SCRIPT_EV_CUE, level, cue, gain, plays ? script : 0);
	if (!plays) {
		return -1;
	}
	/* Always on both grips, through their two players. */
	return osc2_hap_rt_script(rt, (1u << OSC2_HAP_CH_LEFT_GRIP) | (1u << OSC2_HAP_CH_RIGHT_GRIP),
				  script, gain, now_ms);
}

bool osc2_hap_rt_fall(struct osc2_hap_rt *rt, uint32_t now_ms)
{
	bool taken = osc2_hap_drop_fall(&rt->drop, now_ms);

	sync(rt);
	note_script(rt, OSC2_HAP_SCRIPT_EV_FALL, OSC2_HAP_CH_LEFT_GRIP, 0, 0, taken ? 1u : 0u);
	return taken;
}

/* ---- the clock -------------------------------------------------------------------------------- */

uint8_t osc2_hap_rt_poll(struct osc2_hap_rt *rt, uint32_t now_ms, uint8_t random8)
{
	uint8_t wake = 0;

	sync(rt);
	for (uint8_t c = 0; c < OSC2_HAP_CHANNELS; c++) {
		struct osc2_hap_player *p = &rt->player[c];

		for (;;) {
			struct osc2_hap_cmd cmd;
			uint8_t step = p->next;
			enum osc2_hap_poll r = osc2_hap_player_poll(p, now_ms, &cmd);

			if (r == OSC2_HAP_POLL_STEP) {
				note_script(rt, OSC2_HAP_SCRIPT_EV_STEP, c, p->id, p->gain_db, step);
				if (osc2_hap_queue(&rt->engine, &cmd)) {
					wake |= path_bit(c);
				}
				continue;
			}
			if (r == OSC2_HAP_POLL_DONE) {
				note_script(rt, OSC2_HAP_SCRIPT_EV_DONE, c, p->id, p->gain_db, 0);
			}
			break;
		}
	}
	switch (osc2_hap_drop_poll(&rt->drop, now_ms)) {
	case OSC2_HAP_DROP_PLAY: {
		uint8_t byte = rt->forced_random >= 0 ? (uint8_t)rt->forced_random : random8;
		uint8_t script = osc2_hap_drop_pick(byte);

		rt->forced_random = -1;
		note_script(rt, OSC2_HAP_SCRIPT_EV_DROP, OSC2_HAP_CH_LEFT_GRIP, script, 0, byte);
		if (script) {
			/* On the left grip at a script gain of 0 dB, whatever the chime level. */
			(void)start_script(rt, OSC2_HAP_CH_LEFT_GRIP, script, 0, now_ms);
		}
		break;
	}
	case OSC2_HAP_DROP_REARMED:
		note_script(rt, OSC2_HAP_SCRIPT_EV_REARM, OSC2_HAP_CH_LEFT_GRIP, 0, 0, 0);
		break;
	default:
		break;
	}
	drain(rt, OSC2_HAP_PADS);
	drain(rt, OSC2_HAP_GRIPS);
	return wake;
}

int32_t osc2_hap_rt_next(const struct osc2_hap_rt *rt, uint32_t now_ms)
{
	int32_t next = osc2_hap_drop_wait(&rt->drop, now_ms);

	for (uint8_t c = 0; c < OSC2_HAP_CHANNELS; c++) {
		int32_t w = osc2_hap_player_wait(&rt->player[c], now_ms);

		if (w >= 0 && (next < 0 || w < next)) {
			next = w;
		}
	}
	return next;
}

/* ---- blocks ----------------------------------------------------------------------------------- */

size_t osc2_hap_rt_block(struct osc2_hap_rt *rt, uint8_t path, bool first, void *out, bool *busy)
{
	size_t bytes;

	sync(rt);
	osc2_hap_apply(&rt->engine, path);
	drain(rt, path);
	if (first) {
		rt->index[path] = 0;
	}
	bytes = osc2_hap_render(&rt->engine, path, first, out);
	*busy = osc2_hap_busy(&rt->engine, path);
	if (rt->capture[path]) {
		uint8_t d[12] = { path };

		rt->capture[path]--;
		put32(&d[1], rt->index[path]);
		put32(&d[5], osc2_crc32(0, out, bytes));
		put16(&d[9], osc2_hap_block_peak(path, out));
		d[11] = (uint8_t)((first ? OSC2_HAP_BLOCK_FIRST : 0u) |
				  (*busy ? 0u : OSC2_HAP_BLOCK_LAST));
		note(rt, OSC2_HAP_RING_BLOCK, d, sizeof(d));
	}
	rt->index[path]++;
	drain(rt, path);
	return bytes;
}

bool osc2_hap_rt_idle(struct osc2_hap_rt *rt, uint8_t path)
{
	sync(rt);
	osc2_hap_apply(&rt->engine, path);
	drain(rt, path);
	return osc2_hap_busy(&rt->engine, path);
}

/* ---- the test hooks' ops ---------------------------------------------------------------------- */

static uint8_t play(struct osc2_hap_rt *rt, const uint8_t *args, uint8_t alen, uint32_t now_ms,
		    int32_t *rc, uint8_t *wake, bool *schedule)
{
	switch (args[0]) {
	case OSC2_HAP_PLAY_CUE:
		if (alen != 3) {
			return 2;
		}
		*rc = osc2_hap_rt_cue(rt, args[1], args[2], now_ms);
		*schedule = true;
		return 0;
	case OSC2_HAP_PLAY_CLICK:
		if (alen != 4) {
			return 2;
		}
		if (args[2] != 1 && args[2] != 2) {
			return 4;
		}
		*rc = 0;
		for (uint8_t c = 0; c < OSC2_HAP_CHANNELS; c++) {
			struct osc2_hap_cmd cmd;

			if (!(args[1] & (1u << c))) {
				continue;
			}
			memset(&cmd, 0, sizeof(cmd));
			cmd.op = OSC2_HAP_OP_START;
			cmd.channel = c;
			cmd.type = args[2] == 1 ? OSC2_HAP_GEN_CLICK_A : OSC2_HAP_GEN_CLICK_B;
			cmd.gain_db = (int8_t)args[3];
			if (osc2_hap_queue(&rt->engine, &cmd)) {
				(*rc)++;
				*wake |= path_bit(c);
			}
		}
		drain(rt, OSC2_HAP_PADS);
		drain(rt, OSC2_HAP_GRIPS);
		return 0;
	case OSC2_HAP_PLAY_SCRIPT:
		if (alen != 4) {
			return 2;
		}
		*rc = osc2_hap_rt_script(rt, args[1], args[2], (int8_t)args[3], now_ms);
		*schedule = true;
		return 0;
	case OSC2_HAP_PLAY_FALL:
		if (alen != 2) {
			return 2;
		}
		*rc = osc2_hap_rt_fall(rt, now_ms) ? 1 : 0;
		if (*rc) {
			rt->forced_random = args[1];   /* the byte this fall's sound will use */
		}
		*schedule = true;
		return 0;
	case OSC2_HAP_PLAY_STOP:
		if (alen != 2) {
			return 2;
		}
		*rc = 0;
		for (uint8_t c = 0; c < OSC2_HAP_CHANNELS; c++) {
			struct osc2_hap_cmd cmd;

			if (!(args[1] & (1u << c))) {
				continue;
			}
			memset(&cmd, 0, sizeof(cmd));
			cmd.op = OSC2_HAP_OP_STOP;
			cmd.channel = c;
			cmd.type = OSC2_HAP_GEN_ALL;
			if (osc2_hap_queue(&rt->engine, &cmd)) {
				(*rc)++;
				*wake |= path_bit(c);
			}
		}
		drain(rt, OSC2_HAP_PADS);
		drain(rt, OSC2_HAP_GRIPS);
		return 0;
	default:
		return 4;
	}
}

static void status(const struct osc2_hap_rt *rt, const struct osc2_hap_rt_glue *glue,
		   uint8_t out[OSC2_HAP_STATUS_LEN])
{
	const struct osc2_hap *h = &rt->engine;
	uint8_t players = 0;

	memset(out, 0, OSC2_HAP_STATUS_LEN);
	out[0] = glue->features;
	out[1] = (uint8_t)((osc2_hap_busy(h, OSC2_HAP_PADS) ? 1u : 0u) |
			   (osc2_hap_busy(h, OSC2_HAP_GRIPS) ? 2u : 0u));
	out[2] = glue->outputs;
	out[3] = glue->amp_mode;
	out[4] = (uint8_t)(int8_t)(h->master_db < -128 ? -128 : h->master_db > 127 ? 127 : h->master_db);
	for (uint8_t c = 0; c < OSC2_HAP_CHANNELS; c++) {
		players |= rt->player[c].busy ? (uint8_t)(1u << c) : 0u;
	}
	out[5] = players;
	out[6] = rt->drop.state;
	out[7] = glue->running;
	put32(&out[8], h->seq[OSC2_HAP_PADS].blocks);
	put32(&out[12], h->seq[OSC2_HAP_GRIPS].blocks);
	put16(&out[16], (uint16_t)rt->underruns[OSC2_HAP_PADS]);
	put16(&out[18], (uint16_t)rt->underruns[OSC2_HAP_GRIPS]);
	put16(&out[20], (uint16_t)(h->seq[OSC2_HAP_PADS].lost + h->seq[OSC2_HAP_GRIPS].lost));
	for (uint8_t c = 0; c < OSC2_HAP_CHANNELS; c++) {
		out[22 + c] = (uint8_t)osc2_hap_gen_mask(h, c);
		put16(&out[26 + 2 * c], osc2_hap_stream_fill(h, c));
		out[34 + c] = osc2_hap_stream_format(h, c);
	}
}

uint8_t osc2_hap_rt_test_op(struct osc2_hap_rt *rt, uint8_t op, const uint8_t *args, uint8_t alen,
			    uint32_t now_ms, const struct osc2_hap_rt_glue *glue, uint8_t *result,
			    uint8_t *rlen, uint8_t *wake, bool *schedule)
{
	*rlen = 0;
	*wake = 0;
	*schedule = false;
	sync(rt);
	switch (op) {
	case OSC2_HAP_OP_RESET: {
		int16_t master = rt->engine.master_db;
		bool notes = rt->notes;

		if (alen != 4) {
			return 2;
		}
		/* Everything stops at once, the queues and rings empty, the formats forgotten, the
		 * players and the drop sounds idle; the threads drain what they have queued. */
		osc2_hap_reset(&rt->engine);
		osc2_hap_seed(&rt->engine, (uint32_t)args[0] | ((uint32_t)args[1] << 8) |
				  ((uint32_t)args[2] << 16) | ((uint32_t)args[3] << 24));
		rt->engine.master_db = master;
		players_init(rt);
		osc2_hap_drop_init(&rt->drop);
		rt->capture[0] = rt->capture[1] = 0;
		rt->forced_random = -1;
		rt->out_count = 0;
		rt->notes = notes;
		*wake = 3;
		return 0;
	}
	case OSC2_HAP_OP_CAPTURE:
		if (alen != 2) {
			return 2;
		}
		if (args[0] > 2) {
			return 4;
		}
		if (args[0] != OSC2_HAP_GRIPS) {
			rt->capture[OSC2_HAP_PADS] = args[1];
		}
		if (args[0] != OSC2_HAP_PADS) {
			rt->capture[OSC2_HAP_GRIPS] = args[1];
		}
		return 0;
	case OSC2_HAP_OP_STATUS:
		if (alen != 0) {
			return 2;
		}
		status(rt, glue, result);
		*rlen = OSC2_HAP_STATUS_LEN;
		return 0;
	case OSC2_HAP_OP_PLAY: {
		int32_t rc = 0;
		uint8_t st;

		if (alen < 1) {
			return 2;
		}
		st = play(rt, args, alen, now_ms, &rc, wake, schedule);
		if (st == 0) {
			put32(result, (uint32_t)rc);
			*rlen = 4;
		}
		return st;
	}
	default:
		return 1;
	}
}
