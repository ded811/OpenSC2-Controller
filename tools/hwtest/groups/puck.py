# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# puck: writes through Valve's puck to the board's slot (docs/testing.md, group "puck"), opt-in
# (--through-puck), Steam closed. Only the slot proven to be the board's within the last 10 s is
# written, only output reports 0x80..0x89 and feature report 1 (the pass-through to the board);
# feature report 2, the puck's own commands, is never built. Every write is logged as
# [through-puck] and counted in the results.
import struct

from . import base
from .. import config as C
from ..proto import console as cproto
from ..proto import testproto as T
from ..proto import vendor as V

GROUP = "puck"
TESTS = [("puck.output", "P", ["through-puck"]), ("puck.feature", "P", ["through-puck"]),
         ("puck.cache", "P", ["through-puck"]), ("puck.refused", "P", ["through-puck"]),
         ("puck.test_channel", "P", ["through-puck"])]


def run(ctx):
    rig = ctx.rig
    if not ctx.opt.get("through_puck"):
        for tid, _p, _n in TESTS:
            if ctx.want(tid):
                ctx.record(GROUP, tid, base.SKIP, "opt-in (--through-puck)")
        return
    if not rig.ctx.armed:
        rig.arm()
    rig.lizard_off()

    if ctx.want("puck.output"):
        with ctx.check(GROUP, "puck.output") as c:
            base.need(rig, "puck")
            start = rig.ring_next()
            rig.puck_output(bytes.fromhex("8000E8031027" "00204E00"))
            rig.puck_output(bytes([0x81, 0x00, 0x10, 0x27, 0x20, 0x4E, 0x03, 0x00]))
            rig.puck_output(bytes([0x80]) + bytes(9))
            _f, _n, entries = rig.ring_wait(start, lambda es: len(base.of_kind(
                es, C.RING_OUTPUT)) >= 3, 1.0)
            outs = base.of_kind(entries, C.RING_OUTPUT)
            c.eq("OUTPUT entries", [e["report_id"] for e in outs], [0x80, 0x81, 0x80])
            c.true("RUMBLE and HAPTIC entries", base.of_kind(entries, C.RING_RUMBLE) and
                   base.of_kind(entries, C.RING_HAPTIC))
            c.eq("every entry on the radio link",
                 sorted(set(e["link"] for e in entries if e["kind"] in (
                     C.RING_OUTPUT, C.RING_RUMBLE, C.RING_HAPTIC))), [0])

    if ctx.want("puck.feature"):
        with ctx.check(GROUP, "puck.feature") as c:
            base.need(rig, "puck")
            a = rig.puck_vendor(C.VC_DEVICE_INFO, b"\x02")
            c.eq("0xF2 02 through the puck: n", a.n, 9)
            value = rig.settings_read()[45]
            a = rig.puck_vendor(C.VC_SETTINGS_GET, bytes([45]))
            c.eq("0x89 2D through the puck", (a.n, a.data[:1],
                                              struct.unpack_from("<h", a.data + bytes(3), 1)[0]),
                 (3, bytes([45]), value))

    if ctx.want("puck.cache"):
        with ctx.check(GROUP, "puck.cache") as c:
            base.need(rig, "puck")
            ident = getattr(rig, "identity_r", None) or {}
            a = rig.puck_vendor(C.VC_ATTRIBUTES)
            d = dict(V.decode_attributes(a.data))
            c.eq("0x83 from the puck = opensc2 id", (d.get(1), d.get(10), d.get(4), d.get(9),
                                                     d.get(11)),
                 (ident.get("pid"), ident.get("blbuild"), ident.get("build"), ident.get("rev"),
                  4000))
            a = rig.puck_vendor(C.VC_STRING_ATTR, b"\x01")
            c.true("0xAE 01 from the puck = the unit serial",
                   a.data[1:20].split(b"\0", 1)[0].decode("ascii", "replace") == ident.get("unit"))
            a = rig.puck_vendor(C.VC_STRING_ATTR, b"\x03")
            c.eq("0xAE 03 from the puck", a.data[1:20].split(b"\0", 1)[0].decode(
                "ascii", "replace"), ident.get("git"))

    if ctx.want("puck.refused"):
        with ctx.check(GROUP, "puck.refused") as c:
            base.need(rig, "puck")
            start = rig.ring_next()
            try:
                rig.puck_vendor(0xC1, bytes.fromhex("FFFFFFFF030905") + bytes([0xFF] * 9))
                c.fail("the GET of 0xC1 through the puck succeeded")
            except base.Refused:
                c.true("the GET never succeeds within 1.5 s", True)
            entries = rig.ring_all(start)[2]
            ven = [e for e in base.of_kind(entries, C.RING_VENDOR) if e["opcode"] == 0xC1]
            c.true("a VENDOR SET entry with rc < 0 on the radio link",
                   ven and ven[0]["direction"] == 0 and ven[0]["result"] < 0 and
                   ven[0]["link"] == 0)

    if ctx.want("puck.test_channel"):
        with ctx.check(GROUP, "puck.test_channel") as c:
            base.need(rig, "puck")
            a = rig.test(C.OP_STATUS, path="P")
            c.eq("STATUS through the puck", a["status"], C.ST_OK)
            st = T.decode_status(a["result"])
            rc, hwt = rig.console_test("status")
            kv = cproto.parse_status_kv(dict(hwt).get("STATUS", ""))
            c.eq("the same flags as the console's STATUS",
                 (int(st["armed"]), int(st["guard"]), int(st["lizard"])),
                 (int(kv.get("armed", -1)), int(kv.get("guard", -1)), int(kv.get("lizard", -1))))
