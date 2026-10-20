# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# inputs: every input field, injected and read back from the board's own state reports (W) or its
# slot on the puck (R) (docs/testing.md, group "inputs"). Each injected state carries a fresh
# nonce in the IMU timestamp, so the runner knows which reports carry which state; each report is
# compared with the expected-report model (proto/model.py, the input rules of the board's rule
# word), which the shared vectors hold to the library's own rules. Lizard mode is off (0x81
# first); the firmware refuses an injection while it is on, which inputs.lizard_lock checks.
import random
import struct

from . import base
from .. import config as C
from ..proto import model as M
from ..proto import reports as R

GROUP = "inputs"
TESTS = [("inputs.buttons", "WR", []), ("inputs.sticks", "WR", []), ("inputs.triggers", "WR", []),
         ("inputs.trigger_bits", "WR", []), ("inputs.pads", "WR", []), ("inputs.imu", "WR", []),
         ("inputs.full", "WR", []), ("inputs.stick_touch", "WR", []),
         ("inputs.pad_blank", "WR", []), ("inputs.raw_trigger_bits", "WR", []),
         ("inputs.pad_click", "WR", []), ("inputs.latency", "WR", []), ("inputs.lizard_lock", "WR", []),
         ("inputs.decoupled", "WR", [])]

LT_BIT = 1 << R.BIT["LTRIGGER_DIGITAL"]
RT_BIT = 1 << R.BIT["RTRIGGER_DIGITAL"]
LST, RST = 1 << R.BIT["LSTICK_TOUCH"], 1 << R.BIT["RSTICK_TOUCH"]
LPT, RPT = 1 << R.BIT["LPAD_TOUCH"], 1 << R.BIT["RPAD_TOUCH"]
LPC, RPC = 1 << R.BIT["LPAD_CLICK"], 1 << R.BIT["RPAD_CLICK"]
LGRIP, RGRIP = 1 << R.BIT["LGRIP"], 1 << R.BIT["RGRIP"]
AXES = ("lstick_x", "lstick_y", "rstick_x", "rstick_y")
IMU = ("accel_x", "accel_y", "accel_z", "gyro_x", "gyro_y", "gyro_z", "quat_w", "quat_x",
       "quat_y", "quat_z")


def diff(got, want):
    """The fields that differ, as `name got/want` (input values identify nothing)."""
    g, w = R.decode_state52(got), R.decode_state52(want)
    out = []
    for name, _o, _k in R.STATE_FIELDS:
        if g[name] != w[name]:
            fmt = "%s 0x%08x/0x%08x" if name == "buttons" else "%s %d/%d"
            out.append(fmt % (name, g[name], w[name]))
    return ", ".join(out[:3])


LEFT_STICK = ("lstick_x", "lstick_y")


def app_state(got52, baseline52):
    """[R21] How a report's state compares with the application's baseline: "equal",
    "left_stick" (only the left stick differs: the unwired ADC pins of the left stick may pick up
    noise, a WARN) or "differs"."""
    if bytes(got52) == bytes(baseline52):
        return "equal"
    g, b = R.decode_state52(got52), R.decode_state52(baseline52)
    others = [n for n, _o, _k in R.STATE_FIELDS if n not in LEFT_STICK and g[n] != b[n]]
    return "differs" if others else "left_stick"


def _is_app_state(rig, report):
    return report[0] == C.RPT_STATE and len(report) >= C.STATE_REPORT_LEN and \
        app_state(R.state52_of_report(report), rig.baseline()) != "differs"


class Tracker:
    """Injects states and compares each report that carries the state's nonce with the model: the
    input rules of the board's rule word (rig.rules()) on the board's settings as they are now
    (`setting68` and `settings` change them in the model only: write them to the board first).
    The triggers' digital bits and the pads' clicks by pressure depend on the reports before: the
    tracker keeps them, the trigger bits read from the last report, the clicks starting off (the
    firmware clears them when a threshold changes; prime() makes sure)."""

    def __init__(self, rig, setting68=None, auto=True, rules=None, settings=None):
        self.rig = rig
        self.settings = rig.settings_read() if settings is None else dict(settings)
        if setting68 is not None:
            self.settings[68] = setting68
        if rules is None:
            rules = rig.rules()[0]
        if not auto:
            rules &= ~M.RULE_TRIGGER_BITS
        self.model = M.StateRules(rules)
        self.reports = 0
        # with the click by pressure on, the clicks the board keeps cannot be read from a report
        # (an untouched pad shows none): the first check primes them
        self.primed = not rules & M.RULE_PAD_CLICK
        last = [r for _t, r in rig.read_stream(count=2) if r[0] == C.RPT_STATE]
        if last:
            b = R.decode_state52(R.state52_of_report(last[-1]))["buttons"]
            self.model.trigger_bit = [bool(b & LT_BIT), bool(b & RT_BIT)]

    def prime(self, c):
        """Both pads touched at pressure 0: both clicks by pressure are off (thresholds of 9 and
        up), in the board and in the model."""
        self.primed = True
        self.check(c, "both pads touched at pressure 0", R.make_state52(buttons=LPT | RPT))
        self.model.pad_click = [False, False]

    def check(self, c, what, state52, keep_timestamp=False):
        if not self.primed:
            self.prime(c)
        rep, n = self.rig.inject_and_read(state52, keep_timestamp=keep_timestamp)
        if rep is None:
            c.fail("%s: no report carried the nonce" % what)
            return None
        injected = bytes(state52) if keep_timestamp else R.set_field(state52, "imu_timestamp", n)
        want = self.model.apply(injected, self.settings)
        got = R.state52_of_report(rep)
        b = R.decode_state52(got)["buttons"]
        # what the board did is the start of the next comparison: no failure repeats itself
        self.model.trigger_bit = [bool(b & LT_BIT), bool(b & RT_BIT)]
        for side, touch, click in ((0, LPT, LPC), (1, RPT, RPC)):
            if b & touch:
                self.model.pad_click[side] = bool(b & click)
        self.reports += 1
        if got != want:
            c.fail("%s: %s" % (what, diff(got, want)))
        else:
            c.checks += 1
        return got


def _start(rig):
    rig.arm() if not rig.ctx.armed else None
    rig.lizard_off()


def _setting_back(rig, number, value):
    if rig.settings_read()[number] != value:
        rig.write_settings([(number, value)])


def _stick_touch(rig, c):
    """inputs.stick_touch: a stick further than 12000 from its centre (the controller's distance
    approximation) counts as touched; never with raw stick output (setting 46)."""
    if not rig.rules()[0] & M.RULE_STICK_FALLBACK:
        c.skip("this build has no stick-touch fallback")
    start46 = rig.settings_read()[46]
    try:
        _setting_back(rig, 46, 0)
        t = Tracker(rig)
        for name, v in (("lstick_x", 12203), ("lstick_x", 12204), ("lstick_y", -12204),
                        ("rstick_x", 12204), ("rstick_y", -12203), ("rstick_y", -32768)):
            t.check(c, "%s %d" % (name, v), R.make_state52(**{name: v}))
        for x, y in ((8727, 8727), (8728, -8728), (-8728, 8728)):
            t.check(c, "both sticks at %d,%d" % (x, y),
                    R.make_state52(lstick_x=x, lstick_y=y, rstick_x=x, rstick_y=y))
        t.check(c, "the sensor's bits with centred sticks", R.make_state52(buttons=LST | RST))
        t.check(c, "the fallback blanks its side's pad (setting 82)",
                R.make_state52(buttons=LPT | RPT, lstick_x=-20000, lpad_x=3000, lpad_y=3000,
                               rpad_x=-3000, rpad_y=-3000, lpad_pressure=5000))
        rig.write_settings([(46, 2)])
        t = Tracker(rig)
        t.check(c, "raw sticks (46 = 2): 12204 is no touch", R.make_state52(lstick_x=12204))
        t.check(c, "raw sticks (46 = 2): 32767, 32767 is no touch",
                R.make_state52(rstick_x=32767, rstick_y=32767))
    finally:
        _setting_back(rig, 46, start46)


def _pad_blank(rig, c):
    """inputs.pad_blank: setting 82 blanks a pad in 0x42 while the stick on its side is touched."""
    if not rig.rules()[0] & M.RULE_PAD_BLANK:
        c.skip("this build does not blank the pads in report 0x42")
    start = rig.settings_read()
    pads = dict(lpad_x=-3000, lpad_y=2500, lpad_pressure=9000, rpad_x=1000, rpad_y=-1000,
                rpad_pressure=20000)
    try:
        for s82 in (3, 1, 2, 0):
            rig.write_settings([(82, s82)])
            t = Tracker(rig)
            for sticks in (0, LST, RST, LST | RST):
                t.check(c, "82=%d sticks 0x%08x" % (s82, sticks),
                        R.make_state52(buttons=sticks | LPT | LPC | RPT | RPC | LGRIP | RGRIP,
                                       **pads))
        rig.write_settings([(82, 3), (60, 1)])
        t = Tracker(rig)
        t.check(c, "raw pressure (60 = 1) passes a blanked pad",
                R.make_state52(buttons=RST | RPT, **pads))
    finally:
        for n in (82, 60):
            _setting_back(rig, n, start[n])


def _raw_trigger_bits(rig, c):
    """inputs.raw_trigger_bits: no trigger bit while setting 62 asks for raw triggers."""
    if not rig.rules()[0] & M.RULE_RAW_TRIGGERS:
        c.skip("this build has no rule for raw trigger output")
    start62 = rig.settings_read()[62]
    try:
        for v62 in (1, 2):
            rig.write_settings([(62, v62)])
            t = Tracker(rig)
            t.check(c, "62=%d both triggers full" % v62,
                    R.make_state52(left_trigger=32767, right_trigger=32767))
            t.check(c, "62=%d the bits injected" % v62,
                    R.make_state52(buttons=LT_BIT | RT_BIT, left_trigger=32767))
        rig.write_settings([(62, 0)])
        t = Tracker(rig)
        t.check(c, "62=0 both triggers full", R.make_state52(left_trigger=32767,
                                                            right_trigger=32767))
        t.check(c, "62=0 released", R.make_state52())
    finally:
        _setting_back(rig, 62, start62)


def _pad_click(rig, c):
    """inputs.pad_click: the pads' clicks from their pressure (settings 52, 53, 8 points of
    hysteresis), with the click by pressure switched on for the test where the build has it off
    (RULES_SET; the end of arming puts the build's rule word back)."""
    now, _build = rig.rules()
    switched = False
    if not now & M.RULE_PAD_CLICK:
        if rig.rules_set(now | M.RULE_PAD_CLICK) is None:
            c.skip("this firmware cannot switch the click by pressure on (no RULES_SET)")
        switched = True
        c.note("the click by pressure switched on for the test (off in this build)")
    start = rig.settings_read()
    try:
        rig.write_settings([(52, 40), (53, 40)])
        t = Tracker(rig)
        t.prime(c)
        for side, touch, field in (("right", RPT, "rpad_pressure"), ("left", LPT, "lpad_pressure")):
            for v in (15728, 15729, 20000, 10486, 10485, 0):
                t.check(c, "%s pad %d" % (side, v), R.make_state52(buttons=touch, **{field: v}))
        t.check(c, "untouched, pressed: no click reported",
                R.make_state52(rpad_pressure=20000))
        t.check(c, "touched again: the click it kept", R.make_state52(buttons=RPT,
                                                                       rpad_pressure=20000))
        t.check(c, "the application's click bit is replaced",
                R.make_state52(buttons=LPT | LPC | RPT | RPC))
        t.check(c, "a blanked pad does not click (82 = 3)",
                R.make_state52(buttons=RST | RPT, rpad_pressure=32767))
        rig.write_settings([(52, 0), (53, -1)])
        t = Tracker(rig)
        for v in (2621, 2622, 32767):
            t.check(c, "52=0 left %d" % v, R.make_state52(buttons=LPT, lpad_pressure=v))
            t.check(c, "53=-1 right %d" % v, R.make_state52(buttons=RPT, rpad_pressure=v))
    finally:
        for n in (52, 53):
            _setting_back(rig, n, start[n])
        if switched:
            rig.rules_set(now)


def _lizard_lock(ctx, rig, c):
    """inputs.lizard_lock: the firmware refuses an injection while lizard mode is on, and stops
    one when lizard mode comes on."""
    if not rig.caps["build_flags"] & (1 << C.CAP_LIZARD_MODE):
        c.skip("this build has no lizard mode")
    rig.lizard_off()
    rig.write_settings([(9, 1)])           # lizard mode on (diverted: armed)
    a = rig.probe_inject_denied(R.make_state52(buttons=1))
    if not c.eq("INJECT without the lizard-test flag while lizard mode is on",
                a["status"], C.ST_DENIED):
        # S7's firmware layer is broken: a safety refusal (exit 2), and no injection
        # for the rest of the run (the rig refuses it from here on)
        ctx.precondition = C.EXIT_PRECONDITION
        rig.lizard_off()
        return
    rig.lizard_off()
    a, n = rig.inject(R.make_state52(buttons=1))   # A held
    fails_before = len(c.fails)
    done = False
    try:
        c.eq("INJECT with lizard mode off", a["status"], C.ST_OK)
        c.true("the injection runs", rig.wait_nonce(n, 0.5) is not None)
        start = rig.ring_next()
        rig.write_settings([(9, 1)])           # lizard mode comes on while it runs
        rig.inject_active = False              # the firmware must have stopped it (no keep-off)
        rig.read_stream(count=8)
        _f, _n, entries = rig.ring_all(start)
        enters = [e for e in entries if e["kind"] == C.RING_LIZARD and
                  e.get("what") == C.LIZARD_ENTER]
        stops = base.of_kind(entries, C.RING_INJECT_STOP)
        c.eq("one LIZARD enter", len(enters), 1)
        c.eq("one INJECT_STOP", len(stops), 1)
        if stops:
            c.eq("INJECT_STOP reason and what", (stops[0]["reason"], stops[0]["what"]), (1, 1))
        rpts = [e["report"] for e in base.of_kind(entries, C.RING_LIZARD_RPT)]
        c.eq("only the enter's reports were diverted", base.byte_lists(rpts),
             base.byte_lists([bytes([C.RPT_MOUSE]) + bytes(5), bytes([C.RPT_KEYBOARD]) + bytes(8)]))
        after = [r for _t, r in rig.read_stream(count=10) if r[0] == C.RPT_STATE]
        how = [app_state(R.state52_of_report(r), rig.baseline()) for r in after]
        c.true("every 0x42 from the stop on carries the application's state",
               after and "differs" not in how)
        if "left_stick" in how:
            c.warn("only the left stick differs from the baseline (the unwired ADC pins may "
                   "pick up noise)")
        st = rig.status()
        c.true("STATUS: no injection", st and not st["inject"])
        done = True
    finally:
        failed = len(c.fails) > fails_before
        if failed or not done:
            # the injection may still run with lizard mode on: stop it here (the keep-off timer
            # is not running for it any more)
            for fn in (rig.seq_stop, rig.inject_off):
                try:
                    fn()
                except Exception:
                    pass
        if failed:
            # the firmware's layer of S7 is broken: no injection for the rest of the run (exit 2)
            rig.block_injection("the firmware did not stop an injection when lizard mode came "
                                "on (inputs.lizard_lock)")
            ctx.precondition = C.EXIT_PRECONDITION
    rig.lizard_off()


def run(ctx):
    rig = ctx.rig
    _start(rig)
    auto = rig.caps is not None and bool(rig.caps["build_flags"] & (1 << C.CAP_TRIGGER_CLICK_AUTO))

    if ctx.want("inputs.buttons"):
        with ctx.check(GROUP, "inputs.buttons") as c:
            t = Tracker(rig, auto=auto)
            for bit in range(32):
                t.check(c, "bit %d" % bit, R.make_state52(buttons=1 << bit))
            t.check(c, "bits 0..29 together", R.make_state52(buttons=(1 << 30) - 1))
            t.check(c, "no button", R.make_state52())
            c.note("32 single bits, all 30, none: %d reports as the model" % t.reports)

    if ctx.want("inputs.sticks"):
        with ctx.check(GROUP, "inputs.sticks") as c:
            t = Tracker(rig, auto=auto)
            for axis in AXES:
                for v in (0, 1, -1, 12345, -12345, 32767, -32767, -32768):
                    t.check(c, "%s %d" % (axis, v), R.make_state52(**{axis: v}))
            t.check(c, "four distinct", R.make_state52(lstick_x=1111, lstick_y=-2222,
                                                       rstick_x=3333, rstick_y=-4444))

    if ctx.want("inputs.triggers"):
        with ctx.check(GROUP, "inputs.triggers") as c:
            t = Tracker(rig, auto=auto)
            for side in ("left_trigger", "right_trigger"):
                for v in (0, 1, 16384, 32767, 32768, 65535):
                    t.check(c, "%s %d" % (side, v), R.make_state52(**{side: v}))
            t.check(c, "released", R.make_state52())

    if ctx.want("inputs.trigger_bits"):
        with ctx.check(GROUP, "inputs.trigger_bits") as c:
            if not auto:
                c.note("TRIGGER_CLICK_AUTO off: the bits pass as injected")
            start68 = rig.settings_read()[68]
            for p in (90, 40, 99):
                rig.write_settings([(68, p)])
                c.eq("setting 68 written", rig.settings_read()[68], p)
                t = Tracker(rig, setting68=p, auto=auto)
                on = min(32767 * (p + 5) // 100, 32766)
                off = 32767 * (p - 5) // 100
                for side in ("left_trigger", "right_trigger"):
                    for v in (0, on, on + 1, off, off - 1, 32767, 0):
                        t.check(c, "68=%d %s %d" % (p, side, v), R.make_state52(**{side: v}))
            for v, stored in ((30, 40), (120, 99)):
                rig.write_settings([(68, v)])
                c.eq("setting 68 = %d stored as" % v, rig.settings_read()[68], stored)
            rig.write_settings([(68, start68)])
            c.eq("setting 68 restored", rig.settings_read()[68], start68)

    if ctx.want("inputs.pads"):
        with ctx.check(GROUP, "inputs.pads") as c:
            t = Tracker(rig, auto=auto)
            for side, touch, click, x, y, p in (("rpad", 21, 22, "rpad_x", "rpad_y",
                                                 "rpad_pressure"),
                                                ("lpad", 25, 26, "lpad_x", "lpad_y",
                                                 "lpad_pressure")):
                for vx, vy in ((32767, -32768), (-1, 1), (0, 0)):
                    t.check(c, "%s touched at %d,%d" % (side, vx, vy),
                            R.make_state52(buttons=1 << touch, **{x: vx, y: vy}))
                t.check(c, "%s untouched at 500,-500" % side, R.make_state52(**{x: 500, y: -500}))
                for pr in (0, 1, 32767, 40000):
                    t.check(c, "%s pressure %d" % (side, pr), R.make_state52(**{p: pr}))
                t.check(c, "%s click with touch" % side,
                        R.make_state52(buttons=(1 << touch) | (1 << click), **{x: 100, y: 100}))
                t.check(c, "%s click without touch" % side,
                        R.make_state52(buttons=1 << click, **{x: 100, y: 100}))

    if ctx.want("inputs.imu"):
        with ctx.check(GROUP, "inputs.imu") as c:
            t = Tracker(rig, auto=auto)
            for ts in (0, 1, 0x7FFFFFFF, 0xFFFFFFFF):
                t.check(c, "timestamp 0x%08x" % ts, R.make_state52(imu_timestamp=ts),
                        keep_timestamp=True)
                t.check(c, "a nonce after it", R.make_state52())
            for field in IMU:
                for v in (0, 1, -1, 16384, 32767, -32768):
                    t.check(c, "%s %d" % (field, v), R.make_state52(**{field: v}))

    if ctx.want("inputs.full"):
        with ctx.check(GROUP, "inputs.full") as c:
            seed = 20261018
            rng = random.Random(seed)
            t = Tracker(rig, auto=auto)
            for i in range(20):
                fields = {}
                for name, _o, kind in R.STATE_FIELDS:
                    if name == "imu_timestamp":
                        continue
                    fields[name] = rng.getrandbits(32 if kind == "u32" else 16)
                t.check(c, "random state %d" % i, R.make_state52(**fields))
            c.note("20 random full states (seed %d) as the model" % seed)

    if ctx.want("inputs.stick_touch"):
        with ctx.check(GROUP, "inputs.stick_touch") as c:
            _stick_touch(rig, c)

    if ctx.want("inputs.pad_blank"):
        with ctx.check(GROUP, "inputs.pad_blank") as c:
            _pad_blank(rig, c)

    if ctx.want("inputs.raw_trigger_bits"):
        with ctx.check(GROUP, "inputs.raw_trigger_bits") as c:
            _raw_trigger_bits(rig, c)

    if ctx.want("inputs.pad_click"):
        with ctx.check(GROUP, "inputs.pad_click") as c:
            _pad_click(rig, c)

    if ctx.want("inputs.latency"):
        with ctx.check(GROUP, "inputs.latency") as c:
            limit_frames = 3 if rig.path == "W" else 10
            limit_ms = 20.0 if rig.path == "W" else 60.0
            # S7's round trips (the Steam check, STATUS, a 0x81) first, so that they are not
            # counted: the row counts from the INJECT's answer
            rig.ready_to_inject()
            rig.flush()
            a, n = rig.inject(R.make_state52(buttons=1, lstick_x=777))
            t_answer = rig.clock.perf_ns()
            c.eq("INJECT", a["status"], C.ST_OK)
            got = rig.wait_nonce(n, timeout=C.NONCE_DEADLINE_MS[rig.path] / 1000.0)
            if got is None:
                c.fail("the nonce did not appear within %d ms" % C.NONCE_DEADLINE_MS[rig.path])
            else:
                ts, _rep, before = got
                states = [r for t, r in before if r[0] == C.RPT_STATE and t >= t_answer]
                c.true("the nonce within %d frames of the INJECT answer (%d)"
                       % (limit_frames, len(states)), len(states) <= limit_frames)
                ms = max(0.0, (ts - t_answer) / 1e6)
                c.observe(latency_ms=round(ms, 2), frames_before=len(states))
                if ms > limit_ms:
                    c.warn("latency %.1f ms (above %.0f)" % (ms, limit_ms))
                later = [r for _t, r in rig.read_stream(count=20) if r[0] == C.RPT_STATE]
                c.true("every later report keeps the nonce",
                       later and all(R.nonce_of_report(r) == n for r in later))
            rig.flush()
            rig.inject_off()
            t_off = rig.clock.perf_ns()
            back = rig.wait_report(lambda r: _is_app_state(rig, r), 0.5)
            c.true("after INJECT_OFF the application's state again", back is not None)
            if back is not None:
                if app_state(R.state52_of_report(back[1]), rig.baseline()) == "left_stick":
                    c.warn("only the left stick differs from the baseline (the unwired ADC pins "
                           "may pick up noise)")
                n_before = len([r for t, r in back[2] if r[0] == C.RPT_STATE and t >= t_off])
                c.true("within %d frames of the INJECT_OFF answer (%d)" % (limit_frames,
                                                                          n_before),
                       n_before <= limit_frames + 1)

    if ctx.want("inputs.lizard_lock"):
        with ctx.check(GROUP, "inputs.lizard_lock") as c:
            _lizard_lock(ctx, rig, c)

    if ctx.want("inputs.decoupled"):
        with ctx.check(GROUP, "inputs.decoupled") as c:
            st0 = rig.status()
            start = st0["ring_next"]
            a, n = rig.inject(R.make_state52(buttons=1 << R.BIT["STEAM"]))
            c.eq("INJECT STEAM", a["status"], C.ST_OK)
            rig.read_stream(duration_s=2.0)
            st1 = rig.status()
            _f, _n, entries = rig.ring_all(start)
            c.eq("POWER entries", len(base.of_kind(entries, C.RING_POWER)), 0)
            c.true("no mode change and no reboot", st1 and st1["mode"] == st0["mode"] and
                   st1["uptime_ms"] > st0["uptime_ms"])
            if rig.path == "R":
                lines = rig.console_run("opensc2 state")
                c.true("opensc2 state shows the application's buttons, not the injected",
                       any(ln.strip() == "buttons   0x00000000" for ln in lines))
            rig.inject_off()
