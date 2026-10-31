/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * key_store.h: the values of the blob keys (key_rules.h) the library keeps for vendor commands
 * 0xED, 0xEE, 0xEF and 0xF0, in RAM and, with CONFIG_OPENSC2_STORAGE, in Zephyr settings under
 * "opensc2/k/" (key_store.c; the host tests have a stand-in). Not a public interface. Keys are
 * numbered as enum osc2_key_id.
 */
#ifndef OPENSC2_KEY_STORE_H_
#define OPENSC2_KEY_STORE_H_

#include <stdbool.h>
#include <stdint.h>
#include "key_rules.h"

/* Every blob at its default: before the stored values load (opensc2_init()). */
void osc2_keys_boot(void);
/* 0xEE: the value in RAM only; -EINVAL unless exactly the key's size. */
int osc2_keys_set(uint8_t id, const uint8_t *value, uint8_t len);
/* 0xEF: the RAM value to flash. */
int osc2_keys_save(uint8_t id);
/* 0xF0: deleted from flash, and the RAM value back at its default. */
int osc2_keys_delete(uint8_t id);
/* The RAM value into `out` (OSC2_KEY_VALUE_MAX bytes of room), its size into `*len`. */
int osc2_keys_get(uint8_t id, uint8_t *out, uint8_t *len);
/* Set and, with `save`, store at once (the crash record at boot, after 0xF2 selector 1). */
int osc2_keys_put(uint8_t id, const uint8_t *value, uint8_t len, bool save);
/* Store `value` (exactly the key's size) for the next boot and leave the RAM value as it is (the
 * keylock chord, whose lock counts from the next power-on). */
int osc2_keys_store(uint8_t id, const uint8_t *value, uint8_t len);
/* The factory reset (0x86): every blob it deletes (OSC2_KEYF_FACTORY), in RAM and in flash. */
int osc2_keys_factory_reset(void);
/* How often the charge window (mte/charge_level) was loaded or set since boot: the fuel gauge
 * applies it after each change of this count. */
uint32_t osc2_keys_window_changes(void);

#endif /* OPENSC2_KEY_STORE_H_ */
