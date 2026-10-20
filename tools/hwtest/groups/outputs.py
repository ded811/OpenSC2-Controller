# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# outputs: rumble and the haptic output reports (docs/testing.md, group "outputs"). The runner
# sends each report as a host does (W: WriteFile on interrupt OUT, which Windows pads to 64 bytes;
# B: WriteFile on the Bluetooth HID device, which writes the report's characteristic and the
# library hands its whole declared size on; R: HOST_OUT on the test link) and checks, byte for
# byte, the ring entries the library made: an
# OUTPUT entry with the decode result, then the RUMBLE or HAPTIC entries with the decoded fields and
# the channels the target selects. Nothing is wired, so the ring is the only place these show. A
# stop (0x81 with an on time of 0, 0x82 command 0, a zero speed in 0x80, 0x86's stream stop) also
# brings the real controller's 0x44 replies: a STOP_STATUS entry and the report, per channel.
import struct

from . import base
from .. import config as C
from ..proto import model as M
from ..proto import testproto as T

GROUP = "outputs"
TESTS = [("outputs.rumble", "WRB", []), ("outputs.rumble_timeout", "WRB", []),
         ("outputs.rumble_linkdown", "RB", []), ("outputs.haptics", "WRB", []),
         ("outputs.pcm", "WRB", []), ("outputs.ignored", "WRB", []),
         ("outputs.short", "WRB", []), ("outputs.exact_length", "WRB", []),
         ("outputs.stop_status", "WRB", [])]

RUMBLE_A = bytes.fromhex("8000E80310270020" "4E00")
RUMBLE_B = bytes.fromhex("8007E8031027FD20" "4E04")
RUMBLE_STOP = bytes([0x80]) + bytes(9)

# outputs.stop_status: a report, and the 0x44 replies {channel, 0x02} the real controller sends for
# it, in order (proto_rules.c; left grip 0, right grip 1, left pad 3, right pad 4)
LG, RG, LP, RP = C.STATUS_LEFT_GRIP, C.STATUS_RIGHT_GRIP, C.STATUS_LEFT_PAD, C.STATUS_RIGHT_PAD
STOP_CASES = (
    ("Steam's zero pulse, target 0 (the right pad)", bytes([0x81, 0]) + bytes(6), [RP]),
    ("Steam's zero pulse, target 1 (the left pad)", bytes([0x81, 1]) + bytes(6), [LP]),
    ("0x81 on time 0, both pads: left first", bytes([0x81, 2, 0, 0, 1, 0, 3, 0]), [LP, RP]),
    ("0x81 on time 0, both grips", bytes([0x81, 5]) + bytes(6), [LG, RG]),
    ("0x82 stop all, both pads", bytes([0x82, 6, 0, 0]), [LP, RP]),
    ("0x82 stop all, the left grip", bytes([0x82, 3, 0, 0]), [LG]),
    ("0x80 with both speeds (no stop)", RUMBLE_A, []),
    ("0x80, the right speed 0", bytes([0x80, 0]) + struct.pack("<HHbHb", 1000, 10000, 0, 0, 0),
     [RG]),
    ("0x80, both speeds 0: the right grip first", RUMBLE_STOP, [RG, LG]),
    ("0x86 stream stop, both pads: right first", bytes([0x86, 1, 5, 0]), [RP, LP]),
    ("0x86 stream stop, both grips", bytes([0x86, 1, 2, 0]), [RG, LG]),
    ("0x81 pulse train (no stop)", bytes([0x81, 2]) + struct.pack("<HHH", 1000, 1000, 2), []),
    ("0x82 click A (no stop)", bytes([0x82, 2, 1, 0]), []),
    ("0x86 set format (no stop)", bytes([0x86, 2, 5, 1]), []),
    ("0x81 on time 0, a target that selects nothing", bytes([0x81, 8]) + bytes(6), []),
)

# one report per ID with distinct field values (the target byte is filled in), and what the HAPTIC
# entry decodes from it
HAPTIC_VECTORS = {
    0x81: (bytes.fromhex("10272" "04E0300"), dict(haptic_kind=0, on_us=10000, off_us=20000, count=3)),
    0x82: (bytes([2, 0xFB]), dict(haptic_kind=1, command=2, gain=-5)),
    0x83: (bytes.fromhex("FD2C0164000A0032"), dict(haptic_kind=2, gain=-3, freq=300,
                                                    duration=100, lfo=10, depth=50)),
    0x84: (bytes.fromhex("05640064" "00E803"), dict(haptic_kind=3, gain=5, duration=100,
                                                     start=100, end=1000)),
    0x85: (bytes([7, 0xFF]), dict(haptic_kind=4, id=7, gain=-1)),
}
MIN_LEN = {0x80: 9, 0x81: 7, 0x82: 3, 0x83: 9, 0x84: 8, 0x85: 3, 0x86: 2, 0x87: 1}
# the output reports' sizes after the ID, as the report map declares them (B: each characteristic)
DECLARED = {0x80: 9, 0x81: 7, 0x82: 3, 0x83: 9, 0x84: 8, 0x85: 3, 0x86: 3, 0x87: 63, 0x88: 63,
            0x89: 63}


def _link(rig):
    return {"W": 1, "B": 4}.get(rig.path, 2)


def _send(rig, report):
    """The report as a host sends it; returns its length as the board sees it on W (64) or as
    sent on R (HOST_OUT)."""
    rig.output(report)
    return len(report)


def _entries_after(rig, start, report_count, timeout=0.5):
    """The ring from `start` once `report_count` OUTPUT entries are there."""
    _f, _n, entries = rig.ring_wait(start, lambda es: len(base.of_kind(es, C.RING_OUTPUT)) >=
                                    report_count, timeout)
    return entries


def _check_output(c, what, rig, e, report, decoded):
    c.eq("%s: OUTPUT id and result" % what, (e["report_id"], e["decode_result"]),
         (report[0], decoded))
    if rig.path == "W":
        lens = (len(report), 64)
    elif rig.path == "B":
        lens = (1 + DECLARED.get(report[0], 63),)
    else:
        lens = (len(report),)
    c.true("%s: OUTPUT length %d (%s)" % (what, e["total_len"], "/".join(map(str, lens))),
           e["total_len"] in lens)
    c.eq("%s: OUTPUT link" % what, e["link"], _link(rig))


def run(ctx):
    rig = ctx.rig
    if not rig.ctx.armed:
        rig.arm()

    if ctx.want("outputs.rumble"):
        with ctx.check(GROUP, "outputs.rumble") as c:
            base.need(rig, "outputs")
            start = rig.ring_next()
            _send(rig, RUMBLE_A)
            _send(rig, RUMBLE_B)
            entries = _entries_after(rig, start, 2)
            outs = base.of_kind(entries, C.RING_OUTPUT)
            rums = base.of_kind(entries, C.RING_RUMBLE)
            c.eq("OUTPUT entries", len(outs), 2)
            c.eq("RUMBLE entries", len(rums), 2)
            if len(outs) == 2 and len(rums) == 2:
                _check_output(c, "first", rig, outs[0], RUMBLE_A, 1)
                f = ("left", "right", "intensity", "left_gain", "right_gain", "type", "timeout")
                c.eq("first RUMBLE", tuple(rums[0][k] for k in f), (10000, 20000, 1000, 0, 0, 0, 0))
                c.eq("second RUMBLE (every field distinct)", tuple(rums[1][k] for k in f),
                     (10000, 20000, 1000, -3, 4, 7, 0))
                c.eq("RUMBLE link", rums[0]["link"], _link(rig))
            start = rig.ring_next()
            _send(rig, RUMBLE_STOP)
            entries = _entries_after(rig, start, 1)
            rums = base.of_kind(entries, C.RING_RUMBLE)
            c.true("a zero-speed 0x80 gives a zero RUMBLE at once",
                   len(rums) == 1 and rums[0]["left"] == rums[0]["right"] == 0 and
                   rums[0]["timeout"] == 0)

    if ctx.want("outputs.rumble_timeout"):
        with ctx.check(GROUP, "outputs.rumble_timeout") as c:
            base.need(rig, "rumble_timeout")
            timeout_ms = (rig.caps or {}).get("rumble_timeout_ms", C.RUMBLE_TIMEOUT_MS)
            # (a) one report, then nothing
            start = rig.ring_next()
            _send(rig, RUMBLE_A)
            rig.sleep(timeout_ms / 1000.0 + 0.4)
            _f, _n, entries = rig.ring_all(start)
            rums = base.of_kind(entries, C.RING_RUMBLE)
            if c.eq("(a) RUMBLE entries", len(rums), 2):
                dt = (rums[1]["time_us"] - rums[0]["time_us"]) / 1000.0
                c.eq("(a) the timeout entry", (rums[1]["left"], rums[1]["right"],
                                               rums[1]["timeout"], rums[1]["link"]), (0, 0, 1, 3))
                c.true("(a) %.0f ms after the report (%d +- 50)" % (dt, timeout_ms),
                       abs(dt - timeout_ms) <= 50)
            rig.sleep(1.0)
            # (b) every 100 ms for 1 s, then nothing
            start = rig.ring_next()
            for _ in range(10):
                _send(rig, RUMBLE_A)
                rig.sleep(0.1)
            rig.sleep(timeout_ms / 1000.0 + 0.3)
            _f, _n, entries = rig.ring_all(start)
            rums = base.of_kind(entries, C.RING_RUMBLE)
            stops = [e for e in rums if e["timeout"]]
            c.eq("(b) one timeout entry, after the repeats", len(stops), 1)
            if stops and len(rums) >= 11:
                c.true("(b) none while it was repeated", rums.index(stops[0]) == len(rums) - 1)
                dt = (stops[0]["time_us"] - rums[-2]["time_us"]) / 1000.0
                c.true("(b) %.0f ms after the last report (%d +- 50)" % (dt, timeout_ms),
                       abs(dt - timeout_ms) <= 50)
            rig.sleep(1.0)
            # (c) a zero-speed report 100 ms later: an immediate stop, no timeout later
            start = rig.ring_next()
            _send(rig, RUMBLE_A)
            rig.sleep(0.1)
            _send(rig, RUMBLE_STOP)
            rig.sleep(timeout_ms / 1000.0 + 0.3)
            _f, _n, entries = rig.ring_all(start)
            rums = base.of_kind(entries, C.RING_RUMBLE)
            c.eq("(c) entries: the report, its stop", len(rums), 2)
            if len(rums) == 2:
                c.eq("(c) the stop", (rums[1]["left"], rums[1]["timeout"], rums[1]["link"]),
                     (0, 0, _link(rig)))

    if ctx.want("outputs.rumble_linkdown"):
        with ctx.check(GROUP, "outputs.rumble_linkdown") as c:
            base.need(rig, "radio")
            start = rig.ring_next()
            _send(rig, RUMBLE_A)
            with rig.own_link_drop():       # the puck's empty reports at the drop: no leak
                rig.console_run("opensc2 stop")
                rig.wait_link("off", 2.0)
                _f, _n, entries = rig.ring_all(start, path=rig.console_path)
                rums = base.of_kind(entries, C.RING_RUMBLE)
                c.true("a zero RUMBLE with timeout 1 when the link goes down",
                       len(rums) >= 2 and rums[-1]["timeout"] == 1 and rums[-1]["left"] == 0)
                rig.console_run("opensc2 start")
                c.true("the link came back", rig.wait_link("connected", rig.reconnect_s))

    if ctx.want("outputs.haptics"):
        with ctx.check(GROUP, "outputs.haptics") as c:
            base.need(rig, "outputs")
            sent = 0
            start = rig.ring_next()
            for rid, (body, fields) in sorted(HAPTIC_VECTORS.items()):
                for target in list(range(8)) + [t | 0x80 for t in range(8)]:
                    report = bytes([rid, target]) + body
                    _send(rig, report)
                    sent += 1
                    entries = _entries_after(rig, start, 1)
                    start = rig.ring_next()
                    haps = base.of_kind(entries, C.RING_HAPTIC)
                    outs = base.of_kind(entries, C.RING_OUTPUT)
                    what = "0x%02x target 0x%02x" % (rid, target)
                    if not c.eq("%s: OUTPUT then HAPTIC" % what, (len(outs), len(haps)), (1, 1)):
                        continue
                    _check_output(c, what, rig, outs[0], report, 1)
                    h = haps[0]
                    c.eq("%s: id, target, channels" % what,
                         (h["report_id"], h["target"], h["channels"]),
                         (rid, target, M.target_channels(rid, target)))
                    want = dict(fields)
                    if rid == 0x84:
                        want["antiphase"] = 1 if (target & 0x7F) in (6, 7) else 0
                    c.eq("%s: fields" % what, {k: h.get(k) for k in want}, want)
            c.note("%d haptic reports (0x81..0x85, every target with and without bit 7)" % sent)

    if ctx.want("outputs.pcm"):
        with ctx.check(GROUP, "outputs.pcm") as c:
            base.need(rig, "outputs")
            count = 0
            for op in (0, 1, 2):
                for fmt in range(12):
                    start = rig.ring_next()
                    report = bytes([0x86, op, 4, fmt])
                    _send(rig, report)
                    haps = base.of_kind(_entries_after(rig, start, 1), C.RING_HAPTIC)
                    count += 1
                    if c.eq("0x86 op %d format %d: one HAPTIC" % (op, fmt), len(haps), 1):
                        h = haps[0]
                        c.eq("0x86 op %d format %d" % (op, fmt),
                             (h["haptic_kind"], h["operation"], h["format"], h["encoding"],
                              h["rate"], h["channels"]),
                             (5, op, fmt, fmt // 4 if fmt // 4 <= 2 else 3, 8000 >> (fmt % 4),
                              M.target_channels(0x86, 4)))
            for target in (0, 1, 2, 3, 4, 5, 0x40, 0x80, 0x41):
                start = rig.ring_next()
                report = bytes([0x86, 2, target, 1])
                _send(rig, report)
                haps = base.of_kind(_entries_after(rig, start, 1), C.RING_HAPTIC)
                if c.eq("0x86 target 0x%02x: one HAPTIC" % target, len(haps), 1):
                    c.eq("0x86 target 0x%02x channels" % target, haps[0]["channels"],
                         M.target_channels(0x86, target))
            samples = bytes([1, 2, 3, 4])
            for rid in (0x87, 0x89):
                for target in (0, 1, 2, 3, 4, 5, 0x40, 0x80):
                    start = rig.ring_next()
                    report = bytes([rid, target]) + samples if rid == 0x87 else \
                        bytes([rid, len(samples), target]) + samples
                    _send(rig, report)
                    count += 1
                    haps = base.of_kind(_entries_after(rig, start, 1), C.RING_HAPTIC)
                    what = "0x%02x target 0x%02x" % (rid, target)
                    if not c.eq("%s: one HAPTIC" % what, len(haps), 1):
                        continue
                    h = haps[0]
                    if rid == 0x87:
                        padded = rig.own_hid
                        data = samples + (bytes(64 - 2 - len(samples)) if padded else b"")
                    else:
                        data = samples
                    c.eq("%s: PCM data" % what, (h["haptic_kind"], h["len"], h["crc32"],
                                                 h["channels"]),
                         (6, len(data), T.crc32(data), M.target_channels(rid, target)))
            # HOST_OUT carries at most 54 bytes: on R the right grip's samples are cut to n
            for n in ((4, 31) if rig.own_hid else (4, 21)):
                start = rig.ring_next()
                left = bytes(range(1, 32))
                right = bytes(range(101, 101 + n))
                report = bytes([0x88, n]) + left + right
                _send(rig, report)
                count += 1
                haps = base.of_kind(_entries_after(rig, start, 1), C.RING_HAPTIC)
                if c.eq("0x88 n %d: two HAPTIC" % n, len(haps), 2):
                    c.eq("0x88 n %d: left grip then right grip" % n,
                         [(h["channels"], h["len"], h["crc32"]) for h in haps],
                         [(M.CH_LGRIP, n, T.crc32(left[:n])), (M.CH_RGRIP, n, T.crc32(right[:n]))])
            c.note("%d PCM reports" % count)

    if ctx.want("outputs.ignored"):
        with ctx.check(GROUP, "outputs.ignored") as c:
            base.need(rig, "outputs")
            start = rig.ring_next()
            n = rig.host_out(bytes([0x8A, 0x00]))
            _f, _n, entries = rig.ring_all(start)
            entries = base.without_vendor(entries)
            c.eq("HOST_OUT 8A 00: decode result", n, 0)
            c.eq("only an OUTPUT entry", base.kinds(entries), [C.RING_OUTPUT])
            if entries:
                c.eq("OUTPUT", (entries[0]["report_id"], entries[0]["decode_result"]), (0x8A, 0))
            if rig.own_hid:
                try:
                    rig.output(bytes([0x8A, 0x00]))   # through the Rig: checked, under the lock
                    c.note("Windows sent an undeclared 0x8A (informational)")
                except OSError:
                    c.note("Windows refused the undeclared 0x8A (informational)")

    if ctx.want("outputs.short"):
        with ctx.check(GROUP, "outputs.short") as c:
            base.need(rig, "outputs")
            short0 = rig.cnt_read().get("out_short", 0)
            cases = [bytes([rid]) + bytes(m - 1) for rid, m in sorted(MIN_LEN.items())]
            cases.append(bytes([0x88, 20]) + bytes(32 + 20 - 2))       # 32 + n bytes, one short
            cases.append(bytes([0x89, 20, 0]) + bytes(19))              # 2 + n bytes, one short
            for report in cases:
                start = rig.ring_next()
                n = rig.host_out(report)
                _f, _n, entries = rig.ring_all(start)
                entries = base.without_vendor(entries)
                what = "0x%02x with %d bytes" % (report[0], len(report) - 1)
                c.eq("%s: decoded -1" % what, n, -1)
                c.eq("%s: only an OUTPUT entry" % what, base.kinds(entries), [C.RING_OUTPUT])
            c.eq("out_short grew", rig.cnt_read().get("out_short", 0) - short0, len(cases))

    if ctx.want("outputs.exact_length"):
        with ctx.check(GROUP, "outputs.exact_length") as c:
            base.need(rig, "outputs")
            body = bytes([0x81, 0]) + HAPTIC_VECTORS[0x81][0]
            got = []
            for report in (body, body + bytes(54 - len(body))):
                start = rig.ring_next()
                rig.host_out(report)
                got.append(base.of_kind(rig.ring_all(start)[2], C.RING_HAPTIC))
            c.true("the same HAPTIC at 8 bytes and padded to 54",
                   len(got[0]) == len(got[1]) == 1 and
                   {k: v for k, v in got[0][0].items() if k not in ("time_us", "raw", "seq")} ==
                   {k: v for k, v in got[1][0].items() if k not in ("time_us", "raw", "seq")})

    if ctx.want("outputs.stop_status"):
        with ctx.check(GROUP, "outputs.stop_status") as c:
            base.need(rig, "outputs")
            # the reports themselves: on W the board's collection, on B its Bluetooth HID device, on
            # R the board's proven slot
            stream = rig.models("app_send") and (rig.own_hid or rig.slot_proven_at is not None
                                                 or not rig.models("puck"))
            if not stream:
                c.note("the ring only (the board's slot on the puck is not proven)")
            replies = T.cap_flag(rig.caps, C.CAP_HAPTIC_STOP_STATUS) if rig.caps else True
            if not replies:
                c.note("a build without CONFIG_OPENSC2_HAPTIC_STOP_STATUS: no replies expected")
            window = 0.15 if rig.path == "W" else 0.5
            for what, report, want in STOP_CASES:
                want = want if replies else []
                if stream:
                    rig.flush()
                start = rig.ring_next()
                _send(rig, report)
                _f, _n, entries = rig.ring_wait(
                    start, lambda es, k=len(want): len(base.of_kind(es, C.RING_OUTPUT)) >= 1 and
                    len(base.of_kind(es, C.RING_STOP_STATUS)) >= k)
                stops = base.of_kind(entries, C.RING_STOP_STATUS)
                c.eq("%s: STOP_STATUS entries" % what, [e["channel"] for e in stops], want)
                if stops:
                    c.eq("%s: flag 0x02, on the output's link" % what,
                         {(e["flags"], e["link"]) for e in stops},
                         {(C.STATUS_STOPPED, _link(rig))})
                if stream:
                    reps = rig.read_stream(duration_s=window)
                    got = [(r[1], struct.unpack_from("<I", r, 2)[0]) for _t, r in reps
                           if r[0] == C.RPT_HAPTIC_STATUS and len(r) >= 6]
                    c.eq("%s: the 0x44 reports, in order" % what, got,
                         [(ch, C.STATUS_STOPPED) for ch in want])
            if replies:
                c.note("%d reports: every stop-all (0x81 on time 0, 0x82 command 0, a zero speed "
                       "in 0x80) and 0x86's stream stop answered with 0x44 {channel, 0x02}" %
                       len(STOP_CASES))
