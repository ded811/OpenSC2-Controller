# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# settings: the LED and the controller settings (docs/testing.md, group "settings"). 0xC5 and 0x87
# make LED and SETTING entries in the ring and change what 0xE9, 0x89 and SETTINGS_READ read back
# (0xE9 and 0x89 with the real controller's read-back rules: a colour channel reads 0 unless it is
# 100, and 0x89 answers the defaults of the IMU's settings 48, 84 and 85); 0x8E restores the
# defaults; the ranged settings clamp; the Steam watchdog brings lizard mode and
# the defaults back 10 s after the last 0x81. Exact ring comparisons start right after a fresh 0x81
# (which leaves 10 s before the watchdog could add entries of its own). Every setting a test
# changes is written back before the next test.
import struct

from . import base
from .. import config as C
from ..proto import model as M
from ..proto import vendor as V

GROUP = "settings"
TESTS = [("settings.led_color", "WR", []), ("settings.imu_answers", "WR", []),
         ("settings.led_settings", "WR", []),
         ("settings.defaults", "WR", []), ("settings.clamps", "WR", []),
         ("settings.changes", "WR", []), ("settings.invalid", "WR", []),
         ("settings.watchdog", "WR", []), ("settings.watchdog_rearm", "W", []),
         ("settings.watchdog_off", "W", [])]


def _restore(rig, snap, numbers):
    """Write `numbers` back to `snap` (9 and 71 last)."""
    now = rig.settings_read()
    pairs = [(n, snap[n]) for n in numbers if n not in (9, 71) and now.get(n) != snap[n]]
    pairs += [(n, snap[n]) for n in (71, 9) if n in numbers and now.get(n) != snap[n]]
    for i in range(0, len(pairs), 4):
        rig.write_settings(pairs[i:i + 4])


def _entries(rig, start):
    return base.without_vendor(rig.ring_all(start)[2])


def _expiry(rig, entries):
    """The watchdog's entries: (the index of its LIZARD switches-up entry, or None)."""
    for i, e in enumerate(entries):
        if e["kind"] == C.RING_LIZARD and e.get("what") == C.LIZARD_SWITCHES_UP and \
                e["link"] == 3:
            return i
    return None


def _vendor_time(rig, start, opcode):
    for e in rig.ring_all(start)[2]:
        if e["kind"] == C.RING_VENDOR and e.get("opcode") == opcode and e.get("direction") == 0:
            return e["time_us"]
    return None


def run(ctx):
    rig = ctx.rig
    if not rig.ctx.armed:
        rig.arm()
    snap = rig.settings_read()

    if ctx.want("settings.led_color"):
        with ctx.check(GROUP, "settings.led_color") as c:
            base.need(rig, "settings")
            colour = rig.led_color()
            rig.lizard_off()
            start = rig.ring_next()
            # sent, kept (each channel clamped to 100); 0xE9 reads the kept colour back as the
            # real controller does: a channel of exactly 100 reads 100, anything else 0
            steps = (((100, 0, 50, 10), (100, 0, 50, 10)), ((0xFF, 0, 0, 0), (100, 0, 0, 0)),
                     ((100, 100, 100, 100), (100, 100, 100, 100)), ((99, 1, 100, 50),
                                                                    (99, 1, 100, 50)))
            for sent, kept in steps:
                rig.vendor_set(C.VC_LED_SET, bytes(sent))
                c.eq("0xE9 after %02X %02X %02X %02X" % sent, rig.led_color(),
                     tuple(M.led_readback(v) for v in kept))
            leds = base.of_kind(_entries(rig, start), C.RING_LED)
            c.eq("LED entries (the colour as kept)",
                 [(e["red"], e["green"], e["blue"], e["white"]) for e in leds],
                 [kept for _sent, kept in steps])
            if leds:
                c.eq("LED override and brightness as set", (leds[0]["override"],
                                                            leds[0]["brightness"]),
                     (1 if snap[37] else 0, max(0, min(100, snap[45]))))
            rig.vendor_set(C.VC_LED_SET, bytes(colour))

    if ctx.want("settings.imu_answers"):
        with ctx.check(GROUP, "settings.imu_answers") as c:
            base.need(rig, "settings")
            rig.lizard_off()
            rig.write_settings([(48, 0x18), (84, 0), (85, 50)])
            s = rig.settings_read()
            c.eq("SETTINGS_READ: the written values (what the application sees)",
                 (s[48], s[84], s[85]), (0x18, 0, 50))
            for n in M.IMU_SETTINGS:
                a = rig.vendor(C.VC_SETTINGS_GET, bytes([n]))
                c.eq("0x89 %d answers its default, as the real controller's IMU keeps it" % n,
                     (a.n, a.data[:1], V.decode_setting(a.data + bytes(3))["value"]),
                     (3, bytes([n]), M.setting_default(n)))
            rig.write_settings([(49, 2)])
            a = rig.vendor(C.VC_SETTINGS_GET, bytes([49]))
            c.eq("0x89 49 answers what was written", V.decode_setting(a.data + bytes(3))["value"], 2)
            _restore(rig, snap, (48, 49, 84, 85))

    if ctx.want("settings.led_settings"):
        with ctx.check(GROUP, "settings.led_settings") as c:
            base.need(rig, "settings")
            rig.lizard_off()
            rig.write_settings([(37, 0), (45, 50)])        # a known start
            start = rig.ring_next()
            for pair in ((37, 1), (45, 100), (45, 150), (45, -5), (37, 0)):
                rig.write_settings([pair])
            entries = _entries(rig, start)
            leds = [(e["override"], e["brightness"]) for e in base.of_kind(entries, C.RING_LED)]
            sets = [(e["number"], e["value"]) for e in base.of_kind(entries, C.RING_SETTING)]
            c.eq("LED entries (override, brightness)", leds,
                 [(1, 50), (1, 100), (1, 100), (1, 0), (0, 0)])
            c.eq("SETTING entries, only for changes", sets,
                 [(37, 1), (45, 100), (45, 0), (37, 0)])
            c.eq("each SETTING before its LED", base.kinds(entries),
                 [C.RING_SETTING, C.RING_LED, C.RING_SETTING, C.RING_LED, C.RING_LED,
                  C.RING_SETTING, C.RING_LED, C.RING_SETTING, C.RING_LED])
            _restore(rig, snap, (37, 45))

    if ctx.want("settings.defaults"):
        with ctx.check(GROUP, "settings.defaults") as c:
            base.need(rig, "settings")
            if rig.guard_failed:
                c.skip("it checks the guard's diverted reports, and the guard canary failed")
            rig.lizard_off()
            rig.write_settings([(48, 0x10), (84, 7), (85, 33)])
            was_off = rig.settings_read()[9] == 0
            start = rig.ring_next()
            rig.vendor_set(C.VC_SETTINGS_LOAD_DEFAULTS)
            entries = _entries(rig, start)
            want = ([C.RING_LIZARD, C.RING_LIZARD_RPT, C.RING_LIZARD_RPT] if was_off else []) + \
                [C.RING_SETTING, C.RING_LED]
            c.eq("entries in order", base.kinds(entries), want)
            sets = base.of_kind(entries, C.RING_SETTING)
            c.true("a SETTING 0xFF entry", sets and sets[-1]["number"] == 0xFF)
            s = rig.settings_read()
            bad = [n for n in range(86) if n not in (48, 84, 85) and s[n] != M.setting_default(n)]
            c.eq("settings not at their defaults", bad, [])
            c.eq("48, 84, 85 kept", (s[48], s[84], s[85]), (0x10, 7, 33))
            _restore(rig, snap, range(86))
            rig.lizard_off()

    if ctx.want("settings.clamps"):
        with ctx.check(GROUP, "settings.clamps") as c:
            base.need(rig, "settings")
            for n in (9, 11, 12, 14, 36, 45, 68, 70, 71):
                lo, hi = M.setting_range(n)
                values = [lo - 1] + ([hi + 1] if hi < 32767 else []) + [-32768, 32767]
                for v in values:
                    rig.write_settings([(n, v)])
                    c.eq("setting %d = %d reads back" % (n, v), rig.settings_read()[n],
                         M.setting_clamp(n, v))
                    if n in (9, 71):
                        _restore(rig, snap, (n,))
            for n in (2, 27, 50):
                for v in (12345, -1):
                    rig.write_settings([(n, v)])
                    c.eq("setting %d = %d (not clamped)" % (n, v), rig.settings_read()[n], v)
            _restore(rig, snap, range(86))
            rig.lizard_off()

    if ctx.want("settings.changes"):
        with ctx.check(GROUP, "settings.changes") as c:
            base.need(rig, "settings")
            rig.lizard_off()
            rig.write_settings([(27, snap[27])])
            start = rig.ring_next()
            for v in (-20, -20, snap[27]):
                rig.write_settings([(27, v)])
            sets = [(e["number"], e["value"]) for e in base.of_kind(_entries(rig, start),
                                                                    C.RING_SETTING)]
            c.eq("SETTING entries", sets, [(27, -20), (27, snap[27])] if snap[27] != -20 else [])

    if ctx.want("settings.invalid"):
        with ctx.check(GROUP, "settings.invalid") as c:
            base.need(rig, "settings")
            four = bytes([27, 0x05, 0x00, 0x00])
            if rig.path == "W":
                c.true("0x87 with 4 bytes is refused (STALL)", rig.refused(C.VC_SETTINGS_SET,
                                                                            four))
            else:
                c.eq("0x87 with 4 bytes: rc", rig.host_set(C.VC_SETTINGS_SET, four), -22)
            start = rig.ring_next()
            rig.vendor_set(C.VC_SETTINGS_SET, bytes([90]) + struct.pack("<h", 5) + bytes([27]) +
                           struct.pack("<h", -11))
            c.eq("27 = -11 taken, 90 ignored", rig.settings_read()[27], -11)
            c.true("no SETTING entry for 90", not [e for e in base.of_kind(
                _entries(rig, start), C.RING_SETTING) if e["number"] == 90])
            if rig.path == "W":
                with rig.exclusive():          # the SET and its GET: no keep-alive between
                    rig.vendor_set(C.VC_SETTINGS_GET, bytes([86]))
                    c.true("0x89 86: the GET STALLs", rig.refused_get())
            else:
                c.eq("HOST_SET 0x89 86", rig.host_set(C.VC_SETTINGS_GET, bytes([86])), 0)
                a = rig.host_get()
                c.eq("HOST_GET answers {0x89, 0}", (a.opcode, a.n), (C.VC_SETTINGS_GET, 0))
            _restore(rig, snap, (27,))

    if ctx.want("settings.watchdog"):
        with ctx.check(GROUP, "settings.watchdog") as c:
            base.need(rig, "watchdog")
            if rig.guard_failed:
                c.skip("it checks the guard's diverted reports, and the guard canary failed")
            if not rig.caps["build_flags"] & (1 << C.CAP_STEAM_WATCHDOG):
                c.skip("this build has no Steam watchdog")
            start = rig.ring_next()
            rig.lizard_off()
            t0 = _vendor_time(rig, start, C.VC_LIZARD_OFF)
            rig.sleep(12.0)
            entries = _entries(rig, start)
            i = _expiry(rig, entries)
            c.true("the watchdog ran", i is not None)
            if i is not None and t0 is not None:
                dt = (entries[i]["time_us"] - t0) / 1e6
                c.true("10.0 +- 0.3 s after the 0x81 (%.2f s)" % dt, abs(dt - 10.0) <= 0.3)
                tail = base.kinds(entries[i:])
                c.eq("in order: switches up, enter, the two diverted reports, SETTING 0xFF, LED",
                     tail[:6], [C.RING_LIZARD, C.RING_LIZARD, C.RING_LIZARD_RPT,
                                C.RING_LIZARD_RPT, C.RING_SETTING, C.RING_LED])
            s = rig.settings_read()
            c.eq("setting 9 = 1", s[9], 1)
            bad = [n for n in range(86) if n not in (48, 84, 85) and s[n] != M.setting_default(n)]
            c.eq("settings back at their defaults", bad, [])
            _restore(rig, snap, range(86))

    if ctx.want("settings.watchdog_rearm"):
        with ctx.check(GROUP, "settings.watchdog_rearm") as c:
            base.need(rig, "watchdog")
            start = rig.ring_next()
            rig.lizard_off()
            t0 = _vendor_time(rig, start, C.VC_LIZARD_OFF)
            rig.sleep(7.0)
            rig.vendor_set(C.VC_LIZARD_OFF)
            rig.sleep(7.0)
            rig.write_settings([(9, 0)])
            rig.sleep(6.0)
            rig.vendor_set(C.VC_LIZARD_DEFAULTS_ON)
            rig.sleep(6.0)
            entries = _entries(rig, start)
            i = _expiry(rig, entries)
            c.true("the watchdog ran", i is not None)
            if i is not None and t0 is not None:
                dt = (entries[i]["time_us"] - t0) / 1e6
                c.true("about 24 s after the start, 10 s after the last re-arm (%.2f s)" % dt,
                       abs(dt - 24.0) <= 0.4)
            _restore(rig, snap, range(86))

    if ctx.want("settings.watchdog_off"):
        with ctx.check(GROUP, "settings.watchdog_off") as c:
            base.need(rig, "watchdog")
            start = rig.ring_next()
            rig.write_settings([(71, 0)])
            rig.vendor_set(C.VC_LIZARD_OFF)
            rig.sleep(12.0)
            c.true("no expiry while 71 = 0", _expiry(rig, _entries(rig, start)) is None)
            start = rig.ring_next()
            rig.write_settings([(71, 1)])
            t1 = _vendor_time(rig, start, C.VC_SETTINGS_SET)
            rig.sleep(11.0)
            entries = _entries(rig, start)
            i = _expiry(rig, entries)
            c.true("an expiry after 71 -> 1", i is not None)
            if i is not None and t1 is not None:
                dt = (entries[i]["time_us"] - t1) / 1e6
                c.true("10 s after 71 -> 1 (%.2f s)" % dt, abs(dt - 10.0) <= 0.3)
            _restore(rig, snap, range(86))
