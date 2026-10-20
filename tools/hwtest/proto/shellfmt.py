# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# shellfmt.py: the lines of the board's existing console commands, as lib/opensc2/shell.c prints
# them (its format strings, in Python's % notation, which these C formats share). The offline
# backends print with these, the runner's parsers (console.py) read them, and a self-test checks
# that every template here is still a format string of shell.c: a change there needs one here.
# FORMATS maps a name to (the C format string as it is in shell.c, the Python template).
FORMATS = {
    "status_mode": ("mode:      %s (%s), policy %s", "mode:      %s (%s), policy %s"),
    "status_usb": ("usb:       %s%s, %u reports, %u dropped, %u requests, %u stalls",
                   "usb:       %s%s, %d reports, %d dropped, %d requests, %d stalls"),
    "status_link": ("link:      %s (last reason %d), bond slot %u",
                    "link:      %s (last reason %d), bond slot %d"),
    "status_channels": ("channels:  %u, alternates %u %u %u; protocol %u; %d dBm",
                        "channels:  %d, alternates %d %d %d; protocol %d; %d dBm"),
    "status_traffic": ("traffic:   %u packets, %u reports, %u dropped, up %u s",
                       "traffic:   %d packets, %d reports, %d dropped, up %d s"),
    "status_lizard": ("lizard:    %s, IMU mode 0x%02x", "lizard:    %s, IMU mode 0x%02x"),
    "status_identity": ("identity:  %s / %s, pid 0x%04x, rev 0x%x, build 0x%08x, %s",
                        "identity:  %s / %s, pid 0x%04x, rev 0x%x, build 0x%08x, %s"),
    "last_none": ("last wired: none since the session log was set up",
                  "last wired: none since the session log was set up"),
    "last_session": ("last wired: boot %u, %u requests, %u stalls, %u outputs, %u reports, "
                     "%u dropped; ended: %s",
                     "last wired: boot %d, %d requests, %d stalls, %d outputs, %d reports, "
                     "%d dropped; ended: %s"),
    "mode": ("%s now (%s); policy %s", "%s now (%s); policy %s"),
    "mode_stored": ("policy %s stored", "policy %s stored"),
    "mode_stored_reboot": ("policy %s stored; rebooting %s", "policy %s stored; rebooting %s"),
    "mode_usage": ("mode auto (wired at a boot with USB power) or mode wireless (never)",
                   "mode auto (wired at a boot with USB power) or mode wireless (never)"),
    "usb_wired": ("%s%s; %u reports, %u dropped; %u requests, %u stalls, %u outputs",
                  "%s%s; %d reports, %d dropped; %d requests, %d stalls, %d outputs"),
    "usb_wireless": ("not wired in this boot (USB power %s)", "not wired in this boot (USB power %s)"),
    "usb_log_head": ("session log: %u of %u bytes; boot %u; the retained area %s at this boot",
                     "session log: %d of %d bytes; boot %d; the retained area %s at this boot"),
    "slot": ("bond slot %u", "bond slot %d"),
    "bond": ("slot %u%s: puck %s", "slot %d%s: puck %s"),
    "bond_empty": ("slot %u%s: empty", "slot %d%s: empty"),
    "id_unit": ("unit      %s", "unit      %s"),
    "id_board": ("board     %s", "board     %s"),
    "id_git": ("git       %s", "git       %s"),
    "id_pid": ("pid       0x%04x", "pid       0x%04x"),
    "id_rev": ("rev       0x%x", "rev       0x%x"),
    "id_build": ("build     0x%08x", "build     0x%08x"),
    "id_blbuild": ("blbuild   0x%08x", "blbuild   0x%08x"),
    "id_name": ("name      %s", "name      %s"),
    "state_buttons": ("buttons   0x%08x", "buttons   0x%08x"),
    "state_sticks": ("sticks    L %d %d  R %d %d", "sticks    L %d %d  R %d %d"),
    "state_triggers": ("triggers  L %u  R %u", "triggers  L %d  R %d"),
    "state_pad": ("pad %c     %s %d %d pressure %u%s", "pad %s     %s %d %d pressure %d%s"),
    "state_imu": ("imu       t %u  a %d %d %d  g %d %d %d  q %d %d %d %d",
                  "imu       t %d  a %d %d %d  g %d %d %d  q %d %d %d %d"),
    "reboot": ("rebooting%s", "rebooting%s"),
    # the wireless transports and Bluetooth (a build with CONFIG_OPENSC2_BLE)
    "status_transport": ("transport: %s (stored: %s)", "transport: %s (stored: %s)"),
    "status_ble_link": ("link:      Bluetooth %s (last reason %d), slot %u",
                        "link:      Bluetooth %s (last reason %d), slot %d"),
    "transport": ("running %s (%s), chosen %s, stored %s", "running %s (%s), chosen %s, stored %s"),
    "ble_status": ("bluetooth: %s%s; slot %u, identity %u %s",
                   "bluetooth: %s%s; slot %d, identity %d %s"),
    "ble_name": ("name:      %s", "name:      %s"),
    "ble_bonds": ("bonds:     identity 1 %s, 2 %s (slot 1); 3 %s, 4 %s (slot 2)",
                  "bonds:     identity 1 %s, 2 %s (slot 1); 3 %s, 4 %s (slot 2)"),
    "ble_connected": ("link:      connected%s, security %u, ATT MTU %u, interval %u us, "
                      "paced by %s",
                      "link:      connected%s, security %d, ATT MTU %d, interval %d us, "
                      "paced by %s"),
    "ble_traffic": ("traffic:   %u state reports, %u dropped",
                    "traffic:   %d state reports, %d dropped"),
    "ble_advertising": ("link:      advertising %s; %u s left of the window",
                        "link:      advertising %s; %d s left of the window"),
    "ble_link": ("link:      %s", "link:      %s"),
    "ble_last": ("last disconnect: reason 0x%02x", "last disconnect: reason 0x%02x"),
}

# `opensc2 ble status`: the advertising forms' words and the idle texts (shell.c)
BLE_FORM_TEXT = {0: "open to any host (pairing mode)", 1: "for the bonded host only",
                 2: "directed to the bonded host"}
BLE_IDLE_TEXT = {"idle": "idle", "window": "wireless off: no host within the advertising window",
                 "host": "wireless off: the host ended the link"}

# the texts shell.c prints for the enums (mode_reason_text, the state names)
MODE_REASON_TEXT = {0: "no USB power", 1: "USB power", 2: "warm restart", 3: "restart into wired",
                    4: "policy wireless", 5: "chosen at boot", 6: "no USB support",
                    7: "USB set-up failed"}
LINK_STATE_TEXT = ["off", "no bond", "pairing", "listening", "connected"]
USB_STATE_TEXT = ["off", "attached", "no host", "configured"]
# the same in the HWT STATUS line of "opensc2 test status" (test_core.c)
LINK_STATE_KEY = ["off", "no-bond", "pairing", "listening", "connected"]
USB_STATE_KEY = ["off", "attached", "no-host", "configured"]


def line(name, *values):
    return FORMATS[name][1] % values


def status_lines(mode_wired, reason, policy, lizard_on, imu_mode, ident, usb=None, link=None,
                 last=None, transport=None, ble_link=None):
    """`opensc2 status`. `usb`: (state, suspended, reports, dropped, requests, stalls); `link`:
    (state, last reason, slot 0/1, channel, alternates (3), protocol, rssi, packets, reports,
    dropped, up s); `last`: (boot, requests, stalls, outputs, reports, dropped, end text) or None;
    `ident`: dict unit, board, pid, rev, build, git. `transport`: (running name, stored name) for
    the transport line; `ble_link`: (state, last reason, slot 0/1) when Bluetooth runs."""
    out = [line("status_mode", "wired" if mode_wired else "wireless",
                MODE_REASON_TEXT.get(reason, "?"), policy)]
    if transport is not None:
        out.append(line("status_transport", transport[0], transport[1]))
    if mode_wired:
        state, suspended, reports, dropped, requests, stalls = usb
        out.append(line("status_usb", USB_STATE_TEXT[state], " (suspended)" if suspended else "",
                        reports, dropped, requests, stalls))
    elif ble_link is not None:
        state, reason_l, slot = ble_link
        out.append(line("status_ble_link", LINK_STATE_TEXT[state], reason_l, slot + 1))
    else:
        (state, reason_l, slot, channel, alternates, protocol, rssi, packets, reports, dropped,
         up_s) = link
        out.append(line("status_link", LINK_STATE_TEXT[state], reason_l, slot + 1))
        if state == 4:
            out.append(line("status_channels", channel, alternates[0], alternates[1],
                            alternates[2], protocol, rssi))
            out.append(line("status_traffic", packets, reports, dropped, up_s))
    out.append(line("status_lizard", "on" if lizard_on else "off", imu_mode))
    out.append(line("status_identity", ident["unit"], ident["board"], ident["pid"], ident["rev"],
                    ident["build"], ident["git"]))
    out.append(last_line(last))
    return out


def last_line(last):
    if last is None:
        return line("last_none")
    boot, requests, stalls, outputs, reports, dropped, end = last
    return line("last_session", boot, requests, stalls, outputs, reports, dropped,
                end or "(no clean end: reset or power loss)")


def id_lines(ident):
    return [line("id_unit", ident["unit"]), line("id_board", ident["board"]),
            line("id_git", ident["git"]), line("id_pid", ident["pid"]),
            line("id_rev", ident["rev"]), line("id_build", ident["build"]),
            line("id_blbuild", ident["blbuild"]), line("id_name", ident["name"])]


def ble_status_lines(st):
    """`opensc2 ble status` from a dict: enabled, running, slot (0/1), identity, address (text
    or None), name, bonded (four bools), connected, active, security, mtu, interval_us, trigger,
    reports, dropped, advertising, form (0..2), window_left_s, idle ("idle", "window", "host"),
    last_reason."""
    yes = ["yes" if b else "no" for b in st.get("bonded", [False] * 4)]
    out = [line("ble_status", "on" if st.get("enabled") else "off",
                "" if st.get("running", True) else " (not the transport)", st.get("slot", 0) + 1,
                st.get("identity", 1), st.get("address") or "-"),
           line("ble_name", st.get("name") or "-"),
           line("ble_bonds", *yes)]
    if st.get("connected"):
        out.append(line("ble_connected", " and active" if st.get("active") else
                        " (waiting for encryption and an ATT MTU of 64)", st.get("security", 0),
                        st.get("mtu", 23), st.get("interval_us", 0),
                        "the connection events" if st.get("trigger", True) else "a timer"))
        out.append(line("ble_traffic", st.get("reports", 0), st.get("dropped", 0)))
    elif st.get("advertising"):
        out.append(line("ble_advertising", BLE_FORM_TEXT[st.get("form", 0)],
                        st.get("window_left_s", 0)))
    else:
        out.append(line("ble_link", BLE_IDLE_TEXT[st.get("idle", "idle")]))
    if st.get("last_reason"):
        out.append(line("ble_last", st["last_reason"]))
    return out


def bond_lines(bonds, selected):
    """`bonds`: per slot the puck serial or None."""
    out = []
    for i, serial in enumerate(bonds):
        sel = " (selected)" if i == selected else ""
        out.append(line("bond", i + 1, sel, serial) if serial else
                   line("bond_empty", i + 1, sel))
    return out


def mode_line(mode_wired, reason, policy):
    return line("mode", "wired" if mode_wired else "wireless", MODE_REASON_TEXT.get(reason, "?"),
                policy)
