# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# battery: input report 0x43 and vendor command 0xBE (docs/testing.md, group "battery"), on every
# path (over Bluetooth a 0x43 also sets the battery service's level). The blank canvas never sets
# a battery, so the library's default 0x43 is checked byte for byte, then the BATTERY op injects
# values (with send-now) and both the next 0x43 and the 0xBE answer must carry them; the 3.5 s
# cadence is measured over 30 s.
from . import base
from .. import config as C
from ..proto import reports as R

GROUP = "battery"
TESTS = [("battery.default", "WRB", []), ("battery.cadence", "WRB", []),
         ("battery.values", "WRB", []), ("battery.be", "WRB", []), ("battery.off", "WRB", [])]

BASE = {"charge_state": 2, "level": 55, "battery_mv": 3900, "system_mv": 3300, "input_mv": 5000,
        "current_ma": -250, "input_current_ma": 500, "temperature": 0x1234, "gauge_flags": 0xA5}


def steps():
    """The battery.values steps: every field over its range, one at a time."""
    out = []
    for cs in (0, 1, 2, 3, 4, 0x81):
        out.append(("charge state 0x%02x" % cs, dict(BASE, charge_state=cs)))
    for lv in (0, 50, 100, 101, 255):
        out.append(("level %d" % lv, dict(BASE, level=lv)))
    for field in ("battery_mv", "system_mv", "input_mv", "input_current_ma"):
        for v in (0, 65535):
            out.append(("%s %d" % (field, v), dict(BASE, **{field: v})))
    for v in (-32768, 32767):
        out.append(("current %d" % v, dict(BASE, current_ma=v)))
    out.append(("every field different", dict(BASE)))
    return out


def _is_43(rep):
    return rep and rep[0] == C.RPT_BATTERY


def _next_43(rig, timeout):
    got = rig.wait_report(_is_43, timeout)
    return got


def run(ctx):
    rig = ctx.rig
    if not rig.ctx.armed:
        rig.arm()
    interval = (rig.caps or {}).get("battery_interval_ms", C.BATTERY_INTERVAL_MS)
    tol = 150 if rig.path == "W" else 250
    deadline = 0.2 if rig.path == "W" else 0.5

    if ctx.want("battery.default"):
        with ctx.check(GROUP, "battery.default") as c:
            base.need(rig, "battery")
            rig.battery_off()
            rig.flush()
            got = _next_43(rig, interval / 1000.0 + 1.0)
            c.true("a 0x43 arrived", got is not None)
            if got:
                c.eq("the default 0x43", list(got[1][:15]), list(R.BATTERY_DEFAULT))

    if ctx.want("battery.cadence"):
        with ctx.check(GROUP, "battery.cadence") as c:
            base.need(rig, "battery_cadence")
            dropped0 = rig.cnt_read().get("battery_dropped", 0)
            rig.flush()
            reps = rig.read_stream(duration_s=30.0)
            dropped = rig.cnt_read().get("battery_dropped", 0) - dropped0
            times = [ts for ts, r in reps if _is_43(r)]
            c.true("at least 7 battery reports in 30 s (%d)" % len(times), len(times) >= 7)
            gaps = [(b - a) / 1e6 for a, b in zip(times, times[1:])]
            for g in gaps:
                k = max(1, int(round(g / interval)))
                c.true("an interval of %.0f ms: %d x %d +- %d ms" % (g, k, interval, tol),
                       abs(g - k * interval) <= tol * k and (k == 1 or dropped >= k - 1))
                c.true("never two within 1 s (%.0f ms)" % g, g >= 1000)
            if gaps:
                c.observe(mean_ms=round(sum(gaps) / len(gaps), 1), dropped=dropped)

    want_values = ctx.want("battery.values")
    want_be = ctx.want("battery.be")
    results = []
    if want_values or want_be:
        with ctx.check(GROUP, "battery.values") as c:
            base.need(rig, "battery")
            last = None
            for name, b in steps():
                rig.flush()
                a = rig.battery(send_now=True, **b)
                c.eq("%s: BATTERY" % name, a["status"], C.ST_OK)
                want = R.encode_battery(b)
                got = rig.wait_report(lambda r, w=want: _is_43(r) and r[:15] == w, deadline)
                c.true("%s: a 0x43 with these values within %d ms" % (name, deadline * 1000),
                       got is not None)
                if got:
                    last = got[0]
                    c.true("%s: no gauge flags in 0x43" % name, len(got[1]) < 16 or
                           got[1][15:].count(0) == len(got[1][15:]))
                be = rig.vendor(C.VC_BATTERY_DATA)
                results.append((name, b, be))
            c.true("the level is clamped to 100",
                   R.encode_battery(dict(BASE, level=101))[2] == 100)
            if last is not None:
                nxt = rig.wait_report(_is_43, interval / 1000.0 + 0.5)
                if nxt is None:
                    c.fail("no periodic 0x43 after the last send-now")
                else:
                    gap = (nxt[0] - last) / 1e6
                    c.true("the send-now restarted the period: the next 0x43 %.0f ms later "
                           "(%d +- %d)" % (gap, interval, tol), abs(gap - interval) <= tol)
            if not want_values:
                c.skip("run for battery.be")
        if want_be:
            with ctx.check(GROUP, "battery.be") as c:
                base.need(rig, "battery")
                for name, b, be in results:
                    c.eq("%s: 0xBE n" % name, be.n, 15)
                    c.eq("%s: 0xBE" % name, list(be.data[:15]), list(R.encode_be(b)))
                    if be.n >= 15:
                        c.eq("%s: gauge flags at offset 13" % name, be.data[13], b["gauge_flags"])
                c.true("steps run", bool(results))

    if ctx.want("battery.off"):
        with ctx.check(GROUP, "battery.off") as c:
            base.need(rig, "battery")
            rig.battery_off()
            rig.flush()
            got = _next_43(rig, interval / 1000.0 + 1.0)
            c.true("a 0x43 arrived", got is not None)
            if got:
                c.eq("the default values again", list(got[1][:15]), list(R.BATTERY_DEFAULT))
            be = rig.vendor(C.VC_BATTERY_DATA)
            c.eq("0xBE the default values again", list(be.data[:15]),
                 list(R.encode_be(R.BATTERY_DEFAULT_FIELDS)))
