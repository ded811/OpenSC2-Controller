/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * storage.c: what the library keeps across reboots, in the Zephyr settings subtree "opensc2":
 *   opensc2/bond0, opensc2/bond1   the two bonds, 24 bytes each (the vendor command 0xA2 format)
 *   opensc2/transport              the wireless transport, 1 byte: user/wireless_transport's
 *                                  values (0, 1 Bluetooth; 2, 3 the puck with bond 1 or 2; any
 *                                  other byte a host stored, which starts the main puck)
 *   opensc2/slot                   the puck's bond slot, 1 byte (written with a puck transport;
 *                                  firmware before the transport key reads only this one)
 *   opensc2/chime                  the power-on chime level (vendor command 0xDC), 1 byte
 *   opensc2/id                     an identity override (opensc2_identity_store, the shell)
 *   opensc2/name                   the stored USB product name, its characters (no NUL)
 *   opensc2/mode                   the mode policy (opensc2_policy_set), 1 byte
 * Without CONFIG_OPENSC2_STORAGE everything lives in RAM only.
 *
 * The by-name commands also reach every other Zephyr settings handler of the build, as on the real
 * controller (Bluetooth's "bt" keys, the application's own handlers): osc2_storage_named_*().
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "internal.h"
#include "system.h"

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

#if defined(CONFIG_OPENSC2_STORAGE)
#include <zephyr/settings/settings.h>

/* opensc2/id: a version byte, the field mask, then the fields. The product name has a key of its
 * own (opensc2/name), so this layout stays as it was and firmware without names still reads it. */
#define ID_BLOB_VERSION 1
#define ID_BLOB_LEN     (2 + 14 + 14 + 13 + 2 + 4 + 4 + 4)

static void id_pack(const struct osc2_id_override *o, uint8_t out[ID_BLOB_LEN])
{
	out[0] = ID_BLOB_VERSION;
	out[1] = o->mask & OSC2_ID_BLOB_FIELDS;
	memcpy(&out[2], o->id.unit_serial, 14);
	memcpy(&out[16], o->id.board_serial, 14);
	memcpy(&out[30], o->id.git_revision, 13);
	opensc2_put_le16(&out[43], o->id.product_id);
	opensc2_put_le32(&out[45], o->id.board_revision);
	opensc2_put_le32(&out[49], o->id.build_time);
	opensc2_put_le32(&out[53], o->id.bootloader_build_time);
}

/* The fields of opensc2/id into `o`; the name, which opensc2/name may have loaded before (the keys
 * come in any order), is kept. */
static int id_unpack(const uint8_t in[ID_BLOB_LEN], struct osc2_id_override *o)
{
	struct osc2_id_override blob;

	if (in[0] != ID_BLOB_VERSION) {
		return -EINVAL;
	}
	memset(&blob, 0, sizeof(blob));
	blob.mask = in[1] & OSC2_ID_BLOB_FIELDS;
	memcpy(blob.id.unit_serial, &in[2], 13);
	memcpy(blob.id.board_serial, &in[16], 13);
	memcpy(blob.id.git_revision, &in[30], 12);
	blob.id.product_id = opensc2_get_le16(&in[43]);
	blob.id.board_revision = opensc2_get_le32(&in[45]);
	blob.id.build_time = opensc2_get_le32(&in[49]);
	blob.id.bootloader_build_time = opensc2_get_le32(&in[53]);
	osc2_id_copy(&o->id, &blob.id, OSC2_ID_BLOB_FIELDS);
	o->mask = (uint8_t)((o->mask & ~OSC2_ID_BLOB_FIELDS) | blob.mask);
	return 0;
}

static int on_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;
	uint8_t buf[ID_BLOB_LEN];
	ssize_t n;

	if (len > sizeof(buf)) {
		return -EINVAL;
	}
	n = read_cb(cb_arg, buf, len);
	if (n < 0) {
		return (int)n;
	}

	for (int slot = 0; slot < OPENSC2_BOND_SLOTS; slot++) {
		static const char *const names[OPENSC2_BOND_SLOTS] = { "bond0", "bond1" };

		if (settings_name_steq(key, names[slot], &next) && !next) {
			if (n != OPENSC2_BOND_LEN) {
				return -EINVAL;
			}
			opensc2_bond_unpack(buf, &osc2.bonds[slot]);
			return 0;
		}
	}
	if (settings_name_steq(key, "slot", &next) && !next) {
		/* The transport key, when there is one, decides the slot (osc2_transport_init()). */
		if (n == 1 && buf[0] < OPENSC2_BOND_SLOTS && !osc2.transport_stored) {
			osc2.slot = buf[0];
		}
		return 0;
	}
	if (settings_name_steq(key, "transport", &next) && !next) {
		if (n == 1) {
			/* Any byte a host stored: it runs the puck with bond 1 unless it is 0, 1 or 3
			 * (transport_rules.h), and the key reads it back as it is. */
			uint8_t runs = osc2_transport_runs(buf[0], IS_ENABLED(CONFIG_OPENSC2_BLE));

			osc2.transport_saved = buf[0];
			osc2.transport_stored = true;
			if (osc2_transport_is_puck(runs)) {
				osc2.slot = (uint8_t)(runs - OSC2_TRANSPORT_PUCK_1);
			}
		}
		return 0;
	}
	if (settings_name_steq(key, "chime", &next) && !next) {
		if (n == 1) {
			osc2.chime = buf[0];
		}
		return 0;
	}
	if (settings_name_steq(key, "id", &next) && !next) {
		if (n == ID_BLOB_LEN) {
			(void)id_unpack(buf, &osc2.stored);
		}
		return 0;
	}
	if (settings_name_steq(key, "name", &next) && !next) {
		if (n >= 1 && n <= OPENSC2_PRODUCT_NAME_MAX) {
			memset(osc2.stored.id.product_name, 0, sizeof(osc2.stored.id.product_name));
			memcpy(osc2.stored.id.product_name, buf, (size_t)n);
			osc2.stored.mask |= OSC2_ID_NAME;   /* checked with the rest at refresh */
		}
		return 0;
	}
	if (settings_name_steq(key, "mode", &next) && !next) {
		if (n == 1 && buf[0] <= OPENSC2_POLICY_WIRELESS) {
			osc2.policy = (enum opensc2_policy)buf[0];
		}
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(opensc2, "opensc2", NULL, on_set, NULL, NULL);

int osc2_storage_load(void)
{
	int err = settings_subsys_init();

	if (err) {
		return err;
	}
	return settings_load_subtree("opensc2");
}

int osc2_storage_store_bond(uint8_t slot, const struct opensc2_bond *bond)
{
	char key[16] = "opensc2/bond0";
	uint8_t buf[OPENSC2_BOND_LEN];

	key[12] = (char)('0' + slot);
	if (!opensc2_bond_valid(bond)) {
		return settings_delete(key);
	}
	opensc2_bond_pack(bond, buf);
	return settings_save_one(key, buf, sizeof(buf));
}

int osc2_storage_save_bond(uint8_t slot)
{
	struct opensc2_bond b;
	k_spinlock_key_t lk = k_spin_lock(&osc2.lock);

	b = osc2.bonds[slot];
	k_spin_unlock(&osc2.lock, lk);
	return osc2_storage_store_bond(slot, &b);
}

int osc2_storage_save_transport(uint8_t t)
{
	int slot = osc2_transport_puck_slot(t);
	int err = settings_save_one("opensc2/transport", &t, 1);

	if (err == 0 && slot >= 0) {
		uint8_t s = (uint8_t)slot;

		err = settings_save_one("opensc2/slot", &s, 1);
	} else if (err == 0 && !osc2_transport_usable(t, true)) {
		/* A value that names no transport starts the main puck: no older slot choice. */
		err = settings_delete("opensc2/slot");
	}
	if (err == 0) {
		/* Stored, and read back into the key, as the real controller loads a key it saved. */
		osc2.transport_saved = t;
		osc2.transport_stored = true;
		osc2.transport = t;
	}
	return err;
}

int osc2_storage_delete_transport(void)
{
	int err = settings_delete("opensc2/transport");

	err |= settings_delete("opensc2/slot");
	if (err) {
		return -EIO;
	}
	osc2.transport_saved = OSC2_TRANSPORT_NONE;
	osc2.transport_stored = false;
	return 0;
}

int osc2_storage_delete_bond(uint8_t slot)
{
	char key[16] = "opensc2/bond0";

	key[12] = (char)('0' + slot);
	return settings_delete(key);
}

int osc2_storage_store_chime(uint8_t level)
{
	return settings_save_one("opensc2/chime", &level, 1);
}

int osc2_storage_save_chime(void)
{
	return osc2_storage_store_chime(osc2.chime);
}

int osc2_storage_save_identity(void)
{
	uint8_t buf[ID_BLOB_LEN];
	int err;

	if ((osc2.stored.mask & OSC2_ID_BLOB_FIELDS) == 0) {
		err = settings_delete("opensc2/id");
	} else {
		id_pack(&osc2.stored, buf);
		err = settings_save_one("opensc2/id", buf, sizeof(buf));
	}
	if (err) {
		return err;
	}
	if (!(osc2.stored.mask & OSC2_ID_NAME)) {
		return settings_delete("opensc2/name");   /* nothing written when there is none */
	}
	return settings_save_one("opensc2/name", osc2.stored.id.product_name,
				 strlen(osc2.stored.id.product_name));
}

int osc2_storage_save_policy(void)
{
	uint8_t policy = (uint8_t)osc2.policy;

	return settings_save_one("opensc2/mode", &policy, 1);
}

int osc2_storage_factory_reset(void)
{
	/* What a factory reset forgets: pairings, the transport choice, the chime level. The
	 * identity override and the mode policy are kept, as the real controller keeps its
	 * calibration and serials. */
	int err = 0;

	err |= settings_delete("opensc2/bond0");
	err |= settings_delete("opensc2/bond1");
	err |= osc2_storage_delete_transport();
	err |= settings_delete("opensc2/chime");
#if defined(CONFIG_OPENSC2_BLE)
	err |= osc2_ble_factory_reset();   /* the whole bt subtree: bonds, identities, the name */
#endif
	return err ? -EIO : 0;
}

/* ---- other settings handlers, by name ------------------------------------------------------- */

/* The library's own subtree ("opensc2", "opensc2/...") is never reached this way: a host must not
 * write the board's identity, bonds or policy under their internal names. */
#if defined(CONFIG_SETTINGS_RUNTIME)
static bool own_subtree(const char *name)
{
	return settings_name_steq(name, "opensc2", NULL) != 0;
}
#endif

int osc2_storage_named_set(const char *name, const void *value, size_t len)
{
#if defined(CONFIG_SETTINGS_RUNTIME)
	if (own_subtree(name)) {
		return -EINVAL;
	}
	return settings_runtime_set(name, value, len);
#else
	ARG_UNUSED(name);
	ARG_UNUSED(value);
	ARG_UNUSED(len);
	return -EINVAL;
#endif
}

int osc2_storage_named_get(const char *name, void *out, size_t size)
{
#if defined(CONFIG_SETTINGS_RUNTIME)
	if (own_subtree(name)) {
		return -EINVAL;
	}
	return (int)settings_runtime_get(name, out, size);
#else
	ARG_UNUSED(name);
	ARG_UNUSED(out);
	ARG_UNUSED(size);
	return -EINVAL;
#endif
}

int osc2_storage_named_save(const char *name, const void *value, size_t len)
{
#if defined(CONFIG_SETTINGS_RUNTIME)
	if (own_subtree(name)) {
		return -EINVAL;
	}
	return settings_save_one(name, value, len);
#else
	ARG_UNUSED(name);
	ARG_UNUSED(value);
	ARG_UNUSED(len);
	return -EINVAL;
#endif
}

int osc2_storage_named_delete(const char *name)
{
#if defined(CONFIG_SETTINGS_RUNTIME)
	int err;

	if (own_subtree(name)) {
		return 0;   /* nothing of that name is the host's to delete */
	}
	err = settings_delete(name);   /* a name nothing stored is no error */
	if (err == 0) {
		(void)osc2_storage_named_set(name, NULL, 0);   /* its handler's "deleted" value */
	}
	return err;
#else
	/* Without SETTINGS_RUNTIME no other handler is reached by name (as 0xEE and 0xEF find
	 * none): acknowledged, nothing deleted, so a host cannot remove another handler's keys
	 * (Bluetooth's bonds) from flash. */
	ARG_UNUSED(name);
	return 0;
#endif
}

#else /* !CONFIG_OPENSC2_STORAGE */

int osc2_storage_load(void)
{
	LOG_WRN("CONFIG_OPENSC2_STORAGE is off: bonds are forgotten at every reboot");
	return 0;
}
int osc2_storage_save_bond(uint8_t slot)
{
	ARG_UNUSED(slot);
	return 0;
}
int osc2_storage_store_bond(uint8_t slot, const struct opensc2_bond *bond)
{
	ARG_UNUSED(slot);
	ARG_UNUSED(bond);
	return 0;
}
int osc2_storage_save_transport(uint8_t t)
{
	osc2.transport_saved = t;
	osc2.transport_stored = true;
	osc2.transport = t;
	return 0;
}
int osc2_storage_delete_transport(void)
{
	osc2.transport_saved = OSC2_TRANSPORT_NONE;
	osc2.transport_stored = false;
	return 0;
}
int osc2_storage_delete_bond(uint8_t slot)
{
	ARG_UNUSED(slot);
	return 0;
}
int osc2_storage_save_chime(void) { return 0; }
int osc2_storage_store_chime(uint8_t level)
{
	ARG_UNUSED(level);
	return 0;
}
int osc2_storage_named_set(const char *name, const void *value, size_t len)
{
	ARG_UNUSED(name);
	ARG_UNUSED(value);
	ARG_UNUSED(len);
	return -EINVAL;
}
int osc2_storage_named_get(const char *name, void *out, size_t size)
{
	ARG_UNUSED(name);
	ARG_UNUSED(out);
	ARG_UNUSED(size);
	return -EINVAL;
}
int osc2_storage_named_save(const char *name, const void *value, size_t len)
{
	ARG_UNUSED(name);
	ARG_UNUSED(value);
	ARG_UNUSED(len);
	return -EINVAL;
}
int osc2_storage_named_delete(const char *name)
{
	ARG_UNUSED(name);
	return 0;
}
int osc2_storage_save_identity(void) { return 0; }
int osc2_storage_save_policy(void) { return 0; }
int osc2_storage_factory_reset(void) { return 0; }

#endif /* CONFIG_OPENSC2_STORAGE */
