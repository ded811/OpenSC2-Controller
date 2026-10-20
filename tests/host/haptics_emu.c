/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * haptics_emu.c: the haptics synthesiser in the firmware emulator (fwemu.c, built with
 * CONFIG_OPENSC2_HAPTICS_SYNTH): the library's own logic (haptics_run.c and the engine under it)
 * on the emulator's clock, in place of haptics.c's threads. Each millisecond it does what the
 * firmware's threads would have done by then:
 *   - the work item: the script steps, drop sounds and re-arms that fell due;
 *   - each path's thread: an idle path applies what was queued, and once something plays renders
 *     its blocks at the times the firmware does without an output (the first three at once, then
 *     one every 8 ms, staying one block ahead), then drains for as long as the written blocks
 *     play, during which new commands wait.
 * The 0x44 reports go out as the application's would (fwemu_send()); the ring entries go to the
 * test hooks as in the firmware.
 */
#include <string.h>
#include "internal.h"
#include "haptics_glue.h"
#include "haptics_run.h"
#include "haptics_emu.h"
#include "test_hooks.h"

#define BLOCK_US 8000u

static struct osc2_hap_rt rt;
static uint32_t random_state;

static struct {
	bool playing, draining;
	uint64_t t0_us, drain_until_us;
	uint32_t k;                       /* blocks rendered in this playback */
	uint8_t block[OSC2_HAP_BLOCK_MAX_BYTES];
} seqs[OSC2_HAP_PATHS];

static uint64_t emu_now_us;

static uint32_t now_ms(void)
{
	return (uint32_t)(emu_now_us / 1000u);
}

/* What haptics.c's lock() refreshes: whether the hooks are armed, the master gain. */
static void sync(void)
{
	rt.notes = osc2_test_core_armed(&osc2_test_core_obj);
	rt.engine.master_db = osc2.settings[OPENSC2_SETTING_HAPTIC_GAIN_DB];
}

static void flush(void)
{
	struct osc2_hap_out_item it[8];
	uint8_t n;

	do {
		n = osc2_hap_rt_take(&rt, it, 8);
		for (uint8_t i = 0; i < n; i++) {
			if (it[i].what == OSC2_HAP_OUT_STATUS) {
				uint8_t report[OPENSC2_HAPTIC_STATUS_LEN];

				opensc2_encode_haptic_status(it[i].data[0],
							     opensc2_get_le32(&it[i].data[1]), report);
				fwemu_send(report, sizeof(report));
			} else {
				osc2_test_core_note(&osc2_test_core_obj, &osc2_test_env_obj, it[i].kind,
						    OSC2_TEST_LINK_NONE, it[i].data, it[i].len);
			}
		}
	} while (n == 8);
}

static uint8_t emu_random(void)
{
	random_state = random_state * 1103515245u + 12345u;
	return (uint8_t)(random_state >> 16);
}

void haptics_emu_boot(uint64_t now_us)
{
	osc2_hap_rt_init(&rt);
	memset(seqs, 0, sizeof(seqs));
	random_state = 1;
	emu_now_us = now_us;
}

void haptics_emu_tick(uint64_t now_us)
{
	emu_now_us = now_us;
	sync();
	if (osc2_hap_rt_next(&rt, now_ms()) == 0) {
		(void)osc2_hap_rt_poll(&rt, now_ms(), emu_random());
	}
	for (uint8_t p = 0; p < OSC2_HAP_PATHS; p++) {
		bool busy;

		if (seqs[p].draining) {
			if (now_us < seqs[p].drain_until_us) {
				continue;            /* new commands wait for the next start */
			}
			seqs[p].draining = false;
		}
		if (!seqs[p].playing) {
			if (!osc2_hap_rt_idle(&rt, p)) {
				continue;
			}
			seqs[p].playing = true;
			seqs[p].t0_us = now_us;
			seqs[p].k = 0;
		}
		while (seqs[p].playing) {
			/* blocks 0..2 at once, then block k at t0 + (k - 2) * 8 ms */
			uint64_t due = seqs[p].t0_us +
				       (seqs[p].k <= 2 ? 0u : (uint64_t)(seqs[p].k - 2) * BLOCK_US);

			if (now_us < due) {
				break;
			}
			(void)osc2_hap_rt_block(&rt, p, seqs[p].k == 0, seqs[p].block, &busy);
			seqs[p].k++;
			if (!busy) {
				seqs[p].playing = false;
				seqs[p].draining = true;
				seqs[p].drain_until_us = seqs[p].t0_us + (uint64_t)seqs[p].k * BLOCK_US;
			}
		}
	}
	flush();
}

void osc2_haptics_outputs(uint8_t link, const struct opensc2_output *out, int n)
{
	(void)link;
	if (n <= 0) {
		return;
	}
	sync();
	(void)osc2_hap_rt_feed(&rt, out, n, osc2.settings[OPENSC2_SETTING_RUMBLE_GAIN_DB], now_ms());
	flush();
}

void osc2_haptics_library(const struct opensc2_haptic *h, uint8_t n)
{
	sync();
	(void)osc2_hap_rt_clicks(&rt, h, n);
	flush();
}

uint8_t osc2_haptics_test_op(uint8_t op, const uint8_t *args, uint8_t alen, uint8_t *result,
			     uint8_t *rlen)
{
	struct osc2_hap_rt_glue glue = {
		.amp_mode = (uint8_t)osc2.settings[OPENSC2_SETTING_AMPLIFIER_MODE],
		.features = 1,                   /* the drop sounds, as in app/haptics.conf */
	};

	for (uint8_t p = 0; p < OSC2_HAP_PATHS; p++) {
		if (seqs[p].playing || seqs[p].draining) {
			glue.running |= (uint8_t)(1u << p);
		}
	}
	uint8_t status, paths;
	bool schedule;

	sync();
	status = osc2_hap_rt_test_op(&rt, op, args, alen, now_ms(), &glue, result, rlen, &paths,
				     &schedule);
	flush();
	return status;
}
