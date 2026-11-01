/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * identity_set.c: opensc2_identity_set(), the board's identity changed in RAM at any time after
 * opensc2_init(), nothing stored. The real controller reads its identity from its own chip at
 * every start (the serials, the hardware ID, its firmware's and its bootloader's build times); a
 * board that learns its values later than opensc2_init() (a revision read from straps or an
 * EEPROM, a bootloader that leaves its build time somewhere) hands them over here.
 */
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "internal.h"

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

int opensc2_identity_set(const struct opensc2_identity *identity)
{
	struct opensc2_identity next;
	k_spinlock_key_t key;
	uint32_t problems;

	if (!osc2.initialised) {
		return -EACCES;
	}
	if (!identity) {
		return -EINVAL;
	}
	/* One change at a time, also against a host writing a stored field: the identity in use is
	 * worked out from both. */
	(void)k_mutex_lock(&osc2_id_lock, K_FOREVER);
	next = osc2.base;
	/* As opensc2_init() reads its identity: empty and zero fields keep what is there. */
	osc2_id_copy(&next, identity, osc2_id_fields_set(identity));
	problems = opensc2_identity_check(&next);
	if (problems != 0) {
		(void)k_mutex_unlock(&osc2_id_lock);
		LOG_ERR("opensc2_identity_set(): invalid identity (problems 0x%x)",
			(unsigned int)problems);
		return -EINVAL;
	}
	key = k_spin_lock(&osc2.lock);
	osc2.base = next;
	k_spin_unlock(&osc2.lock, key);
	osc2_identity_refresh();   /* a stored identity's fields still win */
	(void)k_mutex_unlock(&osc2_id_lock);
	return 0;
}
