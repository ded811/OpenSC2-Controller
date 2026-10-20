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
 *   - the master gain (setting 76) before every block, the amplifier mode (setting 75), the rumble
 *     gain offset (setting 27) as each rumble report arrives.
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

#define IDLE_POLL_MS 100     /* an idle sequencer looks at setting 75 this often */
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
	bool amp_on;
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
	rt.engine.master_db = opensc2_setting_get(OPENSC2_SETTING_HAPTIC_GAIN_DB);
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

static bool amp_held(void)
{
	return opensc2_setting_get(OPENSC2_SETTING_AMPLIFIER_MODE) == 1;
}

static void amp(struct seq *s, const struct opensc2_haptics_output *out, bool on)
{
	if (s->amp_on != on) {
		s->amp_on = on;
		if (out && out->api->enable) {
			out->api->enable(out, on);
		}
	}
}

static void playback(struct seq *s, const struct opensc2_haptics_output *out,
		     opensc2_haptics_level_fn level_fn, void *level_user)
{
	size_t frames = s->path == OSC2_HAP_PADS ? OPENSC2_HAPTICS_PAD_FRAMES :
			OPENSC2_HAPTICS_GRIP_FRAMES;
	int64_t t0 = 0;
	uint32_t k = 0;
	bool busy = true;

	/* The amplifier comes on with the first start (setting 75 = 1 already holds it). */
	amp(s, out, true);
	while (busy) {
		bool first = k == 0;

		lock();
		(void)osc2_hap_rt_block(&rt, s->path, first, s->block, &busy);
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
			if (first && out->api->start) {
				(void)out->api->start(out);
			}
		} else {
			if (first) {
				t0 = (int64_t)k_ticks_to_us_floor64((uint64_t)k_uptime_ticks());
			} else if (k >= 2) {
				/* No output: take blocks as one with two buffers would, one every
				 * 8 ms. */
				k_sleep(K_TIMEOUT_ABS_US(t0 + (int64_t)(k - 1) * BLOCK_US));
			}
			if (level_fn) {
				level_fn((enum opensc2_haptics_path)s->path,
					 opensc2_haptics_block_level((enum opensc2_haptics_path)s->path,
								     s->block, frames),
					 level_user);
			}
		}
		k++;
	}
	/* Nothing plays any more: let what was written play out, then the amplifier off. */
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
	if (!amp_held()) {
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
		bool busy;

		(void)k_sem_take(s->wake, K_MSEC(IDLE_POLL_MS));
		lock();
		/* Commands that arrive while idle (a format, a stop) are answered now. */
		busy = osc2_hap_rt_idle(&rt, s->path);
		out = s->out;
		level_fn = s->level_fn;
		level_user = s->level_user;
		unlock();
		flush();
		/* Setting 75 = 1 holds the amplifier on between sounds; 0 lets it follow playback. */
		amp(s, out, amp_held());
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

	ARG_UNUSED(link);
	if (n <= 0) {
		return;
	}
	lock();
	paths = osc2_hap_rt_feed(&rt, out, n, opensc2_setting_get(OPENSC2_SETTING_RUMBLE_GAIN_DB),
				 now_ms());
	schedule_locked();
	unlock();
	wake(paths);
	flush();
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
