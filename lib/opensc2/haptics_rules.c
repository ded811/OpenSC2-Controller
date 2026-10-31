/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * haptics_rules.c: the synthesiser's rules (haptics_rules.h). Pure C. The behaviour is the
 * protocol documentation's description of the real controller (not public yet; docs/protocol.md
 * lists the pages), restated here as code of OpenSC2's own.
 */
#include <string.h>
#include "haptics_rules.h"

/* Wrapping times: a is at or after b. */
static bool after_or_at(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) >= 0;
}

/* ---- the rumble emulation (report 0x80, October) ---------------------------------------------- */

int8_t osc2_hap_rumble_level_db(uint16_t intensity)
{
	switch (intensity) {
	case 16: return 16;
	case 60: return 12;
	case 100: return 8;
	case 200: return 4;
	case 1000: return -4;
	case 8000: return -8;
	case 16000: return -12;
	default: return 0;
	}
}

int8_t osc2_hap_rumble_right_db(uint16_t right_speed)
{
	/* trunc(speed * 0.0001) - 14 dB, -20 dB below 1000 (the integer division gives the double
	 * product's truncation for every 16-bit speed). The level step and setting 27 do not apply. */
	return right_speed < 1000 ? -20 : (int8_t)(right_speed / 10000 - 14);
}

int8_t osc2_hap_rumble_left_db(uint16_t left_speed, int16_t setting27, uint16_t intensity)
{
	/* trunc(speed * 0.00024) + 2 dB + setting 27 + the level step, less 12 dB (18 below 1000). */
	int32_t db = (int32_t)left_speed * 3 / 12500 + 2 + setting27 +
		     osc2_hap_rumble_level_db(intensity) - (left_speed < 1000 ? 18 : 12);

	/* kept in 8 bits as the real controller keeps it: a sum beyond them wraps round */
	return osc2_hap_db_add(db, 0);
}

uint32_t osc2_hap_rumble_lfo_mhz(uint16_t left_speed)
{
	/* 10 Hz up to a fifth of full speed, then 10 + 6 * (speed / 65535 - 0.4) Hz: just above a
	 * fifth that is 8.8 Hz, back at 10 Hz at 40 %, as the real controller computes it (kept as it
	 * is: the dip is what the real one plays). */
	if (left_speed <= 13107) {
		return 10000;
	}
	return 7600u + (6000u * left_speed + 32767u) / 65535u;
}

uint32_t osc2_hap_rumble_burst_mhz(uint16_t left_speed)
{
	/* Written as 12 + 35 * speed / (2^32 - 1) Hz: the 32-bit maximum where the 16-bit one would
	 * make the rate follow the speed, so the bursts come at 12 Hz whatever the speed. Kept as the
	 * real controller plays it, capped at 120 Hz as it caps it (never reached). */
	uint32_t mhz = 12000u + (uint32_t)((35000ull * left_speed) / 4294967295ull);

	return mhz > 120000u ? 120000u : mhz;
}

/* ---- output reports --------------------------------------------------------------------------- */

static struct osc2_hap_cmd *add_cmd(struct osc2_hap_actions *a, uint8_t op, uint8_t channel,
				    uint8_t type, int8_t gain_db)
{
	struct osc2_hap_cmd *c;

	if (a->n_cmd >= OSC2_HAP_ACT_CMDS) {
		return NULL;
	}
	c = &a->cmd[a->n_cmd++];
	memset(c, 0, sizeof(*c));
	c->op = op;
	c->channel = channel;
	c->type = type;
	c->gain_db = gain_db;
	return c;
}

/* The channels of a mask, the left one of each pair first, or the right one first. */
static uint8_t order_channels(uint8_t mask, bool right_first, uint8_t out[OSC2_HAP_CHANNELS])
{
	static const uint8_t left_first[4] = { 0, 1, 2, 3 };
	static const uint8_t right_order[4] = { 1, 0, 3, 2 };
	const uint8_t *order = right_first ? right_order : left_first;
	uint8_t n = 0;

	for (int i = 0; i < 4; i++) {
		if (mask & (1u << order[i])) {
			out[n++] = order[i];
		}
	}
	return n;
}

static void rumble(const struct opensc2_rumble *r, int16_t setting27, struct osc2_hap_actions *a)
{
	struct osc2_hap_cmd *c;

	if (r->timeout) {
		return;
	}
	/* The right grip first: the rumble generator on a 50 Hz carrier started downwards (on a
	 * channel whose gain is itself negated); a speed of 0 is a stop-all for that grip. */
	if (r->right_speed == 0) {
		(void)add_cmd(a, OSC2_HAP_OP_STOP, OSC2_HAP_CH_RIGHT_GRIP, OSC2_HAP_GEN_ALL, 0);
	} else {
		c = add_cmd(a, OSC2_HAP_OP_START, OSC2_HAP_CH_RIGHT_GRIP, OSC2_HAP_GEN_RUMBLE,
			    osc2_hap_rumble_right_db(r->right_speed));
		c->flags = OSC2_HAP_CMD_INVERT;
		c->duration_ms = OSC2_HAP_RUMBLE_MS;
		c->rumble.freq_mhz = OSC2_HAP_RUMBLE_HZ * 1000u;
		c->rumble.burst_mhz = osc2_hap_rumble_burst_mhz(r->left_speed);
	}
	/* The left grip: a 50 Hz tone with a full-depth LFO. */
	if (r->left_speed == 0) {
		(void)add_cmd(a, OSC2_HAP_OP_STOP, OSC2_HAP_CH_LEFT_GRIP, OSC2_HAP_GEN_ALL, 0);
	} else {
		c = add_cmd(a, OSC2_HAP_OP_START, OSC2_HAP_CH_LEFT_GRIP, OSC2_HAP_GEN_TONE,
			    osc2_hap_rumble_left_db(r->left_speed, setting27, r->intensity));
		c->duration_ms = OSC2_HAP_RUMBLE_MS;
		c->tone.freq_mhz = OSC2_HAP_RUMBLE_HZ * 1000u;
		c->tone.lfo_mhz = osc2_hap_rumble_lfo_mhz(r->left_speed);
		c->tone.depth = 100;
	}
}

static void pulse(const struct opensc2_haptic *h, struct osc2_hap_actions *a)
{
	uint8_t ch[OSC2_HAP_CHANNELS];
	uint8_t n = order_channels(h->channels, false, ch);
	struct osc2_hap_cmd *c;

	for (uint8_t i = 0; i < n; i++) {
		/* The special cases, in this order. */
		if (h->pulse.on_us == 0) {
			(void)add_cmd(a, OSC2_HAP_OP_STOP, ch[i], OSC2_HAP_GEN_ALL, 0);
		} else if (h->pulse.off_us == 0) {
			(void)add_cmd(a, OSC2_HAP_OP_START, ch[i], OSC2_HAP_GEN_CLICK_A, 8);
		} else if (h->pulse.count == 3 || h->pulse.count == 4) {
			(void)add_cmd(a, OSC2_HAP_OP_START, ch[i], OSC2_HAP_GEN_CLICK_B, 4);
		} else if (h->pulse.count == 0) {
			continue;   /* nothing */
		} else {
			c = add_cmd(a, OSC2_HAP_OP_START, ch[i], OSC2_HAP_GEN_PULSE, 0);
			if (c) {
				c->pulse.on_us = h->pulse.on_us;
				c->pulse.off_us = h->pulse.off_us;
				c->pulse.count = h->pulse.count;
			}
		}
	}
}

static void command(const struct opensc2_haptic *h, struct osc2_hap_actions *a)
{
	uint8_t ch[OSC2_HAP_CHANNELS];
	uint8_t n = order_channels(h->channels, false, ch);

	for (uint8_t i = 0; i < n; i++) {
		switch (h->command.command) {
		case OPENSC2_HAPTIC_CMD_STOP_ALL:
			(void)add_cmd(a, OSC2_HAP_OP_STOP, ch[i], OSC2_HAP_GEN_ALL, 0);
			break;
		case OPENSC2_HAPTIC_CMD_CLICK_A:
			/* click A plays 6 dB above the gain, added in 8 bits: from +122 dB up it
			 * wraps round to the bottom of the table */
			(void)add_cmd(a, OSC2_HAP_OP_START, ch[i], OSC2_HAP_GEN_CLICK_A,
				      osc2_hap_db_add(h->command.gain_db, 6));
			break;
		case OPENSC2_HAPTIC_CMD_CLICK_B:
			(void)add_cmd(a, OSC2_HAP_OP_START, ch[i], OSC2_HAP_GEN_CLICK_B,
				      h->command.gain_db);
			break;
		default:
			return;   /* any other command: the whole report is ignored */
		}
	}
}

static void tone(const struct opensc2_haptic *h, struct osc2_hap_actions *a)
{
	uint8_t ch[OSC2_HAP_CHANNELS];
	uint8_t n = order_channels(h->channels, false, ch);

	for (uint8_t i = 0; i < n; i++) {
		struct osc2_hap_cmd *c = add_cmd(a, OSC2_HAP_OP_START, ch[i], OSC2_HAP_GEN_TONE,
						 h->tone.gain_db);

		if (c) {
			c->duration_ms = h->tone.duration_ms;
			c->tone.freq_mhz = (uint32_t)h->tone.freq_hz * 1000u;
			c->tone.lfo_mhz = (uint32_t)h->tone.lfo_hz * 1000u;
			c->tone.depth = h->tone.lfo_depth;
		}
	}
}

static void sweep(const struct opensc2_haptic *h, struct osc2_hap_actions *a)
{
	uint8_t ch[OSC2_HAP_CHANNELS];
	uint8_t n = order_channels(h->channels, false, ch);

	for (uint8_t i = 0; i < n; i++) {
		struct osc2_hap_cmd *c = add_cmd(a, OSC2_HAP_OP_START, ch[i], OSC2_HAP_GEN_SWEEP,
						 h->sweep.gain_db);

		if (c) {
			c->duration_ms = h->sweep.duration_ms;
			c->sweep.f0_hz = h->sweep.start_hz;
			c->sweep.f1_hz = h->sweep.end_hz;
			/* targets 6 and 7: the right channel in antiphase */
			if (h->sweep.antiphase && (ch[i] & 1u)) {
				c->flags = OSC2_HAP_CMD_INVERT;
			}
		}
	}
}

static void pcm_control(const struct opensc2_haptic *h, struct osc2_hap_actions *a)
{
	uint8_t ch[OSC2_HAP_CHANNELS];
	uint8_t n = order_channels(h->channels, true, ch);

	for (uint8_t i = 0; i < n; i++) {
		struct osc2_hap_cmd *c;

		switch (h->pcm_control.operation) {
		case OPENSC2_PCM_STOP:
			(void)add_cmd(a, OSC2_HAP_OP_STOP, ch[i], OSC2_HAP_GEN_STREAM, 0);
			break;
		case OPENSC2_PCM_SET_FORMAT:
			c = add_cmd(a, OSC2_HAP_OP_FORMAT, ch[i], 0, 0);
			if (c) {
				c->format = h->pcm_control.format;
			}
			break;
		default:
			return;   /* 0: nothing; anything else: only a warning on the real controller */
		}
	}
}

void osc2_hap_actions(const struct opensc2_output *out, int16_t setting27,
		      struct osc2_hap_actions *a)
{
	const struct opensc2_haptic *h = &out->haptic;

	memset(a, 0, sizeof(*a));
	if (out->type == OPENSC2_OUTPUT_RUMBLE) {
		rumble(&out->rumble, setting27, a);
		return;
	}
	if (out->type != OPENSC2_OUTPUT_HAPTIC) {
		return;
	}
	switch (h->kind) {
	case OPENSC2_HAPTIC_PULSE:
		pulse(h, a);
		break;
	case OPENSC2_HAPTIC_COMMAND:
		command(h, a);
		break;
	case OPENSC2_HAPTIC_TONE:
		tone(h, a);
		break;
	case OPENSC2_HAPTIC_SWEEP:
		sweep(h, a);
		break;
	case OPENSC2_HAPTIC_SCRIPT: {
		uint8_t ch[OSC2_HAP_CHANNELS];
		uint8_t n = order_channels(h->channels, false, ch);

		for (uint8_t i = 0; i < n; i++) {
			a->script[a->n_script].channel = ch[i];
			a->script[a->n_script].id = h->script.id;
			a->script[a->n_script].gain_db = h->script.gain_db;
			a->n_script++;
		}
		break;
	}
	case OPENSC2_HAPTIC_PCM_CONTROL:
		pcm_control(h, a);
		break;
	case OPENSC2_HAPTIC_PCM_DATA: {
		uint8_t ch[OSC2_HAP_CHANNELS];
		uint8_t n = order_channels(h->channels, true, ch);

		for (uint8_t i = 0; i < n && i < 2; i++) {
			a->write[a->n_write].channel = ch[i];
			a->write[a->n_write].data = h->pcm.data;
			a->write[a->n_write].len = h->pcm.len;
			a->n_write++;
		}
		break;
	}
	default:
		break;
	}
}

void osc2_hap_library_clicks(const struct opensc2_haptic *h, uint8_t n,
			     struct osc2_hap_actions *a)
{
	memset(a, 0, sizeof(*a));
	for (uint8_t i = 0; i < n; i++) {
		uint8_t ch[OSC2_HAP_CHANNELS];
		uint8_t m;

		if (h[i].kind != OPENSC2_HAPTIC_COMMAND ||
		    (h[i].command.command != OPENSC2_HAPTIC_CMD_CLICK_A &&
		     h[i].command.command != OPENSC2_HAPTIC_CMD_CLICK_B)) {
			continue;
		}
		m = order_channels(h[i].channels & (OPENSC2_HAPTIC_LEFT_PAD | OPENSC2_HAPTIC_RIGHT_PAD),
				   false, ch);
		for (uint8_t k = 0; k < m; k++) {
			(void)add_cmd(a, OSC2_HAP_OP_START, ch[k],
				      h[i].command.command == OPENSC2_HAPTIC_CMD_CLICK_A ?
				      OSC2_HAP_GEN_CLICK_A : OSC2_HAP_GEN_CLICK_B,
				      h[i].command.gain_db);
		}
	}
}

/* ---- scripts ---------------------------------------------------------------------------------- */

void osc2_hap_step_cmd(const struct opensc2_haptics_step *step, uint8_t channel, int8_t gain_db,
		       struct osc2_hap_cmd *cmd)
{
	memset(cmd, 0, sizeof(*cmd));
	cmd->op = OSC2_HAP_OP_START;
	/* a step of the real controller's can name its own channel and gain */
	cmd->channel = (step->flags & OPENSC2_HAPTICS_STEP_ON_CHANNEL) ? (step->channel & 3u) :
		       channel;
	cmd->gain_db = (step->flags & OPENSC2_HAPTICS_STEP_FIXED_GAIN) ? step->gain_db :
		       osc2_hap_db_add(gain_db, step->gain_db);
	switch (step->kind) {
	case OPENSC2_HAPTICS_STEP_TONE:
		cmd->type = OSC2_HAP_GEN_TONE;
		cmd->tone.freq_mhz = (uint32_t)step->a * 1000u;
		cmd->duration_ms = step->b;
		cmd->tone.lfo_mhz = (uint32_t)step->c * 100u;
		cmd->tone.depth = step->d;
		break;
	case OPENSC2_HAPTICS_STEP_SWEEP:
		cmd->type = OSC2_HAP_GEN_SWEEP;
		cmd->sweep.f0_hz = step->a;
		cmd->sweep.f1_hz = step->b;
		cmd->duration_ms = step->c;
		break;
	case OPENSC2_HAPTICS_STEP_PULSE:
		cmd->type = OSC2_HAP_GEN_PULSE;
		cmd->gain_db = 0;          /* pulse steps play at full scale whatever the gain */
		cmd->pulse.on_us = step->a;
		cmd->pulse.off_us = step->b;
		cmd->pulse.count = step->c;
		break;
	case OPENSC2_HAPTICS_STEP_CLICK_A:
		cmd->type = OSC2_HAP_GEN_CLICK_A;
		break;
	case OPENSC2_HAPTICS_STEP_CLICK_B:
		cmd->type = OSC2_HAP_GEN_CLICK_B;
		break;
	case OPENSC2_HAPTICS_STEP_CLIP:
		cmd->type = OSC2_HAP_GEN_CLIP;
		if (step->clip) {
			cmd->format = step->clip->format;
			cmd->clip.data = step->clip->data;
			cmd->clip.samples = step->clip->samples;
		}
		break;
	default:
		cmd->op = OSC2_HAP_OP_STOP;    /* not reached: the player never plays an END */
		cmd->type = OSC2_HAP_GEN_COUNT;
		break;
	}
}

void osc2_hap_player_init(struct osc2_hap_player *p, uint8_t channel)
{
	memset(p, 0, sizeof(*p));
	p->channel = channel;
}

static uint32_t step_delay(const struct opensc2_haptics_step *s)
{
	return s->delay_ms ? s->delay_ms : 1u;   /* at least 1 ms */
}

bool osc2_hap_player_start(struct osc2_hap_player *p, uint8_t id, int8_t gain_db, uint32_t now_ms)
{
	const struct opensc2_haptics_step *steps;
	uint8_t count;

	if (p->busy || id < OSC2_HAP_SCRIPT_FIRST || id > OSC2_HAP_SCRIPT_LAST) {
		return false;   /* a busy channel: the running script goes on */
	}
	steps = osc2_hap_script(id, &count);
	if (!steps || count == 0) {
		return false;
	}
	p->steps = steps;
	p->count = count;
	p->next = 0;
	p->id = id;
	p->gain_db = gain_db;
	p->busy = true;
	p->due_ms = now_ms + step_delay(&steps[0]);
	return true;
}

enum osc2_hap_poll osc2_hap_player_poll(struct osc2_hap_player *p, uint32_t now_ms,
					struct osc2_hap_cmd *cmd)
{
	const struct opensc2_haptics_step *step;

	if (!p->busy || !after_or_at(now_ms, p->due_ms)) {
		return OSC2_HAP_POLL_NONE;
	}
	step = p->next < p->count ? &p->steps[p->next] : NULL;
	if (!step || step->kind == OPENSC2_HAPTICS_STEP_END) {
		/* The end marker's delay has passed: the channel is free. */
		p->busy = false;
		return OSC2_HAP_POLL_DONE;
	}
	osc2_hap_step_cmd(step, p->channel, p->gain_db, cmd);
	p->next++;
	/* The next delay counts from the moment this step ran; the step does not wait for its sound. */
	p->due_ms = now_ms + (p->next < p->count ? step_delay(&p->steps[p->next]) : 1u);
	return OSC2_HAP_POLL_STEP;
}

int32_t osc2_hap_player_wait(const struct osc2_hap_player *p, uint32_t now_ms)
{
	if (!p->busy) {
		return -1;
	}
	return after_or_at(now_ms, p->due_ms) ? 0 : (int32_t)(p->due_ms - now_ms);
}

/* ---- cues ------------------------------------------------------------------------------------- */

bool osc2_hap_cue(uint8_t cue, uint8_t level, uint8_t *script, int8_t *gain_db)
{
	static const uint8_t scripts[OSC2_HAP_CUE_COUNT] = { 1, 1, 2, 1, 2, 5, 8, 9, 10, 3, 4 };
	int8_t gain;

	if (cue >= OSC2_HAP_CUE_COUNT) {
		return false;
	}
	*script = scripts[cue];
	if (cue == 8) {
		*gain_db = 2;           /* the factory charge window's sweep, whatever the level */
		return true;
	}
	if (level == 1) {
		gain = -18;
	} else if (level == 2) {
		gain = -12;
	} else {
		return false;           /* silent */
	}
	*gain_db = (int8_t)(cue == 6 || cue == 7 ? gain - 1 : gain);
	return true;
}

/* ---- drop sounds ------------------------------------------------------------------------------ */

void osc2_hap_drop_init(struct osc2_hap_drop *d)
{
	memset(d, 0, sizeof(*d));
	d->state = OSC2_HAP_DROP_ARMED;
}

bool osc2_hap_drop_fall(struct osc2_hap_drop *d, uint32_t now_ms)
{
	if (d->state != OSC2_HAP_DROP_ARMED) {
		return false;
	}
	d->state = OSC2_HAP_DROP_PENDING;
	d->play_ms = now_ms + OSC2_HAP_DROP_DELAY_MS;
	d->rearm_ms = now_ms + OSC2_HAP_DROP_REARM_MS;
	return true;
}

enum osc2_hap_drop_poll osc2_hap_drop_poll(struct osc2_hap_drop *d, uint32_t now_ms)
{
	if (d->state == OSC2_HAP_DROP_PENDING && after_or_at(now_ms, d->play_ms)) {
		d->state = OSC2_HAP_DROP_WAITING;
		return OSC2_HAP_DROP_PLAY;
	}
	if (d->state == OSC2_HAP_DROP_WAITING && after_or_at(now_ms, d->rearm_ms)) {
		d->state = OSC2_HAP_DROP_ARMED;
		return OSC2_HAP_DROP_REARMED;
	}
	return OSC2_HAP_DROP_NOTHING;
}

bool osc2_hap_drop_imu_stopped(struct osc2_hap_drop *d)
{
	if (d->state != OSC2_HAP_DROP_PENDING && d->state != OSC2_HAP_DROP_WAITING) {
		return false;
	}
	d->state = OSC2_HAP_DROP_OFF;
	return true;
}

int32_t osc2_hap_drop_wait(const struct osc2_hap_drop *d, uint32_t now_ms)
{
	uint32_t due;

	if (d->state == OSC2_HAP_DROP_PENDING) {
		due = d->play_ms;
	} else if (d->state == OSC2_HAP_DROP_WAITING) {
		due = d->rearm_ms;
	} else {
		return -1;
	}
	return after_or_at(now_ms, due) ? 0 : (int32_t)(due - now_ms);
}

uint8_t osc2_hap_drop_pick(uint8_t random_byte)
{
	if (random_byte < 48) {
		return OSC2_HAP_SCRIPT_DROP_1;
	}
	if (random_byte < 128) {
		return OSC2_HAP_SCRIPT_DROP_2;
	}
	return 0;
}
