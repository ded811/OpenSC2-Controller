# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# lizard: lizard mode's logic on injected inputs (docs/testing.md, group "lizard"), opt-in
# (--lizard). The only group that injects with lizard mode on: its sequences carry the lizard-test
# flag, so only the guard keeps the mouse and keyboard reports lizard mode makes off the PC; the
# ring records each one (LIZARD_RPT, with its frame) and the runner compares them, frame by frame,
# with the lizard model (proto/model.py, held to lizard.c by the shared vectors). It runs only after
# the guard canary passed, with the leak detector watching, and last in its phase.
from . import base
from .. import config as C
from ..proto import model as M
from ..proto import reports as R

GROUP = "lizard"
TESTS = [("lizard.on_at_start", "WR", ["lizard"]), ("lizard.keyboard", "WR", ["lizard"]),
         ("lizard.pointer", "WR", ["lizard"]), ("lizard.scroll", "WR", ["lizard"]),
         ("lizard.buttons", "WR", ["lizard"]), ("lizard.thumb_on_stick", "WR", ["lizard"]),
         ("lizard.off", "WR", ["lizard"]), ("lizard.on_by_setting", "WR", ["lizard"]),
         ("lizard.switches", "WR", ["lizard"]), ("lizard.no_leak", "WR", ["lizard"])]

A, B, X, VIEW, MENU = 1 << 0, 1 << 1, 1 << 2, 1 << 6, 1 << 14
DOWN, RIGHT, LEFT, UP = 1 << 10, 1 << 11, 1 << 12, 1 << 13
RPAD_TOUCH, LPAD_TOUCH, RPAD_CLICK, RSTICK_TOUCH = 1 << 21, 1 << 25, 1 << 22, 1 << 20
MOUSE_ZERO = bytes([C.RPT_MOUSE]) + bytes(5)
KEYS_EMPTY = bytes([C.RPT_KEYBOARD]) + bytes(8)


def S(buttons=0, rpad=(0, 0), lpad=(0, 0), rt=0, lt=0):
    return R.make_state52(buttons=buttons, rpad_x=rpad[0], rpad_y=rpad[1], lpad_x=lpad[0],
                          lpad_y=lpad[1], right_trigger=rt, left_trigger=lt)


def expected(steps, s68, s82):
    """The lizard model on the steps (state52, hold): per step the reports its frames make, and
    the reports of the frame after the sequence (the application's state, all zero)."""
    lz = M.LizardModel()
    lz.step()                                   # in sync with the application's state
    prev = [False, False]
    out = []
    for state, hold in list(steps) + [(R.make_state52(), 1)]:
        made = []
        for _ in range(hold):
            exp, prev[0], prev[1] = M.expected_report(state, prev[0], prev[1], setting68=s68)
            f = R.decode_state52(exp)
            made += [r for _k, r in lz.step(buttons=f["buttons"], lpad=(f["lpad_x"], f["lpad_y"]),
                                            rpad=(f["rpad_x"], f["rpad_y"]), s82=s82)]
        out.append(made)
    return out[:-1], out[-1]


def run_sequence(rig, steps):
    """Load and run `steps` (state52, hold) with the lizard-test flag: (per step the diverted
    reports of its frames, the reports after it), or None when the steps did not all start."""
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
    # count alone does not prove the sequence is over: wait for the board's own SEQ_END, then one
    # more frame or so for the lizard reports of the frame it ended in.
    rig.ring_wait(start, lambda e: bool(base.of_kind(e, C.RING_SEQ_END)), timeout=1.0)
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
    after = [r for f, r in rpts if end <= f < end + 1]
    return per, after


def check_sequence(c, rig, name, steps, s68=90, s82=3):
    # ARM again first: lizard mode resyncs (its remainders and pads cleared), so the model starts
    # from the same state; the canary runs again before the sequence
    rig.arm()
    rig.ready_to_inject()
    got = run_sequence(rig, steps)
    if got is None:
        c.fail("%s: the sequence did not run" % name)
        return
    want, want_after = expected(steps, s68, s82)
    per, after = got
    # lists of numbers, not hex: a failure line must show the reports (R25's backstop would
    # mask a hex string of 16 digits or more)
    for i, (g, w) in enumerate(zip(per, want)):
        c.eq("%s step %d" % (name, i), base.byte_lists(g), base.byte_lists(w))
    c.eq("%s: after the sequence" % name, base.byte_lists(after), base.byte_lists(want_after))


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
    s68 = snap[68]
    try:
        _run(ctx, rig, snap, s68)
    finally:
        rig.ctx.lizard_group = False
        rig.seq_stop()
        rig.inject_off()
        pairs = [(n, snap[n]) for n in (82, 68, 9) if rig.settings_read()[n] != snap[n]]
        if pairs:
            rig.write_settings(pairs)


def _run(ctx, rig, snap, s68):
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
                                            (S(B | VIEW), 1), (S(0x7F), 1), (S(), 1)], s68)
            check_sequence(c, rig, "arrows", [(S(DOWN), 1), (S(), 1), (S(RIGHT), 1), (S(), 1),
                                              (S(LEFT), 1), (S(), 1), (S(UP), 1), (S(), 1)], s68)
            check_sequence(c, rig, "menu", [(S(MENU), 1), (S(), 1)], s68)

    if ctx.want("lizard.pointer"):
        with ctx.check(GROUP, "lizard.pointer") as c:
            check_sequence(c, rig, "pointer", [
                (S(RPAD_TOUCH, rpad=(0, 0)), 1), (S(RPAD_TOUCH, rpad=(1280, 0)), 1),
                (S(RPAD_TOUCH, rpad=(1280, 1280)), 1), (S(RPAD_TOUCH, rpad=(1280, 1280)), 2),
                (S(), 1)], s68)
            check_sequence(c, rig, "remainders", [
                (S(RPAD_TOUCH, rpad=(0, 0)), 1), (S(RPAD_TOUCH, rpad=(100, 0)), 1),
                (S(RPAD_TOUCH, rpad=(200, 0)), 2), (S(RPAD_TOUCH, rpad=(0, 0)), 1),
                (S(RPAD_TOUCH, rpad=(-100, 0)), 1), (S(RPAD_TOUCH, rpad=(-200, 0)), 1),
                (S(), 1)], s68)

    if ctx.want("lizard.scroll"):
        with ctx.check(GROUP, "lizard.scroll") as c:
            check_sequence(c, rig, "scroll", [(S(LPAD_TOUCH, lpad=(0, 0)), 1),
                                              (S(LPAD_TOUCH, lpad=(3072, 2048)), 1), (S(), 1)], s68)

    if ctx.want("lizard.buttons"):
        with ctx.check(GROUP, "lizard.buttons") as c:
            check_sequence(c, rig, "triggers", [(S(rt=32767), 3), (S(), 1), (S(lt=32767), 2),
                                                (S(), 1)], s68)
            check_sequence(c, rig, "pad click", [(S(RPAD_TOUCH | RPAD_CLICK), 1), (S(), 1)], s68)

    if ctx.want("lizard.thumb_on_stick"):
        with ctx.check(GROUP, "lizard.thumb_on_stick") as c:
            for s82 in (3, 0):
                rig.write_settings([(82, s82)])
                check_sequence(c, rig, "s82=%d" % s82, [
                    (S(RSTICK_TOUCH | RPAD_TOUCH, rpad=(0, 0)), 1),
                    (S(RSTICK_TOUCH | RPAD_TOUCH, rpad=(1280, 0)), 1),
                    (S(RSTICK_TOUCH | RPAD_TOUCH, rpad=(2560, 0)), 1), (S(), 1)], s68, s82)
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
