# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# cal: the library's processing of raw inputs (CONFIG_OPENSC2_CAL, caps bit 21; docs/testing.md,
# group "cal"). RAW_INJECT gives the board raw ADC counts, pressure words and capacitive words as
# an application would, and the board's own reports show what the library made of them: the raw
# output of settings 46, 60 and 62, the uncalibrated rule (0) and the stick-touch and grip bits of
# settings 30, 31, 34 and 35. The pad rules (rotation, settings 2 and 24; the stabiliser, setting
# 80 = 2) are checked on INJECTed pads. Nothing here writes calibration (safety rule S5): what the
# stored calibration does is in the host tests (tests/host/cal_store_cases.h). Settings come back
# at the end of arming (the ARM restore flag), and each test writes its settings back too.
from .. import config as C
from ..proto import reports as R

GROUP = "cal"
TESTS = [("cal.status", "WR", []), ("cal.raw_sticks", "WR", []), ("cal.raw_triggers", "WR", []),
         ("cal.raw_pressure", "WR", []), ("cal.capacitive", "WR", []),
         ("cal.rotation", "WR", []), ("cal.stabiliser", "WR", []), ("cal.raw_off", "WR", [])]

LSTICK_TOUCH = 1 << R.BIT["LSTICK_TOUCH"]
RSTICK_TOUCH = 1 << R.BIT["RSTICK_TOUCH"]
LGRIP = 1 << R.BIT["LGRIP"]
RGRIP = 1 << R.BIT["RGRIP"]
LPAD_TOUCH = 1 << R.LPAD_TOUCH_BIT
RPAD_TOUCH = 1 << R.RPAD_TOUCH_BIT


def stick_mv(counts):
    """A stick reading in the firmware's millivolts: counts x 3300 / 4096, rounded down."""
    return min(counts, 4095) * 3300 // 4096


def trigger_mv(counts):
    return min(counts, 4095) * 1650 // 4096


def _fields(report):
    return R.decode_state52(R.state52_of_report(report))


def _need_cal(rig, c):
    if not (rig.caps or {}).get("build_flags", 0) & (1 << C.CAP_CAL):
        c.skip("this build has no CONFIG_OPENSC2_CAL")


def _raw(rig, c, what, count=1, **raw):
    """RAW_INJECT, then the first `count` reports built with it (decoded), or None."""
    a, n = rig.inject_raw(raw)
    if not c.eq("%s: RAW_INJECT" % what, a["status"], C.ST_OK):
        return None
    got = rig.nonce_reports(n, count)
    if not c.true("%s: %d report(s) built with it" % (what, count), len(got) >= count):
        return None
    return [_fields(r) for r in got]


def _settings(rig, pairs):
    rig.write_settings(pairs)


def run(ctx):
    rig = ctx.rig
    if not rig.ctx.armed:
        rig.arm()
    rig.lizard_off()
    status = {}

    if ctx.want("cal.status"):
        with ctx.check(GROUP, "cal.status") as c:
            _need_cal(rig, c)
            st = rig.cal_status()
            if not c.true("CAL_STATUS answers", st is not None):
                return
            status.update(st)
            c.eq("no raw injection yet", st["injected"], 0)
            c.observe(generation=st["generation"], usable="0x%02x" % st["usable"],
                      raw="0x%02x" % st["raw"], pad_rules=st["pad_rules"],
                      pressure_click=st["pressure_click"], steps=list(st["steps"]))
            # a usable stick calibration reads back with 0xED
            a = rig.vendor(C.VC_BY_NAME_READ, b"cal/joy_l\0")
            if st["usable"] & 0x01:
                c.eq("cal/joy_l of a calibrated left stick: n", a.n, 18)
            else:
                c.true("cal/joy_l without a usable calibration: n 0 or 18", a.n in (0, 18))
    usable = status.get("usable", 0)

    if ctx.want("cal.raw_sticks"):
        with ctx.check(GROUP, "cal.raw_sticks") as c:
            _need_cal(rig, c)
            if usable & 0x03:
                c.skip("the board holds a stick calibration (uncalibrated sticks are checked)")
            sticks = {"lstick_x": 1000, "lstick_y": 3000, "rstick_x": 2048, "rstick_y": 100}
            raw = dict(present=C.RAW_STICK_L | C.RAW_STICK_R, **sticks)
            try:
                f = _raw(rig, c, "uncalibrated", **raw)
                if f:
                    c.eq("uncalibrated sticks read 0",
                         [f[0][k] for k in sticks], [0, 0, 0, 0])
                _settings(rig, [(46, 1)])
                f = _raw(rig, c, "setting 46 = 1", **raw)
                if f:
                    c.eq("46 = 1: millivolts", [f[0][k] for k in sticks],
                         [stick_mv(v) for v in sticks.values()])
                _settings(rig, [(46, 2)])
                f = _raw(rig, c, "setting 46 = 2", **raw)
                if f:
                    c.eq("46 = 2: the counts", [f[0][k] for k in sticks], list(sticks.values()))
                _settings(rig, [(46, 0)])
                f = _raw(rig, c, "setting 46 = 0", **raw)
                if f:
                    c.eq("46 = 0: calibrated again (0)", [f[0][k] for k in sticks], [0, 0, 0, 0])
            finally:
                rig.raw_off()
                _settings(rig, [(46, 0)])

    if ctx.want("cal.raw_triggers"):
        with ctx.check(GROUP, "cal.raw_triggers") as c:
            _need_cal(rig, c)
            if usable & 0x0C:
                c.skip("the board holds a trigger calibration")
            raw = dict(present=C.RAW_TRIGGER_L | C.RAW_TRIGGER_R, ltrigger=4095, rtrigger=1234)
            try:
                f = _raw(rig, c, "uncalibrated", **raw)
                if f:
                    c.eq("uncalibrated triggers read 0",
                         (f[0]["left_trigger"], f[0]["right_trigger"]), (0, 0))
                _settings(rig, [(62, 1)])
                f = _raw(rig, c, "setting 62 = 1", **raw)
                if f:
                    c.eq("62 = 1: millivolts", (f[0]["left_trigger"], f[0]["right_trigger"]),
                         (trigger_mv(4095), trigger_mv(1234)))
                _settings(rig, [(62, 2)])
                f = _raw(rig, c, "setting 62 = 2", **raw)
                if f:
                    c.eq("62 = 2: the counts", (f[0]["left_trigger"], f[0]["right_trigger"]),
                         (4095, 1234))
            finally:
                rig.raw_off()
                _settings(rig, [(62, 0)])

    if ctx.want("cal.raw_pressure"):
        with ctx.check(GROUP, "cal.raw_pressure") as c:
            _need_cal(rig, c)
            if usable & 0x30:
                c.skip("the board holds a pressure calibration")
            raw = dict(present=C.RAW_PRESSURE_L | C.RAW_PRESSURE_R, lpressure=2100,
                       rpressure=-40)
            try:
                f = _raw(rig, c, "uncalibrated", count=3, **raw)
                if f:
                    c.eq("no pressure without a calibration",
                         [(x["lpad_pressure"], x["rpad_pressure"]) for x in f], [(0, 0)] * 3)
                    c.eq("no click without a calibration",
                         [x["buttons"] & ((1 << R.BIT["LPAD_CLICK"]) |
                                          (1 << R.BIT["RPAD_CLICK"])) for x in f], [0] * 3)
                _settings(rig, [(60, 1)])
                f = _raw(rig, c, "setting 60 = 1", **raw)
                if f:
                    c.eq("60 = 1: the raw word, below 0 as 0, also untouched",
                         (f[0]["lpad_pressure"], f[0]["rpad_pressure"]), (2100, 0))
            finally:
                rig.raw_off()
                _settings(rig, [(60, 0)])

    if ctx.want("cal.capacitive"):
        with ctx.check(GROUP, "cal.capacitive") as c:
            _need_cal(rig, c)
            touch = C.RAW_TOUCH_L | C.RAW_TOUCH_R

            def bits(f):
                return f["buttons"] & (LSTICK_TOUCH | RSTICK_TOUCH | LGRIP | RGRIP)

            # Each step: the words, what the bits must be in every report built with them, and
            # why. The steps are chosen so that the first frame (the mean of the old and the new
            # word) and the frames after it (the new word alone) give the same bit, so a report
            # missed on the way does not matter.
            steps = [
                ({}, 0, "nothing touched"),
                ({"lstick_touch": 1100}, LSTICK_TOUCH, "stick touch on above 500 (means 550, 1100)"),
                ({"lstick_touch": 450}, LSTICK_TOUCH, "held while at least 400 (775, 450)"),
                ({"lstick_touch": 300}, 0, "off below 400 (375, 300)"),
                ({"lstick_touch": 450}, 0, "450 does not turn it on again (375, 450)"),
                ({"lstick_touch": 600}, LSTICK_TOUCH, "on again above 500 (525, 600)"),
                ({"rgrip": 2400}, RGRIP, "grip on above 1125, 34 = 100 (1200, 2400)"),
                ({"rgrip": 950}, RGRIP, "held while at least 900, 35 = 80 (1675, 950)"),
                ({"rgrip": 800}, 0, "off below 900 (875, 800)"),
                ({"rgrip": 1000}, 0, "1000 does not turn it on again (900, 1000)"),
            ]
            try:
                for words, want, why in steps:
                    f = _raw(rig, c, why, count=3, present=touch, **words)
                    if f:
                        c.eq(why, [bits(x) for x in f], [want] * 3)
                # setting 34 = 200: on at 2025, held while at least 1620 (the means start over)
                _settings(rig, [(34, 200)])
                f = _raw(rig, c, "34 = 200, grip 1900", count=3, present=touch, rgrip=1900)
                if f:
                    c.eq("1900 under the new on level: off", [bits(x) for x in f], [0] * 3)
                f = _raw(rig, c, "34 = 200, grip 4200", count=3, present=touch, rgrip=4200)
                if f:
                    c.eq("on above 2025 (3050, 4200)", [bits(x) for x in f], [RGRIP] * 3)
                # setting 30 = 800 raises the stick-touch level
                _settings(rig, [(34, 100), (30, 800)])
                f = _raw(rig, c, "30 = 800, stick 700", count=3, present=touch,
                         lstick_touch=700)
                if f:
                    c.eq("700 under 800: off", [bits(x) & LSTICK_TOUCH for x in f], [0] * 3)
            finally:
                rig.raw_off()
                _settings(rig, [(30, 500), (34, 100)])

    if ctx.want("cal.rotation"):
        with ctx.check(GROUP, "cal.rotation") as c:
            _need_cal(rig, c)
            if not status.get("pad_rules"):
                c.skip("this build has no CONFIG_OPENSC2_PAD_RULES")
            pads = R.make_state52(buttons=LPAD_TOUCH | RPAD_TOUCH, lpad_x=2000, lpad_y=0,
                                  rpad_x=2000, rpad_y=0)
            try:
                rep, _n = rig.inject_and_read(pads)
                if c.true("a report", rep is not None):
                    f = _fields(rep)
                    c.eq("with the defaults nothing turns",
                         (f["lpad_x"], f["lpad_y"], f["rpad_x"], f["rpad_y"]), (2000, 0, 2000, 0))
                _settings(rig, [(2, 90), (24, 1)])
                rep, _n = rig.inject_and_read(pads)
                if c.true("a report", rep is not None):
                    f = _fields(rep)
                    c.eq("90 degrees: the right pad counter-clockwise, the left clockwise",
                         (f["lpad_x"], f["lpad_y"], f["rpad_x"], f["rpad_y"]),
                         (0, -2000, 0, 2000))
                _settings(rig, [(2, 7)])
                rep, _n = rig.inject_and_read(R.make_state52(buttons=RPAD_TOUCH, rpad_x=20000,
                                                             rpad_y=-10000))
                if c.true("a report", rep is not None):
                    f = _fields(rep)
                    # setting 7: the right pad turns by 5 degrees, at half scale with y before
                    # the negation (x' = (c x + s y) / 8192, y' = (c y - s x) / 8192), doubled
                    c.eq("7 degrees turn the right pad by 5", (f["rpad_x"], f["rpad_y"]),
                         (20792, -8220))
                _settings(rig, [(24, 0)])
                rep, _n = rig.inject_and_read(pads)
                if c.true("a report", rep is not None):
                    f = _fields(rep)
                    c.eq("24 = 0: off again", (f["rpad_x"], f["rpad_y"]), (2000, 0))
            finally:
                rig.inject_off()
                _settings(rig, [(2, 0), (24, 20)])

    if ctx.want("cal.stabiliser"):
        with ctx.check(GROUP, "cal.stabiliser") as c:
            _need_cal(rig, c)
            if not status.get("pad_rules"):
                c.skip("this build has no CONFIG_OPENSC2_PAD_RULES")
            steps = [(1000, 1000), (3000, 1000), (5400, 1000), (5500, 1000), (6500, 1000)]
            want = [1000, 1000, 1000, 1651, 3133]
            try:
                rig.inject_and_read(R.make_state52())   # untouched: the stabiliser starts over
                _settings(rig, [(80, 2)])
                got = []
                for x, y in steps:
                    rep, _n = rig.inject_and_read(R.make_state52(buttons=LPAD_TOUCH, lpad_x=x,
                                                                 lpad_y=y))
                    if rep is None:
                        c.fail("no report for x = %d" % x)
                        break
                    f = _fields(rep)
                    got.append(f["lpad_x"])
                    c.eq("y held at %d" % y, f["lpad_y"], y)
                c.eq("held within 4369 of the touch point, then following", got, want)
                _settings(rig, [(80, 1)])
                rep, _n = rig.inject_and_read(R.make_state52(buttons=LPAD_TOUCH, lpad_x=7000,
                                                             lpad_y=1000))
                if c.true("a report", rep is not None):
                    c.eq("80 = 1: the reported position is the pad's", _fields(rep)["lpad_x"],
                         7000)
            finally:
                rig.inject_off()
                _settings(rig, [(80, 1)])

    if ctx.want("cal.raw_off"):
        with ctx.check(GROUP, "cal.raw_off") as c:
            _need_cal(rig, c)
            f = _raw(rig, c, "on", present=C.RAW_TRIGGER_L, ltrigger=4095)
            a = rig.raw_off()
            c.eq("RAW_OFF", a["status"], C.ST_OK)
            st = rig.cal_status()
            if c.true("CAL_STATUS answers", st is not None):
                c.eq("no raw injection", st["injected"], 0)
            rig.flush()
            reps = [r for _t, r in rig.read_stream(count=3) if r and r[0] == C.RPT_STATE]
            if c.true("reports after RAW_OFF", len(reps) > 0) and f is not None:
                c.true("the application's trigger again",
                       _fields(reps[-1])["left_trigger"] == R.decode_state52(
                           rig.baseline())["left_trigger"])
