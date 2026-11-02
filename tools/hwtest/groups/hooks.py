# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# hooks: the test hooks themselves (docs/testing.md, group "hooks"): present, invisible while
# unarmed (W, B), arming and its first ring entries, the guard canary, the settings restore at
# the end of arming, the lease, the DISARM answer (W, B), the ring wrapping and the paging of the
# counters and settings. present() runs first in every phase (the runner calls it); run() does the
# rest.
import struct

from . import base
from .. import config as C
from ..proto import model as M
from ..proto import testproto as T

GROUP = "hooks"
TESTS = [("hooks.present", "WRB", []), ("hooks.invisible", "WB", []),
         ("hooks.arm", "WRB", []), ("hooks.guard", "WRB", []), ("hooks.restore", "WRB", []),
         ("hooks.lease", "WRB", []), ("hooks.disarm_answer", "WB", []), ("hooks.ring", "WRB", []),
         ("hooks.paging", "WRB", [])]

# the link a request through the test channel comes on, as the ring names it: feature report 1
# on USB (W) or Bluetooth (B), the console (R)
REQUEST_LINK = {"W": 1, "B": 4, "R": 2}


def present(ctx):
    """hooks.present: W and B a keyed ARM, R `opensc2 test 1 sync` (the handshake). Returns
    whether the firmware has the hooks; without them every group that needs them is skipped."""
    rig = ctx.rig
    have = False
    with ctx.check(GROUP, "hooks.present") as c:
        if rig.wireless and not rig.board_info.get("hooks"):
            c.skip("firmware built without CONFIG_OPENSC2_TEST_HOOKS (no HWT answer)")
        try:
            caps = rig.arm()
        except base.Stall:
            c.skip("firmware built without CONFIG_OPENSC2_TEST_HOOKS (a keyed ARM STALLs)")
        have = True
        c.eq("caps version", caps["version"], 1)
        c.eq("caps mode", caps["mode"], 1 if rig.path == "W" else 0)
        c.eq("caps counters", caps["counter_count"], C.COUNTER_COUNT)
        c.true("caps ring size 512..16384", 512 <= caps["ring_size"] <= 16384)
        c.true("caps sequence depth 2..32", 2 <= caps["sequence_depth"] <= 32)
        c.eq("caps lease", caps["lease_s"], C.LEASE_DEFAULT_S)
        c.true("caps policy 0 or 1", caps["policy"] in (0, 1))
        if rig.path == "B":
            c.true("caps: a Bluetooth build (bit 17)", T.cap_flag(caps, C.CAP_BLE))
        if T.cap_flag(caps, C.CAP_NO_HOST_WIRELESS):
            c.warn("the charger test build (CONFIG_OPENSC2_NO_HOST_WIRELESS_MS): wired with no "
                   "USB host it goes wireless by itself, which the real controller does not; "
                   "flash the default build for normal use")
        c.note("caps: git %s, build flags 0x%04x, ring %d B, %d steps, policy %s" % (
            caps["git_revision"], caps["build_flags"], caps["ring_size"],
            caps["sequence_depth"], "wireless" if caps["policy"] else "auto"))
    rig.hooks = have
    return have


def run(ctx):
    rig = ctx.rig

    if ctx.want("hooks.invisible"):
        with ctx.check(GROUP, "hooks.invisible") as c:
            rig.disarm()
            rig.sleep(0.06)                   # the guard's 50 ms after the end of arming
            rig.vendor_set(C.VC_ATTRIBUTES)   # a known stored request
            probes = [("a wrong key", bytes([0x1E, 0xAB, 0x57, 0x7F, 1, 9, C.OP_STATUS])),
                      ("version 2", bytes([0x1E, 0xAB, 0x57, 0x7E, 2, 9, C.OP_STATUS])),
                      ("n = 6", bytes([0x1E, 0xAB, 0x57, 0x7E, 1, 9])),
                      ("a keyed STATUS", T.build_request(C.OP_STATUS, b"", 9))]
            for name, payload in probes:
                c.true("0x7E with %s STALLs while unarmed" % name, rig.probe_test_set(payload))
                a = rig.vendor_get()
                c.eq("after %s the GET answers" % name, (a.opcode, a.n), (C.VC_ATTRIBUTES, 30))
            rig.arm()

    if ctx.want("hooks.arm"):
        with ctx.check(GROUP, "hooks.arm") as c:
            caps = rig.arm()
            st = rig.status()
            c.true("STATUS armed and guard", st and st["armed"] and st["guard"])
            _first, _next, entries = rig.ring_all(0)
            c.true("two entries after ARM", len(entries) >= 2)
            if len(entries) >= 2:
                e0, e1 = entries[0], entries[1]
                c.eq("entry 0", (e0["seq"], e0["kind"]), (0, C.RING_ARMED))
                c.eq("entry 0 nonce", e0.get("nonce"), rig.nonce)
                c.eq("entry 0 link", e0["link"], REQUEST_LINK.get(rig.path, 2))
                c.eq("entry 1", (e1["seq"], e1["kind"], e1.get("what")),
                     (1, C.RING_LIZARD, C.LIZARD_RESYNC))
                c.true("entry 0 time close to 0", e0["time_us"] < 100000)
            cnt = rig.cnt_read()
            growing = {"frames", "usb_sent", "usb_dropped", "usb_skipped", "radio_sent",
                       "radio_dropped", "radio_packets", "radio_keepalives", "battery_sent",
                       "battery_dropped", "test_requests", "radio_extra_dropped",
                       "radio_rx_overflow"}
            for name in C.COUNTER_NAMES:
                if name not in growing and name != "ring_entries":
                    c.eq("counter %s restarted" % name, cnt.get(name), 0)
            c.eq("ring_entries", cnt.get("ring_entries"), 2)
            c.eq("caps git = the identity's", caps["git_revision"], rig.identity_git())

    if ctx.want("hooks.guard"):
        with ctx.check(GROUP, "hooks.guard") as c:
            if rig.guard_failed:
                c.skip("the canary already failed in this run: it is not sent again")
            ok, info = rig.guard_canary()
            c.observe(**info)
            c.true("the canary's two reports came back diverted (lizard_diverted 2, two "
                   "LIZARD_RPT entries)", ok)
            ctx.results.safety["canary"] = ok
            if not ok:
                ctx.precondition = C.EXIT_PRECONDITION
                rig.disarm()

    if ctx.want("hooks.restore"):
        with ctx.check(GROUP, "hooks.restore") as c:
            rig.disarm()
            rig.sleep(0.06)
            rig.arm(restore=False)
            before = rig.settings_read()
            led_before = rig.led_color()
            rig.disarm()
            rig.sleep(0.06)
            rig.arm(restore=True)
            rig.write_settings([(68, 40), (45, 10)])
            rig.vendor_set(C.VC_LED_SET, bytes([10, 20, 30, 40]))
            st = rig.status()
            c.true("STATUS shows a snapshot held", st and st["snapshot"])
            rig.disarm()
            rig.sleep(0.06)
            rig.arm(restore=False)
            after = rig.settings_read()
            c.eq("setting 68 restored", after.get(68), before.get(68))
            c.eq("setting 45 restored", after.get(45), before.get(45))
            c.eq("LED colour restored", rig.led_color(), led_before)
            rig.disarm()
            rig.sleep(0.06)
            rig.arm()

    if ctx.want("hooks.lease"):
        with ctx.check(GROUP, "hooks.lease") as c:
            with rig.quiet():
                rig.arm(lease=2)
                rig.sleep(3.0)
                if rig.own_hid:
                    c.true("a GET right after the lease ran out STALLs (the held answer is "
                           "dropped)", rig.refused_get())
                    c.true("a keyed STATUS STALLs after the lease",
                           rig.test(C.OP_STATUS, stall_ok=True) is None)
                else:
                    a = rig.test(C.OP_MARK, struct.pack("<I", 0x1EA5E), stall_ok=True)
                    c.eq("MARK after the lease", a["status"], C.ST_NOT_ARMED)
                    st = rig.status()
                    c.true("STATUS armed = 0 after the lease", st is not None and not st["armed"])
                rig.ctx.armed = False
            caps = rig.arm()
            c.eq("ARM again", caps["lease_s"], C.LEASE_DEFAULT_S)

    if ctx.want("hooks.disarm_answer"):
        with ctx.check(GROUP, "hooks.disarm_answer") as c:
            rig.arm()
            a = rig.disarm()
            c.true("DISARM answered", a is not None and a["status"] == C.ST_OK)
            rid = a["request_id"] if a else 0
            for i in (1, 2):
                g = rig.vendor_get()
                c.eq("GET %d after DISARM" % i, (g.opcode, g.n, g.data[:4]),
                     (C.TEST_OPCODE, 4, bytes([C.TEST_VERSION, rid, C.OP_DISARM, C.ST_OK])))
            c.true("a keyed STATUS after it STALLs", rig.test(C.OP_STATUS, stall_ok=True) is None)
            rig.sleep(0.06)
            rig.arm()

    if ctx.want("hooks.ring"):
        with ctx.check(GROUP, "hooks.ring") as c:
            rig.arm()
            size = rig.caps["ring_size"]
            marks = size // 11 + 120         # MARK entries are 11 bytes: enough to wrap
            for i in range(marks):
                rig.mark(i)
            first, nxt, entries = rig.ring_all(0)
            cnt = rig.cnt_read()
            c.true("the ring wrapped (first > 0)", first > 0)
            s0 = entries[0]["seq"] if entries else None
            if rig.path == "W":
                c.eq("ring_evicted = first", cnt.get("ring_evicted"), first)
                c.eq("next", nxt, marks + 2)
                c.eq("entries read = next - first", len(entries), nxt - first)
                c.eq("sequence of the first entry read", s0, first)
            else:
                # Wireless the board also logs on its own while the MARKs go in and while the
                # ring is read page by page (a link or channel change, the Steam watchdog's
                # lizard and setting entries; over Bluetooth the link's own events): the ring can
                # move on between two reads, so `first` may pass the first entry read. The extra
                # entries that were read must be of these kinds.
                own = (C.RING_LINK, C.RING_CHANNELS, C.RING_VERSION, C.RING_IDENTITY,
                       C.RING_LIZARD, C.RING_LIZARD_RPT, C.RING_SETTING, C.RING_HOST_ASLEEP,
                       C.RING_BLE)
                extra = [e for e in entries if e["kind"] not in (C.RING_MARK, C.RING_ARMED)]
                c.true("next = MARKs + 2, or up to 8 more of the board's own entries (next %d, "
                       "MARKs + 2 = %d)" % (nxt, marks + 2), marks + 2 <= nxt <= marks + 10)
                c.true("every extra entry is one the board logs on its own",
                       all(e["kind"] in own for e in extra))
                c.true("ring_evicted >= first (it may move on after the read)",
                       (cnt.get("ring_evicted") or 0) >= first)
                c.true("entries read = next - the first one's sequence",
                       s0 is not None and len(entries) == nxt - s0)
                c.true("the first entry read is no newer than first", s0 is not None and s0 <= first)
                if nxt > marks + 2:
                    c.note("%d entries of the board's own on its wireless link"
                           % (nxt - marks - 2))
            got = [e.get("marker") for e in entries if e["kind"] == C.RING_MARK]
            c.true("consecutive MARKs in order", got == list(range(marks - len(got), marks)))
            a = rig.test(C.OP_RING_READ, struct.pack("<IB", nxt + 5, 0))
            c.eq("RING_READ past next", a["status"], C.ST_BAD_ARG)
            a = rig.test(C.OP_RING_READ, struct.pack("<IB", 0, 1))
            f0, n0, e0 = T.parse_ring_read(a["result"])
            if rig.path == "W":
                c.eq("RING_READ from an evicted sequence starts at first, max 1",
                     (f0, len(e0)), (first, 1))
            else:
                c.true("RING_READ from an evicted sequence starts at first or later, max 1",
                       f0 >= first and len(e0) == 1)
            c.note("%d MARKs: first %d, next %d" % (marks, first, nxt))
            rig.arm()

    if ctx.want("hooks.paging"):
        with ctx.check(GROUP, "hooks.paging") as c:
            for args, want in (((0, 13), (C.ST_OK, 13)), ((13, 13), (C.ST_OK, 13)),
                               ((26, 13), (C.ST_OK, 1)), ((27, 1), (C.ST_OK, 0)),
                               ((0, 14), (C.ST_BAD_ARG, None)), ((0, 0), (C.ST_BAD_ARG, None))):
                a = rig.test(C.OP_CNT_READ, bytes(args))
                n = a["result"][1] if a["status"] == C.ST_OK else None
                c.eq("CNT_READ %r" % (args,), (a["status"], n), want)
            for args, want in (((0, 27), (C.ST_OK, 27)), ((81, 27), (C.ST_OK, 5)),
                               ((86, 1), (C.ST_OK, 0)), ((0, 28), (C.ST_BAD_ARG, None))):
                a = rig.test(C.OP_SETTINGS_READ, bytes(args))
                n = a["result"][1] if a["status"] == C.ST_OK else None
                c.eq("SETTINGS_READ %r" % (args,), (a["status"], n), want)
            settings = rig.settings_read()
            c.eq("SETTINGS_READ pages", sorted(settings), list(range(86)))
            for num in (2, 9, 11, 27, 45, 50, 68, 71, 76, 85):
                ans = rig.vendor(C.VC_SETTINGS_GET, bytes([num]))
                # 48, 84 and 85 answer their defaults whatever is kept (M.setting_answer)
                c.eq("0x89 %d" % num, (ans.n, ans.data[:1], struct.unpack_from(
                    "<h", ans.data + bytes(3), 1)[0]),
                     (3, bytes([num]), M.setting_answer(num, settings[num])))
