/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * haptics.c: the haptics synthesiser on Zephyr (CONFIG_OPENSC2_HAPTICS_SYNTH). The logic is pure C
 * (haptics_run.c around the engine of haptics_core.c); this file runs it:
 *   - one thread per path (pads, grips), as the real controller has one per sequencer: it sleeps
 *     until something plays, then renders block after block for the path's output, one block
 *     ahead, and drains the output once nothing plays any more;
 *   - the script players and the drop sounds on one work item of the system work queue;
 *   - one mutex around every call into the logic, never held while an output plays, a report is
 *     sent or an entry is written into the test hooks' ring, and one that lets a single thread at
 *     a time send what the logic made, in order;
 *   - settings 75 (amplifier mode) and 76 (master gain) handed to the four channels as configure
 *     commands at the end of start-up and whenever they change, as the real controller's settings
 *     handler does (osc2_haptics_setting()), and the rumble gain offset (setting 27) as each
 *     rumble report arrives.
 * The output reports come from report.c's dispatcher (osc2_haptics_outputs()), after the
 * application's callbacks.
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <opensc2/haptics.h>
#include "internal.h"
#include "haptics_glue.h"
#include "haptics_run.h"
#include "test_hooks.h"

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

#define BLOCK_US 8000        /* both paths: 32 frames at 4 kHz, 64 at 8 kHz */
#define FLUSH_CHUNK 8

static struct osc2_hap_rt rt;
static K_MUTEX_DEFINE(hap_lock);
/* Held while the outbox is emptied and sent, so the reports go out in the order they were made
 * even when two threads flush at once (taken before hap_lock, never while holding it). */
static K_MUTEX_DEFINE(flush_lock);
static K_SEM_DEFINE(wake_pads, 0, 1);
static K_SEM_DEFINE(wake_grips, 0, 1);

struct seq {
	uint8_t path;
	struct k_sem *wake;
	/* the application's output, or its level callback (under hap_lock) */
	const struct opensc2_haptics_output *out;
	opensc2_haptics_level_fn level_fn;
	void *level_user;
	bool amp_on;             /* the enable as last set */
	bool amp_held;           /* amplifier mode 1 applied: the enable held on */
	volatile bool running;   /* in a playback or its drain */
	uint8_t block[OSC2_HAP_BLOCK_MAX_BYTES];
};

static struct seq seqs[OSC2_HAP_PATHS] = {
	{ .path = OSC2_HAP_PADS, .wake = &wake_pads },
	{ .path = OSC2_HAP_GRIPS, .wake = &wake_grips },
};

static uint32_t now_ms(void)
{
	return k_uptime_get_32();
}

/* ---- the lock, and what goes out after it ----------------------------------------------------- */

static void lock(void)
{
	(void)k_mutex_lock(&hap_lock, K_FOREVER);
#if defined(CONFIG_OPENSC2_TEST_HOOKS)
	rt.notes = osc2_test_core_armed(&osc2_test_core_obj);
#endif
}

static void unlock(void)
{
	(void)k_mutex_unlock(&hap_lock);
}

static void emit(const struct osc2_hap_out_item *it)
{
	if (it->what == OSC2_HAP_OUT_STATUS) {
		/* flow control, input report 0x44, on the link in use */
		(void)opensc2_send_haptic_status(it->data[0], opensc2_get_le32(&it->data[1]));
#if defined(CONFIG_OPENSC2_TEST_HOOKS)
		if (it->data[5] != 0xFFu) {
			/* the reply to a stop, noted with the link of the report that stopped */
			osc2_test_note_status(it->data[5], it->data[0], opensc2_get_le32(&it->data[1]));
		}
#endif
		return;
	}
#if defined(CONFIG_OPENSC2_TEST_HOOKS)
	osc2_test_core_note(&osc2_test_core_obj, &osc2_test_env_obj, it->kind, OSC2_TEST_LINK_NONE,
			    it->data, it->len);
#endif
}

/* Send what the last calls left in the outbox (without the lock). One thread at a time: a 0x08 of
 * a write and the 0x04 the sequencer makes right after it must not swap places on their way out. */
static void flush(void)
{
	struct osc2_hap_out_item it[FLUSH_CHUNK];
	uint8_t n;

	(void)k_mutex_lock(&flush_lock, K_FOREVER);
	do {
		lock();
		n = osc2_hap_rt_take(&rt, it, FLUSH_CHUNK);
		unlock();
		for (uint8_t i = 0; i < n; i++) {
			emit(&it[i]);
		}
	} while (n == FLUSH_CHUNK);
	(void)k_mutex_unlock(&flush_lock);
}

static void wake(uint8_t paths)
{
	for (uint8_t p = 0; p < OSC2_HAP_PATHS; p++) {
		if (paths & (1u << p)) {
			k_sem_give(seqs[p].wake);
		}
	}
}

/* ---- scripts and drop sounds: the system work queue -------------------------------------------- */

static void script_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(script_work, script_fn);

/* Time the work item for the next step, drop sound or re-arm. Call with the lock held. */
static void schedule_locked(void)
{
	int32_t next = osc2_hap_rt_next(&rt, now_ms());

	if (next >= 0) {
		(void)k_work_reschedule(&script_work, K_MSEC(next));
	}
}

static void script_fn(struct k_work *work)
{
	uint8_t paths;

	ARG_UNUSED(work);
	lock();
	/* The system's random source picks the drop sound (only drawn when one is due). */
	paths = osc2_hap_rt_poll(&rt, now_ms(),
				 rt.drop.state == OSC2_HAP_DROP_PENDING ? sys_rand8_get() : 0);
	schedule_locked();
	unlock();
	wake(paths);
	flush();
}

/* ---- the sequencers -------------------------------------------------------------------------- */

/* The enable, as the real sequencer drives its pin: on as a playback starts and off once it has
 * drained, unless amplifier mode 1 holds it on. */
static void amp(struct seq *s, const struct opensc2_haptics_output *out, bool on)
{
	if (s->amp_on != on) {
		s->amp_on = on;
		if (out && out->api->enable) {
			out->api->enable(out, on);
		}
	}
}

/* An amplifier mode the sequencer just applied (osc2_hap_rt_amp()): mode 1 raises the enable at
 * once and holds it, mode 0 drops it at once, even in the middle of a sound. Call with the lock
 * not held. */
static void amp_mode(struct seq *s, const struct opensc2_haptics_output *out, int8_t mode)
{
	if (mode < 0) {
		return;
	}
	s->amp_held = mode == 1;
	if (s->amp_held) {
		amp(s, out, true);
	} else {
		s->amp_on = true;            /* driven low whatever it was */
		amp(s, out, false);
	}
}

/* Wait until the output can take the next block without waiting, as the real sequencer waits for
 * one of its two blocks to come back before it renders the next: with the output's ready(), or
 * without an output on the timer (block k at t0 + (k - 1) * 8 ms). */
static void wait_room(struct seq *s, const struct opensc2_haptics_output *out, int64_t t0,
		      uint32_t k)
{
	if (out) {
		if (out->api->ready) {
			(void)out->api->ready(out);
		}
	} else if (k >= 2) {
		k_sleep(K_TIMEOUT_ABS_US(t0 + (int64_t)(k - 1) * BLOCK_US));
	}
	ARG_UNUSED(s);
}

static void playback(struct seq *s, const struct opensc2_haptics_output *out,
		     opensc2_haptics_level_fn level_fn, void *level_user)
{
	size_t frames = s->path == OSC2_HAP_PADS ? OPENSC2_HAPTICS_PAD_FRAMES :
			OPENSC2_HAPTICS_GRIP_FRAMES;
	int64_t t0 = (int64_t)k_ticks_to_us_floor64((uint64_t)k_uptime_ticks());
	uint32_t k = 0;
	bool busy = true;

	while (busy) {
		bool first = k == 0;
		int8_t mode = -1;

		if (!first) {
			/* Every command queued since the last block, then the wait for room: what
			 * arrives during the wait is applied to the block after this one. */
			lock();
			osc2_hap_rt_apply(&rt, s->path);
			mode = osc2_hap_rt_amp(&rt, s->path);
			unlock();
			flush();
			amp_mode(s, out, mode);
			wait_room(s, out, t0, k);
		}
		lock();
		(void)osc2_hap_rt_render(&rt, s->path, first, s->block, &busy);
		unlock();
		flush();
		if (out) {
			int err = out->api->write(out, s->block, frames);

			if (err == -EPIPE) {
				/* An underrun (the thread was held up): stop, the block again, start. */
				lock();
				rt.underruns[s->path]++;
				unlock();
				LOG_DBG("Haptics path %u ran out of blocks", s->path);
				if (out->api->stop) {
					(void)out->api->stop(out, true);
				}
				err = out->api->write(out, s->block, frames);
				first = err == 0;
			}
			if (first) {
				/* The first block queued: the enable up (unless held), then start. */
				if (!s->amp_held) {
					amp(s, out, true);
				}
				if (out->api->start) {
					(void)out->api->start(out);
				}
			}
		} else if (level_fn) {
			level_fn((enum opensc2_haptics_path)s->path,
				 opensc2_haptics_block_level((enum opensc2_haptics_path)s->path,
							     s->block, frames),
				 level_user);
		}
		k++;
	}
	/* Nothing plays any more: let what was written play out, then the enable off. Commands that
	 * arrive meanwhile wait for the next start. */
	if (out) {
		if (out->api->stop) {
			(void)out->api->stop(out, false);
		}
	} else {
		k_sleep(K_TIMEOUT_ABS_US(t0 + (int64_t)k * BLOCK_US));
		if (level_fn) {
			level_fn((enum opensc2_haptics_path)s->path, 0, level_user);
		}
	}
	if (!s->amp_held) {
		amp(s, out, false);
	}
}

static void seq_thread(void *p1, void *p2, void *p3)
{
	struct seq *s = &seqs[POINTER_TO_UINT(p1)];

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	for (;;) {
		const struct opensc2_haptics_output *out;
		opensc2_haptics_level_fn level_fn;
		void *level_user;
		int8_t mode;
		bool busy;

		/* Asleep until a command arrives. */
		(void)k_sem_take(s->wake, K_FOREVER);
		lock();
		/* Every command waiting is applied now (a format, a stop, a gain: answered at once,
		 * without starting the output). */
		busy = osc2_hap_rt_idle(&rt, s->path);
		mode = osc2_hap_rt_amp(&rt, s->path);
		out = s->out;
		level_fn = s->level_fn;
		level_user = s->level_user;
		unlock();
		flush();
		amp_mode(s, out, mode);
		if (busy) {
			s->running = true;
			playback(s, out, level_fn, level_user);
			s->running = false;
		}
	}
}

K_THREAD_DEFINE(osc2_hap_pads, CONFIG_OPENSC2_HAPTICS_THREAD_STACK_SIZE, seq_thread,
		UINT_TO_POINTER(OSC2_HAP_PADS), NULL, NULL, CONFIG_OPENSC2_HAPTICS_THREAD_PRIORITY,
		0, 0);
K_THREAD_DEFINE(osc2_hap_grips, CONFIG_OPENSC2_HAPTICS_THREAD_STACK_SIZE, seq_thread,
		UINT_TO_POINTER(OSC2_HAP_GRIPS), NULL, NULL, CONFIG_OPENSC2_HAPTICS_THREAD_PRIORITY,
		0, 0);

static int haptics_init(void)
{
	osc2_hap_rt_init(&rt);
	return 0;
}

/* Before the static threads start, and before the outputs register themselves. */
SYS_INIT(haptics_init, POST_KERNEL, 99);

/* ---- the library's side --------------------------------------------------------------------- */

void osc2_haptics_outputs(uint8_t link, const struct opensc2_output *out, int n)
{
	uint8_t paths;

	if (n <= 0) {
		return;
	}
	lock();
	paths = osc2_hap_rt_feed(&rt, link, out, n,
				 opensc2_setting_get(OPENSC2_SETTING_RUMBLE_GAIN_DB), now_ms());
	schedule_locked();
	unlock();
	wake(paths);
	flush();
}

void osc2_haptics_setting(uint8_t number)
{
	uint8_t paths;

	if (number != OPENSC2_SETTINGS_ALL && number != OPENSC2_SETTING_AMPLIFIER_MODE &&
	    number != OPENSC2_SETTING_HAPTIC_GAIN_DB) {
		return;
	}
	lock();
	paths = osc2_hap_rt_settings(&rt, number,
				     opensc2_setting_get(OPENSC2_SETTING_AMPLIFIER_MODE),
				     opensc2_setting_get(OPENSC2_SETTING_HAPTIC_GAIN_DB));
	unlock();
	wake(paths);
	flush();
}

int osc2_haptics_key(uint8_t key, int16_t value)
{
	uint8_t paths;
	int err;

	lock();
	err = osc2_hap_rt_key(&rt, key, value, &paths);
	unlock();
	wake(paths);
	flush();
	return err;
}

void osc2_haptics_library(const struct opensc2_haptic *h, uint8_t n)
{
	uint8_t paths;

	if (n == 0) {
		return;
	}
	lock();
	paths = osc2_hap_rt_clicks(&rt, h, n);
	unlock();
	wake(paths);
	flush();
}

uint8_t osc2_haptics_test_op(uint8_t op, const uint8_t *args, uint8_t alen, uint8_t *result,
			     uint8_t *rlen)
{
	struct osc2_hap_rt_glue glue = {
		.amp_mode = (uint8_t)opensc2_setting_get(OPENSC2_SETTING_AMPLIFIER_MODE),
		.master_db = opensc2_setting_get(OPENSC2_SETTING_HAPTIC_GAIN_DB),
		.features = IS_ENABLED(CONFIG_OPENSC2_HAPTICS_DROP_SOUNDS) ? 1u : 0u,
	};
	uint8_t status, paths = 0;
	bool scripts = false;

	lock();
	glue.outputs = (uint8_t)((seqs[0].out || seqs[0].level_fn ? 1u : 0u) |
				 (seqs[1].out || seqs[1].level_fn ? 2u : 0u));
	glue.running = (uint8_t)((seqs[0].running ? 1u : 0u) | (seqs[1].running ? 2u : 0u));
	status = osc2_hap_rt_test_op(&rt, op, args, alen, now_ms(), &glue, result, rlen, &paths,
				     &scripts);
	schedule_locked();
	unlock();
	wake(paths);
	flush();
	return status;
}

/* ---- the public API --------------------------------------------------------------------------- */

int opensc2_haptics_set_output(enum opensc2_haptics_path path,
			       const struct opensc2_haptics_output *out)
{
	if ((unsigned int)path >= OSC2_HAP_PATHS || (out && (!out->api || !out->api->write))) {
		return -EINVAL;
	}
	lock();
	seqs[path].out = out;
	unlock();
	return 0;
}

int opensc2_haptics_set_level_output(enum opensc2_haptics_path path, opensc2_haptics_level_fn fn,
				     void *user_data)
{
	if ((unsigned int)path >= OSC2_HAP_PATHS) {
		return -EINVAL;
	}
	lock();
	seqs[path].level_fn = fn;
	seqs[path].level_user = user_data;
	unlock();
	return 0;
}

int opensc2_haptics_play_cue(uint8_t cue, uint8_t level)
{
	int started;

	lock();
	started = osc2_hap_rt_cue(&rt, cue, level, now_ms());
	schedule_locked();
	unlock();
	flush();
	if (started < 0) {
		return -ENODATA;
	}
	return started ? 0 : -EBUSY;
}

int opensc2_haptics_play_script(uint8_t channels, uint8_t id, int8_t gain_db)
{
	uint8_t started;

	if (id < OSC2_HAP_SCRIPT_FIRST || id > OSC2_HAP_SCRIPT_LAST) {
		return -EINVAL;
	}
	lock();
	started = osc2_hap_rt_script(&rt, channels & 0x0Fu, id, gain_db, now_ms());
	schedule_locked();
	unlock();
	flush();
	return started;
}

static int queue_each(uint8_t channels, struct osc2_hap_cmd *cmd)
{
	uint8_t paths = 0;
	int queued = 0;

	lock();
	for (uint8_t c = 0; c < OSC2_HAP_CHANNELS; c++) {
		if (channels & (1u << c)) {
			cmd->channel = c;
			if (osc2_hap_rt_queue(&rt, cmd)) {
				queued++;
				paths |= (uint8_t)(1u << osc2_hap_path_of(c));
			}
		}
	}
	unlock();
	wake(paths);
	flush();
	return queued;
}

int opensc2_haptics_click(uint8_t channels, uint8_t click, int8_t gain_db)
{
	struct osc2_hap_cmd cmd = { .op = OSC2_HAP_OP_START, .gain_db = gain_db };

	if (click != OPENSC2_HAPTIC_CMD_CLICK_A && click != OPENSC2_HAPTIC_CMD_CLICK_B) {
		return -EINVAL;
	}
	cmd.type = click == OPENSC2_HAPTIC_CMD_CLICK_A ? OSC2_HAP_GEN_CLICK_A : OSC2_HAP_GEN_CLICK_B;
	return queue_each(channels, &cmd);
}

int opensc2_haptics_stop(uint8_t channels)
{
	struct osc2_hap_cmd cmd = { .op = OSC2_HAP_OP_STOP, .type = OSC2_HAP_GEN_ALL };

	return queue_each(channels, &cmd);
}

int opensc2_haptics_play_clip(uint8_t channel, const uint8_t *data, uint16_t samples,
			      uint8_t format, int8_t gain_db)
{
	struct osc2_hap_cmd cmd = {
		.op = OSC2_HAP_OP_START, .type = OSC2_HAP_GEN_CLIP, .gain_db = gain_db,
		.format = format,
	};

	if (channel >= OSC2_HAP_CHANNELS || !data || samples == 0 || format >= 12u) {
		return -EINVAL;
	}
	cmd.clip.data = data;
	cmd.clip.samples = samples;
	return queue_each((uint8_t)(1u << channel), &cmd) == 1 ? 0 : -ENOMEM;
}

#if defined(CONFIG_OPENSC2_HAPTICS_DROP_SOUNDS)
/* A fall noted from any context (an IMU's interrupt too): its time, taken to the logic under the
 * lock from the system work queue. The drop sound comes 250 ms after the fall's time anyway. */
static atomic_t fall_time;
static atomic_t fall_waiting;

static void fall_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (!atomic_cas(&fall_waiting, 1, 0)) {
		return;
	}
	lock();
	(void)osc2_hap_rt_fall(&rt, (uint32_t)atomic_get(&fall_time));
	schedule_locked();
	unlock();
	flush();
}
static K_WORK_DEFINE(fall_work, fall_fn);
#endif

void opensc2_haptics_free_fall(void)
{
#if defined(CONFIG_OPENSC2_HAPTICS_DROP_SOUNDS)
	/* No mutex here: it may be an interrupt. */
	atomic_set(&fall_time, (atomic_val_t)now_ms());
	atomic_set(&fall_waiting, 1);
	(void)k_work_submit(&fall_work);
#endif
}

void opensc2_haptics_imu_stopped(void)
{
	lock();
	(void)osc2_hap_rt_imu_stopped(&rt);
	schedule_locked();
	unlock();
	flush();
}

int opensc2_haptics_set_click(uint8_t click, const struct opensc2_haptics_sound *sound)
{
	if ((click != OPENSC2_HAPTIC_CMD_CLICK_A && click != OPENSC2_HAPTIC_CMD_CLICK_B) ||
	    (sound && (!sound->data || sound->format >= 12u))) {
		return -EINVAL;
	}
	lock();
	osc2_hap_set_click(&rt.engine, click == OPENSC2_HAPTIC_CMD_CLICK_B ? 1u : 0u, sound);
	unlock();
	return 0;
}

int opensc2_haptics_set_script(uint8_t id, const struct opensc2_haptics_step *steps,
			       uint8_t count)
{
	int err;

	lock();
	err = osc2_hap_script_set(id, steps, count);
	unlock();
	return err;
}

int opensc2_haptics_info(struct opensc2_haptics_info *info)
{
	memset(info, 0, sizeof(*info));
	lock();
	for (uint8_t p = 0; p < OSC2_HAP_PATHS; p++) {
		info->busy[p] = osc2_hap_busy(&rt.engine, p);
		info->output[p] = seqs[p].out != NULL || seqs[p].level_fn != NULL;
		info->blocks[p] = rt.engine.seq[p].blocks;
		info->underruns[p] = rt.underruns[p];
		info->lost += rt.engine.seq[p].lost;
	}
	for (uint8_t c = 0; c < OSC2_HAP_CHANNELS; c++) {
		info->stream_fill[c] = osc2_hap_stream_fill(&rt.engine, c);
		info->scripts |= rt.player[c].busy ? (uint8_t)(1u << c) : 0u;
	}
	unlock();
	return 0;
}
