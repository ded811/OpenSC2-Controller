/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * key_store.c: the blob keys of the settings-by-name commands (key_store.h): user/keylock,
 * user/fully_charged_led_intensity, mte/charge_level, debug/resetreason and settings/haptics/...
 * Their RAM values, and with CONFIG_OPENSC2_STORAGE the stored ones, in the Zephyr settings
 * subtree "opensc2/k" (its own handler: storage.c's "opensc2" handler does not see these keys).
 * A stored value of another size than its key's is not taken, as on the real controller.
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include "internal.h"
#include "key_store.h"

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

static struct osc2_blob_store blobs;
static struct k_spinlock blob_lock;
/* Loads, sets and command deletes of the charge window (mte/charge_level): the fuel gauge applies
 * it after each, as the real controller does after every load of the key, a value of the wrong
 * size included (the window then stays as it was), and after 0xF0 (which loads it again as empty:
 * no window). The factory reset's delete is none: the charger keeps its cap until the next boot. */
static atomic_t window_changes;

uint32_t osc2_keys_window_changes(void)
{
	return (uint32_t)atomic_get(&window_changes);
}

static void window_touched(uint8_t id)
{
	if (id == OSC2_KEY_CHARGE_WINDOW) {
		atomic_inc(&window_changes);
	}
}

static const struct osc2_key_info *blob_key(uint8_t id)
{
	const struct osc2_key_info *k = osc2_key_by_id(id);

	return k && k->kind == OSC2_KEYK_BLOB ? k : NULL;
}

#if defined(CONFIG_OPENSC2_STORAGE)
#include <stdio.h>
#include <zephyr/settings/settings.h>

static int keys_on_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	uint8_t buf[OSC2_KEY_VALUE_MAX];
	const struct osc2_key_info *k;
	const char *next;
	ssize_t n;

	if (settings_name_next(name, &next) == 0 || next) {
		return -ENOENT;
	}
	k = osc2_key_by_store(name);
	if (!k) {
		return -ENOENT;
	}
	window_touched(k->id);
	if (len != k->size) {
		return 0;   /* another size: the default stays */
	}
	n = read_cb(cb_arg, buf, len);
	if (n == (ssize_t)k->size) {
		k_spinlock_key_t lk = k_spin_lock(&blob_lock);

		(void)osc2_blob_set(&blobs, k->id, buf, (size_t)n);
		k_spin_unlock(&blob_lock, lk);
	}
	return n < 0 ? (int)n : 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(opensc2_keys, "opensc2/k", NULL, keys_on_set, NULL, NULL);

static void store_name(const struct osc2_key_info *k, char *out, size_t size)
{
	snprintf(out, size, "opensc2/k/%s", k->store);
}

static int store_save(const struct osc2_key_info *k)
{
	uint8_t value[OSC2_KEY_VALUE_MAX];
	char name[24];
	uint8_t len = 0;
	k_spinlock_key_t lk = k_spin_lock(&blob_lock);
	const uint8_t *v = osc2_blob_get(&blobs, k->id, &len);

	if (v) {
		memcpy(value, v, len);
	}
	k_spin_unlock(&blob_lock, lk);
	if (!v) {
		return -EINVAL;
	}
	store_name(k, name, sizeof(name));
	return settings_save_one(name, value, len);
}

static int store_delete(const struct osc2_key_info *k)
{
	char name[24];

	store_name(k, name, sizeof(name));
	return settings_delete(name);
}

static int store_value(const struct osc2_key_info *k, const uint8_t *value, uint8_t len)
{
	char name[24];

	store_name(k, name, sizeof(name));
	return settings_save_one(name, value, len);
}
#else
static int store_save(const struct osc2_key_info *k)
{
	ARG_UNUSED(k);
	return 0;
}

static int store_delete(const struct osc2_key_info *k)
{
	ARG_UNUSED(k);
	return 0;
}

static int store_value(const struct osc2_key_info *k, const uint8_t *value, uint8_t len)
{
	ARG_UNUSED(k);
	ARG_UNUSED(value);
	ARG_UNUSED(len);
	return 0;
}
#endif /* CONFIG_OPENSC2_STORAGE */

void osc2_keys_boot(void)
{
	k_spinlock_key_t lk = k_spin_lock(&blob_lock);

	osc2_blob_defaults(&blobs);
	k_spin_unlock(&blob_lock, lk);
}

int osc2_keys_set(uint8_t id, const uint8_t *value, uint8_t len)
{
	k_spinlock_key_t lk;
	int err;

	if (!blob_key(id)) {
		return -EINVAL;
	}
	lk = k_spin_lock(&blob_lock);
	err = osc2_blob_set(&blobs, id, value, len);
	k_spin_unlock(&blob_lock, lk);
	window_touched(id);   /* a refused size too, as on the real controller */
	return err;
}

int osc2_keys_save(uint8_t id)
{
	const struct osc2_key_info *k = blob_key(id);

	return k ? store_save(k) : -EINVAL;
}

int osc2_keys_delete(uint8_t id)
{
	const struct osc2_key_info *k = blob_key(id);
	k_spinlock_key_t lk;

	if (!k) {
		return -EINVAL;
	}
	lk = k_spin_lock(&blob_lock);
	(void)osc2_blob_reset(&blobs, id);
	k_spin_unlock(&blob_lock, lk);
	return store_delete(k);
}

int osc2_keys_delete_cmd(uint8_t id)
{
	int err = osc2_keys_delete(id);

	if (err == 0) {
		window_touched(id);
	}
	return err;
}

int osc2_keys_get(uint8_t id, uint8_t *out, uint8_t *len)
{
	k_spinlock_key_t lk;
	const uint8_t *v;

	if (!blob_key(id)) {
		return -EINVAL;
	}
	lk = k_spin_lock(&blob_lock);
	v = osc2_blob_get(&blobs, id, len);
	if (v) {
		memcpy(out, v, *len);
	}
	k_spin_unlock(&blob_lock, lk);
	return v ? 0 : -EINVAL;
}

int osc2_keys_put(uint8_t id, const uint8_t *value, uint8_t len, bool save)
{
	int err = osc2_keys_set(id, value, len);

	return err || !save ? err : osc2_keys_save(id);
}

int osc2_keys_store(uint8_t id, const uint8_t *value, uint8_t len)
{
	const struct osc2_key_info *k = blob_key(id);

	if (!k || len != k->size) {
		return -EINVAL;
	}
	return store_value(k, value, len);
}

int osc2_keys_factory_reset(void)
{
	int err = 0;

	/* Flash only: the RAM values stay until the reboot that follows, as on the real controller. */
	for (uint8_t id = 1; id < OSC2_KEY_COUNT; id++) {
		const struct osc2_key_info *k = blob_key(id);

		if (k && (k->flags & OSC2_KEYF_FACTORY) && store_delete(k) != 0) {
			err = -EIO;
		}
	}
	return err;
}
