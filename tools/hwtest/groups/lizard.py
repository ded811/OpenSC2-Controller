# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# lizard: lizard mode's logic on injected inputs (docs/testing.md, group "lizard"), opt-in
# (--lizard). The only group that injects with lizard mode on: its sequences carry the lizard-test
# flag, so only the guard keeps the mouse and keyboard reports lizard mode makes off the PC; the
# ring records each one (LIZARD_RPT, with its frame) and the library's haptic events (HAPTIC with
# report ID 0), and the runner compares them, frame by frame, with the lizard model (proto/model.py,
# held to lizard.c by the shared vectors). After a sequence the reports are followed until the
# model goes quiet: a glide keeps them going for up to setting 12 frames. The mapping tests run
# with lizard mode's processing switched off (RULES_SET, on a firmware that has it), the
# processing tests with one part each, lizard.default with the build's. It runs only after the
# guard canary passed, with the leak detector watching, and last in its phase.
import struct

from . import base
from .. import config as C
from ..proto import model as M
from ..proto import reports as R

GROUP = "lizard"
TESTS = [("lizard.on_at_start", "WR", ["lizard"]), ("lizard.keyboard", "WR", ["lizard"]),
         ("lizard.pointer", "WR", ["lizard"]), ("lizard.scroll", "WR", ["lizard"]),
         ("lizard.buttons", "WR", ["lizard"]), ("lizard.thumb_on_stick", "WR", ["lizard"]),
         ("lizard.off", "WR", ["lizard"]), ("lizard.on_by_setting", "WR", ["lizard"]),
         ("lizard.switches", "WR", ["lizard"]), ("lizard.stabiliser", "WR", ["lizard"]),
         ("lizard.smoothing", "WR", ["lizard"]), ("lizard.glide", "WR", ["lizard"]),
         ("lizard.glide_stops", "WR", ["lizard"]), ("lizard.ticks", "WR", ["lizard"]),
         ("lizard.default", "WR", ["lizard"]), ("lizard.no_leak", "WR", ["lizard"])]

A, B, X, VIEW, MENU = 1 << 0, 1 << 1, 1 << 2, 1 << 6, 1 << 14
DOWN, RIGHT, LEFT, UP = 1 << 10, 1 << 11, 1 << 12, 1 << 13
RPAD_TOUCH, LPAD_TOUCH, RPAD_CLICK, RSTICK_TOUCH = 1 << 21, 1 << 25, 1 << 22, 1 << 20
MOUSE_ZERO = bytes([C.RPT_MOUSE]) + bytes(5)
KEYS_EMPTY = bytes([C.RPT_KEYBOARD]) + bytes(8)
# the settings this group writes, restored at its end
TOUCHED = (11, 12, 14, 52, 53, 70, 80, 82, 68, 9)
AFTER_MAX = 1100            # frames after a sequence the model may run (a glide: setting 12)


def S(buttons=0, rpad=(0, 0), lpad=(0, 0), rt=0, lt=0):
    return R.make_state52(buttons=buttons, rpad_x=rpad[0], rpad_y=rpad[1], lpad_x=lpad[0],
                          lpad_y=lpad[1], right_trigger=rt, left_trigger=lt)


def expected(steps, rules, settings):
    """The lizard model, behind the input rules, on the steps (state52, hold): per step the
    reports its frames make, the reports of the frames after the sequence (the application's
    state, all zero) until the model goes quiet, how many frames that takes, and every haptic
    event in order (("tick" or "click", side))."""
    rules_in = M.StateRules(rules)
    lz = M.LizardModel(rules)
    zero = R.make_state52()
    lz.step_state52(rules_in.apply(zero, settings), settings)   # in sync with the application
    haptics = []
    per = []
    for state, hold in steps:
        made = []
        for _ in range(hold):
            made += [r for _k, r in lz.step_state52(rules_in.apply(state, settings), settings)]
            haptics += lz.haptics
        per.append(made)
    after = []
    frames = 0
    for frames in range(1, AFTER_MAX + 1):
        after += [r for _k, r in lz.step_state52(rules_in.apply(zero, settings), settings)]
        haptics += lz.haptics
        if not lz.mouse_owed and not lz.gliding():
            break
    return per, after, frames, haptics


def run_sequence(rig, steps, after_frames=1):
    """Load and run `steps` (state52, hold) with the lizard-test flag: (per step the diverted
    reports of its frames, the reports from the end of it on, the library's haptic events in
    order), or None when the steps did not all start. `after_frames`: the frames after the
    sequence that may still make reports (a glide)."""
    for i, (state, hold) in enumerate(steps):
        a = rig.seq_load(i, hold, state)
        if a["status"] != C.ST_OK:
            return None
    rig.read_stream(count=2)                    # a frame of the application's state first
    start = rig.ring_next()
    a = rig.seq_run(len(steps), C.SEQ_FLAG_LIZARD)
    if a["status"] != C.ST_OK:
        return None
    rig.read_stream(count=sum(h for _s, h in steps) + 6)
    # Through the puck those reports can be ones the slot had buffered before the run, so the
    # count alone does not prove the sequence is over: wait for the board's own SEQ_END, then the
    # frames after it that the model says may still make reports (and one more frame or so).
    rig.ring_wait(start, lambda e: bool(base.of_kind(e, C.RING_SEQ_END)), timeout=1.0)
    if after_frames > 1:
        rig.read_stream(count=after_frames + 2)
    rig.sleep(0.03)
    rig.inject_active = False
    entries = rig.ring_all(start)[2]
    frames = {e["step"]: e["frame"] for e in base.of_kind(entries, C.RING_SEQ_STEP)}
    if sorted(frames) != list(range(len(steps))):
        return None
    ends = base.of_kind(entries, C.RING_SEQ_END)
    end = ends[0]["frame"] if ends else frames[len(steps) - 1] + steps[-1][1]
    rpts = [(e["frame"], e["report"]) for e in base.of_kind(entries, C.RING_LIZARD_RPT)]
    per = []
    for i in range(len(steps)):
        lo = frames[i]
        hi = frames[i + 1] if i + 1 < len(steps) else end
        per.append([r for f, r in rpts if lo <= f < hi])
    after = [r for f, r in rpts if f >= end]
    haptics = [h for h in (M.lizard_haptic_of_entry(e) for e in entries) if h is not None]
    return per, after, haptics


def processing_rules(rig, lizard_bits):
    """The board's rule word with lizard mode's processing set to `lizard_bits` (RULES_LIZARD
    bits), or None when the firmware cannot switch it (no RULES_SET) and the build differs."""
    now, build = rig.rules()
    want = (build & ~M.RULES_LIZARD) | lizard_bits
    if want == build or (rig.caps and rig.caps["build_flags"] & (1 << C.CAP_RULES)):
        return want
    return None


def check_sequence(c, rig, name, steps, rules=None, sets=None):
    """One sequence against the model. `rules`: the rule word to run it with (RULES_SET after the
    ARM; None: the build's); `sets`: settings to write first ([(number, value)])."""
    # ARM again first: lizard mode resyncs (its trackers, glides and remainders cleared, and the
    # build's rule word back), so the model starts from the same state; the canary runs again
    # before the sequence
    rig.arm()
    if rules is not None and rules != rig.rules()[0]:
        rig.rules_set(rules)
    if sets:
        rig.write_settings(sets)
    rig.ready_to_inject()
    settings = rig.settings_read()
    rules_now = rig.rules()[0]
    want, want_after, after_frames, want_haptics = expected(steps, rules_now, settings)
    got = run_sequence(rig, steps, after_frames)
    if got is None:
        c.fail("%s: the sequence did not run" % name)
        return
    per, after, haptics = got
    # lists of numbers, not hex: a failure line must show the reports (R25's backstop would
    # mask a hex string of 16 digits or more)
    for i, (g, w) in enumerate(zip(per, want)):
        c.eq("%s step %d" % (name, i), base.byte_lists(g), base.byte_lists(w))
    c.eq("%s: after the sequence, until the zero report" % name, base.byte_lists(after),
         base.byte_lists(want_after))
    c.eq("%s: the haptic events" % name, haptics, want_haptics)
    return want, want_after, want_haptics


def _ring_since(rig, start):
    return base.without_vendor(rig.ring_all(start)[2])


def _record_all(ctx, status, why):
    for tid, _p, _n in TESTS:
        if ctx.want(tid):
            ctx.record(GROUP, tid, status, why)


def _r4_refusal(ctx):
    """[R4] The group runs only after the rest of the run passed: no FAIL or ERROR so far, and
    inputs.lizard_lock (the firmware stops an injection when lizard mode comes on) passed on this
    path. The reason it does not run, or None."""
    counts = ctx.results.counts()
    if counts.get(base.FAIL) or counts.get(base.ERROR):
        return "an earlier test failed (R4: the lizard group runs only after the rest of the run " \
               "has passed)"
    passed = any(t.id == "inputs.lizard_lock" and t.path == ctx.path and t.status == base.PASS
                 for g in ctx.results.groups for t in g["tests"])
    if not passed:
        return "inputs.lizard_lock did not pass on this path (R4: run it in the same run, for " \
               "example --group hooks,inputs,lizard)"
    return None


def leak_watching(rig):
    """S13 on this path: the leak detector watches the board's mouse and keyboard collections
    (W) or the proven slot's (R), both listed as raw-input devices. Backends without a detector
    (the dry runs) have nothing to watch."""
    fn = getattr(rig.backend, "leak_watching", None)
    return True if fn is None else bool(fn())


def run(ctx):
    rig = ctx.rig
    if not ctx.opt.get("lizard"):
        _record_all(ctx, base.SKIP, "opt-in (--lizard)")
        return
    if not ctx.results.safety.get("canary"):
        _record_all(ctx, base.SKIP, "the guard canary did not pass")
        return
    why = _r4_refusal(ctx)
    if why:
        _record_all(ctx, base.SKIP, why)
        return
    if not rig.models("lizard"):
        _record_all(ctx, base.SKIP, "lizard frames not modelled by the %s backend"
                    % rig.backend.kind)
        return
    if not leak_watching(rig):
        # a leak detector that cannot watch the collections where a leak would show is
        # off, and --lizard is refused on this path
        _record_all(ctx, base.ERROR, "the leak detector does not watch the board's mouse and "
                                     "keyboard collections on this path (%s): --lizard refused "
                                     "(S13)" % ("its own collections" if rig.path == "W" else
                                                "the proven puck slot's"))
        ctx.precondition = C.EXIT_PRECONDITION
        return
    rig.arm()
    ok, _info = rig.guard_canary()
    if not ok:
        for tid, _p, _n in TESTS:
            if ctx.want(tid):
                ctx.record(GROUP, tid, base.ERROR, "the guard canary failed: nothing injected")
        ctx.precondition = C.EXIT_PRECONDITION
        return
    rig.ctx.lizard_group = True
    snap = rig.settings_read()
    _now, build = rig.rules()
    try:
        _run(ctx, rig, snap)
    finally:
        rig.ctx.lizard_group = False
        rig.seq_stop()
        rig.inject_off()
        pairs = [(n, snap[n]) for n in TOUCHED if rig.settings_read()[n] != snap[n]]
        if pairs:
            rig.write_settings(pairs)
        if rig.rules()[0] != build:
            rig.rules_set(build)


def _mapping(rig):
    """The rule word of the mapping tests: lizard mode's processing off."""
    return processing_rules(rig, 0)


def _run(ctx, rig, snap):
    mapping = _mapping(rig)

    if ctx.want("lizard.on_at_start"):
        with ctx.check(GROUP, "lizard.on_at_start") as c:
            rig.lizard_off()
            start = rig.ring_next()
            rig.vendor_set(C.VC_SETTINGS_LOAD_DEFAULTS)
            c.eq("setting 9", rig.settings_read()[9], 1)
            e = _ring_since(rig, start)
            c.eq("LIZARD enter, then mouse zero, then keyboard empty (diverted)",
                 [(x["kind"], x.get("what"), x.get("report")) for x in e[:3]],
                 [(C.RING_LIZARD, C.LIZARD_ENTER, None), (C.RING_LIZARD_RPT, None, MOUSE_ZERO),
                  (C.RING_LIZARD_RPT, None, KEYS_EMPTY)])
            rig.write_settings([(n, snap[n]) for n in (48, 84, 85)])

    if rig.settings_read()[9] == 0:
        rig.write_settings([(9, 1)])

    if ctx.want("lizard.keyboard"):
        with ctx.check(GROUP, "lizard.keyboard") as c:
            check_sequence(c, rig, "keys", [(S(), 1), (S(A), 1), (S(A | X), 1), (S(A | X | B), 1),
                                            (S(B | VIEW), 1), (S(0x7F), 1), (S(), 1)], mapping)
            check_sequence(c, rig, "arrows", [(S(DOWN), 1), (S(), 1), (S(RIGHT), 1), (S(), 1),
                                              (S(LEFT), 1), (S(), 1), (S(UP), 1), (S(), 1)],
                           mapping)
            check_sequence(c, rig, "menu", [(S(MENU), 1), (S(), 1)], mapping)

    if ctx.want("lizard.pointer"):
        with ctx.check(GROUP, "lizard.pointer") as c:
            check_sequence(c, rig, "pointer", [
                (S(RPAD_TOUCH, rpad=(0, 0)), 1), (S(RPAD_TOUCH, rpad=(1280, 0)), 1),
                (S(RPAD_TOUCH, rpad=(1280, 1280)), 1), (S(RPAD_TOUCH, rpad=(1280, 1280)), 2),
                (S(), 1)], mapping)
            check_sequence(c, rig, "remainders", [
                (S(RPAD_TOUCH, rpad=(0, 0)), 1), (S(RPAD_TOUCH, rpad=(100, 0)), 1),
                (S(RPAD_TOUCH, rpad=(200, 0)), 2), (S(RPAD_TOUCH, rpad=(0, 0)), 1),
                (S(RPAD_TOUCH, rpad=(-100, 0)), 1), (S(RPAD_TOUCH, rpad=(-200, 0)), 1),
                (S(), 1)], mapping)

    if ctx.want("lizard.scroll"):
        with ctx.check(GROUP, "lizard.scroll") as c:
            check_sequence(c, rig, "scroll", [(S(LPAD_TOUCH, lpad=(0, 0)), 1),
                                              (S(LPAD_TOUCH, lpad=(3072, 2048)), 1), (S(), 1)],
                           mapping)

    if ctx.want("lizard.buttons"):
        with ctx.check(GROUP, "lizard.buttons") as c:
            check_sequence(c, rig, "triggers", [(S(rt=32767), 3), (S(), 1), (S(lt=32767), 2),
                                                (S(), 1)], mapping)
            check_sequence(c, rig, "pad click", [(S(RPAD_TOUCH | RPAD_CLICK), 1), (S(), 1)],
                           mapping)

    if ctx.want("lizard.thumb_on_stick"):
        with ctx.check(GROUP, "lizard.thumb_on_stick") as c:
            for s82 in (3, 0):
                check_sequence(c, rig, "s82=%d" % s82, [
                    (S(RSTICK_TOUCH | RPAD_TOUCH, rpad=(0, 0)), 1),
                    (S(RSTICK_TOUCH | RPAD_TOUCH, rpad=(1280, 0)), 1),
                    (S(RSTICK_TOUCH | RPAD_TOUCH, rpad=(2560, 0)), 1), (S(), 1)], mapping,
                    [(82, s82)])
            rig.write_settings([(82, snap[82])])

    if ctx.want("lizard.off"):
        with ctx.check(GROUP, "lizard.off") as c:
            rig.write_settings([(9, 1)])
            start = rig.ring_next()
            rig.vendor_set(C.VC_LIZARD_OFF)
            e = _ring_since(rig, start)
            c.eq("0x81 while on: leave, keyboard empty, mouse zero, SETTING 9 = 0",
                 [(x["kind"], x.get("what"), x.get("report"), x.get("number")) for x in e],
                 [(C.RING_LIZARD, C.LIZARD_LEAVE, None, None),
                  (C.RING_LIZARD_RPT, None, KEYS_EMPTY, None),
                  (C.RING_LIZARD_RPT, None, MOUSE_ZERO, None),
                  (C.RING_SETTING, None, None, 9)])
            start = rig.ring_next()
            rig.vendor_set(C.VC_LIZARD_OFF)
            e = _ring_since(rig, start)
            c.eq("0x81 again: switches down only", [(x["kind"], x.get("what")) for x in e],
                 [(C.RING_LIZARD, C.LIZARD_SWITCHES_DOWN)])

    if ctx.want("lizard.on_by_setting"):
        with ctx.check(GROUP, "lizard.on_by_setting") as c:
            rig.vendor_set(C.VC_LIZARD_OFF)
            start = rig.ring_next()
            rig.write_settings([(9, 1)])
            e = _ring_since(rig, start)
            c.eq("0x87 {9,1} while off: enter, mouse zero, keyboard empty, SETTING 9 = 1",
                 [(x["kind"], x.get("what"), x.get("report"), x.get("number")) for x in e],
                 [(C.RING_LIZARD, C.LIZARD_ENTER, None, None),
                  (C.RING_LIZARD_RPT, None, MOUSE_ZERO, None),
                  (C.RING_LIZARD_RPT, None, KEYS_EMPTY, None),
                  (C.RING_SETTING, None, None, 9)])

    if ctx.want("lizard.switches"):
        with ctx.check(GROUP, "lizard.switches") as c:
            rig.vendor_set(C.VC_LIZARD_OFF)
            rig.vendor_set(C.VC_LIZARD_DEFAULTS_ON)
            got = run_sequence(rig, [(S(A), 2), (S(), 1)])
            c.true("nothing while 9 = 0", got is not None and not any(got[0]) and not got[1])
            rig.write_settings([(9, 1)])
            got = run_sequence(rig, [(S(), 1), (S(A), 1), (S(), 1)])
            if got is None:
                c.fail("the sequence did not run")
            else:
                c.eq("after 9 = 1: Enter in the frame A is injected", base.byte_lists(got[0][1]),
                     base.byte_lists([bytes([C.RPT_KEYBOARD, 0, 0, 0x28]) + bytes(5)]))

    _processing(ctx, rig, snap)

    if ctx.want("lizard.no_leak"):
        with ctx.check(GROUP, "lizard.no_leak") as c:
            cnt = rig.cnt_read()
            first, _nxt, entries = rig.ring_all(0)
            canary = 2
            c.eq("lizard_diverted = lizard_made + the canary's two",
                 cnt.get("lizard_diverted"), cnt.get("lizard_made", 0) + canary)
            rpts = len(base.of_kind(entries, C.RING_LIZARD_RPT))
            if first == 0:
                c.eq("LIZARD_RPT entries = lizard_diverted", rpts, cnt.get("lizard_diverted"))
            else:
                c.true("fewer LIZARD_RPT entries (some evicted)",
                       rpts <= cnt.get("lizard_diverted", 0))
            c.true("the leak detector watches the board's mouse and keyboard on this path",
                   leak_watching(rig))
            c.true("the leak detector saw no event from the board's mouse or keyboard",
                   not rig.backend.leak_fired())


def _flick(x0=-28000, step=8000, hold=3, touch=RPAD_TOUCH, axis=0):
    """Eight steps of `step` pad units, each held `hold` frames: a touch of 24 frames whose last
    20 movements have a mean speed (the flick speed) of about step * 5 / 15 per frame, then the
    lift-off of the end of the sequence."""
    out = []
    for k in range(8):
        v = x0 + step * k
        pos = (v, 0) if axis == 0 else (0, v)
        out.append((S(touch, rpad=pos) if touch == RPAD_TOUCH else S(touch, lpad=pos), hold))
    return out


def _start_glide(rig, rules):
    """A flick with setting 12 = 100 (about 0.4 s of glide), run until a few frames after the
    sequence ended: whether glide reports came after its end (the glide is still on)."""
    rig.arm()
    if rules is not None and rules != rig.rules()[0]:
        rig.rules_set(rules)
    rig.write_settings([(11, 200), (12, 100)])
    rig.ready_to_inject()
    steps = _flick()
    for i, (state, hold) in enumerate(steps):
        if rig.seq_load(i, hold, state)["status"] != C.ST_OK:
            return False
    rig.read_stream(count=2)
    start = rig.ring_next()
    if rig.seq_run(len(steps), C.SEQ_FLAG_LIZARD)["status"] != C.ST_OK:
        return False
    rig.read_stream(count=sum(h for _s, h in steps) + 6)
    rig.ring_wait(start, lambda e: bool(base.of_kind(e, C.RING_SEQ_END)), timeout=1.0)
    rig.read_stream(count=3)
    rig.inject_active = False
    entries = rig.ring_all(start)[2]
    ends = base.of_kind(entries, C.RING_SEQ_END)
    if not ends:
        return False
    return any(e["frame"] >= ends[0]["frame"] and e["report"] != MOUSE_ZERO
               for e in base.of_kind(entries, C.RING_LIZARD_RPT))


def _processing(ctx, rig, snap):
    """The tests of lizard mode's processing: each part alone, the glide's ends, the ticks, and
    the build's own rule word."""
    has_rules = bool(rig.caps and rig.caps["build_flags"] & (1 << C.CAP_RULES))

    def need_rules(c):
        if not has_rules:
            c.skip("this firmware has none of lizard mode's processing (no RULES)")

    if ctx.want("lizard.stabiliser"):
        with ctx.check(GROUP, "lizard.stabiliser") as c:
            need_rules(c)
            rules = processing_rules(rig, M.RULE_LIZARD_STABILISER)
            check_sequence(c, rig, "right pad, setting 80 = 1", [
                (S(RPAD_TOUCH, rpad=(0, 0)), 1), (S(RPAD_TOUCH, rpad=(3000, 0)), 2),
                (S(RPAD_TOUCH, rpad=(4300, 0)), 1), (S(RPAD_TOUCH, rpad=(6000, 0)), 1),
                (S(RPAD_TOUCH, rpad=(7000, 0)), 1), (S(RPAD_TOUCH, rpad=(7000, 0)), 2),
                (S(RPAD_TOUCH, rpad=(9000, 2000)), 1), (S(), 1)], rules, [(80, 1)])
            check_sequence(c, rig, "left pad, always", [
                (S(LPAD_TOUCH, lpad=(0, 0)), 1), (S(LPAD_TOUCH, lpad=(8000, 0)), 1),
                (S(LPAD_TOUCH, lpad=(12000, 0)), 1), (S(LPAD_TOUCH, lpad=(16000, 4000)), 2),
                (S(), 1)], rules, [(80, 0)])
            check_sequence(c, rig, "right pad, setting 80 = 0", [
                (S(RPAD_TOUCH, rpad=(0, 0)), 1), (S(RPAD_TOUCH, rpad=(1280, 0)), 1),
                (S(), 1)], rules, [(80, 0)])
            rig.write_settings([(80, snap[80])])

    if ctx.want("lizard.smoothing"):
        with ctx.check(GROUP, "lizard.smoothing") as c:
            need_rules(c)
            rules = processing_rules(rig, M.RULE_LIZARD_SMOOTHING)
            check_sequence(c, rig, "a step of 8192", [
                (S(RPAD_TOUCH, rpad=(0, 0)), 1), (S(RPAD_TOUCH, rpad=(8192, 0)), 1),
                (S(RPAD_TOUCH, rpad=(8192, 0)), 10), (S(), 1)], rules)
            check_sequence(c, rig, "constant speed, then the lift-off", [
                (S(RPAD_TOUCH, rpad=(0, 0)), 1), (S(RPAD_TOUCH, rpad=(-1000, 300)), 1),
                (S(RPAD_TOUCH, rpad=(-2000, 600)), 1), (S(RPAD_TOUCH, rpad=(-3000, 900)), 1),
                (S(LPAD_TOUCH, lpad=(0, 0)), 1), (S(LPAD_TOUCH, lpad=(0, 16384)), 1), (S(), 1)],
                rules)

    if ctx.want("lizard.glide"):
        with ctx.check(GROUP, "lizard.glide") as c:
            need_rules(c)
            rules = processing_rules(rig, M.RULE_LIZARD_GLIDE)
            check_sequence(c, rig, "a flick on the right pad, setting 12 = 8", _flick(), rules,
                           [(11, 200), (12, 8)])
            check_sequence(c, rig, "a flick on the left pad (scroll), setting 12 = 6",
                           _flick(touch=LPAD_TOUCH, axis=1), rules, [(12, 6)])
            slow = _flick(step=2000, x0=-8000)    # a flick speed of 666 per frame
            check_sequence(c, rig, "a flick speed of 666, setting 11 = 200: it glides", slow,
                           rules, [(11, 200), (12, 8)])
            check_sequence(c, rig, "the same, setting 11 = 1000: too slow to glide", slow,
                           rules, [(11, 1000), (12, 8)])
            check_sequence(c, rig, "a short touch (16 frames) does not glide",
                           _flick(hold=2), rules, [(11, 200), (12, 8)])

    if ctx.want("lizard.glide_stops"):
        with ctx.check(GROUP, "lizard.glide_stops") as c:
            need_rules(c)
            rules = processing_rules(rig, M.RULE_LIZARD_GLIDE)
            # a new touch ends the glide: lift for two frames, then a touch held still
            steps = [(state, 4) for state, _h in _flick()[2:]] + [
                (S(), 2), (S(RPAD_TOUCH, rpad=(0, 0)), 4)]
            check_sequence(c, rig, "a new touch", steps, rules, [(11, 200), (12, 100)])
            # 0x81 in the middle of a glide: the release pair, then nothing
            c.true("a glide runs before the 0x81", _start_glide(rig, rules))
            mark = rig.ring_next()
            rig.vendor_set(C.VC_LIZARD_OFF)
            rig.read_stream(count=12)
            e = [x for x in _ring_since(rig, mark) if x["kind"] in (C.RING_LIZARD,
                                                                   C.RING_LIZARD_RPT)]
            c.eq("0x81 mid-glide: leave, keyboard empty, mouse zero, then nothing",
                 [(x["kind"], x.get("what"), x.get("report")) for x in e],
                 [(C.RING_LIZARD, C.LIZARD_LEAVE, None), (C.RING_LIZARD_RPT, None, KEYS_EMPTY),
                  (C.RING_LIZARD_RPT, None, MOUSE_ZERO)])
            rig.write_settings([(9, 1)])
            # ARM in the middle of a glide: its resync zeroes the momentum
            c.true("a glide runs before the ARM", _start_glide(rig, rules))
            rig.arm()
            mark = rig.ring_next()
            rig.read_stream(count=12)
            rpts = base.of_kind(rig.ring_all(mark)[2], C.RING_LIZARD_RPT)
            c.eq("after ARM mid-glide: no lizard report", len(rpts), 0)
            rig.ready_to_inject()   # this arming's canary too, as after every other ARM here
            rig.write_settings([(12, snap[12])])

    if ctx.want("lizard.ticks"):
        with ctx.check(GROUP, "lizard.ticks") as c:
            need_rules(c)
            rules = processing_rules(rig, M.RULE_LIZARD_TICKS) & ~M.RULE_PAD_CLICK
            moves = [(S(RPAD_TOUCH, rpad=(-9600 + 1600 * k, 0)), 1) for k in range(8)]
            check_sequence(c, rig, "steps of 1600, setting 14 = 5500 (a tick every fourth)",
                           moves, rules, [(14, 5500), (70, 1)])
            check_sequence(c, rig, "setting 14 = 0: a tick every step", moves, rules,
                           [(14, 0), (70, 1)])
            check_sequence(c, rig, "the pad click, on and off", [
                (S(RPAD_TOUCH), 1), (S(RPAD_TOUCH | RPAD_CLICK), 2), (S(RPAD_TOUCH), 1),
                (S(), 1)], rules, [(14, 5500), (53, 40)])
            check_sequence(c, rig, "setting 70 = 0: none", moves, rules, [(14, 0), (70, 0)])
            rig.write_settings([(n, snap[n]) for n in (14, 53, 70)])

    if ctx.want("lizard.default"):
        with ctx.check(GROUP, "lizard.default") as c:
            need_rules(c)
            _now, build = rig.rules()
            c.note("the build's rule word 0x%03x: %s" % (build, ", ".join(M.rule_names(build))))
            check_sequence(c, rig, "a flick with everything the build has, setting 12 = 12",
                           _flick(step=7000, x0=-25000), build, [(12, 12), (14, 5500)])
            rig.write_settings([(12, snap[12])])
