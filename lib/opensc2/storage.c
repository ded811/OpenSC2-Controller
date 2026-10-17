/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * storage.c: what the library keeps across reboots, in the Zephyr settings subtree "opensc2":
 *   opensc2/bond0, opensc2/bond1   the two bonds, 24 bytes each (the vendor command 0xA2 format)
 *   opensc2/slot                   the selected bond slot, 1 byte
 *   opensc2/chime                  the power-on chime level (vendor command 0xDC), 1 byte
 *   opensc2/id                     an identity override (opensc2_identity_store, the shell)
 * Without CONFIG_OPENSC2_STORAGE everything lives in RAM only.
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "internal.h"

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

#if defined(CONFIG_OPENSC2_STORAGE)
#include <zephyr/settings/settings.h>

/* opensc2/id: a version byte, the field mask, then the fields. */
#define ID_BLOB_VERSION 1
#define ID_BLOB_LEN     (2 + 14 + 14 + 13 + 2 + 4 + 4 + 4)

static void id_pack(const struct osc2_id_override *o, uint8_t out[ID_BLOB_LEN])
{
	out[0] = ID_BLOB_VERSION;
	out[1] = o->mask;
	memcpy(&out[2], o->id.unit_serial, 14);
	memcpy(&out[16], o->id.board_serial, 14);
	memcpy(&out[30], o->id.git_revision, 13);
	opensc2_put_le16(&out[43], o->id.product_id);
	opensc2_put_le32(&out[45], o->id.board_revision);
	opensc2_put_le32(&out[49], o->id.build_time);
	opensc2_put_le32(&out[53], o->id.bootloader_build_time);
}

static int id_unpack(const uint8_t in[ID_BLOB_LEN], struct osc2_id_override *o)
{
	if (in[0] != ID_BLOB_VERSION) {
		return -EINVAL;
	}
	memset(o, 0, sizeof(*o));
	o->mask = in[1] & OSC2_ID_ALL;
	memcpy(o->id.unit_serial, &in[2], 13);
	memcpy(o->id.board_serial, &in[16], 13);
	memcpy(o->id.git_revision, &in[30], 12);
	o->id.product_id = opensc2_get_le16(&in[43]);
	o->id.board_revision = opensc2_get_le32(&in[45]);
	o->id.build_time = opensc2_get_le32(&in[49]);
	o->id.bootloader_build_time = opensc2_get_le32(&in[53]);
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
		if (n == 1 && buf[0] < OPENSC2_BOND_SLOTS) {
			osc2.slot = buf[0];
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

int osc2_storage_save_bond(uint8_t slot)
{
	char key[16] = "opensc2/bond0";
	uint8_t buf[OPENSC2_BOND_LEN];

	key[12] = (char)('0' + slot);
	if (!opensc2_bond_valid(&osc2.bonds[slot])) {
		return settings_delete(key);
	}
	opensc2_bond_pack(&osc2.bonds[slot], buf);
	return settings_save_one(key, buf, sizeof(buf));
}

int osc2_storage_save_slot(void)
{
	return settings_save_one("opensc2/slot", &osc2.slot, 1);
}

int osc2_storage_save_chime(void)
{
	return settings_save_one("opensc2/chime", &osc2.chime, 1);
}

int osc2_storage_save_identity(void)
{
	uint8_t buf[ID_BLOB_LEN];

	if (osc2.stored.mask == 0) {
		return settings_delete("opensc2/id");
	}
	id_pack(&osc2.stored, buf);
	return settings_save_one("opensc2/id", buf, sizeof(buf));
}

int osc2_storage_factory_reset(void)
{
	/* What a factory reset forgets: pairings, the slot choice, the chime level. The identity
	 * override is kept, as the real controller keeps its calibration and serials. */
	int err = 0;

	err |= settings_delete("opensc2/bond0");
	err |= settings_delete("opensc2/bond1");
	err |= settings_delete("opensc2/slot");
	err |= settings_delete("opensc2/chime");
	return err ? -EIO : 0;
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
int osc2_storage_save_slot(void) { return 0; }
int osc2_storage_save_chime(void) { return 0; }
int osc2_storage_save_identity(void) { return 0; }
int osc2_storage_factory_reset(void) { return 0; }

#endif /* CONFIG_OPENSC2_STORAGE */
