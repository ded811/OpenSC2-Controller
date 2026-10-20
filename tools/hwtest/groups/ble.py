# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# ble: the Bluetooth link itself (docs/testing.md, group "ble"), B only: a board of the Bluetooth
# build on its Bluetooth transport, paired with Windows. Through the console, which works while the
# link is down: BLE_STATUS (the link's state) and `opensc2 ble status`, BLE_DROP (the board ends the
# link and acts as if the host had, with the reason given) and BLE_WINDOW (a short advertising
# window); the ring's Bluetooth entries (advertising, its end, identities, bonds, disconnects,
# wireless off) are read the same way. Windows connects the board again by itself when it
# advertises for it. ble.window (--flash) opens pairing mode on the slot's other identity, which
# resets that identity (a settings write), and lets its window run out unanswered.
import struct

from . import base
from .. import config as C
from ..proto import console as cproto
from ..proto import reports as R

GROUP = "ble"
TESTS = [("ble.link", "B", []), ("ble.cut", "B", []), ("ble.reconnect", "B", []),
         ("ble.wireless_off", "B", []), ("ble.window", "B", ["flash"])]

LINK_CONNECTED = 4
LINK_OFF = 0
REASON_LISTEN_TIMEOUT = 18          # enum opensc2_reason
REASON_HOST_DISCONNECTED = 20
POWER_WIRELESS_OFF = 8              # enum opensc2_power_request
HCI_CONN_TIMEOUT = 0x08
HCI_REMOTE_USER_TERM = 0x13
BOND_KEPT = 2


def _ble_entries(rig, start):
    """The ring from `start`, through the console (the link may be down)."""
    return rig.ring_all(start, path=rig.console_path)[2]


def _events(entries, event):
    return [e for e in entries if e["kind"] == C.RING_BLE and e.get("event") == event]


def _ring_next(rig):
    st = rig.link_status()
    return st["ring_next"] if st else 0


def _wait_down(rig, timeout):
    """The link no longer connected (BLE_DROP ends it 100 ms after its answer)."""
    deadline = rig.clock.monotonic() + rig.t(timeout)
    while rig.clock.monotonic() < deadline:
        st = rig.link_status()
        if st and st["link_state"] != LINK_CONNECTED:
            return True
        rig.sleep(0.02)
    return False


def _wait_back(rig, c, what):
    """Windows connects the board again: the link connected, past its gate, and its HID device
    there again. True when it is."""
    if not rig.wait_link("connected", rig.reconnect_s):
        c.fail("%s: Windows did not connect the board again within %.0f s" % (what,
                                                                             rig.reconnect_s))
        return False
    st = rig.ble_link_status() or {}
    deadline = rig.clock.monotonic() + rig.t(5.0)
    while not st.get("active") and rig.clock.monotonic() < deadline:
        rig.sleep(0.2)
        st = rig.ble_link_status() or {}
    c.true("%s: connected and past the gate (encrypted, ATT MTU %s)" % (what, st.get("mtu")),
           bool(st.get("active")))
    return bool(st.get("active"))


def run(ctx):
    rig = ctx.rig
    if rig.path != "B":
        return
    if not rig.ctx.armed:
        rig.arm()

    if ctx.want("ble.link"):
        with ctx.check(GROUP, "ble.link") as c:
            st = rig.ble_link_status()
            c.true("BLE_STATUS answers (a Bluetooth build)", st is not None)
            if st:
                c.true("Bluetooth is the running transport", st["running"])
                c.true("connected and past the gate", st["connected"] and st["active"])
                c.true("encrypted (security level %d >= 2)" % st["security"], st["security"] >= 2)
                c.true("ATT MTU %d >= 64" % st["mtu"], st["mtu"] >= 64)
                c.true("five identities", st["identities"] == 5)
                c.true("the identity in use (%d) is the slot's (%s)" % (
                    st["identity"], (1, 2) if st["slot"] == 0 else (3, 4)),
                    st["identity"] in ((1, 2) if st["slot"] == 0 else (3, 4)))
                c.true("the identity in use holds the host's bond",
                       bool(st["bonded"] & (1 << st["identity"])))
                c.true("one bond per slot at most", bin(st["bonded"] & 0x06).count("1") <= 1 and
                       bin(st["bonded"] & 0x18).count("1") <= 1)
                c.true("a connection interval", st["interval_us"] > 0)
                if not st["trigger"]:
                    c.warn("the reports are paced by a timer: the connection-event trigger "
                           "could not be set")
                if st["interval_us"] > 7500:
                    c.note("Windows granted %d us, not the 7.5 ms asked for" % st["interval_us"])
                c.observe(interval_us=st["interval_us"], mtu=st["mtu"], security=st["security"],
                          slot=st["slot"] + 1, identity=st["identity"])
            shown = cproto.parse_ble_status(rig.console_run("opensc2 ble status"))
            c.true("opensc2 ble status: connected and active",
                   shown.get("connected") and shown.get("active"))
            c.true("opensc2 ble status: the name is the board's",
                   shown.get("name") == C.BLE_NAME_PREFIX + (rig.unit_serial or ""))
            c.eq("opensc2 ble status: the address is the one Windows connected",
                 cproto.address_hex(shown.get("address")),
                 getattr(rig.ctx.ble_expect, "address", None))

    if ctx.want("ble.cut"):
        with ctx.check(GROUP, "ble.cut") as c:
            state = R.make_state52(buttons=1, accel_x=111, gyro_z=-222, quat_w=0x4000,
                                   quat_x=-1, quat_y=2, quat_z=-3)
            rig.ready_to_inject()
            a, n = rig.inject(state)
            c.eq("INJECT", a["status"], C.ST_OK)
            got = rig.wait_report(lambda r: r[0] == C.RPT_BT_STATE and len(r) >= 6 and
                                  struct.unpack_from("<I", r, 30)[0] == n, rig.t(0.5), raw=True)
            c.true("a 0x45 carried the nonce", got is not None)
            if got:
                rep = got[1]
                injected = R.set_field(state, "imu_timestamp", n)
                c.eq("its 44 bytes of state: the injected ones up to the angular rate",
                     list(rep[2:2 + C.BLE_STATE52_KEPT]), list(injected[:C.BLE_STATE52_KEPT]))
                c.true("nothing after its 45 bytes: the quaternion is cut",
                       not any(rep[1 + C.BLE_STATE_LEN:]))
            rig.inject_off()

    if ctx.want("ble.reconnect"):
        with ctx.check(GROUP, "ble.reconnect") as c:
            # a lost link (a supervision timeout, reason 0x08): the same identity advertises again
            # 500 ms later, directed to its host first; Windows connects it again
            before = rig.ble_link_status() or {}
            start = _ring_next(rig)
            with rig.own_link_drop(grace_s=2.0):
                a = rig.test(C.OP_BLE_DROP, bytes([HCI_CONN_TIMEOUT]), path=rig.console_path)
                c.eq("BLE_DROP 0x08", a["status"], C.ST_OK)
                c.true("the link went down", _wait_down(rig, 2.0))
                back = _wait_back(rig, c, "after the drop")
            entries = _ble_entries(rig, start)
            disc = _events(entries, C.BLE_EV_DISCONNECT)
            adv = _events(entries, C.BLE_EV_ADV)
            c.true("a DISCONNECT entry, reason 0x08, then advertising again",
                   disc and disc[0]["a"] == HCI_CONN_TIMEOUT and disc[0]["b"] == 0)
            again = [e for e in adv if (e["c"] >> 16) & 0xFF == 1]
            c.true("advertising again on the same identity (%s)" % before.get("identity"),
                   again and (again[0]["c"] >> 8) & 0xFF == before.get("identity"))
            if disc and again:
                dt = (again[0]["time_us"] - disc[0]["time_us"]) / 1000.0
                c.true("500 ms after the disconnect (%.0f ms, +- 150)" % dt, abs(dt - 500) <= 150)
                c.eq("directed to the bonded host first", again[0]["a"], 2)
                forms = [C.BLE_FORM_NAMES.get(e["a"], "?") for e in adv]
                c.note("advertising: %s" % ", ".join(forms))
            if back:
                c.true("the board streams again",
                       rig.wait_report(lambda r: r[0] == C.RPT_STATE, rig.t(5.0)) is not None)

    if ctx.want("ble.wireless_off"):
        with ctx.check(GROUP, "ble.wireless_off") as c:
            # the host ends the link (0x13): wireless off; `opensc2 start` brings it back
            start = _ring_next(rig)
            with rig.own_link_drop(grace_s=2.0):
                a = rig.test(C.OP_BLE_DROP, bytes([HCI_REMOTE_USER_TERM]), path=rig.console_path)
                c.eq("BLE_DROP 0x13", a["status"], C.ST_OK)
                c.true("the link went off", rig.wait_link("off", 3.0))
                st = rig.ble_link_status() or {}
                c.true("BLE_STATUS: wireless off, not advertising",
                       st.get("wireless_off") and not st.get("advertising"))
                c.eq("the link's reason (the host left)", st.get("reason"),
                     REASON_HOST_DISCONNECTED)
                rig.sleep(1.0)
                st = rig.ble_link_status() or {}
                c.true("still off 1 s later (no advertising)", not st.get("advertising") and
                       st.get("link_state") == LINK_OFF)
                entries = _ble_entries(rig, start)
                rig.console_run("opensc2 start")
                _wait_back(rig, c, "after opensc2 start")
            disc = _events(entries, C.BLE_EV_DISCONNECT)
            off = _events(entries, C.BLE_EV_OFF)
            power = [e for e in entries if e["kind"] == C.RING_POWER]
            c.true("a DISCONNECT entry, reason 0x13, then wireless off",
                   disc and disc[0]["a"] == HCI_REMOTE_USER_TERM and disc[0]["b"] == 1)
            c.true("an OFF entry: the host", off and off[0]["a"] == 1)
            c.true("the power request WIRELESS_OFF (8)",
                   any(e.get("request") == POWER_WIRELESS_OFF for e in power))

    if ctx.want("ble.window"):
        with ctx.check(GROUP, "ble.window") as c:
            # --flash: a pairing request opens the slot's other identity (reset first: a settings
            # write); with a 3 s window and nobody to pair, wireless goes off; the slot's host
            # keeps its bond, and `opensc2 start` advertises for it again
            if not ctx.opt.get("flash"):
                c.skip("opt-in (--flash): a pairing request resets the slot's other identity")
            before = rig.ble_link_status() or {}
            start = _ring_next(rig)
            with rig.own_link_drop(grace_s=2.0):
                a = rig.test(C.OP_BLE_WINDOW, struct.pack("<I", 3000), path=rig.console_path)
                c.eq("BLE_WINDOW 3000 ms", a["status"], C.ST_OK)
                rig.console_run("opensc2 ble pair")
                c.true("the link went off (the window ran out)", rig.wait_link("off", 8.0))
                st = rig.ble_link_status() or {}
                entries = _ble_entries(rig, start)
                # a window not taken (the pairing request failed) must not shorten a later one
                rig.test(C.OP_BLE_WINDOW, struct.pack("<I", 0), path=rig.console_path)
                rig.console_run("opensc2 start")
                _wait_back(rig, c, "after opensc2 start")
            other = 1 + 2 * before.get("slot", 0) + (1 if before.get("identity") in (1, 3) else 0)
            ids = _events(entries, C.BLE_EV_IDENTITY)
            adv = _events(entries, C.BLE_EV_ADV)
            ends = _events(entries, C.BLE_EV_ADV_END)
            off = _events(entries, C.BLE_EV_OFF)
            c.true("the slot's other identity (%d) was reset" % other,
                   any(e["a"] == 1 and e["c"] == other for e in ids))
            c.true("pairing mode on it", any(e["a"] == 0 and (e["c"] >> 8) & 0xFF == other
                                             for e in adv))
            c.true("the window ran out (ADV_END 2) and wireless went off (OFF 0)",
                   any(e["a"] == 2 for e in ends) and off and off[0]["a"] == 0)
            c.eq("the link's reason (the window)", st.get("reason"), REASON_LISTEN_TIMEOUT)
            c.true("the slot's host kept its bond (identity %s)" % before.get("identity"),
                   bool(st.get("bonded", 0) & (1 << before.get("identity", 0))))
