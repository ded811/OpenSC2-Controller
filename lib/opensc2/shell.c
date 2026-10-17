/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * shell.c: "opensc2" shell commands: link status, pairing, bonds, the identity (stored on the
 * board, no rebuild needed) and the current input state.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include "internal.h"

static const char *const state_names[] = { "off", "no bond", "pairing", "listening",
					   "connected" };

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	struct opensc2_link_info info;
	struct opensc2_identity id;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (opensc2_link_info(&info)) {
		shell_error(sh, "not initialised");
		return -EACCES;
	}
	opensc2_identity_get(&id);
	shell_print(sh, "link:      %s (last reason %d), bond slot %u",
		    state_names[info.state], info.reason, info.slot + 1);
	if (info.state == OPENSC2_LINK_CONNECTED) {
		shell_print(sh, "channels:  %u, alternates %u %u %u; protocol %u; %d dBm",
			    info.channel, info.channels[1], info.channels[2], info.channels[3],
			    info.protocol, info.rssi_dbm);
		shell_print(sh, "traffic:   %u packets, %u reports, %u dropped, up %u s",
			    info.packets, info.reports, info.dropped, info.connected_ms / 1000);
	}
	shell_print(sh, "lizard:    %s, IMU mode 0x%02x", opensc2_lizard_mode() ? "on" : "off",
		    (unsigned int)opensc2_setting_get(OPENSC2_SETTING_IMU_MODE));
	shell_print(sh, "identity:  %s / %s, pid 0x%04x, rev 0x%x, build 0x%08x, %s",
		    id.unit_serial, id.board_serial, id.product_id, id.board_revision,
		    id.build_time, id.git_revision);
	return 0;
}

static int cmd_pair(const struct shell *sh, size_t argc, char **argv)
{
	uint16_t seconds = argc > 1 ? (uint16_t)strtoul(argv[1], NULL, 0) : 0;

	shell_print(sh, "Pairing into slot %u; open the puck's pairing window in Steam",
		    opensc2_selected_slot() + 1);
	return opensc2_pair_start(seconds);
}

static int cmd_cancel(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return opensc2_pair_cancel();
}

static int cmd_start(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return opensc2_start();
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return opensc2_stop();
}

static int cmd_slot(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		unsigned long slot = strtoul(argv[1], NULL, 0);

		if (slot < 1 || slot > OPENSC2_BOND_SLOTS) {
			shell_error(sh, "slot 1 or 2");
			return -EINVAL;
		}
		return opensc2_select_slot((uint8_t)(slot - 1));
	}
	shell_print(sh, "bond slot %u", opensc2_selected_slot() + 1);
	return 0;
}

static int cmd_bonds(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	for (uint8_t slot = 0; slot < OPENSC2_BOND_SLOTS; slot++) {
		struct opensc2_bond b;
		char serial[17] = { 0 };

		opensc2_bond_get(slot, &b);
		memcpy(serial, b.puck_serial, 16);
		if (opensc2_bond_valid(&b)) {
			/* The IDs identify the puck: show only that they exist. */
			shell_print(sh, "slot %u%s: puck %s", slot + 1,
				    slot == opensc2_selected_slot() ? " (selected)" : "", serial);
		} else {
			shell_print(sh, "slot %u%s: empty", slot + 1,
				    slot == opensc2_selected_slot() ? " (selected)" : "");
		}
	}
	return 0;
}

static int cmd_forget(const struct shell *sh, size_t argc, char **argv)
{
	struct opensc2_bond none = { 0 };
	unsigned long slot = strtoul(argv[1], NULL, 0);

	ARG_UNUSED(argc);
	if (slot < 1 || slot > OPENSC2_BOND_SLOTS) {
		shell_error(sh, "slot 1 or 2");
		return -EINVAL;
	}
	return opensc2_bond_set((uint8_t)(slot - 1), &none);
}

static int cmd_id_show(const struct shell *sh, size_t argc, char **argv)
{
	struct opensc2_identity id;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	opensc2_identity_get(&id);
	shell_print(sh, "unit      %s", id.unit_serial);
	shell_print(sh, "board     %s", id.board_serial);
	shell_print(sh, "git       %s", id.git_revision);
	shell_print(sh, "pid       0x%04x", id.product_id);
	shell_print(sh, "rev       0x%x", id.board_revision);
	shell_print(sh, "build     0x%08x", id.build_time);
	shell_print(sh, "blbuild   0x%08x", id.bootloader_build_time);
	return 0;
}

static int cmd_id_set(const struct shell *sh, size_t argc, char **argv)
{
	struct opensc2_identity id = { 0 };
	const char *field = argv[1];
	const char *value = argv[2];
	int err;

	ARG_UNUSED(argc);
	if (!strcmp(field, "unit") || !strcmp(field, "board")) {
		char *dst = field[0] == 'u' ? id.unit_serial : id.board_serial;

		if (strlen(value) != OPENSC2_SERIAL_LEN) {
			shell_error(sh, "a serial is exactly 13 characters, 0-9 and A-Z");
			return -EINVAL;
		}
		memcpy(dst, value, OPENSC2_SERIAL_LEN);
	} else if (!strcmp(field, "git")) {
		if (strlen(value) < 1 || strlen(value) > OPENSC2_GIT_REVISION_MAX) {
			shell_error(sh, "1 to 12 characters");
			return -EINVAL;
		}
		memcpy(id.git_revision, value, strlen(value));
	} else if (!strcmp(field, "pid")) {
		id.product_id = (uint16_t)strtoul(value, NULL, 0);
	} else if (!strcmp(field, "rev")) {
		id.board_revision = (uint32_t)strtoul(value, NULL, 0);
	} else if (!strcmp(field, "build")) {
		id.build_time = (uint32_t)strtoul(value, NULL, 0);
	} else if (!strcmp(field, "blbuild")) {
		id.bootloader_build_time = (uint32_t)strtoul(value, NULL, 0);
	} else {
		shell_error(sh, "fields: unit, board, git, pid, rev, build, blbuild");
		return -EINVAL;
	}
	err = opensc2_identity_store(&id);
	if (err == -EINVAL) {
		shell_error(sh, "not a valid identity");
	} else if (err) {
		shell_error(sh, "could not store it (%d)", err);
	} else {
		shell_print(sh, "stored; new connections use it");
	}
	return err;
}

static int cmd_id_clear(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "back to the built-in identity");
	return opensc2_identity_clear();
}

static int cmd_state(const struct shell *sh, size_t argc, char **argv)
{
	struct opensc2_state s;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	opensc2_get_state(&s);
	shell_print(sh, "buttons   0x%08x", s.buttons);
	shell_print(sh, "sticks    L %d %d  R %d %d", s.stick[0].x, s.stick[0].y, s.stick[1].x,
		    s.stick[1].y);
	shell_print(sh, "triggers  L %u  R %u", s.trigger[0], s.trigger[1]);
	for (int side = 0; side < 2; side++) {
		shell_print(sh, "pad %c     %s %d %d pressure %u%s", side ? 'R' : 'L',
			    s.pad[side].touched ? "touch" : "-", s.pad[side].x, s.pad[side].y,
			    s.pad[side].pressure, s.pad[side].clicked ? " click" : "");
	}
	shell_print(sh, "imu       t %u  a %d %d %d  g %d %d %d  q %d %d %d %d",
		    s.imu.timestamp_us, s.imu.accel[0], s.imu.accel[1], s.imu.accel[2],
		    s.imu.gyro[0], s.imu.gyro[1], s.imu.gyro[2], s.imu.quat[0], s.imu.quat[1],
		    s.imu.quat[2], s.imu.quat[3]);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_id,
	SHELL_CMD(show, NULL, "Show the identity in use", cmd_id_show),
	SHELL_CMD_ARG(set, NULL,
		      "Store one field on the board: set <unit|board|git|pid|rev|build|blbuild> "
		      "<value>", cmd_id_set, 3, 0),
	SHELL_CMD(clear, NULL, "Forget the stored identity", cmd_id_clear),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_opensc2,
	SHELL_CMD(status, NULL, "Link, identity and mode", cmd_status),
	SHELL_CMD_ARG(pair, NULL, "Pair with a puck: pair [seconds]", cmd_pair, 1, 1),
	SHELL_CMD(cancel, NULL, "Cancel pairing", cmd_cancel),
	SHELL_CMD(start, NULL, "Radio on", cmd_start),
	SHELL_CMD(stop, NULL, "Radio off", cmd_stop),
	SHELL_CMD_ARG(slot, NULL, "Show or select the bond slot: slot [1|2]", cmd_slot, 1, 1),
	SHELL_CMD(bonds, NULL, "List the bonds", cmd_bonds),
	SHELL_CMD_ARG(forget, NULL, "Delete a bond: forget <1|2>", cmd_forget, 2, 0),
	SHELL_CMD(id, &sub_id, "Identity", cmd_id_show),
	SHELL_CMD(state, NULL, "Current input state", cmd_state),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(opensc2, &sub_opensc2, "Steam Controller 2 emulation", NULL);
