# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# wireless: the radio link through Valve's puck (docs/testing.md, group "wireless"), R. The puck is
# only read: its four slot collections are opened read-only to find which one carries the board
# (an injected signature), then that slot's reports are read: the board's own and the puck's link
# status 0x79 and link quality 0x7B. The link is taken down and brought back with the console
# (`opensc2 stop` / `start`) and with RADIO_DEAF, which makes the radio stop listening for a while.
import struct

from . import base
from .. import config as C
from ..proto import console as cproto
from ..proto import reports as R
from ..proto import vendor as V

GROUP = "wireless"
TESTS = [("wireless.slot", "R", []), ("wireless.quality", "R", []),
         ("wireless.identity", "R", []), ("wireless.reconnect", "R", []),
         ("wireless.link_loss", "R", []), ("wireless.turn_off", "R", [])]

LINK = {"off": 0, "no-bond": 1, "pairing": 2, "listening": 3, "connected": 4}


def slot(ctx):
    """wireless.slot: which of the puck's slots is the board's. Runs first in an R phase."""
    rig = ctx.rig
    with ctx.check(GROUP, "wireless.slot") as c:
        base.need(rig, "puck")
        found = rig.prove_slot()
        if found is not None and found.get("no_link"):
            # 3.10, a stuck link: it did not connect, one stop and start included; the slot
            # groups are skipped ("not proved") and the runner goes on to the restore
            c.fail("no link: the board's radio link did not connect (one opensc2 stop and start "
                   "included): is it bonded to this puck?")
            return
        if found is None:
            again = rig.find_puck() and rig.prove_slot()
            c.error("the puck re-enumerated (no slot answered); %s" % (
                "reopened and proved again" if again and again.get("count") == 1
                else "no slot carries the board"))
            return
        c.true("exactly one slot carries the injected signature", found["count"] == 1)
        if found.get("unstuck"):
            c.warn("the radio link connected only after one opensc2 stop and start")
        c.note("the board is on the puck's slot MI_0%d" % found["slot"])
        ctx.results.puck_slot = found["slot"]


def _need_link(rig, c):
    if rig.link_stuck:
        c.skip("no link: it did not come back after one stop and start (a stuck link)")


def _link_entries(entries):
    return [(e["state"], e["reason"]) for e in base.of_kind(entries, C.RING_LINK)]


def _status_byte(reps, value):
    return any(r[0] == C.RPT_LINK_STATUS and r[1] == value for _t, r in reps)


def _reason(rig):
    rig.host_set(C.VC_DEVICE_INFO, b"\x02")
    a = rig.host_get()
    return V.decode_device_info2(a.data + bytes(9))["reason"] if a.n >= 9 else None


def run(ctx):
    rig = ctx.rig
    if not rig.ctx.armed:
        rig.arm()
    reconnect_entries = []

    if ctx.want("wireless.quality"):
        with ctx.check(GROUP, "wireless.quality") as c:
            base.need(rig, "puck")
            fresh = rig.prove_slot_if_stale()
            if fresh is not None and fresh.get("no_link"):
                c.fail("no link: the board's radio link did not connect")
            elif fresh is None:
                # the puck re-enumerated: its handles are closed; look for it, reopen
                # the slots and prove the board's again; the group goes on
                back = rig.find_puck() and rig.prove_slot() is not None
                c.error("the puck vanished: %s" % ("slots reopened and the board's proved again"
                                                   if back else "it did not come back in 15 s"))
            else:
                rig.flush()
                reps = rig.read_stream(duration_s=5.0)
                q = [(ts, R.decode_link_quality(r)) for ts, r in reps
                     if r[0] == C.RPT_LINK_QUALITY]
                c.true("0x7B reports in 5 s (%d)" % len(q), len(q) >= 8)
                gaps = [(b[0] - a[0]) / 1e6 for a, b in zip(q, q[1:])]
                bad = [round(g) for g in gaps if abs(g - 500) > 100]
                c.eq("0x7B intervals outside 500 +- 100 ms", bad, [])
                c.eq("0x7B reasons other than 0 (periodic)",
                     sorted(set(x["reason"] for _t, x in q) - {0}), [])
                st = cproto.parse_status(rig.console_run("opensc2 status"))
                channel = st.get("channel")
                if channel is not None:
                    c.true("the 0x7B channel equals opensc2 status' (%s) at least once" % channel,
                           any(x["channel"] == channel for _t, x in q))
                if q:
                    c.observe(rssi_dbm=q[-1][1]["rssi_dbm"], channel=q[-1][1]["channel"])

    if ctx.want("wireless.reconnect") or ctx.want("wireless.identity"):
        with ctx.check(GROUP, "wireless.reconnect") as c:
            base.need(rig, "radio")
            start = rig.ring_next()
            rig.flush()
            with rig.own_link_drop():       # the puck's empty reports at the drop: no leak
                rig.console_run("opensc2 stop")
                c.true("LINK off within 1.5 s", rig.wait_link("off", 1.5))
                gone = rig.read_stream(duration_s=1.5)
                c.true("the slot's 0x42 stopped", sum(1 for _t, r in gone[len(gone) // 2:]
                                                      if r[0] == C.RPT_STATE) == 0 or
                       not rig.models("puck"))
                if rig.models("puck"):
                    c.true("the puck reported 0x79 = 1 (no link)", _status_byte(gone, 1))
                rig.sleep(0.5)
                rig.console_run("opensc2 start")
                c.true("connected again within 2 s", rig.wait_link("connected", 2.0))
            back = rig.read_stream(duration_s=1.0)
            c.true("the stream resumed", sum(1 for _t, r in back if r[0] == C.RPT_STATE) > 50)
            if rig.models("puck"):
                c.true("the puck reported 0x79 = 2 (connected)", _status_byte(back, 2))
            entries = rig.ring_all(start)[2]
            reconnect_entries = entries
            links = _link_entries(entries)
            c.true("LINK off with reason 2", (LINK["off"], 2) in links)
            c.true("LINK listening, then connected", [s for s, _r in links].count(LINK["connected"])
                   >= 1 and LINK["listening"] in [s for s, _r in links])
            c.eq("0xF2 02 reason", _reason(rig), 2)
            rig.lizard_off()
        if ctx.want("wireless.identity"):
            with ctx.check(GROUP, "wireless.identity") as c:
                base.need(rig, "radio")
                _need_link(rig, c)
                ids = base.of_kind(reconnect_entries, C.RING_IDENTITY)
                vers = base.of_kind(reconnect_entries, C.RING_VERSION)
                c.eq("IDENTITY entries for the one connection", len(ids), 1)
                c.true("at least one VERSION entry, every one 1",
                       vers and all(v["version"] == 1 for v in vers))
                ident = getattr(rig, "identity_r", None) or {}
                if ids:
                    i = ids[0]
                    c.eq("IDENTITY = opensc2 id", (i["product_id"], i["build_time"],
                                                   i["bootloader_build_time"],
                                                   i["board_revision"], i["git_revision"]),
                         (ident.get("pid"), ident.get("build"), ident.get("blbuild"),
                          ident.get("rev"), ident.get("git")))

    if ctx.want("wireless.link_loss"):
        with ctx.check(GROUP, "wireless.link_loss") as c:
            base.need(rig, "deaf")
            _need_link(rig, c)
            start = rig.ring_next()
            rig.flush()
            with rig.own_link_drop():       # the puck's empty reports at the drop: no leak
                a = rig.radio_deaf(800)
                c.eq("RADIO_DEAF 800", a["status"], C.ST_OK)
                reps = rig.read_stream(duration_s=1.5)
                c.true("connected again within 3 s", rig.wait_link("connected", 3.0))
            reps += rig.read_stream(duration_s=0.5)
            entries = rig.ring_all(start)[2]
            links = base.of_kind(entries, C.RING_LINK)
            lost = [e for e in links if e["state"] == LINK["listening"] and e["reason"] == 1]
            c.true("LINK listening with reason 1", bool(lost))
            if lost:
                dt = (lost[0]["time_us"] - entries[0]["time_us"]) / 1000.0 if entries else 0
                c.observe(lost_after_ms=round(dt))
            c.true("connected again after it", any(e["state"] == LINK["connected"]
                                                   for e in links[links.index(lost[0]):])
                   if lost else False)
            c.eq("0xF2 02 reason", _reason(rig), 1)
            if rig.models("puck"):
                c.true("the slot showed it: a 0x7B with reason 3 or a 0x79 = 1",
                       _status_byte(reps, 1) or any(r[0] == C.RPT_LINK_QUALITY and r[10] == 3
                                                    for _t, r in reps))
            a = rig.radio_deaf(50)
            c.eq("RADIO_DEAF below 100 ms", a["status"], C.ST_BAD_ARG)
            rig.lizard_off()

    if ctx.want("wireless.turn_off"):
        with ctx.check(GROUP, "wireless.turn_off") as c:
            base.need(rig, "radio")
            _need_link(rig, c)
            for name, opcode, payload in (("0x9F", C.VC_TURN_OFF, b""),
                                          ("0x95 with the turn-off key", C.VC_POWER_REBOOT,
                                           C.KEY_TURN_OFF.to_bytes(4, "little"))):
                start = rig.ring_next()
                with rig.own_link_drop():   # the radio stops: the puck's empty reports
                    c.eq("HOST_SET %s" % name, rig.host_set(opcode, payload), 0)
                    rig.sleep(0.1)
                    entries = rig.ring_all(start)[2]
                    power = base.of_kind(entries, C.RING_POWER)
                    c.true("%s: a POWER entry with request 1 (turn off)" % name,
                           power and power[0]["request"] == 1)
                    c.true("%s: the blank canvas stopped the radio (LINK off, reason 2)" % name,
                           rig.wait_link("off", 1.0) and (LINK["off"], 2) in _link_entries(
                               rig.ring_all(start)[2]))
                    rig.console_run("opensc2 start")
                    c.true("%s: opensc2 start reconnects" % name,
                           rig.wait_link("connected", 3.0))
            rig.lizard_off()
