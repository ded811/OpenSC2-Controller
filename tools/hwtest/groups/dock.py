# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# dock: the board's dock link on a bare board (docs/testing.md, group "dock"), opt-in (--dock).
# Nothing is wired and nothing touches Valve's puck's pads: the test hooks' dock ops hand the
# board 73-byte frames as if the puck had sent them over its charging pads, and read the answers
# back (proto/dock.py). Checked: the frame rules (the CRC, a short frame, nothing arriving, the
# start byte ignored), the probe's identity record against the identity the board reports
# elsewhere (its serials only by CRC-32: the board masks them), vendor commands on the dock's own
# stored request, 0x81 and 0x85 doing nothing there, GET input from the copy of the latest state
# report without taking a sequence number, the dock alone with the radio stopped (attribute 11 =
# 64000, one new report per request, no 0x43, 0xBE answered), the pilot's 2-of-3 rule, and the
# docked states on injected supply and events, which the board records and does not act on.
import struct

from . import base
from .. import config as C
from ..proto import dock as D
from ..proto import reports as R
from ..proto import testproto as T
from ..proto import vendor as V

GROUP = "dock"
SETTING_LIZARD = 9
TESTS = [("dock.present", "WR", ["dock"]), ("dock.frames", "WR", ["dock"]),
         ("dock.probe", "WR", ["dock"]), ("dock.commands", "WR", ["dock"]),
         ("dock.lizard", "WR", ["dock"]), ("dock.input", "WR", ["dock"]),
         ("dock.alone", "R", ["dock"]), ("dock.pilot", "WR", ["dock"]),
         ("dock.states", "WR", ["dock"])]

PROBE = D.build_request(D.PROBE)
INPUT = D.build_request(D.INPUT)
GET = D.build_request(D.GET)


def _skip_all(ctx, why):
    for tid, _p, _n in TESTS:
        if ctx.want(tid):
            ctx.record(GROUP, tid, base.SKIP, why)


def _frame(rig, frame, length=None, start=True):
    """One frame to the dock link (a session started when none runs)."""
    return rig.dock_frame(frame, length=length, start_session=start)


def _answered(c, what, r):
    """The run answered: its parsed answer, or None (recorded as a failure)."""
    if not c.eq("%s: DOCK_RUN status" % what, C.STATUS_NAMES.get(r["status"]), "OK"):
        return None
    if not c.eq("%s: outcome" % what, D.RUN_OUTCOMES.get(r["outcome"]), "answered"):
        return None
    a = D.parse_answer(r["answer"])
    c.eq("%s: start byte" % what, a["start"], D.START_CONTROLLER)
    c.true("%s: the answer's CRC checks" % what, r["crc_ok"])
    return a


def _set(c, rig, opcode, payload=b""):
    r = _frame(rig, D.set_request(opcode, payload))
    a = _answered(c, "SET 0x%02x" % opcode, r)
    return a["result"] if a else None


def _get(c, rig, what):
    r = _frame(rig, GET)
    return _answered(c, what, r)


def _ring_since(rig, seq):
    return rig.ring_all(seq)[2]


def _identity(rig):
    """What the board says it is, in memory only: `opensc2 id` (wireless) or the USB commands
    (wired). Serials stay None where they cannot be read unmasked."""
    if rig.identity_r:
        i = dict(rig.identity_r)
        return {"pid": i.get("pid"), "build": i.get("build"), "blbuild": i.get("blbuild"),
                "rev": i.get("rev"), "unit": i.get("unit"), "board": i.get("board"),
                "git": i.get("git")}
    if rig.path != "W":
        return None
    attrs = dict(V.decode_attributes(rig.vendor(C.VC_ATTRIBUTES).data))
    unit = V.decode_string_attribute(rig.vendor(C.VC_STRING_ATTR, b"\x01").data)[1]
    board = V.decode_string_attribute(rig.vendor(C.VC_STRING_ATTR, b"\x00").data)[1]
    git = V.decode_string_attribute(rig.vendor(C.VC_STRING_ATTR, b"\x03").data)[1]
    return {"pid": attrs.get(1), "build": attrs.get(4), "blbuild": attrs.get(10),
            "rev": attrs.get(9), "unit": unit, "board": board, "git": git}


def run(ctx):
    rig = ctx.rig
    if not ctx.opt.get("dock"):
        _skip_all(ctx, "opt-in (--dock)")
        return
    if not rig.models("dock"):
        _skip_all(ctx, "not modelled by the %s backend (checked on hardware)" % rig.backend.kind)
        return
    if not rig.ctx.armed:
        rig.arm()
    caps = rig.caps or {}
    if not caps.get("build_flags", 0) & (1 << C.CAP_DOCK):
        _skip_all(ctx, "this firmware has no dock link (caps bit %d)" % C.CAP_DOCK)
        return
    try:
        _run(ctx, rig)
    finally:
        try:
            # the dock's stored request as after a boot (0x83), no session, the states reset
            rig.dock_frame(D.set_request(C.VC_ATTRIBUTES), start_session=True)
            rig.dock_end()
            rig.dock_ctrl([D.CTRL_RESET])
        except Exception:
            pass


def _run(ctx, rig):
    path = rig.path

    if ctx.want("dock.present"):
        with ctx.check(GROUP, "dock.present") as c:
            st = rig.dock_status()
            if st["active"]:
                c.note("a dock session was running; ended")
                rig.dock_end()
                st = rig.dock_status()
            c.true("no dock session before the first frame", not st["active"])
            c.observe(stored=st["stored"], sessions=st["sessions"], uart=st["uart"])
            c.note("dock link present%s; the dock's stored request 0x%02x" % (
                " with the UART glue" if st["uart"] else "", st["stored"]))

    if ctx.want("dock.frames"):
        with ctx.check(GROUP, "dock.frames") as c:
            seq0 = rig.ring_next()
            rig.dock_end()
            r = _frame(rig, PROBE, start=False)
            c.eq("no session, none asked for: DOCK_RUN", C.STATUS_NAMES.get(r["status"]),
                 "NO_LINK")
            a = _answered(c, "a probe", _frame(rig, PROBE))
            if a:
                c.eq("the probe's type", a["type"], D.PROBE)
            # the start byte and bytes 2..7 are not read; resealed, the frame is still a probe
            odd = bytearray(PROBE)
            odd[2:8] = b"\x11\x22\x33\x44\x55\x66"
            a = _answered(c, "a probe with start byte 0x00", _frame(rig, D.seal(odd, 0x00)))
            if a:
                c.eq("its type", a["type"], D.PROBE)
            # a type the puck never sends: its number back with -22 (OpenSC2's choice; the real
            # controller repeats its previous answer)
            a = _answered(c, "type 4", _frame(rig, D.build_request(4)))
            if a:
                c.eq("type 4: (type, result)", (a["type"], a["result"]), (4, D.RESULT_INVALID))
            r = _frame(rig, INPUT, length=0)
            c.eq("nothing arrives: outcome", D.RUN_OUTCOMES.get(r.get("outcome")), "nothing")
            c.true("... and the session goes on", r.get("session") is True)
            r = _frame(rig, INPUT, length=40)
            c.eq("40 bytes: outcome", D.RUN_OUTCOMES.get(r.get("outcome")), "short")
            c.true("... which ends the session", r.get("session") is False)
            bad = bytearray(INPUT)
            bad[30] ^= 0x04
            r = _frame(rig, bytes(bad))
            c.eq("a flipped bit: outcome", D.RUN_OUTCOMES.get(r.get("outcome")), "bad CRC")
            c.true("... which ends the session", r.get("session") is False)
            ring = _ring_since(rig, seq0)
            sessions = base.of_kind(ring, C.RING_DOCK_SESSION)
            frames = base.of_kind(ring, C.RING_DOCK_FRAME)
            c.eq("DOCK_SESSION entries (start, end, start, end)",
                 [(e["what"], e["reason"]) for e in sessions], [(0, 1), (1, 1), (0, 1), (1, 2)])
            c.eq("DOCK_FRAME outcomes", [e["outcome"] for e in frames], [0, 0, 0, 1, 2])
            c.true("every dock entry carries link 5",
                   all(e["link"] == C.RING_LINK_DOCK for e in sessions + frames))
            st = rig.dock_status()
            c.observe(errors=st["errors"], slowest_us=st["slowest_us"])

    if ctx.want("dock.probe"):
        with ctx.check(GROUP, "dock.probe") as c:
            ident = _identity(rig)
            r = _frame(rig, PROBE)
            a = _answered(c, "the probe", r)
            if a:
                p = a["payload"]
                c.eq("type and result", (a["type"], a["result"]), (D.PROBE, 0))
                pid, build, blbuild, rev = struct.unpack_from("<IIII", p, 0)
                if ident:
                    c.eq("product ID", pid, ident["pid"])
                    c.eq("build time", build, ident["build"])
                    c.eq("bootloader build time", blbuild, ident["blbuild"])
                    c.eq("board revision", rev, ident["rev"])
                    c.eq("git revision", p[44:57].split(b"\0", 1)[0].decode("ascii", "replace"),
                         ident["git"])
                else:
                    c.eq("product ID", pid, 0x1302)
                c.eq("the serials as the PC reads them (masked)", list(p[16:44]), [0] * 28)
                c.eq("the 6 bytes after the record", list(p[57:]), [0] * 6)
                c.true("the answer says it masked the serials", r["masked"])
                if ident and ident.get("unit") and ident.get("board"):
                    want = D.identity_record(ident)
                    c.eq("the serials' CRC-32 (unit and board serial)", r["masked_crc32"],
                         T.crc32(want[16:44]))
                    c.eq("the whole answer's CRC-32 against the model", r["crc32"],
                         T.crc32(D.answer(D.PROBE, 0, want)))
                    c.note("the identity record as the board reports itself elsewhere (serials "
                           "compared by CRC-32)")
                else:
                    c.note("the identity record's numbers checked; the serials are not readable "
                           "unmasked on this path")

    if ctx.want("dock.commands"):
        with ctx.check(GROUP, "dock.commands") as c:
            seq0 = rig.ring_next()
            c.eq("SET 0x83", _set(c, rig, C.VC_ATTRIBUTES), 0)
            a = _get(c, rig, "GET 0x83")
            if a:
                c.eq("GET 0x83: result", a["result"], D.RESULT_GET_OK)
                c.eq("GET 0x83: opcode, length", list(a["payload"][:2]), [0x83, 30])
                attrs = dict(V.decode_attributes(a["payload"][2:32]))
                c.eq("attribute 11 with %s up" % ("USB" if path == "W" else "the radio"),
                     attrs.get(11), 4000)
            settings = rig.settings_read()
            c.eq("SET 0x89 45", _set(c, rig, C.VC_SETTINGS_GET, b"\x2d"), 0)
            a = _get(c, rig, "GET 0x89 45")
            if a:
                c.eq("GET 0x89 45: result", a["result"], D.RESULT_GET_OK)
                c.eq("GET 0x89 45: the answer", list(a["payload"][:5]),
                     [0x89, 3, 45] + list(struct.pack("<h", settings.get(45, 0))))
            # the main link keeps its own stored request: a GET there is not the dock's 0x89
            main = rig.vendor_get()
            c.true("the main link's stored request is its own (0x%02x)" % main.opcode,
                   main.opcode != 0x89 or path == "R")
            c.eq("SET 0x89 200 (no such setting)", _set(c, rig, C.VC_SETTINGS_GET, b"\xc8"), 0)
            a = _get(c, rig, "GET 0x89 200")
            if a:
                c.eq("GET 0x89 200: a negative result", a["result"], -22)
            old = settings.get(45, 50)
            new = old - 1 if old > 0 else 1
            c.eq("SET 0x87 {45, %d}" % new,
                 _set(c, rig, C.VC_SETTINGS_SET, V.settings_triplets([(45, new)])), 0)
            c.eq("setting 45 written from the dock", rig.settings_read().get(45), new)
            a = _get(c, rig, "GET after 0x87")
            if a:
                c.eq("GET after 0x87: nothing to read", a["result"], D.RESULT_NOTHING_TO_READ)
            c.eq("SET 0x87 {45, %d} back" % old,
                 _set(c, rig, C.VC_SETTINGS_SET, V.settings_triplets([(45, old)])), 0)
            vendor = [e for e in base.of_kind(_ring_since(rig, seq0), C.RING_VENDOR)
                      if e["link"] == C.RING_LINK_DOCK]
            c.true("VENDOR entries on link 5 for the dock's commands (%d)" % len(vendor),
                   len(vendor) >= 8)
            # the puck waits 64 ms for an answer, after its own 6.3 ms frame: well inside that
            slowest = rig.dock_status()["slowest_us"]
            c.observe(slowest_us=slowest)
            c.true("every answer within 50 ms of its frame (slowest %d us)" % slowest,
                   slowest < 50000)

    if ctx.want("dock.lizard"):
        with ctx.check(GROUP, "dock.lizard") as c:
            before = rig.settings_read().get(SETTING_LIZARD)
            seq0 = rig.ring_next()
            c.eq("0x81 from the dock: result", _set(c, rig, C.VC_LIZARD_OFF), 0)
            c.eq("0x85 from the dock: result", _set(c, rig, C.VC_LIZARD_DEFAULTS_ON), 0)
            c.eq("setting 9 unchanged", rig.settings_read().get(SETTING_LIZARD), before)
            ring = _ring_since(rig, seq0)
            c.eq("no LIZARD entry", len(base.of_kind(ring, C.RING_LIZARD)), 0)
            c.eq("no SETTING entry", len(base.of_kind(ring, C.RING_SETTING)), 0)

    if ctx.want("dock.input"):
        with ctx.check(GROUP, "dock.input") as c:
            base.need(rig, "inputs")
            rig.lizard_off()
            # a session first: the dock keeps its copy of the stream's reports only while one
            # runs (the first GET input of a new session answers whatever the copy held)
            _answered(c, "a probe (a session)", _frame(rig, PROBE))
            state = R.make_state52(buttons=1 << R.BIT["A"], lstick_x=1234, right_trigger=2000)
            rep, n = rig.inject_and_read(state)
            if rep is None:
                c.fail("no report carried the injected state")
            else:
                r = _frame(rig, INPUT)
                a = _answered(c, "GET input", r)
                if a:
                    c.eq("GET input: type and result", (a["type"], a["result"]), (D.INPUT, 0))
                    body = a["payload"][:53]
                    c.eq("the 52 state bytes are the stream's", base.byte_lists([body[1:]]),
                         base.byte_lists([R.state52_of_report(rep)]))
                    c.eq("nothing after the 53 bytes", list(a["payload"][53:]), [0] * 10)
                # the dock takes no report of the link's stream
                rig.cnt_reset()
                for _ in range(6):
                    _frame(rig, INPUT)
                    rig.sleep(0.02)
                cnt = rig.cnt_read()
                sent = cnt.get("usb_sent" if path == "W" else "radio_sent", 0)
                lost = cnt.get("usb_dropped" if path == "W" else "radio_dropped", 0)
                gap = cnt.get("frames", 0) - (sent + lost)
                c.true("frames - (sent + dropped) is -1, 0 or 1 with six dock requests (%d)" % gap,
                       gap in (-1, 0, 1))
            rig.inject_off()

    if ctx.want("dock.alone") and path == "R":
        with ctx.check(GROUP, "dock.alone") as c:
            base.need(rig, "radio")
            _alone(c, rig)

    if ctx.want("dock.pilot"):
        with ctx.check(GROUP, "dock.pilot") as c:
            rig.dock_end()
            seq0 = rig.ring_next()
            a = rig.dock_ctrl(D.pilot_args([1999, 3301, 2500]))
            c.eq("one reading inside: (present, inside, session)", tuple(a["result"][:3]),
                 (0, 0x04, 0))
            a = rig.dock_ctrl(D.pilot_args([2500, 3300, None]))
            c.eq("two inside, one failed: (present, inside, session)", tuple(a["result"][:3]),
                 (1, 0x03, 1))
            ring = _ring_since(rig, seq0)
            pilots = base.of_kind(ring, C.RING_DOCK_PILOT)
            c.eq("DOCK_PILOT entries (present)", [e["present"] for e in pilots], [0, 1])
            c.eq("their readings", [e["mv"] for e in pilots], [[1999, 3301, 2500],
                                                                [2500, 3300, -1]])
            c.eq("the session the check started",
                 [(e["what"], e["reason"]) for e in base.of_kind(ring, C.RING_DOCK_SESSION)],
                 [(0, 1)])
            rig.dock_end()

    if ctx.want("dock.states"):
        with ctx.check(GROUP, "dock.states") as c:
            _states(c, rig)


def _alone(c, rig):
    """The radio stopped: the dock is the board's only link."""
    with rig.own_link_drop():
        rig.console_run("opensc2 stop")
        if not c.true("the radio off within 1.5 s", rig.wait_link("off", 1.5)):
            rig.console_run("opensc2 start")
            rig.wait_link("connected", 3.0)
            return
        try:
            a = _answered(c, "a probe", _frame(rig, PROBE))
            st = rig.dock_status()
            c.true("the dock is the only link", st["alone"])
            c.eq("SET 0x83", _set(c, rig, C.VC_ATTRIBUTES), 0)
            a = _get(c, rig, "GET 0x83")
            if a:
                c.eq("attribute 11 with the dock alone",
                     dict(V.decode_attributes(a["payload"][2:32])).get(11), D.INTERVAL_US)
            rig.cnt_reset()
            seqs = []
            for _ in range(5):
                a = _answered(c, "GET input", _frame(rig, INPUT))
                if a:
                    seqs.append(a["payload"][0])
            cnt = rig.cnt_read()
            c.eq("one report built per GET input", cnt.get("frames"), 5)
            c.true("their sequence numbers one apart (%s)" % seqs,
                   len(seqs) == 5 and all((b - a) & 0xFF == 1 for a, b in zip(seqs, seqs[1:])))
            # the battery: 0x43 has no way out, 0xBE answers over the dock
            fields = {"charge_state": 2, "level": 42, "battery_mv": 3800, "system_mv": 3300,
                      "input_mv": 5000, "current_ma": 300, "input_current_ma": 400,
                      "temperature": 0x0123, "gauge_flags": 0}
            dropped0 = rig.cnt_read().get("battery_dropped", 0)
            rig.battery(send_now=True, **fields)
            c.true("the 0x43 sent now was dropped (no link but the dock)",
                   rig.cnt_read().get("battery_dropped", 0) > dropped0)
            c.eq("SET 0xBE", _set(c, rig, C.VC_BATTERY_DATA), 0)
            a = _get(c, rig, "GET 0xBE")
            if a:
                c.eq("0xBE over the dock", list(a["payload"][2:17]), list(R.encode_be(fields)))
            rig.battery_off()
        finally:
            rig.dock_end()
            rig.console_run("opensc2 start")
            c.true("connected again within 3 s", rig.wait_link("connected", 3.0))


def _states(c, rig):
    """A script of supply readings and events against the model; the board records each step and
    does nothing it decides (no POWER entry, the link and the mode as they were)."""
    st0 = rig.status()
    seq0 = rig.ring_next()
    rig.dock_ctrl([D.CTRL_RESET])
    model = D.DockStates()
    rule = [False, False, 0]
    noted = 0          # DOCK_STATE entries: one per event, one per supply reading that posts
    script = [("supply", (0, 1, 1, 0, 0)), ("event", (10, 0)), ("event", (3, 3)),
              ("event", (4, 3)), ("event", (8, 0)), ("supply", (0, 0, 0, 0, 0)),
              ("supply", (0, 1, 1, 0, 0)), ("supply", (0, 0, 0, 0, 0)), ("event", (5, 0)),
              ("supply", (0, 1, 1, 2, 0)), ("event", (11, 0)), ("supply", (1, 0, 0, 0, 0)),
              ("supply", (0, 0, 0, 0, 0))]
    for kind, args in script:
        if kind == "supply":
            usb, plus, pilot, entry, warm = args
            a = rig.dock_ctrl([D.CTRL_SUPPLY, usb, plus, pilot, entry, warm])
            ev = D.power_step(rule, usb, plus, pilot)
            if ev == D.SUPPLY_DOCK and model.state == D.AWAY:
                want = model.enter(entry, warm)
            elif ev is not None:
                want = model.step(ev, 0)
            else:
                want = (model.state, model.state, 0, False)
            what = "supply %d%d%d" % (usb, plus, pilot)
            c.eq("%s: event" % what, a["result"][0], 0xFF if ev is None else ev)
            noted += ev is not None
        else:
            ev, chord = args
            a = rig.dock_ctrl([D.CTRL_EVENT, ev, chord])
            want = model.step(ev, chord)
            what = "event %s" % D.EVENTS[ev]
            noted += 1
        res = a["result"]
        c.eq("%s: status" % what, C.STATUS_NAMES.get(a["status"]), "OK")
        c.eq("%s: (from, to, handled)" % what, (D.STATES[res[1]], D.STATES[res[2]], res[3]),
             (D.STATES[want[0]], D.STATES[want[1]], 1 if want[3] else 0))
        c.eq("%s: actions" % what, D.action_names(struct.unpack_from("<I", res, 4)[0]),
             D.action_names(want[2]))
    ring = _ring_since(rig, seq0)
    c.eq("one DOCK_STATE entry per step that posted", len(base.of_kind(ring, C.RING_DOCK_STATE)),
         noted)
    c.eq("no POWER entry: nothing was done", len(base.of_kind(ring, C.RING_POWER)), 0)
    st1 = rig.status()
    c.eq("the mode and the link as they were", (st1["mode"], st1["link_state"]),
         (st0["mode"], st0["link_state"]))
    rig.dock_ctrl([D.CTRL_RESET])
