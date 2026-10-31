/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * battery.c: <opensc2/battery.h>. The library's fuel gauge (CONFIG_OPENSC2_BATTERY_GAUGE): a
 * thread that, once a second, takes the charger's status and values from the application, keeps
 * the battery history, works out and keeps the level, and runs the charge state machine
 * (fuel_rules.c), the way the protocol documentation describes the real controller's gauge; and
 * the charger setters, the getter and the battery cut-off, which work with or without it.
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <opensc2/battery.h>
#include <opensc2/cal.h>
#include <opensc2/opensc2.h>
#include "internal.h"
#include "fuel_rules.h"
#include "key_rules.h"
#include "key_store.h"
#include "retained.h"

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

static struct k_spinlock lock;
static const struct opensc2_battery_board *board;
static uint8_t charger_status = OPENSC2_CHARGER_UNKNOWN;
static struct opensc2_charger_values charger_values;

static bool input_power(void)
{
	return opensc2_power_source() != OPENSC2_POWER_SOURCE_BATTERY;
}

void opensc2_battery_get(struct opensc2_battery *battery)
{
	k_spinlock_key_t key = k_spin_lock(&osc2.lock);

	*battery = osc2.battery;
	k_spin_unlock(&osc2.lock, key);
}

static void values_into(struct opensc2_battery *b, const struct opensc2_charger_values *v)
{
	b->system_mv = v->system_mv;
	b->input_mv = v->input_mv;
	b->current_ma = (int16_t)v->current_ma;
	b->input_current_ma = v->input_current_ma;
	b->temperature = v->temperature;
}

int opensc2_charger_set_status(enum opensc2_charger_status status)
{
	k_spinlock_key_t key;

	if (status > OPENSC2_CHARGER_DONE && status != OPENSC2_CHARGER_UNKNOWN) {
		return -EINVAL;
	}
	key = k_spin_lock(&lock);
	charger_status = (uint8_t)status;
	k_spin_unlock(&lock, key);
	if (!IS_ENABLED(CONFIG_OPENSC2_BATTERY_GAUGE) || board == NULL) {
		/* No gauge: the status is the application's battery's charge state. */
		struct opensc2_battery b;

		if (status == OPENSC2_CHARGER_UNKNOWN) {
			return 0;
		}
		opensc2_battery_get(&b);
		b.charge_state = opensc2_charge_state_of(input_power(),
							 status == OPENSC2_CHARGER_PRECHARGE ||
							 status == OPENSC2_CHARGER_CHARGING);
		if (b.charge_state == OPENSC2_CHARGE_DONE) {
			b.level = 100;
		}
		return opensc2_set_battery(&b);
	}
	return 0;
}

int opensc2_charger_set_values(const struct opensc2_charger_values *values)
{
	k_spinlock_key_t key;

	if (values == NULL) {
		return -EINVAL;
	}
	key = k_spin_lock(&lock);
	charger_values = *values;
	k_spin_unlock(&lock, key);
	if (!IS_ENABLED(CONFIG_OPENSC2_BATTERY_GAUGE) || board == NULL) {
		struct opensc2_battery b;

		opensc2_battery_get(&b);
		values_into(&b, values);
		return opensc2_set_battery(&b);
	}
	return 0;
}

#if defined(CONFIG_OPENSC2_BATTERY_GAUGE)

#define HISTORY        OPENSC2_GAUGE_HISTORY   /* 40 readings */
#define FIRST_PASS_MS  250
#define PASS_MS        1000
#define SETTLE_MS      250                     /* after charging started or stopped */

static struct osc2_fuel fuel;                  /* under `lock` */
static uint8_t scale_charging, scale_discharging;
static uint32_t window_seen;

/* The history (the gauge's thread only). */
static uint16_t ring[HISTORY];
static uint32_t count;                         /* readings since the history started over */

/* The level kept over a warm reboot: in the retained area, or (without one) in no-init RAM. */
#if !defined(CONFIG_OPENSC2_USB)
static uint32_t kept_words[2] __noinit;

bool osc2_retained_level_get(uint8_t *level)
{
	if (kept_words[0] != OSC2_RET_LEVEL_MAGIC || kept_words[1] > 0xFF) {
		return false;
	}
	*level = (uint8_t)kept_words[1];
	return true;
}

void osc2_retained_level_set(uint8_t level)
{
	kept_words[1] = level;
	kept_words[0] = OSC2_RET_LEVEL_MAGIC;
}
#endif

static void publish(void)
{
	struct opensc2_battery b = { 0 };
	struct opensc2_charger_values v;
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint8_t level;

	v = charger_values;
	b.charge_state = osc2_fuel_charge_state(&fuel);
	b.battery_mv = fuel.voltage_mv;
	b.gauge_flags = fuel.state == OSC2_FUEL_CHARGING;
	k_spin_unlock(&lock, key);
	values_into(&b, &v);
	b.level = osc2_retained_level_get(&level) ? level : OPENSC2_GAUGE_NO_LEVEL;
	(void)opensc2_set_battery(&b);
}

static void write_charger(const struct osc2_fuel_out *out)
{
	int err;

	if (out->vin_limit_mv && board->set_input_voltage_limit) {
		err = board->set_input_voltage_limit(board->user, out->vin_limit_mv);
		if (err) {
			LOG_ERR("Battery: input-voltage limit %u mV not set (%d)", out->vin_limit_mv, err);
		}
	}
	if (out->charge_mv && board->set_charge_voltage) {
		err = board->set_charge_voltage(board->user, out->charge_mv);
		if (err) {
			LOG_ERR("Battery: charge voltage %u mV not set (%d)", out->charge_mv, err);
		}
	}
}

/* The history's average: 40 fresh readings in a row after it started over, else one more. */
static int average_mv(uint16_t *avg)
{
	int16_t offset = opensc2_cal_volt_offset();
	uint32_t n, sum = 0;
	int err = 0;

	do {
		uint16_t got[HISTORY];
		size_t want = count < HISTORY ? HISTORY - count : 1;
		int r = board->read_mv(board->user, got, want);

		if (r <= 0) {
			err = r < 0 ? r : -EIO;
			break;
		}
		for (int i = 0; i < r && i < (int)want; i++) {
			int32_t mv = (int32_t)got[i] + offset;

			ring[count % HISTORY] = (uint16_t)CLAMP(mv, 0, UINT16_MAX);
			count++;
		}
	} while (count < HISTORY);
	n = MIN(count, HISTORY);
	if (n == 0) {
		return err ? err : -EIO;
	}
	for (uint32_t i = 0; i < n; i++) {
		sum += ring[i];
	}
	*avg = (uint16_t)(sum / n);
	return err;
}

static uint8_t level_of(uint16_t mv, bool charging)
{
	if (board->level) {
		return MIN(board->level(board->user, mv, charging), 100);
	}
	if (charging) {
		return opensc2_gauge_percent(board->charging ? board->charging :
				opensc2_gauge_lipo_charging, board->charging ? board->charging_len :
				opensc2_gauge_lipo_charging_len, scale_charging, mv);
	}
	return opensc2_gauge_percent(board->discharging ? board->discharging :
			opensc2_gauge_lipo_discharging, board->discharging ? board->discharging_len :
			opensc2_gauge_lipo_discharging_len, scale_discharging, mv);
}

/* The charge window (mte/charge_level), after every load or set of it. */
static void apply_window(void)
{
	uint32_t changes = osc2_keys_window_changes();
	uint8_t v[OSC2_KEY_VALUE_MAX], len = 0;
	k_spinlock_key_t key;
	int32_t cap;

	if (changes == window_seen) {
		return;
	}
	window_seen = changes;
	if (osc2_keys_get(OSC2_KEY_CHARGE_WINDOW, v, &len) != 0 || len != 8) {
		return;
	}
	cap = osc2_fuel_window_cap(sys_get_le32(&v[0]), sys_get_le32(&v[4]));
	key = k_spin_lock(&lock);
	if (cap >= 0) {
		osc2_fuel_set_target(&fuel, (uint16_t)cap);
	} else {
		osc2_fuel_default_target(&fuel);
	}
	k_spin_unlock(&lock, key);
}

static void pass(void)
{
	struct osc2_fuel_out out;
	k_spinlock_key_t key;
	uint8_t start_state, status, was_done;
	bool was_charging, input;
	int err = 0;

	apply_window();
	key = k_spin_lock(&lock);
	start_state = fuel.state;
	was_charging = osc2_fuel_charging(fuel.charger);
	k_spin_unlock(&lock, key);

	if (board->charger_update) {
		err = board->charger_update(board->user);
	}
	input = input_power();
	key = k_spin_lock(&lock);
	status = charger_status;
	if (status == OPENSC2_CHARGER_UNKNOWN) {
		status = osc2_fuel_model_charger(input, fuel.voltage_mv, fuel.charge_mv);
	}
	if (err == 0) {
		fuel.charger = status;
	}
	k_spin_unlock(&lock, key);

	if (err) {
		LOG_ERR("Battery: the charger could not be read (%d)", err);
	} else {
		bool charging = osc2_fuel_charging(status);

		if (was_charging != charging) {
			count = 0;   /* the voltage steps when charging starts or stops */
			k_msleep(SETTLE_MS);
		}
		if (fuel.state != OSC2_FUEL_DONE) {   /* done: the voltage and level stay */
			uint16_t avg = fuel.voltage_mv;
			uint8_t level, kept;
			bool have = osc2_retained_level_get(&kept);

			if (average_mv(&avg) != 0) {
				LOG_DBG("Battery: reading failed, the last average stays");
			}
			key = k_spin_lock(&lock);
			fuel.voltage_mv = avg;
			k_spin_unlock(&lock, key);
			level = level_of(avg, charging);
			osc2_retained_level_set(osc2_fuel_keep(have, kept, level, charging));
		}
	}

	key = k_spin_lock(&lock);
	was_done = fuel.state == OSC2_FUEL_DONE;
	osc2_fuel_run(&fuel, k_uptime_get(), input, start_state, &out);
	if (fuel.state == OSC2_FUEL_DONE && !was_done) {
		osc2_retained_level_set(100);   /* done reads 100 */
	}
	k_spin_unlock(&lock, key);
	write_charger(&out);
	publish();
}

static void gauge_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_msleep(FIRST_PASS_MS);
	for (;;) {
		k_timepoint_t next = sys_timepoint_calc(K_MSEC(PASS_MS));

		pass();
		k_sleep(sys_timepoint_timeout(next));
	}
}

K_THREAD_DEFINE(osc2_gauge_tid, CONFIG_OPENSC2_BATTERY_GAUGE_STACK_SIZE, gauge_thread, NULL, NULL,
		NULL, CONFIG_OPENSC2_BATTERY_GAUGE_PRIORITY, 0, SYS_FOREVER_MS);

int opensc2_battery_start(const struct opensc2_battery_board *b)
{
	static const struct osc2_fuel_cfg cfg = {
		.target_mv = CONFIG_OPENSC2_CHARGE_TARGET_MV,
		.hysteresis_mv = CONFIG_OPENSC2_CHARGE_HYSTERESIS_MV,
		.vin_start_mv = CONFIG_OPENSC2_SUPPLY_CHECK_START_MV,
		.vin_step_mv = CONFIG_OPENSC2_SUPPLY_CHECK_STEP_MV,
		.vin_min_mv = CONFIG_OPENSC2_SUPPLY_CHECK_MIN_MV,
		.step_ms = CONFIG_OPENSC2_SUPPLY_CHECK_STEP_S * 1000u,
		.rest_ms = CONFIG_OPENSC2_SUPPLY_CHECK_REST_S * 1000u,
	};
	struct osc2_fuel_out out;
	k_spinlock_key_t key;

	if (b == NULL || b->read_mv == NULL) {
		return -EINVAL;
	}
	if ((b->charging && b->charging_len == 0) || (b->discharging && b->discharging_len == 0)) {
		return -EINVAL;
	}
	key = k_spin_lock(&lock);
	if (board != NULL) {
		k_spin_unlock(&lock, key);
		return -EALREADY;
	}
	board = b;
	osc2_fuel_init(&fuel, &cfg, &out);
	k_spin_unlock(&lock, key);
	/* The stretch: the curves' readings at the targets as configured now. */
	scale_charging = opensc2_gauge_scale(b->charging ? b->charging : opensc2_gauge_lipo_charging,
					     b->charging ? b->charging_len :
					     opensc2_gauge_lipo_charging_len,
					     cfg.target_mv + cfg.hysteresis_mv);
	scale_discharging = opensc2_gauge_scale(b->discharging ? b->discharging :
						opensc2_gauge_lipo_discharging,
						b->discharging ? b->discharging_len :
						opensc2_gauge_lipo_discharging_len, cfg.target_mv);
	write_charger(&out);
	publish();   /* state 0, the kept level or 255, zeros: the real controller's first report */
	k_thread_name_set(osc2_gauge_tid, "opensc2_gauge");
	k_thread_start(osc2_gauge_tid);
	return 0;
}

int opensc2_battery_set_charge_target(uint16_t mv)
{
	k_spinlock_key_t key;

	if (mv != 0 && (mv < 3400 || mv > 4400)) {
		return -EINVAL;
	}
	key = k_spin_lock(&lock);
	if (board == NULL) {
		k_spin_unlock(&lock, key);
		return -ENOTSUP;
	}
	if (mv) {
		osc2_fuel_set_target(&fuel, mv);
	} else {
		osc2_fuel_default_target(&fuel);
	}
	k_spin_unlock(&lock, key);
	return 0;
}

int opensc2_battery_cutoff(void)
{
	int err;

	if (board == NULL || board->battery_cutoff == NULL) {
		return -ENOTSUP;
	}
	if (input_power()) {
		LOG_WRN("Battery: input power present, the battery stays connected");
		return -EBUSY;
	}
	err = board->battery_cutoff(board->user);
	if (err) {
		LOG_ERR("Battery: cut-off failed (%d)", err);
	}
	return err;
}

#else /* !CONFIG_OPENSC2_BATTERY_GAUGE */

int opensc2_battery_start(const struct opensc2_battery_board *b)
{
	ARG_UNUSED(b);
	return -ENOTSUP;
}

int opensc2_battery_set_charge_target(uint16_t mv)
{
	ARG_UNUSED(mv);
	return -ENOTSUP;
}

int opensc2_battery_cutoff(void)
{
	return -ENOTSUP;
}

#endif /* CONFIG_OPENSC2_BATTERY_GAUGE */
