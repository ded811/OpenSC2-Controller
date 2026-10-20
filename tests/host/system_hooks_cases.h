/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only, in the binary built with CONFIG_OPENSC2_TEST_HOOKS, after hooks_cases.h (whose
 * helpers they use): the hooks' system ops (test_sys.c, test_sys_notes.c) on a stand-in for the
 * system state machine: system injection, the boot-time injection and its hold, the status, the
 * simulated events, arming and its end, and the ring entries of the machine. Bytes are the ones of
 * docs/test-protocol.md.
 */
#include "test_sys.h"

static struct {
	int changed, events, stores;
	uint8_t last_event;
	bool store_null;
	uint32_t words[OSC2_TEST_SYS_BOOT_WORDS];
	int store_rc;
} sysenv;

static void sysenv_status(uint8_t out[OSC2_TEST_SYS_STATUS_LEN])
{
	memset(out, 0, OSC2_TEST_SYS_STATUS_LEN);
	out[0] = OSC2_SYS_BATTERY;
	out[1] = OSC2_TRANSPORT_PUCK1;
	out[3] = OSC2_LED_PUCK;
	out[7] = 0xFF;   /* the injection bits come from the hooks, the others stay */
	out[14] = 5;
}

static void sysenv_changed(void)
{
	sysenv.changed++;
}

static int sysenv_store(const uint32_t words[OSC2_TEST_SYS_BOOT_WORDS])
{
	sysenv.stores++;
	sysenv.store_null = words == NULL;
	if (words) {
		memcpy(sysenv.words, words, sizeof(sysenv.words));
	}
	return sysenv.store_rc;
}

static void sysenv_event(uint8_t kind)
{
	sysenv.events++;
	sysenv.last_event = kind;
}

static const struct osc2_test_sys_env sysenv_obj = {
	.status = sysenv_status,
	.changed = sysenv_changed,
	.boot_store = sysenv_store,
	.event = sysenv_event,
};

static void sys_hooks_reset(void)
{
	stub_reset();
	memset(&sysenv, 0, sizeof(sysenv));
	osc2_test_sys_init();
	osc2_test_sys_env = &sysenv_obj;
}

static uint8_t sys_args[OSC2_TEST_SYS_ARGS_LEN];

static const uint8_t *sargs(uint8_t flags, uint32_t buttons, uint8_t level, uint8_t source,
			    uint8_t last)
{
	sys_args[0] = flags;
	opensc2_put_le32(&sys_args[1], buttons);
	sys_args[5] = level;
	sys_args[6] = source;
	sys_args[7] = last;
	return sys_args;
}

OPENSC2_TEST(sys_hooks_ops)
{
	uint32_t v = 0;
	uint8_t level = 0, source = 0;

	sys_hooks_reset();
	/* Unarmed: the console answers NOT_ARMED, but SYS_STATUS (like STATUS) runs. */
	CHECK_EQ(con_op(OSC2_TEST_OP_SYS_INJECT, sargs(1, 1, 0, 0, 0), 8), OSC2_TEST_NOT_ARMED);
	CHECK_EQ(con_op(OSC2_TEST_OP_SYS_STATUS, NULL, 0), OSC2_TEST_OK);
	CHECK_EQ(hans_len, 4 + OSC2_TEST_SYS_STATUS_LEN);
	CHECK_EQ(RES[0], OSC2_SYS_BATTERY);
	CHECK_EQ(RES[7], 0xFF & ~0x1E);   /* nothing injected */
	CHECK_EQ(arm_on(false, 0x51, 0, 0), OSC2_TEST_OK);
	/* No machine in the build: DENIED. */
	osc2_test_sys_env = NULL;
	CHECK_EQ(con_op(OSC2_TEST_OP_SYS_STATUS, NULL, 0), OSC2_TEST_DENIED);
	osc2_test_sys_env = &sysenv_obj;
	/* Lengths, values, unknown ops in the range. */
	CHECK_EQ(con_op(OSC2_TEST_OP_SYS_INJECT, sargs(1, 1, 0, 0, 0), 7), OSC2_TEST_BAD_LEN);
	CHECK_EQ(con_op(OSC2_TEST_OP_SYS_INJECT, sargs(8, 1, 0, 0, 0), 8), OSC2_TEST_BAD_ARG);
	CHECK_EQ(con_op(OSC2_TEST_OP_SYS_INJECT, sargs(4, 0, 0, 3, 0), 8), OSC2_TEST_BAD_ARG);
	CHECK_EQ(con_op(OSC2_TEST_OP_SYS_INJECT, sargs(1, 0, 0, 0, 1), 8), OSC2_TEST_BAD_ARG);
	CHECK_EQ(con_op(OSC2_TEST_OP_SYS_EVENT, (const uint8_t[]){ 3 }, 1), OSC2_TEST_BAD_ARG);
	CHECK_EQ(con_op(0x75, NULL, 0), OSC2_TEST_BAD_OP);
	CHECK_EQ(con_op(0x77, NULL, 0), OSC2_TEST_BAD_OP);
	CHECK_EQ(sysenv.changed, 0);
	CHECK(!osc2_test_sys_buttons(&v));

	/* Inject the buttons, then the level and the source: each reaches the glue's questions. */
	CHECK_EQ(con_op(OSC2_TEST_OP_SYS_INJECT, sargs(1, OPENSC2_BTN_STEAM, 0, 0, 0), 8),
		 OSC2_TEST_OK);
	CHECK_EQ(sysenv.changed, 1);
	CHECK(osc2_test_sys_buttons(&v));
	CHECK_EQ(v, OPENSC2_BTN_STEAM);
	CHECK(!osc2_test_sys_level(&level));
	CHECK_EQ(usb_op(OSC2_TEST_OP_SYS_INJECT, sargs(6, 0, 7, OSC2_SRC_BATTERY, 0), 8),
		 OSC2_TEST_OK);
	CHECK(osc2_test_sys_level(&level));
	CHECK_EQ(level, 7);
	CHECK(osc2_test_sys_source(&source));
	CHECK_EQ(source, OSC2_SRC_BATTERY);
	CHECK(osc2_test_sys_buttons(&v));   /* untouched by flags without bit 0 */
	CHECK_EQ(con_op(OSC2_TEST_OP_SYS_STATUS, NULL, 0), OSC2_TEST_OK);
	CHECK_EQ(RES[7] & 0x1E, 0x0E);
	CHECK(osc2_test_sys_keep_on());     /* armed */

	/* The events. */
	CHECK_EQ(con_op(OSC2_TEST_OP_SYS_EVENT, (const uint8_t[]){ 1 }, 1), OSC2_TEST_OK);
	CHECK_EQ(sysenv.last_event, OSC2_TEST_SYS_EV_WIRELESS_OFF);
	CHECK_EQ(con_op(OSC2_TEST_OP_SYS_EVENT, (const uint8_t[]){ 2 }, 1), OSC2_TEST_OK);
	CHECK_EQ(sysenv.last_event, OSC2_TEST_SYS_EV_HOST_ASLEEP);
	CHECK_EQ(sysenv.events, 2);

	/* A new ARM ends the runtime injection; so does the end of arming. */
	CHECK_EQ(arm_on(false, 0x52, 0, 0), OSC2_TEST_OK);
	CHECK(!osc2_test_sys_buttons(&v));
	CHECK(!osc2_test_sys_level(&level));
	CHECK(!osc2_test_sys_source(&source));
	CHECK_EQ(con_op(OSC2_TEST_OP_SYS_INJECT, sargs(7, 1, 1, 2, 0), 8), OSC2_TEST_OK);
	CHECK_EQ(con_op(OSC2_TEST_OP_DISARM, NULL, 0), OSC2_TEST_OK);
	CHECK(!osc2_test_sys_buttons(&v));
	CHECK(!osc2_test_sys_source(&source));
	CHECK(!osc2_test_sys_keep_on());
	CHECK_EQ(hk.violations, 0);
}

OPENSC2_TEST(sys_hooks_boot)
{
	struct osc2_test_sys_boot b = { 0 }, back;
	struct osc2_test_sys_end e = { 0 }, eback;
	uint32_t words[OSC2_TEST_SYS_BOOT_WORDS], ew[OSC2_TEST_SYS_END_WORDS], v = 0;
	uint8_t level = 0, source = 0;

	/* The retained words, both ways; a wrong magic word or nothing injected is no record. */
	b.flags = OSC2_TEST_SYS_ALL;
	b.buttons = 0x00010201u;
	b.level = 3;
	b.source = OSC2_SRC_DOCK;
	b.hold = 5;
	osc2_test_sys_boot_pack(&b, words);
	CHECK_EQ(words[0], OSC2_TEST_SYS_BOOT_MAGIC);
	CHECK(osc2_test_sys_boot_unpack(words, &back));
	CHECK(memcmp(&b, &back, sizeof(b)) == 0);
	words[0] ^= 1;
	CHECK(!osc2_test_sys_boot_unpack(words, &back));
	b.flags = 0;
	osc2_test_sys_boot_pack(&b, words);
	CHECK(!osc2_test_sys_boot_unpack(words, &back));
	e.state = OSC2_SYS_SHUTDOWN;
	e.cause = OSC2_SYS_EV_STEAM_HELD;
	e.end = OSC2_SYS_END_REBOOT;
	e.how = OSC2_TEST_SYS_HOW_REBOOT;
	e.uptime_ms = 123456;
	osc2_test_sys_end_pack(&e, ew);
	CHECK(osc2_test_sys_end_unpack(ew, &eback));
	CHECK(memcmp(&e, &eback, sizeof(e)) == 0);
	ew[0] = 0;
	CHECK(!osc2_test_sys_end_unpack(ew, &eback));

	/* SYS_BOOT stores the words for the next boot; flags 0 clears; no retained area: DENIED. */
	sys_hooks_reset();
	CHECK_EQ(arm_on(false, 0x61, 0, 0), OSC2_TEST_OK);
	CHECK_EQ(con_op(OSC2_TEST_OP_SYS_BOOT, sargs(3, OPENSC2_BTN_STEAM, 2, 0, 3), 8),
		 OSC2_TEST_OK);
	CHECK_EQ(sysenv.stores, 1);
	CHECK(!sysenv.store_null);
	CHECK(osc2_test_sys_boot_unpack(sysenv.words, &back));
	CHECK_EQ(back.flags, 3);
	CHECK_EQ(back.buttons, OPENSC2_BTN_STEAM);
	CHECK_EQ(back.level, 2);
	CHECK_EQ(back.hold, 3);
	CHECK_EQ(con_op(OSC2_TEST_OP_SYS_BOOT, sargs(0, 0, 0, 0, 0), 8), OSC2_TEST_OK);
	CHECK(sysenv.store_null);
	CHECK_EQ(con_op(OSC2_TEST_OP_SYS_BOOT, sargs(9, 0, 0, 0, 0), 8), OSC2_TEST_BAD_ARG);
	sysenv.store_rc = -ENOTSUP;
	CHECK_EQ(con_op(OSC2_TEST_OP_SYS_BOOT, sargs(1, 1, 0, 0, 0), 8), OSC2_TEST_DENIED);
	CHECK_EQ(con_op(OSC2_TEST_OP_DISARM, NULL, 0), OSC2_TEST_OK);

	/* At the next boot: the buttons for `hold` x 100 ms, then up; the source and the level for
	 * the boot (an ARM keeps them), until SYS_INJECT_OFF. */
	sys_hooks_reset();
	stub.now_ms = 1000;
	b.flags = OSC2_TEST_SYS_ALL;
	b.buttons = 0x00010201u;
	b.level = 3;
	b.source = OSC2_SRC_BATTERY;
	b.hold = 5;
	osc2_test_sys_boot_pack(&b, words);
	osc2_test_sys_boot_apply(words, 1000);
	CHECK(osc2_test_sys_keep_on());     /* unarmed, but this boot came from an injection */
	CHECK(osc2_test_sys_buttons(&v));
	CHECK_EQ(v, 0x00010201u);
	CHECK_EQ(osc2_test_sys_next(1200), 300);
	CHECK(!osc2_test_sys_poll(1499));
	CHECK(osc2_test_sys_poll(1500));
	CHECK(osc2_test_sys_buttons(&v));
	CHECK_EQ(v, 0);                     /* let go: every button up */
	CHECK_EQ(osc2_test_sys_next(1600), -1);
	CHECK_EQ(arm_on(false, 0x62, 0, 0), OSC2_TEST_OK);
	CHECK(osc2_test_sys_level(&level));
	CHECK_EQ(level, 3);
	CHECK(osc2_test_sys_source(&source));
	CHECK_EQ(source, OSC2_SRC_BATTERY);
	CHECK(!osc2_test_sys_buttons(&v));  /* the ARM ended the buttons */
	CHECK_EQ(con_op(OSC2_TEST_OP_SYS_STATUS, NULL, 0), OSC2_TEST_OK);
	CHECK_EQ(RES[7] & 0x1E, 0x1C);      /* level, source, boot */
	CHECK_EQ(con_op(OSC2_TEST_OP_SYS_INJECT_OFF, NULL, 0), OSC2_TEST_OK);
	CHECK(!osc2_test_sys_source(&source));
	CHECK(!osc2_test_sys_level(&level));
	CHECK_EQ(con_op(OSC2_TEST_OP_DISARM, NULL, 0), OSC2_TEST_OK);
	CHECK(!osc2_test_sys_keep_on());
	/* hold 0: held until SYS_INJECT_OFF. */
	sys_hooks_reset();
	b.hold = 0;
	osc2_test_sys_boot_pack(&b, words);
	osc2_test_sys_boot_apply(words, 0);
	CHECK_EQ(osc2_test_sys_next(100000), -1);
	CHECK(osc2_test_sys_buttons(&v));
	CHECK_EQ(v, 0x00010201u);
	osc2_test_sys_init();
	CHECK_EQ(hk.violations, 0);
}

OPENSC2_TEST(sys_hooks_ring)
{
	static const char *const want[] = {
		" SYS_STATE from=battery to=shutdown cause=steam-held transport=2 source=0",
		" LED_MODE mode=5 flags=4 level=50 full=3",
		" CUE cue=5 script=5 gain=-12 played=1",
		" SYS_END state=shutdown cause=steam-held end=2 how=4",
		" CHORD what=steam-hold chord=1",
		" SOURCE from=1 to=0 event=power-none",
		" IDLE history=0000",
	};
	char line[200];
	int base;

	sys_hooks_reset();
	/* Unarmed: nothing is noted. */
	osc2_test_sys_note_cue(5, 5, -12, true);
	CHECK_EQ(arm_on(false, 0x71, 0, 0), OSC2_TEST_OK);
	osc2_test_sys_note_state(OSC2_SYS_BATTERY, OSC2_SYS_SHUTDOWN, OSC2_SYS_EV_STEAM_HELD,
				 OSC2_TRANSPORT_PUCK1, OSC2_SRC_BATTERY);
	osc2_test_sys_note_led(OSC2_LED_OFF, 0x04, 50, 3);
	osc2_test_sys_note_cue(5, 5, -12, true);
	osc2_test_sys_note_end(OSC2_SYS_SHUTDOWN, OSC2_SYS_EV_STEAM_HELD, OSC2_SYS_END_REBOOT,
			       OSC2_TEST_SYS_HOW_TEST);
	osc2_test_sys_note_chord(OSC2_TEST_CHORD_STEAM_HOLD, OSC2_CHORD_STEAM);
	osc2_test_sys_note_source(OSC2_SRC_USB, OSC2_SRC_BATTERY, OSC2_SYS_EV_POWER_NONE);
	osc2_test_sys_note_idle(0);
	read_ring(0);
	base = find_kind(OSC2_TEST_KIND_SYS_STATE, 0);
	CHECK(base >= 0);
	CHECK_EQ(count_kind(OSC2_TEST_KIND_CUE), 1);
	for (int i = 0; base >= 0 && i < 7 && base + i < n_ents; i++) {
		struct entry *x = &ents[base + i];
		const char *kind;

		CHECK_EQ(x->link, OSC2_TEST_LINK_NONE);
		CHECK_EQ(x->kind, OSC2_TEST_KIND_SYS_STATE + i);
		osc2_test_fmt_entry(line, sizeof(line), x->seq, x->raw, (size_t)(7 + x->len));
		kind = strchr(strstr(line, " none ") + 1, ' ');
		checks++;
		if (!kind || strcmp(kind, want[i]) != 0) {
			printf("  FAIL %s:%d: entry \"%s\", expected \"...%s\"\n", __FILE__, __LINE__,
			       line, want[i]);
			current_failed = 1;
		}
	}
	CHECK_EQ(ents[base + 2].d[2], (uint8_t)(int8_t)-12);
	CHECK(osc2_test_sys_kind_name(0x57) == NULL);
	CHECK(osc2_test_sys_kind_name(OSC2_TEST_KIND_SYS_FIRST) != NULL);
	CHECK_EQ(con_op(OSC2_TEST_OP_DISARM, NULL, 0), OSC2_TEST_OK);
	CHECK_EQ(hk.violations, 0);
}
