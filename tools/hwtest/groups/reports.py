# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# reports: the other input reports an application can send (docs/testing.md, group "reports"):
# the haptic stream status 0x44 and a raw report, sent with APP_SEND as the application would, must
# arrive on the board's Col03 (W), its Bluetooth HID device (B) or its slot (R) as sent. A raw
# 0x45 counts as an injection (the same preconditions); over Bluetooth it shares its
# characteristic with the state reports, so it is told from them by its bytes. On R the puck
# relays these reports only at their exact length.
import struct

from . import base
from .. import config as C

GROUP = "reports"
TESTS = [("reports.app_send", "WRB", []), ("reports.app_send_raw", "WRB", []),
         ("reports.puck_filter", "R", [])]


def _arrives(rig, want, timeout):
    # raw: over Bluetooth the reports as they came (a 0x45 is not read as a state report here).
    # Only the report's own bytes count: Windows returns every read at the collection's longest
    # input report, and past a shorter report's end it leaves what its buffer held before (on a
    # board that sends the identity quaternion, the last 0x42's W).
    return rig.wait_report(lambda r: r[:len(want)] == want, timeout, raw=True)


def run(ctx):
    rig = ctx.rig
    if not rig.ctx.armed:
        rig.arm()
    rig.lizard_off()

    if ctx.want("reports.app_send"):
        with ctx.check(GROUP, "reports.app_send") as c:
            base.need(rig, "app_send")
            before = rig.cnt_read().get("app_reports_sent", 0)
            sent = 0
            for channel, flags in [(3, 0x02)] + [(ch, f) for ch in (0, 1, 3, 4)
                                                 for f in (0x7F, 0xFFFFFFFF)]:
                rig.flush()
                rc = rig.app_send_status(channel, flags)
                c.eq("APP_SEND 0x44 channel %d flags 0x%x" % (channel, flags), rc, 0)
                sent += 1
                want = bytes([C.RPT_HAPTIC_STATUS, channel]) + struct.pack("<I", flags)
                c.true("44 %02x %08x arrived within 100 ms" % (channel, flags),
                       _arrives(rig, want, 0.1) is not None)
            after = rig.cnt_read().get("app_reports_sent", 0)
            c.eq("app_reports_sent grew", after - before, sent)

    if ctx.want("reports.app_send_raw"):
        with ctx.check(GROUP, "reports.app_send_raw") as c:
            base.need(rig, "app_send")
            report = bytes([C.RPT_BT_STATE]) + bytes(range(45))
            rig.ready_to_inject()          # a 0x45 counts as an injection: the canary first
            before = rig.cnt_read().get("app_reports_sent", 0)
            rig.flush()
            rc = rig.app_send_report(report)
            c.eq("APP_SEND kind 0, 0x45 and 45 bytes", rc, 0)
            c.true("the same 46 bytes arrived within 100 ms", _arrives(rig, report, 0.1)
                   is not None)
            c.eq("app_reports_sent + 1", rig.cnt_read().get("app_reports_sent", 0) - before, 1)
            a = rig.test(C.OP_APP_SEND, bytes([0, C.RPT_STATE]) + bytes(52))
            c.eq("APP_SEND of a 0x42 (state reports come only from the frame path)",
                 a["status"], C.ST_DENIED)

    if ctx.want("reports.puck_filter"):
        with ctx.check(GROUP, "reports.puck_filter") as c:
            base.need(rig, "puck")
            report = bytes([C.RPT_HAPTIC_STATUS, 3, 2, 0, 0, 0, 0])   # one byte too long
            rig.flush()
            rc = rig.app_send_report(report)
            c.eq("APP_SEND of a 7-byte 0x44 is queued", rc, 0)
            got = rig.wait_report(lambda r: r[0] == C.RPT_HAPTIC_STATUS, 0.5)
            c.true("nothing arrived on the slot within 500 ms (the puck's exact-length filter)",
                   got is None)
