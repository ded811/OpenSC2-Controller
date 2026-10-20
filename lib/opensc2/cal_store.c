/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * cal_store.c: the library's calibration (CONFIG_OPENSC2_CAL), as the real controller keeps it
 * (the protocol documentation, not public yet; docs/protocol.md lists the pages):
 *   - the cal/ keys in RAM, loaded at boot from "opensc2/cal/..." in Zephyr settings (cal_flash.c),
 *     read with 0xED and changed with 0xEE, 0xEF and 0xF0; a factory reset (0x86) keeps them, only
 *     0xFE with its key erases them, and the RAM copies stay in use until the next boot;
 *   - the calibration commands: the recordings 0xD8 (sticks), 0xC0 (triggers) and 0xC3 (pad
 *     pressure), 0xE2 (save one side's RAM values) and 0xFE (erase);
 *   - the raw inputs (opensc2_set_raw() and friends), processed at every state report with the
 *     stored calibration and settings 30, 31, 34, 35, 46, 52, 53, 60 and 62, which also feed the
 *     recordings;
 *   - after the test hooks' injection, the pad rules: the rotation (settings 2 and 24) and the
 *     touch stabiliser (setting 80 = 2), with CONFIG_OPENSC2_PAD_RULES.
 * The arithmetic is in cal.c and sensor_helpers.c; flash goes through cal_flash.c, from the system work
 * queue except for 0xEF and 0xF0, which wait for that work and then write before they answer, as
 * the bond keys do.
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <opensc2/sensor_helpers.h>
#include "internal.h"
#include "cal_store.h"
#include "test_hooks.h"

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

#if defined(CONFIG_OPENSC2_CAL)

#define KEY_ERASE_CAL  0xC6674885u   /* 0xFE */

#if defined(CONFIG_OPENSC2_CAL_STICK_DEAD_ZONE)
#define STICK_DEAD_ZONE CONFIG_OPENSC2_CAL_STICK_DEAD_ZONE
#else
#define STICK_DEAD_ZONE 0
#endif
#if defined(CONFIG_OPENSC2_CAL_STICK_GROW_RANGE)
#define STICK_GROW_RANGE true
#else
#define STICK_GROW_RANGE false
#endif
#if defined(CONFIG_OPENSC2_PAD_RULES)
#define IS_PAD_RULES true
#else
#define IS_PAD_RULES false
#endif
#if defined(CONFIG_OPENSC2_CAL_PRESSURE_CLICK)
#define IS_PRESSURE_CLICK true
#else
#define IS_PRESSURE_CLICK false
#endif

/* ---- the keys -------------------------------------------------------------------------------- */

enum key_kind {
	KIND_STICK,      /* format word first: 0xED shows it only with a non-zero one */
	KIND_TRIGGER,
	KIND_PRESSURE,
	KIND_TOUCH,      /* the same rule: a format word first (the rest of its layout is unknown) */
	KIND_TRIM,       /* shown while it lies within 1..199 */
	KIND_VOLT,       /* always shown */
	KIND_BIAS,       /* never shown; 0xEF cannot save it */
};

enum key_index {
	K_JOY_L, K_JOY_R, K_TRG_L, K_TRG_R, K_PRS_L, K_PRS_R, K_TOUCH_L, K_TOUCH_R,
	K_RGBW_R, K_RGBW_G, K_RGBW_B, K_RGBW_W, K_VOLT, K_BIAS, K_COUNT,
};

struct key_info {
	const char *name;
	uint8_t size;
	uint8_t kind;
};

static const struct key_info keys[K_COUNT] = {
	[K_JOY_L] = { "cal/joy_l", OPENSC2_CAL_STICK_LEN, KIND_STICK },
	[K_JOY_R] = { "cal/joy_r", OPENSC2_CAL_STICK_LEN, KIND_STICK },
	[K_TRG_L] = { "cal/trg_l", OPENSC2_CAL_TRIGGER_LEN, KIND_TRIGGER },
	[K_TRG_R] = { "cal/trg_r", OPENSC2_CAL_TRIGGER_LEN, KIND_TRIGGER },
	[K_PRS_L] = { "cal/prs_l", OPENSC2_CAL_PRESSURE_LEN, KIND_PRESSURE },
	[K_PRS_R] = { "cal/prs_r", OPENSC2_CAL_PRESSURE_LEN, KIND_PRESSURE },
	[K_TOUCH_L] = { "cal/touch_l", OPENSC2_CAL_TOUCH_LEN, KIND_TOUCH },
	[K_TOUCH_R] = { "cal/touch_r", OPENSC2_CAL_TOUCH_LEN, KIND_TOUCH },
	[K_RGBW_R] = { "cal/rgbw_r", 1, KIND_TRIM },
	[K_RGBW_G] = { "cal/rgbw_g", 1, KIND_TRIM },
	[K_RGBW_B] = { "cal/rgbw_b", 1, KIND_TRIM },
	[K_RGBW_W] = { "cal/rgbw_w", 1, KIND_TRIM },
	[K_VOLT] = { "cal/volt_offset", 2, KIND_VOLT },
	[K_BIAS] = { "cal/sensors/gyroscope/bias", OPENSC2_CAL_GYRO_BIAS_LEN, KIND_BIAS },
};

#define KEY_MAX 18
static const uint8_t trim_default[4] = { 90, 35, 120, 80 };

/* The RAM values, guarded by `lock`. The LED trims start at their defaults; the rest at zero. */
static struct k_spinlock lock;
static uint8_t ram[K_COUNT][KEY_MAX] = {
	[K_RGBW_R] = { 90 }, [K_RGBW_G] = { 35 }, [K_RGBW_B] = { 120 }, [K_RGBW_W] = { 80 },
};
static uint32_t generation;          /* changes with every stick, trigger or pressure key */
static uint32_t pressure_loads[2];   /* changes with every load of cal/prs_l, cal/prs_r */
static uint32_t pending_save;        /* keys to write from the system work queue */
static bool pending_erase;

static int find(const uint8_t *payload, uint8_t len, uint8_t *name_len)
{
	const uint8_t *nul = memchr(payload, 0, len);

	if (!nul) {
		return -1;
	}
	*name_len = (uint8_t)(nul - payload) + 1;
	for (int i = 0; i < K_COUNT; i++) {
		if (strcmp((const char *)payload, keys[i].name) == 0) {
			return i;
		}
	}
	return -1;
}

static bool analog(int k)
{
	return keys[k].kind == KIND_STICK || keys[k].kind == KIND_TRIGGER ||
	       keys[k].kind == KIND_PRESSURE;
}

/* The key has a value to show or save: a non-zero format word for the analog and touch keys. */
static bool has_value_locked(int k)
{
	switch (keys[k].kind) {
	case KIND_STICK:
	case KIND_TRIGGER:
	case KIND_PRESSURE:
	case KIND_TOUCH:
		return opensc2_get_le16(ram[k]) != 0;
	case KIND_TRIM:
		return ram[k][0] >= 1 && ram[k][0] <= 199;
	case KIND_BIAS:
		return false;
	default:
		return true;
	}
}

static void set_ram(int k, const uint8_t *value)
{
	k_spinlock_key_t lk = k_spin_lock(&lock);

	memcpy(ram[k], value, keys[k].size);
	if (analog(k)) {
		generation++;
	}
	if (k == K_PRS_L || k == K_PRS_R) {
		pressure_loads[k - K_PRS_L]++;
	}
	k_spin_unlock(&lock, lk);
}

static void clear_ram(int k)
{
	static const uint8_t zeros[KEY_MAX];

	set_ram(k, zeros);
}

/* A save or the erase, from the system work queue. */
static void defer_save(uint32_t mask)
{
	k_spinlock_key_t lk = k_spin_lock(&lock);

	pending_save |= mask;
	k_spin_unlock(&lock, lk);
	osc2_cal_defer();
}

void osc2_cal_flush(void)
{
	uint8_t value[KEY_MAX];
	uint32_t save;
	bool erase;
	k_spinlock_key_t lk = k_spin_lock(&lock);

	save = pending_save;
	erase = pending_erase;
	pending_save = 0;
	pending_erase = false;
	k_spin_unlock(&lock, lk);

	/* The erase first: a save still to do was asked for after it (0xFE drops the earlier ones). */
	if (erase) {
		LOG_INF("0xFE: deleting every cal/ key from flash");
		for (int k = 0; k < K_COUNT; k++) {
			(void)osc2_cal_flash_delete(keys[k].name);
		}
	}
	for (int k = 0; k < K_COUNT; k++) {
		bool have;

		if (!(save & BIT(k))) {
			continue;
		}
		lk = k_spin_lock(&lock);
		have = has_value_locked(k);
		memcpy(value, ram[k], keys[k].size);
		k_spin_unlock(&lock, lk);
		if (have && osc2_cal_flash_write(keys[k].name, value, keys[k].size) != 0) {
			LOG_WRN("Could not save %s", keys[k].name);
		}
	}
}

int osc2_cal_load(const char *key, const void *value, size_t len)
{
	for (int k = 0; k < K_COUNT; k++) {
		/* the stored name is the key's own without "cal/" (under opensc2/cal/) */
		if (strcmp(key, keys[k].name + 4) == 0) {
			if (len != keys[k].size) {
				return -EINVAL;   /* another length: the built-in value stays */
			}
			set_ram(k, value);
			return 0;
		}
	}
	return -ENOENT;
}

/* ---- by name --------------------------------------------------------------------------------- */

int osc2_cal_read_by_name(const uint8_t *payload, uint8_t len, uint8_t *out, uint8_t *out_len)
{
	uint8_t name_len;
	int k = find(payload, len, &name_len);
	k_spinlock_key_t lk;

	if (k < 0) {
		return -ENOENT;
	}
	lk = k_spin_lock(&lock);
	if (has_value_locked(k)) {
		memcpy(out, ram[k], keys[k].size);
		*out_len = keys[k].size;
	} else {
		*out_len = 0;
	}
	k_spin_unlock(&lock, lk);
	return 0;
}

int osc2_cal_by_name(uint8_t opcode, const uint8_t *payload, uint8_t len)
{
	uint8_t name_len = 0, value[KEY_MAX];
	int k = find(payload, len, &name_len);
	bool have;
	k_spinlock_key_t lk;

	if (k < 0) {
		return -ENOENT;
	}
	if (opcode == 0xEE) {
		/* RAM only, the exact size; a stick, trigger or pressure key applies at the next
		 * frame. The gyro bias is used once at boot. */
		if (len - name_len != keys[k].size) {
			return -EINVAL;
		}
		set_ram(k, payload + name_len);
		return 0;
	}
	if (name_len != len) {
		return -EINVAL;   /* the NUL must be the last payload byte */
	}
	if (opcode == 0xEF) {
		if (keys[k].kind == KIND_BIAS) {
			return -ENOTSUP;   /* no read function: the real controller fails it */
		}
		/* The real controller saves what the key's read function gives: whatever 0xED would
		 * show (an analog or touch key with a format word, a trim within 1..199). */
		lk = k_spin_lock(&lock);
		have = has_value_locked(k);
		memcpy(value, ram[k], keys[k].size);
		k_spin_unlock(&lock, lk);
		if (!have) {
			return -ENOENT;   /* nothing in RAM that could be read back: nothing saved */
		}
		/* After the deferred writes asked for before it (an 0xFE's erase above all), so that
		 * an "erase, then store the new calibration" keeps the new keys. The real controller
		 * does not wait: there an erase still running can delete a key saved after it
		 * (docs/protocol.md, gap 31). */
		osc2_cal_flush_wait();
		return osc2_cal_flash_write(keys[k].name, value, keys[k].size);
	}
	if (opcode == 0xF0) {
		clear_ram(k);   /* zeros, at once */
		osc2_cal_flush_wait();   /* in order with the deferred writes, as for 0xEF */
		return osc2_cal_flash_delete(keys[k].name);
	}
	return -ENOTSUP;
}

/* ---- the raw inputs and their processing ------------------------------------------------------ */

static struct k_spinlock raw_lock;
static struct opensc2_raw_inputs raw;

/* The factory recordings: their commands come from the links' threads, their samples from the
 * frames. */
static struct k_spinlock rec_lock;
static struct opensc2_cal_rec rec;

static struct {
	uint32_t applied;      /* the generation the set-ups come from */
	uint32_t pressure_applied[2];   /* the pressure key loads the pads' set-ups come from */
	bool set_up;
	struct opensc2_stick_proc stick[2];
	struct opensc2_trigger_proc trigger[2];
	struct opensc2_pressure_proc pressure[2];
	struct opensc2_cap_touch stick_touch[2], grip[2];
	int16_t pad_settings[10];   /* a change of any starts the pads over */
	uint8_t raw_seen;      /* the raw inputs of the last frame */
	struct opensc2_stabiliser stab[2];
} proc;

static void set_raw_locked(const struct opensc2_raw_inputs *in)
{
	for (int side = 0; side < 2; side++) {
		if (in->present & (OPENSC2_RAW_STICK_L << side)) {
			raw.stick[side][0] = in->stick[side][0];
			raw.stick[side][1] = in->stick[side][1];
		}
		if (in->present & (OPENSC2_RAW_TRIGGER_L << side)) {
			raw.trigger[side] = in->trigger[side];
		}
		if (in->present & (OPENSC2_RAW_PRESSURE_L << side)) {
			raw.pressure[side] = in->pressure[side];
		}
		if (in->present & (OPENSC2_RAW_TOUCH_L << side)) {
			raw.stick_touch[side] = in->stick_touch[side];
			raw.grip[side] = in->grip[side];
		}
	}
	raw.present |= in->present;
}

int opensc2_set_raw(const struct opensc2_raw_inputs *in)
{
	k_spinlock_key_t lk;

	if (!in) {
		return -EINVAL;
	}
	lk = k_spin_lock(&raw_lock);
	set_raw_locked(in);
	k_spin_unlock(&raw_lock, lk);
	return 0;
}

int opensc2_set_stick_raw(enum opensc2_side side, uint16_t x_counts, uint16_t y_counts)
{
	struct opensc2_raw_inputs in = { 0 };

	if (side > OPENSC2_RIGHT) {
		return -EINVAL;
	}
	in.present = (uint8_t)(OPENSC2_RAW_STICK_L << side);
	in.stick[side][0] = x_counts;
	in.stick[side][1] = y_counts;
	return opensc2_set_raw(&in);
}

int opensc2_set_trigger_raw(enum opensc2_side side, uint16_t counts)
{
	struct opensc2_raw_inputs in = { 0 };

	if (side > OPENSC2_RIGHT) {
		return -EINVAL;
	}
	in.present = (uint8_t)(OPENSC2_RAW_TRIGGER_L << side);
	in.trigger[side] = counts;
	return opensc2_set_raw(&in);
}

int opensc2_set_pressure_raw(enum opensc2_side side, int16_t value)
{
	struct opensc2_raw_inputs in = { 0 };

	if (side > OPENSC2_RIGHT) {
		return -EINVAL;
	}
	in.present = (uint8_t)(OPENSC2_RAW_PRESSURE_L << side);
	in.pressure[side] = value;
	return opensc2_set_raw(&in);
}

int opensc2_set_touch_raw(enum opensc2_side side, int16_t stick_word, int16_t grip_word)
{
	struct opensc2_raw_inputs in = { 0 };

	if (side > OPENSC2_RIGHT) {
		return -EINVAL;
	}
	in.present = (uint8_t)(OPENSC2_RAW_TOUCH_L << side);
	in.stick_touch[side] = stick_word;
	in.grip[side] = grip_word;
	return opensc2_set_raw(&in);
}

int opensc2_cal_raw_release(uint8_t inputs)
{
	k_spinlock_key_t lk = k_spin_lock(&raw_lock);

	raw.present &= (uint8_t)~inputs;
	k_spin_unlock(&raw_lock, lk);
	return 0;
}

/* A pad starts over: the two-word means and the touch states of its stick touch and grip, its
 * click, its stabiliser. The pressure filter is kept. */
static void pad_start_over(int side)
{
	struct opensc2_pressure_proc *p = &proc.pressure[side];

	opensc2_cap_reset(&proc.stick_touch[side]);
	opensc2_cap_reset(&proc.grip[side]);
	opensc2_stabiliser_reset(&proc.stab[side]);
	p->click = false;
	p->click_setting = INT16_MIN;
}

/* Set the processing up again from the RAM keys when they changed: both sticks and both triggers
 * after any of their keys, a pad (and its start-over) only after a load of its own pressure key. */
static void apply_calibration(void)
{
	uint8_t joy[2][OPENSC2_CAL_STICK_LEN], trg[2][OPENSC2_CAL_TRIGGER_LEN];
	uint8_t prs[2][OPENSC2_CAL_PRESSURE_LEN];
	uint32_t gen, loads[2];
	k_spinlock_key_t lk = k_spin_lock(&lock);

	gen = generation;
	if (proc.set_up && gen == proc.applied) {
		k_spin_unlock(&lock, lk);
		return;
	}
	for (int side = 0; side < 2; side++) {
		memcpy(joy[side], ram[K_JOY_L + side], sizeof(joy[side]));
		memcpy(trg[side], ram[K_TRG_L + side], sizeof(trg[side]));
		memcpy(prs[side], ram[K_PRS_L + side], sizeof(prs[side]));
		loads[side] = pressure_loads[side];
	}
	k_spin_unlock(&lock, lk);

	for (int side = 0; side < 2; side++) {
		struct opensc2_cal_stick s;
		struct opensc2_cal_trigger t;
		struct opensc2_cal_pressure p;

		opensc2_cal_stick_unpack(joy[side], &s);
		opensc2_cal_trigger_unpack(trg[side], &t);
		proc.stick[side].dead_zone = STICK_DEAD_ZONE;
		proc.stick[side].grow_range = STICK_GROW_RANGE;
		opensc2_stick_setup(&proc.stick[side], &s);
		opensc2_trigger_setup(&proc.trigger[side], &t);
		if (proc.set_up && loads[side] == proc.pressure_applied[side]) {
			continue;   /* another key changed: this pad goes on as it was */
		}
		opensc2_cal_pressure_unpack(prs[side], &p);
		opensc2_pressure_setup(&proc.pressure[side], &p);   /* the filter is kept */
		pad_start_over(side);
		proc.pressure_applied[side] = loads[side];
	}
	proc.applied = gen;
	proc.set_up = true;
}

/* The settings whose change makes both pads start over. */
static void pads_start_over_on_change(void)
{
	static const uint8_t numbers[10] = { 30, 31, 33, 34, 35, 52, 53, 66, 72, 73 };
	bool changed = false;

	for (int i = 0; i < 10; i++) {
		int16_t v = opensc2_setting_get(numbers[i]);

		changed |= v != proc.pad_settings[i];
		proc.pad_settings[i] = v;
	}
	if (!changed) {
		return;
	}
	for (int side = 0; side < 2; side++) {
		pad_start_over(side);
	}
}

static void set_bit(struct opensc2_state *s, uint32_t bit, bool on)
{
	s->buttons = on ? (s->buttons | bit) : (s->buttons & ~bit);
}

/* A recording finished: its records (`r`, a copy) become the RAM keys (applied from the next
 * frame) and are saved from the system work queue. */
static void take_records(uint32_t bits, const struct opensc2_cal_rec *r)
{
	uint8_t buf[KEY_MAX];
	uint32_t save = 0;

	for (int side = 0; side < 2; side++) {
		if (bits & (OPENSC2_CAL_SAVE_STICK_L << side)) {
			struct opensc2_cal_stick c;

			opensc2_cal_rec_stick(r, (enum opensc2_side)side, &c);
			opensc2_cal_stick_pack(&c, buf);
			set_ram(K_JOY_L + side, buf);
			save |= BIT(K_JOY_L + side);
		}
		if (bits & (OPENSC2_CAL_SAVE_TRIGGER_L << side)) {
			struct opensc2_cal_trigger c;

			opensc2_cal_rec_trigger(r, (enum opensc2_side)side, &c);
			opensc2_cal_trigger_pack(&c, buf);
			set_ram(K_TRG_L + side, buf);
			save |= BIT(K_TRG_L + side);
		}
		if (bits & (OPENSC2_CAL_SAVE_PRESSURE_L << side)) {
			struct opensc2_cal_pressure c;

			opensc2_cal_rec_pressure(r, (enum opensc2_side)side, &c);
			opensc2_cal_pressure_pack(&c, buf);
			set_ram(K_PRS_L + side, buf);
			save |= BIT(K_PRS_L + side);
		}
	}
	if (save) {
		LOG_INF("Calibration recorded (keys 0x%04x)", (unsigned int)save);
		defer_save(save);
	}
}

void osc2_cal_frame(struct opensc2_state *s)
{
	static const uint32_t stick_touch_bit[2] = { OPENSC2_TOUCH_LEFT_STICK,
						     OPENSC2_TOUCH_RIGHT_STICK };
	static const uint32_t grip_bit[2] = { OPENSC2_TOUCH_LEFT_GRIP, OPENSC2_TOUCH_RIGHT_GRIP };
	static const uint32_t pad_touch_bit[2] = { OPENSC2_TOUCH_LEFT_PAD, OPENSC2_TOUCH_RIGHT_PAD };
#if defined(CONFIG_OPENSC2_CAL_PRESSURE_CLICK)
	static const uint32_t pad_click_bit[2] = { OPENSC2_CLICK_LEFT_PAD, OPENSC2_CLICK_RIGHT_PAD };
#endif
	struct opensc2_raw_inputs in;
	struct opensc2_cal_rec done;
	uint16_t stick_mv[2][2] = { { 0 } }, trigger_mv[2] = { 0 };
	int16_t raw46, raw62, raw60;
	uint32_t bits, nonce;
	k_spinlock_key_t lk = k_spin_lock(&raw_lock);

	in = raw;
	k_spin_unlock(&raw_lock, lk);
	/* The hardware test hooks' raw injection replaces the application's raw inputs; its frames
	 * carry the test's nonce in the IMU timestamp. */
	nonce = osc2_test_raw_inputs(&in);
	if (nonce) {
		s->imu.timestamp_us = nonce;
	}
	proc.raw_seen = in.present;

	apply_calibration();
	pads_start_over_on_change();
	raw46 = opensc2_setting_get(OPENSC2_SETTING_RAW_STICKS);
	raw62 = opensc2_setting_get(OPENSC2_SETTING_RAW_TRIGGERS);
	raw60 = opensc2_setting_get(OPENSC2_SETTING_RAW_PRESSURE);

	for (int side = 0; side < 2; side++) {
		if (in.present & (OPENSC2_RAW_STICK_L << side)) {
			uint16_t x = in.stick[side][0], y = in.stick[side][1], vx, vy;

			stick_mv[side][0] = opensc2_cal_counts_to_mv(x, OPENSC2_CAL_STICK_FULL_MV);
			stick_mv[side][1] = opensc2_cal_counts_to_mv(y, OPENSC2_CAL_STICK_FULL_MV);
			if (opensc2_cal_raw_mode(raw46, x, OPENSC2_CAL_STICK_FULL_MV, &vx)) {
				(void)opensc2_cal_raw_mode(raw46, y, OPENSC2_CAL_STICK_FULL_MV, &vy);
				s->stick[side].x = (int16_t)vx;   /* millivolts or counts, never negative */
				s->stick[side].y = (int16_t)vy;
			} else {
				opensc2_stick_value(&proc.stick[side], x, y, &s->stick[side]);
			}
		}
		if (in.present & (OPENSC2_RAW_TRIGGER_L << side)) {
			uint16_t t = in.trigger[side], v;

			trigger_mv[side] = opensc2_cal_counts_to_mv(t, OPENSC2_CAL_TRIGGER_FULL_MV);
			if (opensc2_cal_raw_mode(raw62, t, OPENSC2_CAL_TRIGGER_FULL_MV, &v)) {
				s->trigger[side] = v;
			} else {
				s->trigger[side] = opensc2_trigger_value(&proc.trigger[side], t);
			}
		}
		if (in.present & (OPENSC2_RAW_PRESSURE_L << side)) {
			struct opensc2_pressure_proc *p = &proc.pressure[side];
			bool touched = s->pad[side].touched || (s->buttons & pad_touch_bit[side]);

			opensc2_pressure_sample(p, in.pressure[side],
						opensc2_setting_get(side ? OPENSC2_SETTING_RIGHT_CLICK_PRESSURE :
							OPENSC2_SETTING_LEFT_CLICK_PRESSURE));
			if (raw60 == 1) {
				/* the raw word, even while the pad is not touched */
				s->pad[side].pressure = opensc2_pressure_raw_value(in.pressure[side]);
			} else {
				s->pad[side].pressure = touched ? opensc2_pressure_value(p) : 0;
			}
#if defined(CONFIG_OPENSC2_CAL_PRESSURE_CLICK)
			/* The click comes from the pressure, and only while touched. */
			s->pad[side].clicked = touched && p->click;
			set_bit(s, pad_click_bit[side], s->pad[side].clicked);
#endif
		}
		if (in.present & (OPENSC2_RAW_TOUCH_L << side)) {
			set_bit(s, stick_touch_bit[side],
				opensc2_cap_sample(&proc.stick_touch[side], in.stick_touch[side],
						   opensc2_stick_touch_on_level(opensc2_setting_get(
							   OPENSC2_SETTING_STICK_TOUCH_ON)),
						   opensc2_stick_touch_off_level(opensc2_setting_get(
							   OPENSC2_SETTING_STICK_TOUCH_OFF))));
			set_bit(s, grip_bit[side],
				opensc2_cap_sample(&proc.grip[side], in.grip[side],
						   opensc2_grip_on_level(opensc2_setting_get(
							   OPENSC2_SETTING_GRIP_SENSITIVITY)),
						   opensc2_grip_off_level(
							   opensc2_setting_get(OPENSC2_SETTING_GRIP_SENSITIVITY),
							   opensc2_setting_get(OPENSC2_SETTING_GRIP_RELEASE))));
		}
	}

	/* The factory recordings take this frame's samples. */
	lk = k_spin_lock(&rec_lock);
	bits = opensc2_cal_rec_frame(&rec, stick_mv, (uint8_t)((in.present / OPENSC2_RAW_STICK_L) & 3),
				     trigger_mv, (uint8_t)((in.present / OPENSC2_RAW_TRIGGER_L) & 3),
				     in.pressure, (uint8_t)((in.present / OPENSC2_RAW_PRESSURE_L) & 3));
	if (bits) {
		done = rec;
	}
	k_spin_unlock(&rec_lock, lk);
	if (bits) {
		take_records(bits, &done);
	}
}

/* ---- the pad rules, after the injection -------------------------------------------------------- */

void osc2_cal_rules(struct opensc2_state *s)
{
#if defined(CONFIG_OPENSC2_PAD_RULES)
	static const uint32_t pad_touch_bit[2] = { OPENSC2_TOUCH_LEFT_PAD, OPENSC2_TOUCH_RIGHT_PAD };
	int16_t angle = opensc2_setting_get(OPENSC2_SETTING_PAD_ROTATION_ANGLE);
	int16_t rotate = opensc2_setting_get(OPENSC2_SETTING_PAD_ROTATION);
	bool stabilise = opensc2_setting_get(OPENSC2_SETTING_PAD_STABILIZER) == 2;

	for (int side = 0; side < 2; side++) {
		struct opensc2_trackpad *pad = &s->pad[side];
		/* The real controller turns and stabilises the pad scale's values before it doubles
		 * them and turns Y up: undo both, apply, redo them. */
		int32_t x = pad->x / 2, y = -(int32_t)pad->y / 2;
		bool touched = pad->touched || (s->buttons & pad_touch_bit[side]);
		bool rotated;

		if (!touched || !stabilise) {
			opensc2_stabiliser_reset(&proc.stab[side]);   /* a new touch starts it over */
		}
		if (!touched) {
			continue;
		}
		rotated = opensc2_pad_rotate((enum opensc2_side)side, angle, rotate, &x, &y);
		if (!rotated && !stabilise) {
			continue;   /* the application's values exactly */
		}
		x = CLAMP(2 * x, -OPENSC2_AXIS_MAX, OPENSC2_AXIS_MAX);
		y = CLAMP(2 * y, -OPENSC2_AXIS_MAX, OPENSC2_AXIS_MAX);
		if (stabilise) {
			opensc2_stabiliser_step(&proc.stab[side], OPENSC2_STABILISER_FINE, &x, &y);
		}
		pad->x = (int16_t)x;
		pad->y = (int16_t)-y;
	}
#else
	ARG_UNUSED(s);
#endif
}

/* ---- the calibration commands ----------------------------------------------------------------- */

/* Byte `i` of the payload as the real controller reads it: whatever the length says, within the
 * bytes that exist. */
static uint8_t byte_at(const uint8_t *payload, size_t avail, size_t i)
{
	return i < avail ? payload[i] : 0;
}

/* A recording command; the pressure's step 3 saves at once. */
static void rec_step(enum opensc2_cal_rec_kind kind, uint8_t step)
{
	struct opensc2_cal_rec done;
	k_spinlock_key_t lk = k_spin_lock(&rec_lock);
	uint32_t bits = opensc2_cal_rec_step(&rec, kind, step);

	if (bits) {
		done = rec;
	}
	k_spin_unlock(&rec_lock, lk);
	if (bits) {
		take_records(bits, &done);
	}
}

int osc2_cal_command(uint8_t opcode, const uint8_t *payload, uint8_t len, size_t avail)
{
	ARG_UNUSED(len);
	switch (opcode) {
	case 0xD8:   /* sticks: the step is the second byte */
		rec_step(OPENSC2_CAL_REC_STICKS, byte_at(payload, avail, 1));
		return 0;
	case 0xC0:   /* triggers */
		LOG_INF("0xC0: trigger recording, step %u", byte_at(payload, avail, 0));
		rec_step(OPENSC2_CAL_REC_TRIGGERS, byte_at(payload, avail, 0));
		return 0;
	case 0xC3:   /* pad pressure: step 3 saves at once */
		rec_step(OPENSC2_CAL_REC_PRESSURE, byte_at(payload, avail, 0));
		return 0;
	case 0xE2: { /* save one side's RAM values: mask bit 0 trigger, 1 stick, 2 pressure */
		uint8_t side = byte_at(payload, avail, 0);
		uint8_t mask = byte_at(payload, avail, 1);
		uint32_t save = 0;

		if (side > 1) {
			LOG_WRN("0xE2: side %u is neither 0 nor 1", side);
			return -EINVAL;
		}
		save |= (mask & 1) ? BIT(K_TRG_L + side) : 0;
		save |= (mask & 2) ? BIT(K_JOY_L + side) : 0;
		save |= (mask & 4) ? BIT(K_PRS_L + side) : 0;
		if (save) {
			defer_save(save);   /* only the keys that have a value are written */
		}
		return 0;
	}
	case 0xFE: { /* erase the calibration: only with its key */
		uint8_t k[4];
		k_spinlock_key_t lk;

		for (size_t i = 0; i < 4; i++) {
			k[i] = byte_at(payload, avail, i);
		}
		if (opensc2_get_le32(k) != KEY_ERASE_CAL) {
			LOG_WRN("0xFE refused: wrong key");
			return -EINVAL;
		}
		lk = k_spin_lock(&lock);
		pending_erase = true;   /* the RAM copies stay in use until the next boot */
		pending_save = 0;       /* saves asked for before it would come after it: the erase wins */
		k_spin_unlock(&lock, lk);
		osc2_cal_defer();
		return 0;
	}
	default:
		return -ENOTSUP;
	}
}

/* ---- what the library offers --------------------------------------------------------------------- */

static bool key_ok(int k)
{
	uint8_t v[KEY_MAX];
	k_spinlock_key_t lk = k_spin_lock(&lock);

	memcpy(v, ram[k], sizeof(v));
	k_spin_unlock(&lock, lk);
	switch (keys[k].kind) {
	case KIND_STICK: {
		struct opensc2_cal_stick c;
		struct opensc2_stick_proc p = { 0 };

		opensc2_cal_stick_unpack(v, &c);
		opensc2_stick_setup(&p, &c);
		return p.valid;
	}
	case KIND_TRIGGER: {
		struct opensc2_cal_trigger c;
		struct opensc2_trigger_proc p;

		opensc2_cal_trigger_unpack(v, &c);
		opensc2_trigger_setup(&p, &c);
		return p.valid;
	}
	case KIND_PRESSURE: {
		struct opensc2_cal_pressure c;
		struct opensc2_pressure_proc p = { 0 };

		opensc2_cal_pressure_unpack(v, &c);
		opensc2_pressure_setup(&p, &c);
		return p.usable;
	}
	default:
		return false;
	}
}

bool opensc2_cal_stick_ok(enum opensc2_side side)
{
	return side <= OPENSC2_RIGHT && key_ok(K_JOY_L + side);
}

bool opensc2_cal_trigger_ok(enum opensc2_side side)
{
	return side <= OPENSC2_RIGHT && key_ok(K_TRG_L + side);
}

bool opensc2_cal_pressure_ok(enum opensc2_side side)
{
	return side <= OPENSC2_RIGHT && key_ok(K_PRS_L + side);
}

bool opensc2_cal_recording(enum opensc2_cal_rec_kind kind)
{
	bool active;
	k_spinlock_key_t lk;

	if (kind > OPENSC2_CAL_REC_PRESSURE) {
		return false;
	}
	lk = k_spin_lock(&rec_lock);
	active = rec.active[kind];
	k_spin_unlock(&rec_lock, lk);
	return active;
}

uint8_t opensc2_cal_led_trim(uint8_t channel)
{
	uint8_t v;
	k_spinlock_key_t lk;

	if (channel > 3) {
		return 0;
	}
	/* Whatever byte RAM holds, as the real controller uses it: 0 leaves the channel dark (also
	 * after 0xF0, until the next boot brings the default back), above 100 it is brighter, up to
	 * full duty. Only 0xED limits what it shows to 1..199 (has_value_locked()). */
	lk = k_spin_lock(&lock);
	v = ram[K_RGBW_R + channel][0];
	k_spin_unlock(&lock, lk);
	return v;
}

int16_t opensc2_cal_volt_offset(void)
{
	int16_t v;
	k_spinlock_key_t lk = k_spin_lock(&lock);

	v = (int16_t)opensc2_get_le16(ram[K_VOLT]);
	k_spin_unlock(&lock, lk);
	return v;
}

int opensc2_cal_gyro_bias(float bias[3])
{
	uint8_t v[OPENSC2_CAL_GYRO_BIAS_LEN];
	bool any = false;
	k_spinlock_key_t lk = k_spin_lock(&lock);

	memcpy(v, ram[K_BIAS], sizeof(v));
	k_spin_unlock(&lock, lk);
	for (int i = 0; i < 3; i++) {
		uint32_t word = opensc2_get_le32(&v[4 * i]);

		memcpy(&bias[i], &word, sizeof(bias[i]));
		if (bias[i] != bias[i] || bias[i] > 1e30f || bias[i] < -1e30f) {
			return -ENOENT;   /* not a finite number: refused, as on the real controller */
		}
		any |= word != 0;
	}
	return any ? 0 : -ENOENT;
}

void osc2_cal_status(uint8_t out[16])
{
	uint8_t usable = 0;
	k_spinlock_key_t lk;

	memset(out, 0, 16);
	for (int side = 0; side < 2; side++) {
		usable |= opensc2_cal_stick_ok((enum opensc2_side)side) ? BIT(0 + side) : 0;
		usable |= opensc2_cal_trigger_ok((enum opensc2_side)side) ? BIT(2 + side) : 0;
		usable |= opensc2_cal_pressure_ok((enum opensc2_side)side) ? BIT(4 + side) : 0;
	}
	opensc2_put_le32(&out[0], opensc2_cal_generation());
	out[4] = usable;
	out[5] = proc.raw_seen;
	out[7] = (IS_PAD_RULES ? 0x01 : 0) | (IS_PRESSURE_CLICK ? 0x02 : 0);
	lk = k_spin_lock(&rec_lock);
	for (int k = 0; k < 3; k++) {
		out[8 + k] = (uint8_t)(rec.step[k] | (rec.active[k] ? 0x80 : 0));
	}
	k_spin_unlock(&rec_lock, lk);
	for (int side = 0; side < 2; side++) {
		int32_t p = proc.pressure[side].filtered;

		opensc2_put_le16(&out[11 + 2 * side], (uint16_t)(int16_t)CLAMP(p, -32768, 32767));
		out[15] |= proc.pressure[side].click ? BIT(side) : 0;
	}
}

uint32_t opensc2_cal_generation(void)
{
	uint32_t g;
	k_spinlock_key_t lk = k_spin_lock(&lock);

	g = generation;
	k_spin_unlock(&lock, lk);
	return g;
}

void osc2_cal_reset(void)
{
	k_spinlock_key_t lk = k_spin_lock(&lock);

	memset(ram, 0, sizeof(ram));
	for (int c = 0; c < 4; c++) {
		ram[K_RGBW_R + c][0] = trim_default[c];
	}
	generation++;
	pending_save = 0;
	pending_erase = false;
	k_spin_unlock(&lock, lk);
	lk = k_spin_lock(&raw_lock);
	memset(&raw, 0, sizeof(raw));
	k_spin_unlock(&raw_lock, lk);
	memset(&proc, 0, sizeof(proc));
	lk = k_spin_lock(&rec_lock);
	opensc2_cal_rec_init(&rec);
	k_spin_unlock(&rec_lock, lk);
}

#else /* !CONFIG_OPENSC2_CAL */

int opensc2_set_raw(const struct opensc2_raw_inputs *in)
{
	ARG_UNUSED(in);
	return -ENOTSUP;
}

int opensc2_set_stick_raw(enum opensc2_side side, uint16_t x_counts, uint16_t y_counts)
{
	ARG_UNUSED(side);
	ARG_UNUSED(x_counts);
	ARG_UNUSED(y_counts);
	return -ENOTSUP;
}

int opensc2_set_trigger_raw(enum opensc2_side side, uint16_t counts)
{
	ARG_UNUSED(side);
	ARG_UNUSED(counts);
	return -ENOTSUP;
}

int opensc2_set_pressure_raw(enum opensc2_side side, int16_t value)
{
	ARG_UNUSED(side);
	ARG_UNUSED(value);
	return -ENOTSUP;
}

int opensc2_set_touch_raw(enum opensc2_side side, int16_t stick_word, int16_t grip_word)
{
	ARG_UNUSED(side);
	ARG_UNUSED(stick_word);
	ARG_UNUSED(grip_word);
	return -ENOTSUP;
}

int opensc2_cal_raw_release(uint8_t inputs)
{
	ARG_UNUSED(inputs);
	return -ENOTSUP;
}

bool opensc2_cal_stick_ok(enum opensc2_side side)
{
	ARG_UNUSED(side);
	return false;
}

bool opensc2_cal_trigger_ok(enum opensc2_side side)
{
	ARG_UNUSED(side);
	return false;
}

bool opensc2_cal_pressure_ok(enum opensc2_side side)
{
	ARG_UNUSED(side);
	return false;
}

bool opensc2_cal_recording(enum opensc2_cal_rec_kind kind)
{
	ARG_UNUSED(kind);
	return false;
}

uint8_t opensc2_cal_led_trim(uint8_t channel)
{
	static const uint8_t trim_default[4] = { 90, 35, 120, 80 };

	return channel < 4 ? trim_default[channel] : 0;
}

int16_t opensc2_cal_volt_offset(void)
{
	return 0;
}

int opensc2_cal_gyro_bias(float bias[3])
{
	ARG_UNUSED(bias);
	return -ENOENT;
}

uint32_t opensc2_cal_generation(void)
{
	return 0;
}

#endif /* CONFIG_OPENSC2_CAL */
