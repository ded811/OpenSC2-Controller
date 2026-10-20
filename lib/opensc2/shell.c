/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * shell.c: "opensc2" shell commands: status, the mode and its policy, reboots, the USB session
 * log, pairing, bonds, the identity (stored on the board, no rebuild needed; the USB product name
 * among it, quoted when it has spaces) and the current input state. A wired board has no console
 * (HID only): read the session log afterwards in wireless mode.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include "internal.h"
#include "test_hooks.h"

static const char *const state_names[] = { "off", "no bond", "pairing", "listening",
					   "connected" };
static const char *const usb_state_names[] = { "off", "attached", "no host", "configured" };

static const char *mode_reason_text(enum opensc2_mode_reason reason)
{
	switch (reason) {
	case OPENSC2_MODE_NO_USB_POWER:  return "no USB power";
	case OPENSC2_MODE_USB_POWER:     return "USB power";
	case OPENSC2_MODE_WARM_RESTART:  return "warm restart";
	case OPENSC2_MODE_WIRED_RESTART: return "restart into wired";
	case OPENSC2_MODE_POLICY:        return "policy wireless";
	case OPENSC2_MODE_APP:           return "chosen at boot";
	case OPENSC2_MODE_NO_USB:        return "no USB support";
	case OPENSC2_MODE_USB_FAILED:    return "USB set-up failed";
	default:                         return "?";
	}
}

static const char *policy_text(enum opensc2_policy policy)
{
	return policy == OPENSC2_POLICY_WIRELESS ? "wireless" : "auto";
}

static void print_last_session(const struct shell *sh)
{
#if defined(CONFIG_OPENSC2_USB)
	struct osc2_usblog_info log;

	osc2_usblog_info(&log);
	if (log.session_boot == 0) {
		shell_print(sh, "last wired: none since the session log was set up");
		return;
	}
	shell_print(sh, "last wired: boot %u, %u requests, %u stalls, %u outputs, %u reports, "
		    "%u dropped; ended: %s", log.session_boot, log.requests, log.stalls,
		    log.outputs, log.reports, log.dropped,
		    log.end[0] ? log.end : "(no clean end: reset or power loss)");
#else
	ARG_UNUSED(sh);
#endif
}

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
	shell_print(sh, "mode:      %s (%s), policy %s",
		    opensc2_mode() == OPENSC2_MODE_WIRED ? "wired" : "wireless",
		    mode_reason_text(opensc2_mode_reason()), policy_text(opensc2_policy()));
#if OSC2_NO_HOST_WIRELESS_MS > 0
	shell_print(sh, "test aid:  charger test build: wired with no USB host for %u ms, it goes "
		    "wireless (not the real controller's behaviour)",
		    (unsigned int)OSC2_NO_HOST_WIRELESS_MS);
#endif
	shell_print(sh, "transport: %s (stored: %s)",
		    osc2_transport_name(osc2_transport_running()),
		    osc2.transport_saved == OSC2_TRANSPORT_NONE ? "none, the default" :
		    osc2_transport_name(osc2.transport_saved));
	if (opensc2_mode() == OPENSC2_MODE_WIRED) {
		struct opensc2_usb_info usb;

		opensc2_usb_info(&usb);
		shell_print(sh, "usb:       %s%s, %u reports, %u dropped, %u requests, %u stalls",
			    usb_state_names[usb.state], usb.suspended ? " (suspended)" : "",
			    usb.reports, usb.dropped, usb.requests, usb.stalls);
	} else if (osc2_transport_is_ble(info.transport)) {
		shell_print(sh, "link:      Bluetooth %s (last reason %d), slot %u",
			    state_names[info.state], info.reason, info.slot + 1);
	} else {
		shell_print(sh, "link:      %s (last reason %d), bond slot %u",
			    state_names[info.state], info.reason, info.slot + 1);
		if (info.state == OPENSC2_LINK_CONNECTED) {
			shell_print(sh, "channels:  %u, alternates %u %u %u; protocol %u; %d dBm",
				    info.channel, info.channels[1], info.channels[2],
				    info.channels[3], info.protocol, info.rssi_dbm);
			shell_print(sh, "traffic:   %u packets, %u reports, %u dropped, up %u s",
				    info.packets, info.reports, info.dropped,
				    info.connected_ms / 1000);
		}
	}
	shell_print(sh, "lizard:    %s, IMU mode 0x%02x", opensc2_lizard_mode() ? "on" : "off",
		    (unsigned int)opensc2_setting_get(OPENSC2_SETTING_IMU_MODE));
	shell_print(sh, "identity:  %s / %s, pid 0x%04x, rev 0x%x, build 0x%08x, %s",
		    id.unit_serial, id.board_serial, id.product_id, id.board_revision,
		    id.build_time, id.git_revision);
	print_last_session(sh);
	return 0;
}

/* ---- mode, policy, reboot -------------------------------------------------------------------- */

static int cmd_mode(const struct shell *sh, size_t argc, char **argv)
{
	enum opensc2_policy policy;
	bool changed, next_wired;
	int err;

	if (argc < 2) {
		shell_print(sh, "%s now (%s); policy %s",
			    opensc2_mode() == OPENSC2_MODE_WIRED ? "wired" : "wireless",
			    mode_reason_text(opensc2_mode_reason()), policy_text(opensc2_policy()));
		return 0;
	}
	if (!strcmp(argv[1], "auto")) {
		policy = OPENSC2_POLICY_AUTO;
	} else if (!strcmp(argv[1], "wireless")) {
		policy = OPENSC2_POLICY_WIRELESS;
	} else {
		shell_error(sh, "mode auto (wired at a boot with USB power) or mode wireless (never)");
		return -EINVAL;
	}
	changed = policy != opensc2_policy();
	err = opensc2_policy_set(policy);
	if (err) {
		shell_error(sh, "could not store it (%d)", err);
		return err;
	}
	/* Reboot only for a new policy that changes the next boot's mode: confirming the policy in
	 * use (say, wireless after a warm restart on USB power) leaves the board as it is. */
	next_wired = IS_ENABLED(CONFIG_OPENSC2_USB) && policy == OPENSC2_POLICY_AUTO &&
		     osc2_usb_power();
	if (!changed || next_wired == (opensc2_mode() == OPENSC2_MODE_WIRED)) {
		shell_print(sh, "policy %s stored", policy_text(policy));
		return 0;
	}
	/* Scheduled before the message: if the terminal closes the port now, the shell waits in the
	 * print until a terminal reads it again, and the reboot must not wait with it. */
	opensc2_reboot_later(OPENSC2_REBOOT_PLAIN, "shell: mode", 200);
	shell_print(sh, "policy %s stored; rebooting %s", policy_text(policy),
		    next_wired ? "into wired mode (no console then: hold the pairing button while "
				 "plugging in to come back wireless)" : "into wireless mode");
	return 0;
}

static int cmd_reboot(const struct shell *sh, size_t argc, char **argv)
{
	enum opensc2_reboot how = OPENSC2_REBOOT_PLAIN;

	if (argc > 1) {
		if (!strcmp(argv[1], "wireless")) {
			how = OPENSC2_REBOOT_WIRELESS;
		} else if (!strcmp(argv[1], "wired")) {
			how = OPENSC2_REBOOT_WIRED;
		} else {
			shell_error(sh, "reboot [wireless|wired]");
			return -EINVAL;
		}
	}
	/* Scheduled before the message, as for "mode" above. */
	opensc2_reboot_later(how, "shell: reboot", 100);
	shell_print(sh, "rebooting%s", how == OPENSC2_REBOOT_WIRELESS ? " into wireless mode" :
				       how == OPENSC2_REBOOT_WIRED ? " into wired mode" : "");
	return 0;
}

/* ---- the USB session log --------------------------------------------------------------------- */

#if defined(CONFIG_OPENSC2_USB)
static void print_line(const char *line, size_t len, void *ctx)
{
	shell_print((const struct shell *)ctx, "%.*s", (int)len, line);
}

static int cmd_usb(const struct shell *sh, size_t argc, char **argv)
{
	struct opensc2_usb_info usb;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	opensc2_usb_info(&usb);
	if (opensc2_mode() == OPENSC2_MODE_WIRED) {
		shell_print(sh, "%s%s; %u reports, %u dropped; %u requests, %u stalls, %u outputs",
			    usb_state_names[usb.state], usb.suspended ? " (suspended)" : "",
			    usb.reports, usb.dropped, usb.requests, usb.stalls, usb.outputs);
	} else {
		shell_print(sh, "not wired in this boot (USB power %s)",
			    osc2_usb_power() ? "present" : "absent");
	}
	print_last_session(sh);
	return 0;
}

static int cmd_usb_log(const struct shell *sh, size_t argc, char **argv)
{
	struct osc2_usblog_info log;

	osc2_usblog_info(&log);
	if (argc > 1) {
		if (strcmp(argv[1], "clear")) {
			shell_error(sh, "usb log [clear]");
			return -EINVAL;
		}
		osc2_usblog_clear();
		shell_print(sh, "session log cleared");
		return 0;
	}
	shell_print(sh, "session log: %u of %u bytes; boot %u; the retained area %s at this boot",
		    log.used, log.size, log.boots,
		    log.kept ? "was kept" : "was empty (power loss, or first start)");
	osc2_usblog_lines(print_line, (void *)sh);
	print_last_session(sh);
	return 0;
}
#endif

/* ---- pairing and bonds ----------------------------------------------------------------------- */

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

/* `transport [puck|puck2|ble|ble2] [save]`: show, or run another wireless transport. October's
 * rule stores the choice once its link is up; "save" stores it at once (as a host's 0xEE and 0xEF
 * of user/wireless_transport do), for a reboot into it. */
static int cmd_transport(const struct shell *sh, size_t argc, char **argv)
{
	int t;
	int err;

	if (argc < 2) {
		shell_print(sh, "running %s (%s), chosen %s, stored %s",
			    osc2_transport_word(osc2_transport_running()),
			    osc2_transport_name(osc2_transport_running()),
			    osc2_transport_word(osc2.transport),
			    osc2.transport_saved == OSC2_TRANSPORT_NONE ? "none" :
			    osc2_transport_word(osc2.transport_saved));
		return 0;
	}
	t = osc2_transport_from_word(argv[1]);
	if (t < 0 || (argc > 2 && strcmp(argv[2], "save") != 0)) {
		shell_error(sh, "transport [puck|puck2|ble|ble2] [save]");
		return -EINVAL;
	}
	err = opensc2_select_transport((enum opensc2_transport)t);
	if (err == -ENOTSUP) {
		shell_error(sh, "this build has no Bluetooth (CONFIG_OPENSC2_BLE)");
		return err;
	}
	if (!err && argc > 2) {
		err = osc2_storage_save_transport((uint8_t)t);
	}
	if (err) {
		shell_error(sh, "failed (%d)", err);
		return err;
	}
	shell_print(sh, "%s%s", osc2_transport_name((uint8_t)t),
		    argc > 2 ? ", stored" : opensc2_mode() == OPENSC2_MODE_WIRED ?
		    ", stored for the next wireless boot" : "; stored once its link is up");
	return 0;
}

#if defined(CONFIG_OPENSC2_BLE)
/* `ble [status]`: the Bluetooth link. The address is the board's own (the identity in use). */
static int cmd_ble_status(const struct shell *sh, size_t argc, char **argv)
{
	struct osc2_ble_status st;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	osc2_ble_status(&st);
	shell_print(sh, "bluetooth: %s%s; slot %u, identity %u %s", st.enabled ? "on" : "off",
		    !osc2_transport_is_ble(osc2_transport_running()) ? " (not the transport)" : "",
		    st.slot + 1, st.id, st.address[0] ? st.address : "-");
	shell_print(sh, "name:      %s", st.name[0] ? st.name : "-");
	if (st.connected) {
		shell_print(sh, "link:      connected%s, security %u, ATT MTU %u, interval %u us, "
			    "paced by %s", st.active ? " and active" : " (waiting for encryption and "
			    "an ATT MTU of 64)", st.security, st.mtu, st.interval_us,
			    st.trigger ? "the connection events" : "a timer");
		shell_print(sh, "traffic:   %u state reports, %u dropped", st.reports, st.dropped);
	} else {
		shell_print(sh, "link:      %s", st.advertising ? (st.open ? "advertising, open to any "
			    "host (pairing mode)" : "advertising for the bonded host") : "idle");
	}
	return 0;
}

static int cmd_ble_pair(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (!osc2_transport_is_ble(osc2_transport_running())) {
		shell_error(sh, "Bluetooth is not the transport: opensc2 transport ble");
		return -EBUSY;
	}
	shell_print(sh, "Pairing mode: the slot forgets its host and is open to a new one");
	return osc2_ble_pair();
}
#endif

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

/* ---- identity -------------------------------------------------------------------------------- */

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
	shell_print(sh, "name      %s", id.product_name);
	return 0;
}

/* What a value of `field` must look like, when it does not. */
static const char *id_value_hint(uint8_t field)
{
	switch (field) {
	case OSC2_ID_UNIT:
	case OSC2_ID_BOARD:
		return "a serial is exactly 13 characters, 0-9 and A-Z";
	case OSC2_ID_GIT:
		return "1 to 12 printable characters, no spaces";
	case OSC2_ID_PID:
		return "the product ID in hex with 0x, at most 0xffff (0x1302: Steam Controller 2)";
	case OSC2_ID_NAME:
		return "1 to 31 printable ASCII characters, no space at either end; a name with spaces "
		       "in quotes: id set name \"Steam Controller\"";
	default:
		return "a number in hex with 0x (0x48, 0x6a628345), or 0";
	}
}

/* One field, stored as given (0 too): `id set <field> <value> [force]`. */
static int cmd_id_set(const struct shell *sh, size_t argc, char **argv)
{
	struct opensc2_identity id = { 0 };
	uint8_t field = osc2_id_field(argv[1]);
	bool force = argc > 3 && !strcmp(argv[3], "force");
	int err;

	if (field == 0) {
		shell_error(sh, "fields: unit, board, git, pid, rev, build, blbuild, name");
		return -EINVAL;
	}
	if (field == OSC2_ID_NAME && argc > 3) {
		/* The shell splits at spaces outside quotes. */
		shell_error(sh, "a name with spaces goes in quotes: "
			    "id set name \"My Steam Controller\"");
		return -EINVAL;
	}
	if (argc > 3 && !force) {
		shell_error(sh, "id set <field> <value> [force]");
		return -EINVAL;
	}
	if (osc2_id_parse(field, argv[2], &id) != 0) {
		shell_error(sh, "%s", id_value_hint(field));
		return -EINVAL;
	}
	if (field == OSC2_ID_BUILD && id.build_time <= OSC2_BUILD_TIME_FLOOR) {
		if (!force) {
			shell_error(sh, "Steam forces an update on controllers built at or before "
				    "0x%08x, into a bootloader this board does not have: choose a "
				    "later time, or add \"force\" to store it anyway",
				    OSC2_BUILD_TIME_FLOOR);
			return -EINVAL;
		}
		shell_warn(sh, "at or before 0x%08x: Steam will want to update this controller",
			   OSC2_BUILD_TIME_FLOOR);
	}
	err = osc2_identity_store_fields(field, &id);
	if (err == -EINVAL) {
		shell_error(sh, "not a valid identity");
	} else if (err) {
		shell_error(sh, "could not store it (%d)", err);
	} else if (field == OSC2_ID_NAME) {
		shell_print(sh, "stored; the USB product string from the next wired boot");
	} else {
		shell_print(sh, "stored; new connections use it (USB: from the next boot)");
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
		      "Store one field on the board, as given (0 too): set "
		      "<unit|board|git|pid|rev|build|blbuild|name> <value> [force]; numbers in hex "
		      "with 0x, or 0; a name with spaces in quotes; \"force\" for a build time Steam "
		      "would force an update on",
		      cmd_id_set, 3, 1),
	SHELL_CMD(clear, NULL, "Forget the stored identity (the name too)", cmd_id_clear),
	SHELL_SUBCMD_SET_END);

#if defined(CONFIG_OPENSC2_BLE)
SHELL_STATIC_SUBCMD_SET_CREATE(sub_ble,
	SHELL_CMD(status, NULL, "The Bluetooth link", cmd_ble_status),
	SHELL_CMD(pair, NULL, "Pairing mode: open the slot to a new host (its old one is forgotten)",
		  cmd_ble_pair),
	SHELL_SUBCMD_SET_END);
#endif

#if defined(CONFIG_OPENSC2_USB)
SHELL_STATIC_SUBCMD_SET_CREATE(sub_usb,
	SHELL_CMD_ARG(log, NULL, "The USB session log of the last wired sessions: log [clear]",
		      cmd_usb_log, 1, 1),
	SHELL_SUBCMD_SET_END);
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(sub_opensc2,
	SHELL_CMD(status, NULL, "Mode, link, identity and the last wired session", cmd_status),
	SHELL_CMD_ARG(mode, NULL, "Show the mode, or store the policy: mode [auto|wireless]",
		      cmd_mode, 1, 1),
	SHELL_CMD_ARG(reboot, NULL, "Reboot: plain (back by the rule), wireless or wired once",
		      cmd_reboot, 1, 1),
#if defined(CONFIG_OPENSC2_USB)
	SHELL_CMD(usb, &sub_usb, "USB state and the session log", cmd_usb),
#endif
	SHELL_CMD_ARG(pair, NULL, "Pair with a puck over the air: pair [seconds]", cmd_pair, 1, 1),
	SHELL_CMD(cancel, NULL, "Cancel pairing", cmd_cancel),
	SHELL_CMD(start, NULL, "Radio on", cmd_start),
	SHELL_CMD(stop, NULL, "Radio off", cmd_stop),
	SHELL_CMD_ARG(slot, NULL, "Show or select the bond slot: slot [1|2]", cmd_slot, 1, 1),
	SHELL_CMD_ARG(transport, NULL,
		      "Show or run a wireless transport: transport [puck|puck2|ble|ble2] [save]",
		      cmd_transport, 1, 2),
#if defined(CONFIG_OPENSC2_BLE)
	SHELL_CMD(ble, &sub_ble, "Bluetooth LE: status, pair", cmd_ble_status),
#endif
	SHELL_CMD(bonds, NULL, "List the bonds", cmd_bonds),
	SHELL_CMD_ARG(forget, NULL, "Delete a bond: forget <1|2>", cmd_forget, 2, 0),
	SHELL_CMD(id, &sub_id, "Identity", cmd_id_show),
	SHELL_CMD(state, NULL, "Current input state", cmd_state),
#if defined(CONFIG_OPENSC2_TEST_HOOKS)
	SHELL_CMD_ARG(test, NULL,
		      "Hardware test hooks: test [<id>] sync | raw <hex> | status | arm <key> [<s>] | "
		      "disarm | ring [<from>] | cnt | settings | inject off | battery off | seq stop",
		      osc2_test_shell, 2, 6),
#endif
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(opensc2, &sub_opensc2, "Steam Controller 2 emulation", NULL);
