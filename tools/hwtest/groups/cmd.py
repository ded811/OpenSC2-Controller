# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# cmd: the vendor command channel on feature report 1 (docs/testing.md, group "cmd"), W, Steam
# closed: every request a SET then its GET, as Steam sends them. "Refused" is a STALL. Answers that
# hold serials, bonds or the chip ID are compared in memory only; the results say "equal" or give a
# CRC-32. Only the refusal forms safety.py allows are sent: the firmware refuses each before it
# writes anything. B: the same over Bluetooth, feature report 1 of the HID device Windows made of
# the board (a refusal is an ATT error there, which Windows turns into a failed request); not the
# USB framing, and never 0x9F, which turns a wireless controller's link off.
import struct

from . import base
from .. import config as C
from ..proto import vendor as V

GROUP = "cmd"
TESTS = [("cmd.attributes", "WB", []), ("cmd.strings", "WB", []),
         ("cmd.device_info", "WB", []), ("cmd.reset_info", "WB", []),
         ("cmd.user_store", "WB", []), ("cmd.by_name_read", "WB", []),
         ("cmd.by_name_refusals", "WB", []), ("cmd.by_name_harmless", "WB", []),
         ("cmd.save_refusals", "WB", []), ("cmd.calibration", "WB", []),
         ("cmd.unknown", "WB", []), ("cmd.framing", "W", []), ("cmd.stored_request", "WB", []),
         ("cmd.turn_off_ignored", "W", []), ("cmd.first_get", "W", []),
         ("cmd.no_link", "R", [])]

STEAM_C1 = bytes.fromhex("FFFFFFFF030905") + bytes([0xFF] * 9)
TRANSPORT = b"user/wireless_transport\0"
CHIME = b"user/haptic_boot_level\0"
BOND = b"esb/bond\0"
BOND2 = b"esb/bond_2\0"
_SERIAL_CHARS = set("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ")


def _serial_ok(text):
    return len(text) == 13 and set(text) <= _SERIAL_CHARS


def first_get(ctx):
    """cmd.first_get, right after modes.plain_reboot: a GET with no SET before it."""
    rig = ctx.rig
    with ctx.check(GROUP, "cmd.first_get") as c:
        if rig.path != "W":
            c.skip("the board came back wireless")
        a = rig.vendor_get()
        c.eq("the first GET after a boot answers 0x83", (a.opcode, a.n), (C.VC_ATTRIBUTES, 30))


def run(ctx):
    rig = ctx.rig

    if ctx.want("cmd.attributes"):
        with ctx.check(GROUP, "cmd.attributes") as c:
            a = rig.vendor(C.VC_ATTRIBUTES)
            c.eq("n", a.n, 30)
            pairs = V.decode_attributes(a.data)
            c.eq("tags in order", [t for t, _v in pairs], [1, 2, 10, 4, 9, 11])
            d = dict(pairs)
            c.eq("attribute 1 (product ID)", d.get(1), 0x1302)
            c.eq("attribute 2", d.get(2), 0)
            if rig.path == "B":
                st = rig.ble_link_status() or {}
                c.eq("attribute 11 = the connection interval in us (BLE_STATUS)", d.get(11),
                     st.get("interval_us"))
                c.observe(interval_us=d.get(11))
            else:
                c.eq("attribute 11", d.get(11), 4000)
            c.true("attribute 4 (build time) after 0x6A18D057", (d.get(4) or 0) > 0x6A18D057)
            if d.get(9) not in (0x45, 0x48, 0x49, 0x4A, 0x4B):
                c.warn("board revision 0x%x is none Steam knows (0x45, 0x48..0x4B)" % d.get(9, 0))
            rig.attributes_w = d
            ident = getattr(rig, "identity_r", None)
            if ident:
                c.eq("0x83 = opensc2 id", (d.get(1), d.get(10), d.get(4), d.get(9)),
                     (ident.get("pid"), ident.get("blbuild"), ident.get("build"),
                      ident.get("rev")))

    if ctx.want("cmd.strings"):
        with ctx.check(GROUP, "cmd.strings") as c:
            texts = {}
            for tag in (0, 1, 2, 3, 4, 5, 0xFF):
                a = rig.vendor(C.VC_STRING_ATTR, bytes([tag]))
                c.eq("tag %d: n" % tag, a.n, 20)
                c.eq("tag %d: the tag first" % tag, a.data[:1], bytes([tag]))
                texts[tag] = a.data[1:20].split(b"\0", 1)[0].decode("ascii", "replace")
            c.true("tag 1 = the HID serial string", texts[1] == rig.unit_serial)
            c.true("tags 0 and 4 = the board serial (13 characters 0-9/A-Z)",
                   texts[0] == texts[4] and _serial_ok(texts[0]))
            c.eq("tag 3 (git revision)", texts[3], rig.caps["git_revision"])
            for tag in (2, 5, 0xFF):
                c.eq("tag %d" % tag, texts[tag], "NA")
            a = rig.vendor(C.VC_STRING_ATTR, bytes([1]) + bytes(range(1, 21)))
            c.true("tag 1 with Steam's 21-byte payload answers tag 1",
                   a.n == 20 and a.data[:1] == b"\x01" and
                   a.data[1:14].decode("ascii", "replace") == rig.unit_serial)
            rig.board_serial_w = texts[0]

    if ctx.want("cmd.device_info"):
        with ctx.check(GROUP, "cmd.device_info") as c:
            a = rig.vendor(C.VC_DEVICE_INFO, b"\x00")
            c.eq("selector 0: n", a.n, 41)
            info = V.decode_device_info0(a.data + bytes(41))
            attrs = getattr(rig, "attributes_w", None) or dict(
                V.decode_attributes(rig.vendor(C.VC_ATTRIBUTES).data))
            c.eq("selector 0: build time = attribute 4", info["build_time"], attrs.get(4))
            c.eq("selector 0: revision = attribute 9", info["revision"], attrs.get(9))
            c.eq("selector 0: git revision", info["git_revision"], rig.caps["git_revision"])
            c.true("selector 0: the unit serial", info["serial"] == rig.unit_serial)
            # selector 1: the crash record (debug/resetreason) as it is; a new one would then be
            # marked seen and saved again, a flash write the suite leaves to a host (safety.py)
            record = rig.read_named(b"debug/resetreason\0")
            if record.n == 33 and record.data[0] == 1:
                c.note("selector 1 not read: a new crash record waits (reason %d; 0xED read it, "
                       "0xF2 would mark it seen, a flash write)" % record.data[21])
            else:
                a = rig.vendor(C.VC_DEVICE_INFO, b"\x01")
                c.eq("selector 1: n", a.n, 34)
                c.eq("selector 1: the selector, then the record 0xED reads",
                     (a.data[:1], a.data[1:34]), (b"\x01", record.data[:33]))
            a = rig.vendor(C.VC_DEVICE_INFO, b"\x02")
            c.eq("selector 2: n", a.n, 9)
            t1 = V.decode_device_info2(a.data)["uptime_ms"]
            rig.sleep(1.0)
            a = rig.vendor(C.VC_DEVICE_INFO, b"\x02")
            t2 = V.decode_device_info2(a.data)["uptime_ms"]
            c.true("selector 2: the uptime grew by 1000 +- 100 ms (%d)" % (t2 - t1),
                   900 <= t2 - t1 <= 1100)
            a = rig.vendor(C.VC_DEVICE_INFO, b"\x03")
            c.eq("selector 3: n (an answer, not a STALL)", a.n, 0)

    if ctx.want("cmd.reset_info"):
        with ctx.check(GROUP, "cmd.reset_info") as c:
            a = rig.vendor(C.VC_RESET_INFO, b"\x01")
            c.eq("selector 1: n", a.n, V.RESET_INFO_LEN)
            info = V.decode_reset_info(a.data + bytes(V.RESET_INFO_LEN))
            c.eq("selector 1: the selector", info["selector"], 1)
            if rig.models("hwinfo"):
                c.true("selector 1: a chip ID (masked)", any(info["chip_id"]))
            c.eq("selector 1: the last 8 bytes", a.data[10:18], bytes(8))
            c.eq("selector 1: only the real controller's flag bits", info["reset_cause"] &
                 ~(V.RESET_POWER_ON | V.RESET_BROWNOUT | V.RESET_OTHER | V.RESET_PIN |
                   V.RESET_WATCHDOG | V.RESET_SOFTWARE), 0)
            rig.mask.add_secret(info["chip_id"].hex(), "<chip>")
            if getattr(rig, "did_plain_reboot", False) and \
                    not info["reset_cause"] & V.RESET_SOFTWARE:
                c.warn("after the runner's reboot the reset flags 0x%02x have no software reset "
                       "(the UF2 bootloader may clear them)" % info["reset_cause"])
            c.observe(reset_cause="0x%02x (%s)" % (info["reset_cause"],
                                                   V.reset_cause_text(info["reset_cause"])))
            for sel in (0, 2, 0xFF):
                a = rig.vendor(C.VC_RESET_INFO, bytes([sel]))
                # never print the bytes: a chip ID answered here by mistake would be in them
                c.eq("selector %d: 18 bytes" % sel, a.n, 18)
                c.true("selector %d: all zero (%d of 18 bytes are not)"
                       % (sel, sum(1 for b in a.data[:18] if b)), not any(a.data[:18]))

    if ctx.want("cmd.user_store"):
        with ctx.check(GROUP, "cmd.user_store") as c:
            a = rig.vendor(C.VC_USER_STORE_GET)
            c.eq("0xDB", (a.n, a.data[:1]), (2, b"\x01"))
            chime = a.data[1] if a.n >= 2 else None
            c.true("0xDC with type 2 is refused", rig.refused(C.VC_USER_STORE_SET, b"\x02\x01"))
            a = rig.vendor(C.VC_USER_STORE_GET)
            c.eq("the chime level unchanged", a.data[1:2], bytes([chime]) if chime is not None
                 else b"")

    if ctx.want("cmd.by_name_read"):
        with ctx.check(GROUP, "cmd.by_name_read") as c:
            t = rig.read_named(TRANSPORT)
            c.eq("user/wireless_transport: n", t.n, 1)
            if rig.ctx.caps_ble:
                # A Bluetooth build (caps bit 17) also stores 0 and 1, the Bluetooth slots.
                c.true("user/wireless_transport: 0 to 3", t.n == 1 and t.data[0] <= 3)
                c.observe(wireless_transport=t.data[0] if t.n == 1 else None)
            else:
                c.true("user/wireless_transport: 2 + the slot",
                       t.n == 1 and t.data[0] in (2, 3))
            h = rig.read_named(CHIME)
            c.eq("user/haptic_boot_level: n", h.n, 1)
            u = rig.vendor(C.VC_USER_STORE_GET)
            c.eq("user/haptic_boot_level = 0xDB's level", h.data[:1], u.data[1:2])
            for name in (BOND, BOND2):
                b = rig.read_named(name)
                c.true("%s: n 24 or 0" % name[:-1].decode(), b.n in (0, 24))
                c.observe(**{name[:-1].decode().replace("/", "_"): "empty" if b.n == 0 else
                             "crc32 %08x" % b.crc})
            # the kept keys: their whole value (zeros until set), or 0 without a read function;
            # the charge window reads only while it is valid (none of the suite's doing)
            for name, (size, readable) in sorted(C.KEPT_KEYS.items()):
                a = rig.read_named(name.encode() + b"\0")
                if name == "mte/charge_level" and a.n == 0:
                    c.note("mte/charge_level holds no valid window: 0xED answers it empty")
                    continue
                c.eq("%s: n (%s)" % (name, "its size" if readable else "no read function"), a.n,
                     size if readable else 0)
                if name == "debug/resetreason" and a.n == 33:
                    c.observe(crash_record="none" if not any(a.data[:33]) else
                              "new %d, reason %d" % (a.data[0], a.data[21]))
            # the calibration keys: empty without the library's calibration (caps bit 21); with
            # it what the board's store shows (C.CAL_SHOWN), never written by the suite
            cal = bool((rig.caps or {}).get("build_flags", 0) & (1 << C.CAP_CAL))
            for name in sorted(C.CAL_KEYS):
                a = rig.vendor(C.VC_BY_NAME_READ, name.encode() + b"\0")
                size, shown = C.CAL_KEYS[name], C.CAL_SHOWN[name] if cal else "never"
                if shown == "always":
                    c.eq("%s: n (always shown)" % name, a.n, size)
                elif shown == "never":
                    c.eq("%s: n (%s)" % (name, "never shown" if cal else "no calibration kept"),
                         a.n, 0)
                else:
                    c.true("%s: n 0 or %d" % (name, size), a.n in (0, size))
                if name == "cal/joy_l":
                    c.observe(cal_joy_l="empty" if a.n == 0 else "stored")
            # the IMU's keys are set only (as on the real controller): nothing to read back
            a = rig.vendor(C.VC_BY_NAME_READ, b"settings/sensors/imu/use_bias\0")
            c.eq("settings/sensors/imu/use_bias: n 0 (set only)", a.n, 0)
            a = rig.vendor(C.VC_BY_NAME_READ, C.NO_KEY.encode() + b"\0")
            c.eq("a name that is no key: n", a.n, 0)
            a = rig.vendor(C.VC_BY_NAME_READ, TRANSPORT[:-1])
            c.eq("a name without its NUL: n", a.n, 0)

    if ctx.want("cmd.by_name_refusals"):
        with ctx.check(GROUP, "cmd.by_name_refusals") as c:
            before = [rig.read_named(n).data for n in (TRANSPORT, CHIME, BOND2)]
            c.true("0xEE user/wireless_transport with no value is refused",
                   rig.refused(C.VC_BY_NAME_SET, TRANSPORT))
            c.true("0xEE user/wireless_transport with two bytes is refused",
                   rig.refused(C.VC_BY_NAME_SET, TRANSPORT + b"\x02\x02"))
            c.true("0xEE esb/bond_2 with 23 bytes is refused",
                   rig.refused(C.VC_BY_NAME_SET, BOND2 + bytes(23)))
            c.true("0xEE cal/joy_l 00 is refused", rig.refused(C.VC_BY_NAME_SET,
                                                               b"cal/joy_l\0\x00"))
            # the IMU's keys take exactly their size (RAM only); nothing here changes the IMU
            c.true("0xEE settings/sensors/imu/mode with one byte is refused",
                   rig.refused(C.VC_BY_NAME_SET, b"settings/sensors/imu/mode\0\x01"))
            c.true("0xEE settings/sensors/imu/mounting_matrix with 8 bytes is refused",
                   rig.refused(C.VC_BY_NAME_SET,
                               b"settings/sensors/imu/mounting_matrix\0" + bytes(8)))
            c.true("0xEF settings/sensors/imu/use_bias (no read function) is refused",
                   rig.refused(C.VC_BY_NAME_SAVE, b"settings/sensors/imu/use_bias\0"))
            if rig.ctx.caps_identity_write:
                c.note("opensc2/unit_serial skipped: this build has an identity write path "
                       "(caps bit 7), covered by the host tests")
            else:
                c.true("0xEE opensc2/unit_serial bad! is refused",
                       rig.refused(C.VC_BY_NAME_SET, b"opensc2/unit_serial\0bad!"))
            kept = [n.encode() + b"\0" for n, (_s, readable) in sorted(C.KEPT_KEYS.items())
                    if readable]
            kept_before = [rig.read_named(n).data for n in kept]
            for name, (size, _readable) in sorted(C.KEPT_KEYS.items()):
                key = name.encode() + b"\0"
                c.true("0xEE %s with %d bytes is refused" % (name, size + 1),
                       rig.refused(C.VC_BY_NAME_SET, key + bytes(size + 1)))
                c.true("0xEE %s with no value is refused" % name,
                       rig.refused(C.VC_BY_NAME_SET, key))
            after = [rig.read_named(n).data for n in (TRANSPORT, CHIME, BOND2)]
            c.true("nothing changed (0xED re-read equal)", after == before)
            c.true("the kept keys unchanged (0xED re-read equal)",
                   [rig.read_named(n).data for n in kept] == kept_before)

    if ctx.want("cmd.by_name_harmless"):
        with ctx.check(GROUP, "cmd.by_name_harmless") as c:
            t = rig.read_named(TRANSPORT)
            h = rig.read_named(CHIME)
            b2 = rig.read_named(BOND2)
            if rig.ctx.caps_ble:
                c.note("0xEE user/wireless_transport 01 skipped: in a Bluetooth build (caps "
                       "bit 17) it selects Bluetooth slot 2")
            else:
                c.true("0xEE user/wireless_transport 01 (Bluetooth: ignored) is taken",
                       not rig.refused(C.VC_BY_NAME_SET, TRANSPORT + b"\x01"))
            c.true("0xEE user/wireless_transport with its value is taken",
                   not rig.refused(C.VC_BY_NAME_SET, TRANSPORT + t.data[:1]))
            c.true("0xEE user/haptic_boot_level with its value is taken",
                   not rig.refused(C.VC_BY_NAME_SET, CHIME + h.data[:1]))
            if b2.n == 0:
                c.true("0xEE esb/bond_2 with 24 zero bytes (it is empty) is taken",
                       not rig.refused(C.VC_BY_NAME_SET, BOND2 + bytes(24)))
            kept = [n.encode() + b"\0" for n, (_s, readable) in sorted(C.KEPT_KEYS.items())
                    if readable]
            values = [rig.read_named(n).data for n in kept]
            for key, value in zip(kept, values):
                if len(value) != C.KEPT_KEYS[key[:-1].decode()][0]:
                    continue        # an invalid charge window reads empty: nothing to rewrite
                c.true("0xEE %s with its value (RAM only) is taken" % key[:-1].decode(),
                       not rig.refused(C.VC_BY_NAME_SET, key + value))
            c.true("0xED re-reads unchanged",
                   [rig.read_named(n).data for n in (TRANSPORT, CHIME, BOND2)] ==
                   [t.data, h.data, b2.data] and [rig.read_named(n).data for n in kept] == values)

    if ctx.want("cmd.save_refusals"):
        with ctx.check(GROUP, "cmd.save_refusals") as c:
            b2 = rig.read_named(BOND2)
            h = rig.read_named(CHIME)
            if b2.n == 0:
                c.true("0xEF esb/bond_2 (empty) is refused", rig.refused(C.VC_BY_NAME_SAVE, BOND2))
            else:
                c.note("esb/bond_2 holds a bond: its 0xEF refusal is not sent")
            c.true("0xEF user/haptic_boot_level with a byte after the NUL is refused",
                   rig.refused(C.VC_BY_NAME_SAVE, CHIME + b"\x01"))
            c.true("0xEF user/keylock with a byte after the NUL is refused",
                   rig.refused(C.VC_BY_NAME_SAVE, b"user/keylock\0\x01"))
            c.true("0xF0 of a name that is no key is refused",
                   rig.refused(C.VC_BY_NAME_DELETE, C.NO_KEY.encode() + b"\0"))
            c.true("0xEF cal/joy_l with a byte after the NUL is refused",
                   rig.refused(C.VC_BY_NAME_SAVE, b"cal/joy_l\0\x00"))
            c.true("nothing stored", rig.read_named(BOND2).data == b2.data and
                   rig.read_named(CHIME).data == h.data)

    if ctx.want("cmd.calibration"):
        with ctx.check(GROUP, "cmd.calibration") as c:
            c.true("0xE2 00 is taken", not rig.refused(C.VC_CALIBRATION, b"\x00"))
            c.true("0xE2 01 20 (Steam's) is taken", not rig.refused(C.VC_CALIBRATION, b"\x01\x20"))
            c.true("0xE2 with n = 0 is taken", not rig.refused(C.VC_CALIBRATION, b""))
            c.true("0xE2 02 is refused", rig.refused(C.VC_CALIBRATION, b"\x02"))

    if ctx.want("cmd.unknown"):
        with ctx.check(GROUP, "cmd.unknown") as c, rig.exclusive():
            # one stored request is the subject: no keep-alive STATUS between these [R14]
            rig.vendor_set(C.VC_ATTRIBUTES)
            c.true("0xC1 (Steam's 16 bytes) is refused", rig.refused(0xC1, STEAM_C1))
            a = rig.vendor_get()
            c.eq("after 0xC1 the GET answers the request before", (a.opcode, a.n), (0x83, 30))
            c.true("0xBA is refused", rig.refused(0xBA))
            a = rig.vendor_get()
            c.eq("after 0xBA the GET answers the request before", (a.opcode, a.n), (0x83, 30))
            c.true("0x7E without the key is refused",
                   rig.probe_test_set(bytes([0, 0, 0, 0, 1, 5, C.OP_STATUS])))
            a = rig.vendor_get()
            c.eq("after 0x7E the GET answers the request before", (a.opcode, a.n), (0x83, 30))

    if ctx.want("cmd.framing"):
        with ctx.check(GROUP, "cmd.framing") as c:
            buf = bytearray(C.FEATURE_LEN)
            buf[0], buf[1], buf[2] = C.FEATURE_ID, C.VC_ATTRIBUTES, 62
            c.true("0x83 with n = 62 is refused", rig.refused_raw(bytes(buf)))
            cnt = rig.cnt_read()
            c.true("usb_frame_err counted it", cnt.get("usb_frame_err", 0) >= 1)
            if rig.models("usb_report_ids"):
                c.true("feature report 2 (SET, 02 83 00) is refused", rig.framing_probe("set2"))
                c.true("feature report 2 (GET) is refused", rig.framing_probe("get2"))
                c.true("HidD_SetOutputReport (an all-zero 0x80) is refused",
                       rig.framing_probe("set_output"))
                c.true("HidD_GetInputReport (0x42) is refused", rig.framing_probe("get_input"))
            else:
                c.note("report IDs other than 1: not modelled offline")

    if ctx.want("cmd.stored_request"):
        with ctx.check(GROUP, "cmd.stored_request") as c, rig.exclusive():
            # each step's SET and its separate GETs share USB's one stored request [R14]
            rig.write_settings([(45, rig.settings_read()[45])])
            c.true("a GET after a set-only command (0x87) STALLs", rig.refused_get())
            rig.vendor_set(C.VC_STRING_ATTR, b"\x01")
            c.true("0xC1 refused", rig.refused(0xC1, STEAM_C1))
            a = rig.vendor_get()
            c.eq("after a failed SET the GET answers 0xAE", (a.opcode, a.n, a.data[:1]),
                 (C.VC_STRING_ATTR, 20, b"\x01"))
            rig.vendor_set(C.VC_DEVICE_INFO, b"\x02")
            times = []
            for _ in range(3):
                times.append(V.decode_device_info2(rig.vendor_get().data)["uptime_ms"])
                rig.sleep(0.05)
            c.true("repeated GETs run 0xF2 02 again (the uptime grows)",
                   times[0] < times[1] < times[2])
            if rig.ctx.armed:
                # The test link has a stored request of its own: HOST_SET 0x89 09 (itself a
                # claimed 0x7E SET on USB) does not become USB's stored request, so the USB GET
                # after it answers the 0x7E, and HOST_GET answers the 0x89.
                rig.vendor_set(C.VC_STRING_ATTR, b"\x01")
                rc = rig.host_set(C.VC_SETTINGS_GET, b"\x09")
                usb = rig.vendor_get()
                test = rig.host_get()
                c.eq("HOST_SET 0x89 09", rc, 0)
                c.eq("the USB GET answers USB's last SET (the claimed 0x7E), not the 0x89",
                     usb.opcode, C.TEST_OPCODE)
                c.eq("HOST_GET answers the test link's request", (test.opcode, test.n,
                                                                  test.data[:1]),
                     (C.VC_SETTINGS_GET, 3, b"\x09"))

    if ctx.want("cmd.turn_off_ignored"):
        with ctx.check(GROUP, "cmd.turn_off_ignored") as c:
            start = rig.ring_next()
            c.true("0x9F taken", not rig.refused(C.VC_TURN_OFF))
            c.true("0x95 with the turn-off key taken",
                   not rig.refused(C.VC_POWER_REBOOT, C.KEY_TURN_OFF.to_bytes(4, "little")))
            rig.sleep(0.2)
            _f, _n, entries = rig.ring_all(start)
            c.eq("POWER entries within 200 ms", len(base.of_kind(entries, C.RING_POWER)), 0)
            reps = rig.read_stream(count=10)
            c.true("reports keep flowing", sum(1 for _t, r in reps if r[0] == C.RPT_STATE) >= 10)
            c.true("the device stays", rig.backend.discover().get("board") is not None)

    if ctx.want("cmd.no_link"):
        with ctx.check(GROUP, "cmd.no_link") as c:
            base.need(rig, "radio")
            with rig.own_link_drop():       # the puck's empty reports at the drop: no leak
                rig.console_run("opensc2 stop")
                rig.wait_link("off", 2.0)
                c.eq("HOST_SET 0x81 with the radio off", rig.host_set(C.VC_LIZARD_OFF), -22)
                c.eq("HOST_SET 0x85 with the radio off", rig.host_set(C.VC_LIZARD_DEFAULTS_ON),
                     -22)
                rig.console_run("opensc2 start")
                c.true("the link came back", rig.wait_link("connected", 5.0))
            rig.lizard_off()
