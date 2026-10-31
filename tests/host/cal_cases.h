/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: the calibration helpers of <opensc2/cal.h> (lib/opensc2/cal.c): the millivolt
 * conversions, the keys' byte formats, the sticks' two halves and 10 % band, the triggers' bands,
 * early full output and digital bits, the pads' pressure filter, gain and click, and the factory
 * recordings. The values are the real controller's single-precision results (the documented
 * rules, worked out in its order of operations), quirks included: a turned-over half reads 0, a
 * range end of 0 is "not set", the trigger bit never turns on from setting 68 = 95 up.
 */
#include <string.h>
#include <opensc2/cal.h>

OPENSC2_TEST(cal_millivolts)
{
	CHECK_EQ(opensc2_cal_counts_to_mv(2048, OPENSC2_CAL_STICK_FULL_MV), 1650);
	CHECK_EQ(opensc2_cal_counts_to_mv(4095, OPENSC2_CAL_STICK_FULL_MV), 3299);
	CHECK_EQ(opensc2_cal_counts_to_mv(4095, OPENSC2_CAL_TRIGGER_FULL_MV), 1649);
	CHECK_EQ(opensc2_cal_counts_to_mv(5000, OPENSC2_CAL_STICK_FULL_MV), 3299);   /* limited */
	CHECK_EQ(opensc2_cal_counts_to_mv(1, OPENSC2_CAL_STICK_FULL_MV), 0);
	CHECK_EQ(opensc2_cal_mv_to_counts(1650, OPENSC2_CAL_STICK_FULL_MV), 2048);
	CHECK_EQ(opensc2_cal_mv_to_counts(3299, OPENSC2_CAL_STICK_FULL_MV), 4095);
	CHECK_EQ(opensc2_cal_mv_to_counts(1649, OPENSC2_CAL_TRIGGER_FULL_MV), 4095);
	CHECK_EQ(opensc2_cal_mv_to_counts(65535, OPENSC2_CAL_TRIGGER_FULL_MV), 65535);
	CHECK_EQ(opensc2_cal_mv_to_counts(0, OPENSC2_CAL_STICK_FULL_MV), 0);

	/* Settings 46 and 62: 1 millivolts, 2 the counts, anything else calibrated. */
	uint16_t out = 0;

	CHECK(opensc2_cal_raw_mode(1, 2048, OPENSC2_CAL_STICK_FULL_MV, &out));
	CHECK_EQ(out, 1650);
	CHECK(opensc2_cal_raw_mode(1, 4095, OPENSC2_CAL_TRIGGER_FULL_MV, &out));
	CHECK_EQ(out, 1649);
	CHECK(opensc2_cal_raw_mode(2, 1234, OPENSC2_CAL_STICK_FULL_MV, &out));
	CHECK_EQ(out, 1234);
	CHECK(!opensc2_cal_raw_mode(0, 1234, OPENSC2_CAL_STICK_FULL_MV, &out));
	CHECK(!opensc2_cal_raw_mode(3, 1234, OPENSC2_CAL_STICK_FULL_MV, &out));
}

OPENSC2_TEST(cal_key_formats)
{
	const struct opensc2_cal_stick s = {
		.format = 2,
		.x = { .min = 300, .max = 3000, .rest_low = 1600, .rest_high = 1700 },
		.y = { .min = 0x0102, .max = 0x0304, .rest_low = 0x0506, .rest_high = 0x0708 },
	};
	const struct opensc2_cal_trigger t = { .format = 0x81, .a_mv = 1500, .b_mv = 200,
					       .direction = 1 };
	const struct opensc2_cal_pressure p = { .format = 1, .a = -2, .b = 2100, .gain = 2000 };
	uint8_t b[OPENSC2_CAL_STICK_LEN];
	struct opensc2_cal_stick s2;
	struct opensc2_cal_trigger t2;
	struct opensc2_cal_pressure p2;

	opensc2_cal_stick_pack(&s, b);
	CHECK_EQ(b[0], 2);
	CHECK_EQ(b[1], 0);
	CHECK_EQ(b[2], 300 & 0xFF);           /* X minimum */
	CHECK_EQ(b[3], 300 >> 8);
	CHECK_EQ(opensc2_get_le16(&b[4]), 3000);   /* X maximum */
	CHECK_EQ(opensc2_get_le16(&b[6]), 1600);   /* X rest low */
	CHECK_EQ(opensc2_get_le16(&b[8]), 1700);   /* X rest high */
	CHECK_EQ(b[10], 0x02);                /* Y minimum, then the rest */
	CHECK_EQ(b[11], 0x01);
	CHECK_EQ(b[16], 0x08);
	CHECK_EQ(b[17], 0x07);
	opensc2_cal_stick_unpack(b, &s2);
	CHECK(memcmp(&s, &s2, sizeof(s)) == 0);

	opensc2_cal_trigger_pack(&t, b);
	CHECK_EQ(opensc2_get_le16(&b[0]), 0x81);
	CHECK_EQ(opensc2_get_le16(&b[2]), 1500);
	CHECK_EQ(opensc2_get_le16(&b[4]), 200);
	CHECK_EQ(b[6], 1);
	opensc2_cal_trigger_unpack(b, &t2);
	CHECK_EQ(t2.format, 0x81);
	CHECK_EQ(t2.a_mv, 1500);
	CHECK_EQ(t2.b_mv, 200);
	CHECK_EQ(t2.direction, 1);

	opensc2_cal_pressure_pack(&p, b);
	CHECK_EQ(opensc2_get_le16(&b[0]), 1);
	CHECK_EQ(opensc2_get_le16(&b[2]), 0xFFFE);   /* A is signed */
	CHECK_EQ(opensc2_get_le16(&b[4]), 2100);
	CHECK_EQ(opensc2_get_le16(&b[6]), 2000);
	opensc2_cal_pressure_unpack(b, &p2);
	CHECK_EQ(p2.a, -2);
	CHECK_EQ(p2.b, 2100);
	CHECK_EQ(p2.gain, 2000);
}

static const struct opensc2_cal_stick cal_stick_ref = {
	.format = 2,
	/* in counts: X 372, 3723, rest 1986..2110 (centre 2048); Y 248, 3847, rest 1924..2047 */
	.x = { .min = 300, .max = 3000, .rest_low = 1600, .rest_high = 1700 },
	.y = { .min = 200, .max = 3100, .rest_low = 1550, .rest_high = 1650 },
};

OPENSC2_TEST(cal_stick_halves)
{
	struct opensc2_stick_proc p = { 0 };
	struct opensc2_stick out;

	opensc2_stick_setup(&p, &cal_stick_ref);
	CHECK(p.valid);
	CHECK_EQ(p.axis[0].centre, 2048);
	CHECK_EQ(p.axis[0].lower.lo, 372.0f);
	CHECK_EQ(p.axis[0].upper.hi, 3723.0f);
	CHECK_EQ(p.axis[1].centre, 1985);
	/* X: the centre reads 0 (no dead zone), one count above it already moves */
	CHECK_EQ(opensc2_stick_axis_value(&p, 0, 2048), 0);
	CHECK_EQ(opensc2_stick_axis_value(&p, 0, 2049), 21);
	/* full deflection 90 % of the way to the extreme: the 10 % band */
	CHECK_EQ(opensc2_stick_axis_value(&p, 0, 3555), 32756);
	CHECK_EQ(opensc2_stick_axis_value(&p, 0, 3556), 32767);
	CHECK_EQ(opensc2_stick_axis_value(&p, 0, 3723), 32767);
	CHECK_EQ(opensc2_stick_axis_value(&p, 0, 4095), 32767);
	/* the lower half on its own scale, truncated toward zero */
	CHECK_EQ(opensc2_stick_axis_value(&p, 0, 1000), -22765);
	CHECK_EQ(opensc2_stick_axis_value(&p, 0, 0), -32767);
	/* Y */
	CHECK_EQ(opensc2_stick_axis_value(&p, 1, 2000), 293);
	CHECK_EQ(opensc2_stick_axis_value(&p, 1, 1900), -1781);
	CHECK_EQ(opensc2_stick_axis_value(&p, 1, 2100), 2248);
	CHECK_EQ(opensc2_stick_axis_value(&p, 1, 3000), 19846);
	CHECK_EQ(opensc2_stick_axis_value(&p, 1, 500), -31125);
	opensc2_stick_value(&p, 2049, 2000, &out);
	CHECK_EQ(out.x, 21);
	CHECK_EQ(out.y, 293);
}

OPENSC2_TEST(cal_stick_validity)
{
	struct opensc2_cal_stick c = cal_stick_ref;
	struct opensc2_stick_proc p = { 0 };
	struct opensc2_stick out;

	/* 0x81 behaves as 2; anything else is uncalibrated: 0, 0 */
	c.format = 0x81;
	opensc2_stick_setup(&p, &c);
	CHECK(p.valid);
	c.format = 3;
	opensc2_stick_setup(&p, &c);
	CHECK(!p.valid);
	opensc2_stick_value(&p, 4095, 0, &out);
	CHECK_EQ(out.x, 0);
	CHECK_EQ(out.y, 0);
	opensc2_stick_setup(&p, NULL);
	CHECK(!p.valid);

	/* An extreme under 200 counts from its end of the rest band (X max 2296 counts, rest high
	 * 2110: 186) makes the whole stick uncalibrated. */
	c = cal_stick_ref;
	c.x.max = 1850;
	opensc2_stick_setup(&p, &c);
	CHECK(!p.valid);
	CHECK_EQ(opensc2_stick_axis_value(&p, 1, 3000), 0);

	/* Extremes swapped (only a hand-written calibration has them) are far enough from the rest
	 * band, so the stick is usable, but each half is turned over and reads 0: a dead axis, as on
	 * the real controller. Y is unaffected. */
	c = cal_stick_ref;
	c.x.min = 3000;
	c.x.max = 300;
	opensc2_stick_setup(&p, &c);
	CHECK(p.valid);
	CHECK_EQ(opensc2_stick_axis_value(&p, 0, 3000), 0);
	CHECK_EQ(opensc2_stick_axis_value(&p, 0, 1000), 0);
	CHECK_EQ(opensc2_stick_axis_value(&p, 0, 2048), 0);
	CHECK_EQ(opensc2_stick_axis_value(&p, 1, 3000), 19846);
	/* A maximum of 0 counts is "not set": the upper half takes its range from its first
	 * reading, which leaves it empty, so it reads 0 (the lower half works). */
	c = cal_stick_ref;
	c.y.max = 0;
	c.y.min = 3100;   /* 200 counts and more from the rest band either way round */
	c.x.max = 0;
	c.x.min = 3000;
	opensc2_stick_setup(&p, &c);
	CHECK(p.valid);
	CHECK_EQ(opensc2_stick_axis_value(&p, 0, 3500), 0);
	CHECK_EQ(opensc2_stick_axis_value(&p, 0, 3600), 0);
}

OPENSC2_TEST(cal_stick_options)
{
	struct opensc2_stick_proc p = { .dead_zone = 3277 };   /* 10 % */

	opensc2_stick_setup(&p, &cal_stick_ref);
	CHECK_EQ(p.dead_zone, 3277);   /* kept by the set-up */
	/* 2186 counts read 2999 without the dead zone: inside it now */
	CHECK_EQ(opensc2_stick_axis_value(&p, 0, 2048 + 138), 0);
	CHECK_EQ(opensc2_stick_axis_value(&p, 0, 3600), 32767);
	CHECK_EQ(opensc2_stick_axis_value(&p, 0, 0), -32767);

	/* July's run-time range: a reading held past the stored maximum moves it out. */
	struct opensc2_stick_proc g = { .grow_range = true };
	int16_t first, later = 0;

	opensc2_stick_setup(&g, &cal_stick_ref);
	first = opensc2_stick_axis_value(&g, 0, 3000);
	for (int i = 0; i < 100; i++) {
		(void)opensc2_stick_axis_value(&g, 0, 4095);
	}
	CHECK(g.axis[0].upper.hi > 3723.0f);
	CHECK(g.axis[0].upper.hi <= 4095.0f);
	later = opensc2_stick_axis_value(&g, 0, 3000);
	CHECK(later < first);   /* the same reading is a smaller deflection of the wider range */
	/* October (the default) keeps the stored range */
	struct opensc2_stick_proc o = { 0 };

	opensc2_stick_setup(&o, &cal_stick_ref);
	for (int i = 0; i < 100; i++) {
		(void)opensc2_stick_axis_value(&o, 0, 4095);
	}
	CHECK_EQ(o.axis[0].upper.hi, 3723.0f);
}

OPENSC2_TEST(cal_trigger)
{
	struct opensc2_cal_trigger c = { .format = 2, .a_mv = 1500, .b_mv = 200, .direction = 0 };
	struct opensc2_trigger_proc p;

	/* A 3724 counts, B 496: rising, released at B, full 2 % before 0.98 A */
	opensc2_trigger_setup(&p, &c);
	CHECK(p.valid);
	CHECK_EQ(p.a, 3724);
	CHECK_EQ(p.b, 496);
	CHECK_EQ(opensc2_trigger_value(&p, 0), 0);
	CHECK_EQ(opensc2_trigger_value(&p, 496), 0);
	CHECK_EQ(opensc2_trigger_value(&p, 653), 0);     /* inside the first 5 % */
	CHECK_EQ(opensc2_trigger_value(&p, 654), 4);
	CHECK_EQ(opensc2_trigger_value(&p, 2000), 15546);
	CHECK_EQ(opensc2_trigger_value(&p, 3649), 32767);   /* 95 % of the way: full */
	CHECK_EQ(opensc2_trigger_value(&p, 3724), 32767);

	/* falling: released at A */
	c.direction = 1;
	opensc2_trigger_setup(&p, &c);
	CHECK(p.falls);
	CHECK_EQ(opensc2_trigger_value(&p, 3724), 0);
	CHECK_EQ(opensc2_trigger_value(&p, 3600), 0);
	CHECK_EQ(opensc2_trigger_value(&p, 2000), 17684);
	CHECK_EQ(opensc2_trigger_value(&p, 700), 32392);
	CHECK_EQ(opensc2_trigger_value(&p, 496), 32767);
	c.direction = 7;   /* any value but 0 falls */
	opensc2_trigger_setup(&p, &c);
	CHECK_EQ(opensc2_trigger_value(&p, 2000), 17684);

	/* A and B stored the other way round still work */
	c = (struct opensc2_cal_trigger){ .format = 2, .a_mv = 200, .b_mv = 1500, .direction = 0 };
	opensc2_trigger_setup(&p, &c);
	CHECK_EQ(opensc2_trigger_value(&p, 3724), 0);
	CHECK_EQ(opensc2_trigger_value(&p, 2000), 17564);
	CHECK_EQ(opensc2_trigger_value(&p, 496), 32767);

	/* under 100 counts apart (595 and 496), a bad format, or no key: 0 */
	c = (struct opensc2_cal_trigger){ .format = 2, .a_mv = 240, .b_mv = 200 };
	opensc2_trigger_setup(&p, &c);
	CHECK(!p.valid);
	CHECK_EQ(opensc2_trigger_value(&p, 4000), 0);
	c = (struct opensc2_cal_trigger){ .format = 1, .a_mv = 1500, .b_mv = 200 };
	opensc2_trigger_setup(&p, &c);
	CHECK(!p.valid);
	opensc2_trigger_setup(&p, NULL);
	CHECK_EQ(opensc2_trigger_value(&p, 4000), 0);
}

OPENSC2_TEST(cal_trigger_bit)
{
	struct opensc2_cal_trigger c = { .format = 2, .a_mv = 1500, .b_mv = 200, .direction = 0 };
	struct opensc2_trigger_proc p;
	uint16_t v;

	/* 90 %: on above 95 % of the normalised pull, off below 85 % */
	opensc2_trigger_setup_level(&p, &c, 90);
	CHECK(!opensc2_trigger_step(&p, 3300, &v));
	CHECK(opensc2_trigger_step(&p, 3600, &v));
	CHECK_EQ(v, 32767);
	CHECK(opensc2_trigger_step(&p, 3200, &v));    /* between: kept */
	CHECK(!opensc2_trigger_step(&p, 3000, &v));
	/* a set-up (any key change, setting 68, the dock) clears it */
	CHECK(opensc2_trigger_step(&p, 3700, &v));
	opensc2_trigger_setup_level(&p, &c, 90);
	CHECK(!opensc2_trigger_step(&p, 3200, &v));
	/* 95 and above: 0.95 + 0.05 is 1.0, which a full pull reaches but never passes */
	opensc2_trigger_setup_level(&p, &c, 95);
	CHECK(!opensc2_trigger_step(&p, 4095, &v));
	CHECK_EQ(v, 32767);
	opensc2_trigger_setup_level(&p, &c, 99);
	CHECK(!opensc2_trigger_step(&p, 4095, &v));
	/* direction 1 at 95: the levels are taken from the other end, so a full pull does count */
	c.direction = 1;
	opensc2_trigger_setup_level(&p, &c, 95);
	CHECK(opensc2_trigger_step(&p, 0, &v));
	CHECK_EQ(v, 32767);
	CHECK(opensc2_trigger_step(&p, 700, &v));    /* 98.8 %: kept */
	CHECK(!opensc2_trigger_step(&p, 1000, &v));  /* below 90 %: off */
	/* uncalibrated: never */
	opensc2_trigger_setup_level(&p, NULL, 40);
	CHECK(!opensc2_trigger_step(&p, 4095, &v));
	CHECK_EQ(v, 0);
}

OPENSC2_TEST(cal_pressure)
{
	const struct opensc2_cal_pressure c = { .format = 1, .a = 100, .b = 2100, .gain = 2000 };
	/* raw words and, after each, the filter, the pressure and the click (setting 40) */
	static const struct {
		int16_t raw;
		int32_t p;
		uint16_t pressure;
		bool click;
	} seq[] = {
		{ 2100, 525, 5916, false },  { 2100, 918, 13070, false },
		{ 2100, 1213, 18440, true }, /* v 56 % > 48 % */
		{ 2100, 1434, 22463, true }, { 0, 1075, 15928, true }, /* 40 %: between, kept */
		{ 0, 806, 11031, true },     { 0, 604, 7354, false },  /* v 25 % < 32 % */
		{ 0, 453, 4605, false },
	};
	struct opensc2_pressure_proc p = { 0 };

	opensc2_pressure_setup(&p, &c);
	CHECK(p.usable);
	CHECK_EQ(p.filtered, 0);
	for (size_t i = 0; i < ARRAY_SIZE(seq); i++) {
		opensc2_pressure_sample(&p, seq[i].raw, 40);
		CHECK_EQ(p.filtered, seq[i].p);
		CHECK_EQ(opensc2_pressure_value(&p), seq[i].pressure);
		CHECK_EQ(p.click, seq[i].click);
	}

	/* a set-up keeps the filter and clears the click */
	opensc2_pressure_sample(&p, 3000, 40);
	opensc2_pressure_sample(&p, 3000, 40);
	CHECK(p.click);
	opensc2_pressure_setup(&p, &c);
	CHECK(!p.click);
	CHECK_EQ(p.filtered, 1566);
	/* so does a change of the click setting: with the filter at 920 (v = 40 %, between 32 and
	 * 48 %) the click stays as it was, until setting 53 changes */
	p.filtered = 3000;
	opensc2_pressure_sample(&p, 3000, 40);
	CHECK(p.click);
	p.filtered = 920;
	opensc2_pressure_sample(&p, 920, 40);
	CHECK_EQ(p.filtered, 920);
	CHECK(p.click);
	opensc2_pressure_sample(&p, 920, 41);
	CHECK(!p.click);
	opensc2_pressure_sample(&p, 920, 41);
	CHECK(!p.click);

	/* the filter rounds toward zero, also below zero */
	struct opensc2_pressure_proc n = { 0 };

	opensc2_pressure_setup(&n, &c);
	opensc2_pressure_sample(&n, -100, 40);
	CHECK_EQ(n.filtered, -25);
	opensc2_pressure_sample(&n, -100, 40);
	CHECK_EQ(n.filtered, -43);
	opensc2_pressure_sample(&n, 50, 40);
	CHECK_EQ(n.filtered, -19);
	CHECK_EQ(opensc2_pressure_value(&n), 0);
	CHECK_EQ(opensc2_pressure_raw_value(-5), 0);
	CHECK_EQ(opensc2_pressure_raw_value(300), 300);
}

OPENSC2_TEST(cal_pressure_forms)
{
	struct opensc2_pressure_proc p = { 0 };
	struct opensc2_cal_pressure c = { .format = 1, .a = 100, .b = 2100, .gain = 1000 };
	static const int32_t filters[] = { 525, 918, 1213, 2100, 4000 };
	static const uint16_t half_gain[] = { 2958, 6535, 9220, 17293, 32767 };
	static const int32_t span_filters[] = { 0, 100, 525, 1000, 2000 };
	static const uint16_t span[] = { 0, 0, 7736, 16383, 32767 };

	/* gain C = 1000: half the pressure */
	opensc2_pressure_setup(&p, &c);
	for (size_t i = 0; i < ARRAY_SIZE(filters); i++) {
		p.filtered = filters[i];
		CHECK_EQ(opensc2_pressure_value(&p), half_gain[i]);
	}
	/* format 0x80: the range 0..B-A, the raw word not shifted by A */
	c = (struct opensc2_cal_pressure){ .format = 0x80, .a = 100, .b = 2100, .gain = 2000 };
	opensc2_pressure_setup(&p, &c);
	for (size_t i = 0; i < ARRAY_SIZE(span_filters); i++) {
		p.filtered = span_filters[i];
		CHECK_EQ(opensc2_pressure_value(&p), span[i]);
	}
	/* a range that falls (A above B) works the other way round */
	c = (struct opensc2_cal_pressure){ .format = 1, .a = 2100, .b = 100, .gain = 2000 };
	opensc2_pressure_setup(&p, &c);
	p.filtered = 2100;
	CHECK_EQ(opensc2_pressure_value(&p), 0);
	p.filtered = 1100;
	CHECK_EQ(opensc2_pressure_value(&p), 16383);
	p.filtered = 100;
	CHECK_EQ(opensc2_pressure_value(&p), 32767);

	/* unusable: gain 0, A = 0x7FFF with format 1, another format, no key */
	static const struct opensc2_cal_pressure bad[] = {
		{ .format = 1, .a = 100, .b = 2100, .gain = 0 },
		{ .format = 1, .a = 0x7FFF, .b = 2100, .gain = 2000 },
		{ .format = 2, .a = 100, .b = 2100, .gain = 2000 },
	};

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		opensc2_pressure_setup(&p, &bad[i]);
		CHECK(!p.usable);
		for (int k = 0; k < 8; k++) {
			opensc2_pressure_sample(&p, 30000, 40);
		}
		CHECK_EQ(opensc2_pressure_value(&p), 0);
		CHECK(!p.click);   /* never clicks */
		CHECK_EQ(p.filtered, 100);   /* and p is left as it was */
	}
	/* a usable calibration again: the filter goes on from the p it had */
	c = (struct opensc2_cal_pressure){ .format = 1, .a = 100, .b = 2100, .gain = 2000 };
	opensc2_pressure_setup(&p, &c);
	opensc2_pressure_sample(&p, 2100, 40);
	CHECK_EQ(p.filtered, (3 * 100 + 2100) / 4);
	opensc2_pressure_setup(&p, NULL);
	CHECK(!p.usable);
	/* No range (B = A): usable, and the filter follows, but the pad reads 0 and never clicks;
	 * the same for B - A = 0 with format 0x80 (0x7FFF is uncalibrated only with format 1) and
	 * for a range end B of 0, which the stage takes for "not set". */
	static const struct opensc2_cal_pressure dead[] = {
		{ .format = 1, .a = 500, .b = 500, .gain = 2000 },
		{ .format = 0x80, .a = 0x7FFF, .b = 0x7FFF, .gain = 2000 },
		{ .format = 1, .a = -300, .b = 0, .gain = 2000 },
	};

	for (size_t i = 0; i < ARRAY_SIZE(dead); i++) {
		opensc2_pressure_setup(&p, &dead[i]);
		CHECK(p.usable);
		for (int k = 0; k < 8; k++) {
			opensc2_pressure_sample(&p, 30000, 40);
			CHECK_EQ(p.last, 0);
			CHECK(!p.click);
		}
		CHECK(p.filtered > 20000);
	}
}

OPENSC2_TEST(cal_pressure_click_minus_one)
{
	/* -1 counts as 100: on above 108 % (filter above 2144 here), off below 92 % (1856) */
	const struct opensc2_cal_pressure c = { .format = 1, .a = 100, .b = 2100, .gain = 2000 };
	struct opensc2_pressure_proc p = { 0 };

	opensc2_pressure_setup(&p, &c);
	for (int i = 0; i < 12; i++) {
		opensc2_pressure_sample(&p, 2200, -1);
		CHECK(!p.click);
	}
	CHECK_EQ(p.filtered, 2128);
	opensc2_pressure_sample(&p, 2200, -1);
	CHECK_EQ(p.filtered, 2146);
	CHECK(p.click);
	CHECK_EQ(opensc2_pressure_value(&p), 32767);
	for (int i = 0; i < 6; i++) {
		opensc2_pressure_sample(&p, 2000, -1);
	}
	CHECK(p.click);   /* 92..108 %: kept */
	for (int i = 0; i < 40; i++) {
		opensc2_pressure_sample(&p, 1500, -1);
	}
	CHECK(!p.click);
	/* any other negative value counts as 100 as well; a value above 100 (which 0x87 never
	 * stores) is taken as it is */
	opensc2_pressure_setup(&p, &c);
	p.filtered = 2140;
	opensc2_pressure_sample(&p, 2140, -5);
	CHECK(!p.click);
	p.filtered = 2150;
	opensc2_pressure_sample(&p, 2150, -5);
	CHECK(p.click);
	opensc2_pressure_setup(&p, &c);
	opensc2_pressure_sample(&p, 2150, 150);
	CHECK(!p.click);
}

static uint32_t rec_frames(struct opensc2_cal_rec *r, int n, uint16_t lx, uint16_t ly,
			   uint16_t rx, uint16_t ry, uint16_t lt, uint16_t rt, int16_t lp, int16_t rp)
{
	const uint16_t stick[2][2] = { { lx, ly }, { rx, ry } };
	const uint16_t trig[2] = { lt, rt };
	const int16_t press[2] = { lp, rp };
	uint32_t bits = 0;

	for (int i = 0; i < n; i++) {
		bits |= opensc2_cal_rec_frame(r, stick, 3, trig, 3, press, 3);
	}
	return bits;
}

OPENSC2_TEST(cal_recording_sticks)
{
	struct opensc2_cal_rec r;
	struct opensc2_cal_stick s;
	const uint16_t one_side[2][2] = { { 1000, 1000 }, { 0, 0 } };

	opensc2_cal_rec_init(&r);
	/* 2 or 3 before a 1: nothing */
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_STICKS, 3), 0);
	CHECK_EQ(rec_frames(&r, 2, 1, 2, 3, 4, 0, 0, 0, 0), 0);
	CHECK(!r.active[OPENSC2_CAL_REC_STICKS]);

	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_STICKS, 1), 0);
	opensc2_cal_rec_stick(&r, OPENSC2_LEFT, &s);
	CHECK_EQ(s.format, 2);
	CHECK_EQ(s.x.rest_low, 0x7FFF);
	CHECK_EQ(s.x.min, 0x7FFF);
	CHECK_EQ(s.x.rest_high, 0);
	CHECK_EQ(s.x.max, 0);
	/* released: the rest band */
	CHECK_EQ(rec_frames(&r, 1, 1600, 1550, 1610, 1560, 0, 0, 0, 0), 0);
	CHECK_EQ(rec_frames(&r, 1, 1700, 1650, 1690, 1640, 0, 0, 0, 0), 0);
	/* a step above 3 changes nothing */
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_STICKS, 4), 0);
	CHECK_EQ(r.step[OPENSC2_CAL_REC_STICKS], 1);
	/* circles */
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_STICKS, 2), 0);
	CHECK_EQ(rec_frames(&r, 1, 300, 3100, 250, 3050, 0, 0, 0, 0), 0);
	CHECK_EQ(rec_frames(&r, 1, 3000, 200, 3050, 150, 0, 0, 0, 0), 0);
	/* 3: saved at the next frame, both sides */
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_STICKS, 3), 0);
	CHECK_EQ(rec_frames(&r, 1, 9, 9, 9, 9, 0, 0, 0, 0),
		 OPENSC2_CAL_SAVE_STICK_L | OPENSC2_CAL_SAVE_STICK_R);
	CHECK(!r.active[OPENSC2_CAL_REC_STICKS]);
	opensc2_cal_rec_stick(&r, OPENSC2_LEFT, &s);
	CHECK_EQ(s.x.rest_low, 1600);
	CHECK_EQ(s.x.rest_high, 1700);
	CHECK_EQ(s.y.rest_low, 1550);
	CHECK_EQ(s.y.rest_high, 1650);
	CHECK_EQ(s.x.min, 300);
	CHECK_EQ(s.x.max, 3000);
	CHECK_EQ(s.y.min, 200);
	CHECK_EQ(s.y.max, 3100);
	opensc2_cal_rec_stick(&r, OPENSC2_RIGHT, &s);
	CHECK_EQ(s.x.min, 250);
	CHECK_EQ(s.y.max, 3050);
	/* the frame after the save records nothing more */
	CHECK_EQ(rec_frames(&r, 2, 0, 0, 0, 0, 0, 0, 0, 0), 0);
	opensc2_cal_rec_stick(&r, OPENSC2_LEFT, &s);
	CHECK_EQ(s.x.min, 300);

	/* 0 unhooks at the next frame without saving; a 3 after that does nothing */
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_STICKS, 1), 0);
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_STICKS, 0), 0);
	CHECK_EQ(rec_frames(&r, 1, 0, 0, 0, 0, 0, 0, 0, 0), 0);
	CHECK(!r.active[OPENSC2_CAL_REC_STICKS]);
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_STICKS, 3), 0);
	CHECK_EQ(rec_frames(&r, 1, 0, 0, 0, 0, 0, 0, 0, 0), 0);
	/* but a 0 and a 3 before the next frame: the frame sees the 3 and saves, as the real
	 * controller's recording reads only the last step */
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_STICKS, 1), 0);
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_STICKS, 0), 0);
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_STICKS, 3), 0);
	CHECK_EQ(rec_frames(&r, 1, 0, 0, 0, 0, 0, 0, 0, 0),
		 OPENSC2_CAL_SAVE_STICK_L | OPENSC2_CAL_SAVE_STICK_R);
	/* a board with one stick: only its side is saved (a fresh boot of the recordings) */
	opensc2_cal_rec_init(&r);
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_STICKS, 1), 0);
	CHECK_EQ(opensc2_cal_rec_frame(&r, one_side, 1, NULL, 0, NULL, 0), 0);
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_STICKS, 3), 0);
	CHECK_EQ(opensc2_cal_rec_frame(&r, one_side, 1, NULL, 0, NULL, 0),
		 OPENSC2_CAL_SAVE_STICK_L);
}

OPENSC2_TEST(cal_recording_triggers)
{
	struct opensc2_cal_rec r;
	struct opensc2_cal_trigger t;

	opensc2_cal_rec_init(&r);
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_TRIGGERS, 1), 0);
	/* released: left rests low (rises when pulled), right rests high (falls) */
	CHECK_EQ(rec_frames(&r, 1, 0, 0, 0, 0, 205, 1500, 0, 0), 0);
	CHECK_EQ(rec_frames(&r, 1, 0, 0, 0, 0, 200, 1505, 0, 0), 0);
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_TRIGGERS, 2), 0);
	CHECK_EQ(rec_frames(&r, 1, 0, 0, 0, 0, 1490, 195, 0, 0), 0);
	CHECK_EQ(rec_frames(&r, 1, 0, 0, 0, 0, 1500, 210, 0, 0), 0);
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_TRIGGERS, 3), 0);
	CHECK_EQ(rec_frames(&r, 1, 0, 0, 0, 0, 0, 0, 0, 0),
		 OPENSC2_CAL_SAVE_TRIGGER_L | OPENSC2_CAL_SAVE_TRIGGER_R);
	opensc2_cal_rec_trigger(&r, OPENSC2_LEFT, &t);
	CHECK_EQ(t.format, 2);
	CHECK_EQ(t.direction, 0);
	CHECK_EQ(t.a_mv, 1500);   /* the highest pulled */
	CHECK_EQ(t.b_mv, 200);    /* the lowest released */
	opensc2_cal_rec_trigger(&r, OPENSC2_RIGHT, &t);
	CHECK_EQ(t.direction, 1);
	CHECK_EQ(t.a_mv, 1505);   /* the highest released */
	CHECK_EQ(t.b_mv, 195);    /* the lowest pulled */

	/* a trigger that never moved: both candidates equally far apart, direction 0 */
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_TRIGGERS, 1), 0);
	CHECK_EQ(rec_frames(&r, 1, 0, 0, 0, 0, 800, 800, 0, 0), 0);
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_TRIGGERS, 2), 0);
	CHECK_EQ(rec_frames(&r, 1, 0, 0, 0, 0, 800, 800, 0, 0), 0);
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_TRIGGERS, 3), 0);
	CHECK(rec_frames(&r, 1, 0, 0, 0, 0, 0, 0, 0, 0) != 0);
	opensc2_cal_rec_trigger(&r, OPENSC2_LEFT, &t);
	CHECK_EQ(t.direction, 0);
	CHECK_EQ(t.a_mv, 800);
	CHECK_EQ(t.b_mv, 800);

	/* Without a step 2 the candidates' spans are measured as |A - B|: direction 0 keeps A 0 and
	 * B the released 800 (800 apart), direction 1 A 800 and B still 0x7FFF (31967 apart). */
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_TRIGGERS, 1), 0);
	CHECK_EQ(rec_frames(&r, 1, 0, 0, 0, 0, 800, 800, 0, 0), 0);
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_TRIGGERS, 3), 0);
	CHECK(rec_frames(&r, 1, 0, 0, 0, 0, 0, 0, 0, 0) != 0);
	opensc2_cal_rec_trigger(&r, OPENSC2_LEFT, &t);
	CHECK_EQ(t.direction, 1);
	CHECK_EQ(t.a_mv, 800);
	CHECK_EQ(t.b_mv, 0x7FFF);
}

OPENSC2_TEST(cal_recording_pressure)
{
	struct opensc2_cal_rec r;
	struct opensc2_cal_pressure p;

	opensc2_cal_rec_init(&r);
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_PRESSURE, 2), 0);   /* no step 1 */
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_PRESSURE, 1), 0);
	opensc2_cal_rec_pressure(&r, OPENSC2_RIGHT, &p);
	CHECK_EQ(p.format, 1);
	CHECK_EQ(p.a, 0);
	CHECK_EQ(p.b, 0);
	CHECK_EQ(p.gain, 2000);
	CHECK_EQ(rec_frames(&r, 1, 0, 0, 0, 0, 0, 0, 900, 900), 0);   /* step 1: followed only */
	opensc2_cal_rec_pressure(&r, OPENSC2_LEFT, &p);
	CHECK_EQ(p.b, 0);
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_PRESSURE, 2), 0);
	CHECK_EQ(rec_frames(&r, 1, 0, 0, 0, 0, 0, 0, 300, -50), 0);
	CHECK_EQ(rec_frames(&r, 1, 0, 0, 0, 0, 0, 0, 1200, 700), 0);
	CHECK_EQ(rec_frames(&r, 1, 0, 0, 0, 0, 0, 0, 800, 600), 0);
	/* 0 pauses: nothing recorded, and the recording goes on with a 2 */
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_PRESSURE, 0), 0);
	CHECK_EQ(rec_frames(&r, 1, 0, 0, 0, 0, 0, 0, 5000, 5000), 0);
	opensc2_cal_rec_pressure(&r, OPENSC2_LEFT, &p);
	CHECK_EQ(p.b, 1200);
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_PRESSURE, 2), 0);
	CHECK_EQ(rec_frames(&r, 1, 0, 0, 0, 0, 0, 0, 1000, 1500), 0);
	/* 3: handed over at once */
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_PRESSURE, 3),
		 OPENSC2_CAL_SAVE_PRESSURE_L | OPENSC2_CAL_SAVE_PRESSURE_R);
	opensc2_cal_rec_pressure(&r, OPENSC2_LEFT, &p);
	CHECK_EQ(p.b, 1200);
	opensc2_cal_rec_pressure(&r, OPENSC2_RIGHT, &p);
	CHECK_EQ(p.b, 1500);
	/* another 3 saves the records again (as they are), hooked or not */
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_PRESSURE, 3),
		 OPENSC2_CAL_SAVE_PRESSURE_L | OPENSC2_CAL_SAVE_PRESSURE_R);
	CHECK(!r.active[OPENSC2_CAL_REC_PRESSURE]);
	/* after a boot of the recordings: zeros (format 0), for the pads the board has */
	opensc2_cal_rec_init(&r);
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_PRESSURE, 3), 0);   /* no pads seen yet */
	opensc2_cal_rec_pressure_sample(&r, OPENSC2_RIGHT, 50);
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_PRESSURE, 3), OPENSC2_CAL_SAVE_PRESSURE_R);
	opensc2_cal_rec_pressure(&r, OPENSC2_RIGHT, &p);
	CHECK_EQ(p.format, 0);
	CHECK_EQ(p.gain, 0);
	/* the recordings keep their own steps */
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_STICKS, 1), 0);
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_TRIGGERS, 1), 0);
	CHECK_EQ(opensc2_cal_rec_step(&r, OPENSC2_CAL_REC_TRIGGERS, 0), 0);
	CHECK_EQ(r.step[OPENSC2_CAL_REC_STICKS], 1);
	CHECK(r.active[OPENSC2_CAL_REC_STICKS]);
	CHECK(r.active[OPENSC2_CAL_REC_TRIGGERS]);   /* until the next frame */
	(void)opensc2_cal_rec_frame(&r, NULL, 0, NULL, 0, NULL, 0);
	CHECK(r.active[OPENSC2_CAL_REC_STICKS]);
	CHECK(!r.active[OPENSC2_CAL_REC_TRIGGERS]);
	CHECK(!r.active[OPENSC2_CAL_REC_PRESSURE]);
}
