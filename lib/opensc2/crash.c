/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * crash.c: crash records, as on the real controller (crash_rules.h). With
 * CONFIG_OPENSC2_CRASH_RECORD the library's fatal-error handler writes the record of a fatal error
 * into RAM that a reset keeps and resets the chip at once, without flushing logs or touching
 * flash. At the next boot opensc2_init() stores it as debug/resetreason (key_store.c) and clears
 * the RAM copy; vendor command 0xF2 with selector 1 answers it and then marks it seen, a flash
 * write from the system work queue. The RAM slot is in the devicetree region chosen as
 * opensc2,retained-ram, the one the USB session log uses (usblog.c leaves room for it), so a
 * bootloader that runs at every reset (the UF2 bootloader) does not overwrite it; without such a
 * region it is in no-init RAM, which such a bootloader may clear.
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include "internal.h"
#include "crash.h"
#include "key_store.h"

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

/* The firmware's own build time and revision (core.c's defaults): a crash record names the image
 * that crashed, not a build time stored on the board for Steam. */
#if CONFIG_OPENSC2_BUILD_TIME != 0
#define FIRMWARE_BUILD_TIME ((uint32_t)CONFIG_OPENSC2_BUILD_TIME)
#else
#define FIRMWARE_BUILD_TIME ((uint32_t)OPENSC2_CMAKE_BUILD_TIME)
#endif

/* ---- 0xF2 selector 1: the stored record, then marked seen -------------------------------------- */

static void save_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (osc2_keys_save(OSC2_KEY_RESET_REASON) != 0) {
		LOG_WRN("Could not save the crash record");
	}
}
static K_WORK_DEFINE(save_work, save_fn);

uint8_t osc2_crash_read(uint8_t out[OSC2_CRASH_ANSWER_LEN])
{
	uint8_t record[OSC2_KEY_VALUE_MAX], len = 0;

	if (osc2_keys_get(OSC2_KEY_RESET_REASON, record, &len) != 0 || len != OSC2_CRASH_LEN) {
		memset(record, 0, OSC2_CRASH_LEN);
	}
	if (osc2_crash_answer(record, out)) {
		/* Seen now; saved from the system work queue (this runs in the radio thread, or in the
		 * USB stack's inside the host's request). */
		(void)osc2_keys_set(OSC2_KEY_RESET_REASON, record, OSC2_CRASH_LEN);
		k_work_submit(&save_work);
	}
	return OSC2_CRASH_ANSWER_LEN;
}

#if defined(CONFIG_OPENSC2_CRASH_RECORD)

#if DT_HAS_CHOSEN(opensc2_retained_ram)
#include <zephyr/linker/devicetree_regions.h>
static struct osc2_crash_slot slot __aligned(4)
	__attribute__((__section__(LINKER_DT_NODE_REGION_NAME(DT_CHOSEN(opensc2_retained_ram)))));
#else
static struct osc2_crash_slot slot __aligned(4) __noinit;
#endif

BUILD_ASSERT(sizeof(struct osc2_crash_slot) == OSC2_CRASH_SLOT_SIZE,
	     "usblog.c leaves OSC2_CRASH_SLOT_SIZE bytes of the retained region for the crash slot");

/* Zephyr calls this after any fatal error (a fault, k_oops(), k_panic(), a failed stack check). */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	uint8_t record[OSC2_CRASH_LEN];
	uint32_t pc = 0, lr = 0;

#if defined(CONFIG_ARM)
	if (esf) {
		pc = esf->basic.pc;
		lr = esf->basic.lr;
	}
#else
	ARG_UNUSED(esf);
#endif
	osc2_crash_record(record, FIRMWARE_BUILD_TIME, CONFIG_OPENSC2_GIT_REVISION, reason, pc, lr);
	osc2_crash_slot_write(&slot, record);
	sys_reboot(SYS_REBOOT_COLD);   /* at once: no log flush, no flash write */
	CODE_UNREACHABLE;
}

void osc2_crash_boot(void)
{
	uint8_t stored[OSC2_KEY_VALUE_MAX], fresh[OSC2_CRASH_LEN], len = 0;

	if (osc2_keys_get(OSC2_KEY_RESET_REASON, stored, &len) != 0 || len != OSC2_CRASH_LEN) {
		memset(stored, 0, OSC2_CRASH_LEN);
	}
	if (!osc2_crash_slot_valid(&slot)) {
		osc2_crash_slot_clear(&slot);
		if (osc2_crash_empty(stored)) {
			LOG_INF("No crash record");
		} else {
			LOG_INF("Crash record (%s): reason %u, PC 0x%08x, LR 0x%08x",
				stored[OSC2_CRASH_AT_NEW] ? "not read yet" : "read",
				osc2_crash_field(stored, OSC2_CRASH_AT_REASON),
				osc2_crash_field(stored, OSC2_CRASH_AT_PC),
				osc2_crash_field(stored, OSC2_CRASH_AT_LR));
		}
		return;
	}
	memcpy(fresh, slot.record, sizeof(fresh));
	osc2_crash_slot_clear(&slot);
	LOG_WRN("New crash record: reason %u, PC 0x%08x, LR 0x%08x",
		osc2_crash_field(fresh, OSC2_CRASH_AT_REASON), osc2_crash_field(fresh, OSC2_CRASH_AT_PC),
		osc2_crash_field(fresh, OSC2_CRASH_AT_LR));
	if (osc2_keys_put(OSC2_KEY_RESET_REASON, fresh, sizeof(fresh),
			  osc2_crash_should_save(stored, fresh)) != 0) {
		LOG_WRN("Could not save the crash record");
	}
}

#else /* !CONFIG_OPENSC2_CRASH_RECORD */

void osc2_crash_boot(void)
{
}

#endif /* CONFIG_OPENSC2_CRASH_RECORD */
