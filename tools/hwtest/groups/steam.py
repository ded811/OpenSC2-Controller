# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# steam: Steam's view (docs/testing.md, group "steam"), opt-in (--with-steam), passive: nothing is
# injected and nothing goes to the board on a channel Steam shares. With the board wireless the
# runner arms through the console (without the restore flag: Steam's own settings stay) and reads
# the ring for what Steam sends through the puck; it reads the lines Steam adds to its controller
# log (masked, only the lines with known phrases). With the board wired it only reads the stream.
from . import base
from . import stream as stream_group
from .. import config as C
from ..proto import console as cproto
from ..proto import steamlog

GROUP = "steam"
TESTS = [("steam.session", "S", ["with-steam"]), ("steam.log_reconnect", "S", ["with-steam"]),
         ("steam.wired_passive", "S", ["with-steam"])]


def _skip_all(ctx, why):
    for tid, _p, _n in TESTS:
        if ctx.want(tid):
            ctx.record(GROUP, tid, base.SKIP, why)


def wireless(ctx):
    """steam.session and steam.log_reconnect: the board wireless and connected, Steam open."""
    rig = ctx.rig
    if not ctx.opt.get("with_steam"):
        return
    if ctx.want("steam.session"):
        with ctx.check(GROUP, "steam.session") as c:
            base.need(rig, "steam")
            if not rig.steam_running():
                c.skip("Steam is not running")
            rig.arm(restore=False)
            s9_start = rig.settings_read()[9]
            start = 0
            vendor = []
            t_end = rig.clock.monotonic() + 60.0
            while rig.clock.monotonic() < t_end:
                rig.sleep(2.0)
                first, nxt, entries = rig.ring_all(start)
                if first > start:
                    c.warn("%d ring entries lost to Steam's traffic" % (first - start))
                vendor += [e for e in entries if e["kind"] == C.RING_VENDOR and e["link"] == 0]
                start = nxt
            writes = [e for e in vendor if e["opcode"] == C.VC_SETTINGS_SET]
            pairs = []
            for e in writes:
                p = e["payload"]
                for i in range(0, len(p) - 2, 3):
                    pairs.append((p[i], int.from_bytes(p[i + 1:i + 3], "little", signed=True)))

            def lizard_off(e):
                """0x81, or a 0x87 that writes setting 9 = 0 (Steam uses both)."""
                if e["opcode"] == C.VC_LIZARD_OFF:
                    return True
                p = e["payload"] if e["opcode"] == C.VC_SETTINGS_SET else b""
                return any(p[i] == 9 and int.from_bytes(p[i + 1:i + 3], "little", signed=True) <= 0
                           for i in range(0, len(p) - 2, 3))

            offs = [e for e in vendor if lizard_off(e)]
            # Seen on hardware: through the puck Steam turns lizard mode off once (with 0x87 9 = 0
            # at the connection) and, while idle, repeats nothing for at least 60 s.
            c.true("Steam turned lizard mode off (0x81 or setting 9 = 0: %d in 60 s; setting 9 was "
                   "%d at the start)" % (len(offs), s9_start), s9_start == 0 or len(offs) >= 1)
            gaps = [round((b["time_us"] - a["time_us"]) / 1e6, 2) for a, b in zip(offs, offs[1:])]
            s9_end = rig.settings_read()[9]
            if s9_end != 0:
                c.warn("Steam did not repeat it within 10 s over the radio (%d in 60 s), so the "
                       "board's watchdog, which follows the real firmware's 10 s rule, turned lizard "
                       "mode back on (setting 9 = %d at the end). Open: whether the real controller "
                       "sees the same, or the puck holds back its lizard reports while Steam has "
                       "the slot" % (len(offs), s9_end))
            c.observe(lizard_off_intervals_s=gaps[:20], settings_written=sorted(set(pairs))[:40],
                      opcodes=sorted(set("0x%02x" % e["opcode"] for e in vendor)),
                      setting9_start=s9_start, setting9_end=s9_end)
            rig.disarm()
    if ctx.want("steam.log_reconnect"):
        with ctx.check(GROUP, "steam.log_reconnect") as c:
            base.need(rig, "steam")
            if not rig.steam_running():
                c.skip("Steam is not running")
            mark = rig.backend.steam_log_mark()
            if mark is None:
                c.skip("Steam's controller log was not found")
            with rig.own_link_drop():       # the puck's empty reports at the drop: no leak
                rig.console_run("opensc2 stop")
                rig.sleep(3.0)
                rig.console_run("opensc2 start")
                rig.wait_link("connected", 5.0)
            rig.sleep(3.0)
            # only lines with a known phrase (bond lines without their bond), and of those
            # only the ones about the board: a line naming another device's serial is dropped
            kept = steamlog.board_lines(rig.backend.steam_log_since(mark),
                                        {rig.unit_serial} | set(rig.puck_serials))
            lines = [rig.mask.text(steamlog.redact(ln), transcript=True) for ln in kept]
            ctx.results.steam_log = lines
            c.true("the connection went away and came back (wireless_established)",
                   any("wireless_established" in ln for ln in lines))
            info = [steamlog.parse_controller_info(ln) for ln in lines]
            info = [i for i in info if i]
            ident = getattr(rig, "identity_r", None) or {}
            c.true("Controller Info: HWID and FWTimestamp = opensc2 id",
                   any(i == (ident.get("rev"), ident.get("build")) for i in info))


def wired(ctx):
    """steam.wired_passive: the board wired, Steam open: the stream read only."""
    rig = ctx.rig
    if not ctx.opt.get("with_steam"):
        return
    if ctx.want("steam.wired_passive"):
        with ctx.check(GROUP, "steam.wired_passive") as c:
            base.need(rig, "steam")
            if not rig.steam_running():
                c.skip("Steam is not running")
            reps = rig.read_stream(duration_s=10.0)
            states = [r for _t, r in reps if r[0] == C.RPT_STATE]
            c.true("the stream flows with Steam open (%d reports)" % len(states),
                   len(states) > 1000)
        stream_group.passive(ctx)


def run(ctx):
    if not ctx.opt.get("with_steam"):
        _skip_all(ctx, "opt-in (--with-steam)")
