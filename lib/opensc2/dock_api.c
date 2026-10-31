/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * dock_api.c: the parts of <opensc2/docking.h> every build has: the docked stick offsets (the
 * system state machine turns them on and off; cal_store.c applies them to the library's stick
 * processing), and, without CONFIG_OPENSC2_DOCK_LINK, the dock's calls answering -ENOTSUP.
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <opensc2/docking.h>
#include "internal.h"
#include "cal_store.h"
#include "dock_link.h"
#include "sys_rules.h"

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

static struct k_spinlock lock;
static struct opensc2_dock_stick_offsets offsets[2];   /* none by default */
static bool docked;                                    /* the docked states say "on" */

static uint16_t moved(uint16_t mv, int16_t offset)
{
	int32_t v = (int32_t)mv + offset;

	return (uint16_t)CLAMP(v, 0, 0xFFFF);
}

static void move_axis(const struct opensc2_dock_axis_offsets *o, struct opensc2_cal_axis *a)
{
	a->min = moved(a->min, o->min);
	a->max = moved(a->max, o->max);
	a->rest_low = moved(a->rest_low, o->rest_low);
	a->rest_high = moved(a->rest_high, o->rest_high);
}

void opensc2_dock_stick_offsets_apply(const struct opensc2_dock_stick_offsets *o,
				      struct opensc2_cal_stick *cal)
{
	move_axis(&o->x, &cal->x);
	move_axis(&o->y, &cal->y);
}

int opensc2_dock_set_stick_offsets(enum opensc2_side side,
				   const struct opensc2_dock_stick_offsets *o)
{
	k_spinlock_key_t key;
	bool on;

	if (side != OPENSC2_LEFT && side != OPENSC2_RIGHT) {
		return -EINVAL;
	}
	key = k_spin_lock(&lock);
	if (o) {
		offsets[side] = *o;
	} else {
		memset(&offsets[side], 0, sizeof(offsets[side]));
	}
	on = docked;
	k_spin_unlock(&lock, key);
	if (on) {
		osc2_cal_reapply();   /* the sticks set up again with the new values */
	}
	return 0;
}

bool opensc2_dock_stick_offsets_active(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool on = docked;

	k_spin_unlock(&lock, key);
	return on;
}

void opensc2_dock_stick_adjust(enum opensc2_side side, struct opensc2_cal_stick *cal)
{
	struct opensc2_dock_stick_offsets o;
	k_spinlock_key_t key;
	bool on;

	if ((side != OPENSC2_LEFT && side != OPENSC2_RIGHT) ||
	    (cal->format != OPENSC2_CAL_FORMAT_ANALOG &&
	     cal->format != OPENSC2_CAL_FORMAT_ANALOG_ALT)) {
		return;
	}
	key = k_spin_lock(&lock);
	on = docked;
	o = offsets[side];
	k_spin_unlock(&lock, key);
	if (on) {
		opensc2_dock_stick_offsets_apply(&o, cal);
	}
}

void osc2_dock_offsets_docked(bool on)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool changed = docked != on;

	docked = on;
	k_spin_unlock(&lock, key);
	if (changed) {
		/* As the real controller: the sticks are set up again only on a change. */
		LOG_DBG("Docked stick offsets %s", on ? "on" : "off");
		osc2_cal_reapply();
	}
}

int opensc2_dock_set_resting(bool on)
{
#if defined(CONFIG_OPENSC2_SYSTEM)
	ARG_UNUSED(on);
	return -EPERM;   /* the system state machine's docked states decide */
#else
	osc2_dock_offsets_docked(on);
	return 0;
#endif
}

void osc2_dock_offsets_follow(uint8_t from, uint8_t to)
{
	if (to == OSC2_SYS_DOCK_OFF || to == OSC2_SYS_DOCK_ON) {
		osc2_dock_offsets_docked(true);
	} else if (to == OSC2_SYS_LIFTED || (from == OSC2_SYS_DOCK_ON && to == OSC2_SYS_BATTERY)) {
		osc2_dock_offsets_docked(false);
	}
}

void osc2_dock_offsets_reset(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	memset(offsets, 0, sizeof(offsets));
	docked = false;
	k_spin_unlock(&lock, key);
}

#if !defined(CONFIG_OPENSC2_DOCK_LINK)

int opensc2_dock_set_board(const struct opensc2_dock_board *board)
{
	ARG_UNUSED(board);
	return -ENOTSUP;
}

int opensc2_dock_pilot_check(void)
{
	return -ENOTSUP;
}

int opensc2_dock_status(struct opensc2_dock_status *st)
{
	memset(st, 0, sizeof(*st));
	st->stick_offsets = opensc2_dock_stick_offsets_active();
	return -ENOTSUP;
}

int opensc2_dock_session_start(void)
{
	return -ENOTSUP;
}

int opensc2_dock_session_end(void)
{
	return -ENOTSUP;
}

int opensc2_dock_frame(const uint8_t *frame, size_t len, uint8_t answer[OPENSC2_DOCK_FRAME_LEN])
{
	ARG_UNUSED(frame);
	ARG_UNUSED(len);
	ARG_UNUSED(answer);
	return -ENOTSUP;
}

void opensc2_dock_answer_sent(void)
{
}

#endif /* !CONFIG_OPENSC2_DOCK_LINK */
