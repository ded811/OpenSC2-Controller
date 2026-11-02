/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * gauge.c: the battery helpers of <opensc2/gauge.h>. The rules (divider and offset, 40 readings,
 * stretch, direction) follow the protocol documentation (not public yet; docs/protocol.md lists the
 * pages); the two curves are OpenSC2's own, from the usual shape of a single-cell LiPo's voltage.
 * Pure logic.
 */
#include <string.h>
#include <opensc2/gauge.h>
#include <opensc2/types.h>

int32_t opensc2_gauge_divider_mv(int32_t pin_mv, uint32_t total_ohm, uint32_t bottom_ohm,
				 int16_t offset_mv)
{
	int64_t mv;

	if (bottom_ohm == 0) {
		return pin_mv + offset_mv;
	}
	mv = ((int64_t)pin_mv * total_ohm * 2 + bottom_ohm) / ((int64_t)bottom_ohm * 2);
	return (int32_t)mv + offset_mv;
}

/* ---- the history ----------------------------------------------------------------------------- */

void opensc2_gauge_history_reset(struct opensc2_gauge_history *h)
{
	memset(h, 0, sizeof(*h));
}

void opensc2_gauge_history_add(struct opensc2_gauge_history *h, uint16_t mv)
{
	h->mv[h->next] = mv;
	h->next = (uint8_t)((h->next + 1) % OPENSC2_GAUGE_HISTORY);
	if (h->count < OPENSC2_GAUGE_HISTORY) {
		h->count++;
	}
}

uint16_t opensc2_gauge_latest(const struct opensc2_gauge_history *h)
{
	if (h->count == 0) {
		return 0;
	}
	return h->mv[(h->next + OPENSC2_GAUGE_HISTORY - 1) % OPENSC2_GAUGE_HISTORY];
}

/* After a reset the readings fill the slots from the first on: `count` of them hold one. */
uint16_t opensc2_gauge_average(const struct opensc2_gauge_history *h)
{
	uint32_t sum = 0;

	if (h->count == 0) {
		return 0;
	}
	for (int i = 0; i < h->count; i++) {
		sum += h->mv[i];
	}
	return (uint16_t)(sum / h->count);
}

uint16_t opensc2_gauge_max(const struct opensc2_gauge_history *h)
{
	uint16_t m = 0;

	for (int i = 0; i < h->count; i++) {
		m = h->mv[i] > m ? h->mv[i] : m;
	}
	return m;
}

/* ---- curves ---------------------------------------------------------------------------------- */

const struct opensc2_gauge_point opensc2_gauge_lipo_discharging[] = {
	{ 3670, 0 },   { 3700, 30 },  { 3760, 150 }, { 3790, 230 },  { 3820, 330 },  { 3850, 430 },
	{ 3880, 530 }, { 3920, 640 }, { 3970, 750 }, { 4030, 860 },  { 4080, 940 },  { 4120, 1000 },
};
const size_t opensc2_gauge_lipo_discharging_len =
	sizeof(opensc2_gauge_lipo_discharging) / sizeof(opensc2_gauge_lipo_discharging[0]);

const struct opensc2_gauge_point opensc2_gauge_lipo_charging[] = {
	{ 3750, 0 },   { 3800, 50 },  { 3850, 120 }, { 3900, 220 }, { 3950, 340 },
	{ 4000, 480 }, { 4050, 620 }, { 4100, 780 }, { 4130, 880 }, { 4160, 1000 },
};
const size_t opensc2_gauge_lipo_charging_len =
	sizeof(opensc2_gauge_lipo_charging) / sizeof(opensc2_gauge_lipo_charging[0]);

uint16_t opensc2_gauge_curve(const struct opensc2_gauge_point *points, size_t n, uint16_t mv)
{
	if (n == 0 || mv <= points[0].mv) {
		return n ? points[0].permille : 0;
	}
	for (size_t i = 1; i < n; i++) {
		const struct opensc2_gauge_point *a = &points[i - 1], *b = &points[i];

		if (mv <= b->mv) {
			if (b->mv == a->mv) {
				return b->permille;
			}
			return (uint16_t)(a->permille + (int32_t)(mv - a->mv) *
						 (b->permille - a->permille) / (b->mv - a->mv));
		}
	}
	return 1000;
}

uint8_t opensc2_gauge_stretch(uint16_t permille, uint16_t target_permille)
{
	/* s = the target's reading in whole percent (rounded, at most 100), then
	 * percent = permille / 10 x (200 - s) / 100, rounded */
	int32_t s = ((int32_t)target_permille + 5) / 10;
	int32_t pct;

	s = s > 100 ? 100 : s;
	pct = ((int32_t)permille * (200 - s) + 500) / 1000;
	return (uint8_t)(pct < 0 ? 0 : pct > 100 ? 100 : pct);
}

uint8_t opensc2_gauge_scale(const struct opensc2_gauge_point *points, size_t n, uint16_t target_mv)
{
	/* whole percent, limited to 100 as the real controller limits it (a board's curve may go
	 * past 1000) */
	uint32_t s = ((uint32_t)opensc2_gauge_curve(points, n, target_mv) + 5) / 10;

	return (uint8_t)(s > 100 ? 100 : s);
}

uint8_t opensc2_gauge_percent(const struct opensc2_gauge_point *points, size_t n, uint8_t scale,
			      uint16_t mv)
{
	return opensc2_gauge_stretch(opensc2_gauge_curve(points, n, mv), (uint16_t)scale * 10);
}

/* ---- the level ------------------------------------------------------------------------------- */

void opensc2_gauge_init(struct opensc2_gauge *g, uint8_t kept_level)
{
	opensc2_gauge_history_reset(&g->history);
	g->level = kept_level <= 100 ? kept_level : OPENSC2_GAUGE_NO_LEVEL;
	g->charge_state = OPENSC2_CHARGE_RESET;
	g->charger_charging = false;
	g->voltage_mv = 0;
	g->curves = NULL;
}

void opensc2_gauge_set_curves(struct opensc2_gauge *g, const struct opensc2_gauge_curves *curves)
{
	g->curves = curves;
}

uint8_t opensc2_gauge_begin(struct opensc2_gauge *g, uint8_t charge_state)
{
	return opensc2_gauge_begin_charger(g, charge_state,
					   charge_state == OPENSC2_CHARGE_CHARGING);
}

uint8_t opensc2_gauge_begin_charger(struct opensc2_gauge *g, uint8_t charge_state,
				    bool charger_charging)
{
	if (charger_charging != g->charger_charging) {
		opensc2_gauge_history_reset(&g->history);   /* the readings jump: start over */
	}
	g->charger_charging = charger_charging;
	g->charge_state = charge_state;
	if (charge_state == OPENSC2_CHARGE_DONE) {
		return 0;
	}
	return g->history.count >= OPENSC2_GAUGE_HISTORY ? 1 :
	       (uint8_t)(OPENSC2_GAUGE_HISTORY - g->history.count);
}

void opensc2_gauge_add(struct opensc2_gauge *g, uint16_t battery_mv)
{
	opensc2_gauge_history_add(&g->history, battery_mv);
}

uint16_t opensc2_gauge_voltage(const struct opensc2_gauge *g)
{
	return g->voltage_mv;
}

uint8_t opensc2_gauge_step(struct opensc2_gauge *g, uint16_t battery_mv, uint8_t charge_state)
{
	if (opensc2_gauge_begin(g, charge_state) > 0) {
		opensc2_gauge_add(g, battery_mv);
	}
	return opensc2_gauge_finish(g);
}

uint8_t opensc2_gauge_finish(struct opensc2_gauge *g)
{
	bool charging = g->charge_state == OPENSC2_CHARGE_CHARGING;
	const struct opensc2_gauge_point *curve;
	size_t n;
	uint16_t target;
	uint8_t level;

	if (g->charge_state == OPENSC2_CHARGE_DONE) {
		g->level = 100;   /* no reading taken: the reported voltage holds */
		return g->level;
	}
	if (g->history.count == 0) {
		return g->level;   /* nothing read */
	}
	g->voltage_mv = opensc2_gauge_average(&g->history);
	curve = charging ? opensc2_gauge_lipo_charging : opensc2_gauge_lipo_discharging;
	n = charging ? opensc2_gauge_lipo_charging_len : opensc2_gauge_lipo_discharging_len;
	target = charging ? OPENSC2_GAUGE_TARGET_CHARGING_MV : OPENSC2_GAUGE_TARGET_MV;
	if (g->curves) {
		/* a board's own, where it gives one */
		const struct opensc2_gauge_point *own = charging ? g->curves->charging :
								   g->curves->discharging;
		size_t own_n = charging ? g->curves->charging_len : g->curves->discharging_len;
		uint16_t own_target = charging ? g->curves->target_charging_mv :
						 g->curves->target_mv;

		if (own && own_n) {
			curve = own;
			n = own_n;
		}
		if (own_target) {
			target = own_target;
		}
	}
	level = opensc2_gauge_stretch(opensc2_gauge_curve(curve, n,
							  opensc2_gauge_average(&g->history)),
				      opensc2_gauge_curve(curve, n, target));
	if (g->level != OPENSC2_GAUGE_NO_LEVEL) {
		/* Discharging the level only falls, charging it only rises. */
		if (charging ? level < g->level : level > g->level) {
			level = g->level;
		}
	}
	g->level = level;
	return level;
}
