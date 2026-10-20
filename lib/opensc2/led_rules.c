/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * led_rules.c: the status LED's pattern player, brightness curve and duty (opensc2/led.h). Pure
 * logic. The patterns, colours and timings are the protocol documentation's description of the
 * real controller's LED engine (not public yet; docs/protocol.md lists the pages); none of it has
 * been compared with a real controller's LED. The engine plays one cycle of a pattern at a time
 * and picks the next cycle from the status when one ends; fades are linear in 10 ms steps. The
 * searching and fully-charged displays fade from the colour last shown, the breathing patterns
 * start every cycle from dark, and the steady link display holds its colour for 2 s, ended early
 * only by a mode other than Bluetooth's or the puck's (checked every 100 ms).
 */
#include <string.h>
#include <opensc2/led.h>

enum seg_kind {
	SEG_HOLD,          /* show the colour */
	SEG_FADE,          /* fade from the colour last shown to this one */
	SEG_HOLD_UNTIL,    /* dark, cut short when a link comes up (the searching pattern) */
	SEG_HOLD_LINK,     /* the steady link display: cut short only when the mode leaves 1 and 2 */
};

/* Colours in thousandths: R, G, B, W. */
static const uint16_t BLUE[4] = { 0, 0, 200, 0 };
static const uint16_t PUCK[4] = { 0, 0, 50, 500 };
static const uint16_t GREEN[4] = { 0, 250, 0, 0 };
static const uint16_t AMBER[4] = { 200, 200, 0, 0 };
static const uint16_t AMBER_DIM[4] = { 15, 20, 0, 0 };
static const uint16_t RED[4] = { 100, 0, 0, 0 };
static const uint16_t WHITE[4] = { 0, 0, 0, 200 };
static const uint16_t DARK[4] = { 0, 0, 0, 0 };

/* The fully-charged display's colours: full, medium, dim, per mode 1 to 4. */
static const uint16_t FULL_COLOURS[4][3][4] = {
	{ { 0, 0, 200, 0 }, { 0, 0, 100, 0 }, { 0, 0, 40, 0 } },
	{ { 0, 0, 50, 500 }, { 0, 0, 0, 100 }, { 0, 0, 0, 40 } },
	{ { 0, 250, 0, 0 }, { 0, 100, 0, 0 }, { 0, 50, 0, 0 } },
	{ { 200, 200, 0, 0 }, { 80, 100, 0, 0 }, { 30, 40, 0, 0 } },
};

/* b = 100 x (6^(s/100) - 1) / 5, cut to a whole number, for s = 0..100 (computed exactly). */
static const uint8_t CURVE[101] = {
	0, 0, 0, 1, 1, 1, 2, 2, 3, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11,
	11, 12, 13, 13, 14, 14, 15, 16, 16, 17, 18, 18, 19, 20, 20, 21, 22, 23, 23, 24, 25, 26,
	27, 28, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 44, 45, 46, 47, 48,
	50, 51, 52, 53, 55, 56, 58, 59, 60, 62, 63, 65, 66, 68, 70, 71, 73, 75, 76, 78, 80, 82,
	83, 85, 87, 89, 91, 93, 95, 97, 100,
};

#define FULL_MS          15000   /* the full colour this long after the level read below 100 % */
#define LOW_LEVEL        5       /* at or below: the red displays */
#define LINK_STEADY_MS   2000    /* the steady link display's cycle */
#define LINK_CHECK_MS    100     /* how often it looks whether the mode left the link pattern */

uint8_t opensc2_led_brightness(int16_t setting45, int16_t setting38)
{
	int16_t s = setting45 < 0 ? 0 : setting45 > 100 ? 100 : setting45;

	return setting38 != 0 ? CURVE[s] : (uint8_t)s;
}

uint16_t opensc2_led_duty(uint16_t colour, uint8_t trim, uint8_t brightness)
{
	uint32_t d = (uint32_t)colour * trim / 100u * brightness / 100u;

	return (uint16_t)(d > OPENSC2_LED_FULL_SCALE ? OPENSC2_LED_FULL_SCALE : d);
}

/* ---- building one cycle ---------------------------------------------------------------------- */

static void add(struct opensc2_led_player *p, uint8_t kind, uint16_t ms, const uint16_t c[4])
{
	struct opensc2_led_segment *s = &p->seg[p->count++];

	s->kind = kind;
	s->ms = ms;
	memcpy(s->rgbw, c, sizeof(s->rgbw));
}

static const uint16_t *mode_colour(uint8_t mode)
{
	switch (mode) {
	case OPENSC2_LED_MODE_BLUETOOTH: return BLUE;
	case OPENSC2_LED_MODE_PUCK:      return PUCK;
	case OPENSC2_LED_MODE_WIRED:     return GREEN;
	case OPENSC2_LED_MODE_AMBER:     return AMBER;
	default:                         return DARK;
	}
}

static bool full(const struct opensc2_led_status *st)
{
	return st->level >= 100;   /* 255, no level yet, counts as full */
}

/* Which of the four fully-charged levels shows now: 0 off, 1 dim, 2 medium, 3 full. */
static uint8_t full_choice(const struct opensc2_led_status *st, uint32_t now_ms)
{
	bool follows = st->mode == OPENSC2_LED_MODE_WIRED ||
		       ((st->mode == OPENSC2_LED_MODE_BLUETOOTH || st->mode == OPENSC2_LED_MODE_PUCK) &&
			st->connected && st->external_power);

	if (st->below_full_ms != 0 && now_ms - st->below_full_ms < FULL_MS) {
		return 3;
	}
	if (follows && st->input_recent) {
		return 3;
	}
	return st->full_level <= 2 ? st->full_level : 3;
}

static void steady(struct opensc2_led_player *p, const uint16_t c[4])
{
	add(p, SEG_HOLD, 100, c);   /* looked at again every 100 ms */
}

/* Connected on battery, or a chord state: the colour (or the red at 5 % or less) chosen again
 * every 2 s; the other link mode, a power change, a lost link, the red and the override wait for
 * the end of the cycle, a mode other than 1 or 2 shows at the next 100 ms check. */
static void steady_link(struct opensc2_led_player *p, const uint16_t c[4])
{
	add(p, SEG_HOLD_LINK, LINK_STEADY_MS, c);
}

static void flashes(struct opensc2_led_player *p, const uint16_t c[4], uint16_t dark_ms)
{
	add(p, SEG_HOLD, 75, c);
	add(p, SEG_HOLD, 75, DARK);
	add(p, SEG_HOLD, 75, c);
	add(p, SEG_HOLD, dark_ms, DARK);
}

/* A breathing pattern starts every cycle from dark, whatever showed before it. */
static void from_dark(struct opensc2_led_player *p)
{
	add(p, SEG_HOLD, 0, DARK);
}

static void breathe(struct opensc2_led_player *p, const uint16_t c[4])
{
	from_dark(p);
	add(p, SEG_FADE, 1800, c);
	add(p, SEG_FADE, 1800, DARK);
}

static void full_display(struct opensc2_led_player *p, const struct opensc2_led_status *st,
			 uint32_t now_ms)
{
	uint8_t choice = full_choice(st, now_ms);
	const uint16_t *c = choice == 0 ? DARK : FULL_COLOURS[st->mode - 1][3 - choice];

	add(p, SEG_FADE, 2000, c);   /* fade there over 2 s and hold; chosen again every 2 s */
}

static void build(struct opensc2_led_player *p, const struct opensc2_led_status *st,
		  uint32_t now_ms)
{
	const uint16_t *c = mode_colour(st->mode);
	bool low = st->level <= LOW_LEVEL;

	p->count = 0;
	if (st->override) {
		uint16_t o[4];

		for (int i = 0; i < 4; i++) {
			o[i] = (uint16_t)(st->override_rgbw[i] * 10u);   /* percent: not clamped */
		}
		steady(p, o);
		return;
	}
	switch (st->mode) {
	case OPENSC2_LED_MODE_BLUETOOTH:
	case OPENSC2_LED_MODE_PUCK:
		if (st->steady || (st->connected && !st->external_power)) {
			steady_link(p, low ? RED : c);
		} else if (st->connected) {
			if (full(st)) {
				full_display(p, st, now_ms);
			} else {
				breathe(p, c);
			}
		} else if (st->pairing) {
			flashes(p, c, 300);
		} else {
			/* Listening, advertising for the bonded host, or off. */
			add(p, SEG_FADE, 100, c);
			add(p, SEG_HOLD, 900, c);
			add(p, SEG_FADE, 100, DARK);
			add(p, SEG_HOLD_UNTIL, 900, DARK);
		}
		break;
	case OPENSC2_LED_MODE_WIRED:
		if (full(st)) {
			full_display(p, st, now_ms);
		} else {
			breathe(p, c);
		}
		break;
	case OPENSC2_LED_MODE_AMBER:
		if (full(st)) {
			full_display(p, st, now_ms);
		} else {
			from_dark(p);   /* a breathing pattern: dark to a dim amber, full, back */
			add(p, SEG_FADE, 200, AMBER_DIM);
			add(p, SEG_FADE, 1600, AMBER);
			add(p, SEG_FADE, 1600, AMBER_DIM);
			add(p, SEG_FADE, 200, DARK);
		}
		break;
	case OPENSC2_LED_MODE_LOW_BATTERY:
		if (low) {
			flashes(p, RED, 1075);
		} else {
			add(p, SEG_HOLD, 1000, p->shown);   /* above 5 % it keeps what it showed, 1 s */
		}
		break;
	case OPENSC2_LED_MODE_KEYLOCK:
		flashes(p, WHITE, 10075);
		break;
	default:
		steady(p, DARK);
		break;
	}
}

/* ---- playing it ------------------------------------------------------------------------------ */

void opensc2_led_player_init(struct opensc2_led_player *player)
{
	memset(player, 0, sizeof(*player));
}

static void begin_cycle(struct opensc2_led_player *p, const struct opensc2_led_status *st,
			uint32_t now_ms)
{
	build(p, st, now_ms);
	p->index = 0;
	p->seg_start = now_ms;
	p->running = true;
	memcpy(p->from, p->shown, sizeof(p->from));
	p->peak = 0;
	for (uint8_t i = 0; i < p->count; i++) {
		for (int ch = 0; ch < 4; ch++) {
			if (p->seg[i].rgbw[ch] > p->peak) {
				p->peak = p->seg[i].rgbw[ch];
			}
		}
	}
}

void opensc2_led_player_step(struct opensc2_led_player *player,
			     const struct opensc2_led_status *status, uint32_t now_ms,
			     uint16_t rgbw[4])
{
	struct opensc2_led_player *p = player;
	const struct opensc2_led_segment *s;
	uint32_t elapsed;

	if (!p->running) {
		begin_cycle(p, status, now_ms);
	}
	/* Past the end of the segment (a late call can pass several): on to the next; a cycle that
	 * ends starts the next one from the status as it is now. */
	for (int guard = 0; guard < 16; guard++) {
		uint32_t len;

		s = &p->seg[p->index];
		elapsed = now_ms - p->seg_start;
		len = s->ms;
		if (s->kind == SEG_HOLD_UNTIL && status->connected) {
			len = elapsed < s->ms ? elapsed : s->ms;   /* a link came up: it ends now */
		} else if (s->kind == SEG_HOLD_LINK && status->mode != OPENSC2_LED_MODE_BLUETOOTH &&
			   status->mode != OPENSC2_LED_MODE_PUCK) {
			/* the mode left the link pattern: it ends at the next 100 ms check */
			uint32_t at = (elapsed + LINK_CHECK_MS - 1) / LINK_CHECK_MS * LINK_CHECK_MS;

			at = at < LINK_CHECK_MS ? LINK_CHECK_MS : at;
			len = at < s->ms ? at : s->ms;
		}
		if (elapsed < len) {
			break;
		}
		memcpy(p->from, s->rgbw, sizeof(p->from));
		memcpy(p->shown, s->rgbw, sizeof(p->shown));
		p->seg_start += len;
		if (++p->index >= p->count) {
			begin_cycle(p, status, p->seg_start);
		}
	}
	s = &p->seg[p->index];
	elapsed = now_ms - p->seg_start;
	if (s->kind == SEG_FADE && s->ms > 0) {
		/* Linear, in 10 ms steps. */
		uint32_t at = elapsed / OPENSC2_LED_STEP_MS * OPENSC2_LED_STEP_MS;

		for (int ch = 0; ch < 4; ch++) {
			int32_t a = p->from[ch], b = s->rgbw[ch];

			p->shown[ch] = (uint16_t)(a + (b - a) * (int32_t)at / (int32_t)s->ms);
		}
	} else {
		memcpy(p->shown, s->rgbw, sizeof(p->shown));
	}
	memcpy(rgbw, p->shown, sizeof(p->shown));
}

uint16_t opensc2_led_player_envelope(const struct opensc2_led_player *player)
{
	uint16_t m = 0;

	if (player->peak == 0) {
		return 0;
	}
	for (int ch = 0; ch < 4; ch++) {
		if (player->shown[ch] > m) {
			m = player->shown[ch];
		}
	}
	return (uint16_t)((uint32_t)m * 1000u / player->peak);
}
