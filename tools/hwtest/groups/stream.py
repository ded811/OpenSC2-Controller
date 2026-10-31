# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# stream: the state reports' cadence and sequence (docs/testing.md, group "stream"). The reader
# timestamps every report when it arrives; stats.py computes the mean period, the largest gap and
# the sequence holes, which the counters must explain. W: about 248 reports a second, every hole
# explained by usb_dropped. R: looser, the air and the puck may lose a few. B (Bluetooth): one
# state report per connection event, as 0x45, so the rate follows the connection interval
# (BLE_STATUS, attribute 11 of 0x83), and the holes are explained by the reports the board could
# not send (BLE_STATUS's dropped). passive() is the read-only form the S phases use (no feature
# report, no test channel).
from . import base
from .. import config as C
from .. import stats

GROUP = "stream"
TESTS = [("stream.ids", "WRB", []), ("stream.cadence", "WRB", []),
         ("stream.sequence", "WRB", []), ("stream.counters", "WRB", []),
         ("stream.soak", "WRB", ["soak"])]


def _cadence(c, path, states, interval_us=None):
    cad = stats.cadence([ts for ts, _r in states])
    c.observe(count=cad["count"], mean_ms=cad["mean_period_ms"] and round(cad["mean_period_ms"], 4),
              per_s=cad["per_s"] and round(cad["per_s"], 1),
              max_gap_ms=cad["max_gap_ms"] and round(cad["max_gap_ms"], 2))
    if cad["count"] < 2:
        c.fail("fewer than two state reports")
        return
    if path == "W":
        lo = C.STATE_PERIOD_MS * (1 - C.STATE_PERIOD_TOL)
        hi = C.STATE_PERIOD_MS * (1 + C.STATE_PERIOD_TOL)
        c.true("mean period %.4f ms within %.4f..%.4f" % (cad["mean_period_ms"], lo, hi),
               lo <= cad["mean_period_ms"] <= hi)
    elif path == "B":
        if not interval_us:
            c.true("%.1f reports a second (the connection interval is not known here: "
                   "reported only)" % cad["per_s"], True)
        else:
            want = 1e6 / interval_us
            c.true("%.1f reports a second, one per connection event of %d us (%.1f +- %.0f %%)"
                   % (cad["per_s"], interval_us, want, C.BLE_RATE_TOL * 100),
                   abs(cad["per_s"] - want) <= want * C.BLE_RATE_TOL)
    else:
        c.true("%.0f reports a second (200..350)" % cad["per_s"], 200 <= cad["per_s"] <= 350)
    verdict = stats.gap_verdict(cad["max_gap_ms"], C.CADENCE_GAP_WARN_MS[path],
                                C.CADENCE_GAP_FAIL_MS[path])
    if verdict == "fail":
        c.fail("an arrival gap of %.1f ms" % cad["max_gap_ms"])
    elif verdict == "warn":
        c.warn("an arrival gap of %.1f ms (the PC may have held the reader up)"
               % cad["max_gap_ms"])
    c.note("mean %.4f ms (%.0f/s), largest gap %.1f ms" % (cad["mean_period_ms"], cad["per_s"],
                                                            cad["max_gap_ms"]))


def _sequence(c, path, states, explained):
    g = stats.sequence_gaps([r[1] for _t, r in states])
    if path == "W":
        verdict, unexp = stats.sequence_verdict(g["missing"], explained, g["transitions"],
                                                None, None)
    else:
        verdict, unexp = stats.sequence_verdict(g["missing"], explained, g["transitions"],
                                                0.01, 0.05)
    c.observe(missing=g["missing"], explained=explained, duplicates=g["duplicates"])
    c.eq("repeated sequence numbers", g["duplicates"], 0)
    if verdict == "fail":
        c.fail("%d unexplained holes in %d transitions" % (unexp, g["transitions"]))
    elif verdict == "warn":
        c.warn("%d unexplained holes in %d transitions" % (unexp, g["transitions"]))
    else:
        c.true("sequence +1 per report", True)
        c.note("%d transitions, %d holes, %d explained" % (g["transitions"], g["missing"],
                                                           explained))


def _ble_counts(rig):
    """B: (dropped, reports, interval_us) of BLE_STATUS (through the console)."""
    st = rig.ble_link_status() or {}
    return st.get("dropped", 0), st.get("reports", 0), st.get("interval_us")


def _lost(path, cnt0, cnt1, ble0, ble1):
    """The holes the board itself explains: the state reports it counted as not sent."""
    if path == "B":
        return ble1[0] - ble0[0]
    name = "usb_dropped" if path == "W" else "radio_dropped"
    return cnt1.get(name, 0) - cnt0.get(name, 0)


def run(ctx):
    rig = ctx.rig
    if not rig.ctx.armed:
        rig.arm()
    path = rig.path
    need_stream = rig.models("stream")
    raw = []
    ble0 = ble1 = (0, 0, None)
    if need_stream:
        rig.flush()
        cnt0 = rig.cnt_read()
        if path == "B":
            ble0 = _ble_counts(rig)
            raw = rig.read_stream(duration_s=10.0, raw=True)
            reps = [(ts, rig.ble_as_state(r)) for ts, r in raw]
            ble1 = _ble_counts(rig)
        else:
            reps = rig.read_stream(duration_s=10.0)
        cnt1 = rig.cnt_read()
        states = [(ts, r) for ts, r in reps if r[0] == C.RPT_STATE]
    else:
        reps, states, cnt0, cnt1 = [], [], {}, {}
    interval_us = ble1[2] or ble0[2]

    if ctx.want("stream.ids"):
        with ctx.check(GROUP, "stream.ids") as c:
            base.need(rig, "stream")
            allowed = {C.RPT_STATE, C.RPT_BATTERY}
            if path == "R":
                allowed |= {C.RPT_LINK_STATUS, C.RPT_LINK_QUALITY}
            ids = sorted(set(r[0] for _t, r in reps))
            c.observe(ids=["0x%02x" % i for i in ids])
            c.eq("report IDs besides 0x42/0x43%s" % (" and the puck's 0x79/0x7B" if path == "R"
                                                    else ""),
                 ["0x%02x" % i for i in ids if i not in allowed], [])
            c.true("0x42 reports are 54 bytes", all(len(r) >= C.STATE_REPORT_LEN
                                                    for _t, r in states))
            c.true("state reports arrived", len(states) > 100)
            if path == "B":
                raw_ids = sorted(set(r[0] for _t, r in raw))
                c.observe(raw_ids=["0x%02x" % i for i in raw_ids])
                c.eq("over Bluetooth: report IDs besides 0x45/0x43",
                     ["0x%02x" % i for i in raw_ids if i not in (C.RPT_BT_STATE, C.RPT_BATTERY)],
                     [])
                c.true("no 0x42 over Bluetooth (it goes out as 0x45)",
                       C.RPT_STATE not in raw_ids)
                c.true("every 0x45 has its 45 bytes and nothing after them (Windows pads with "
                       "zeros)", all(len(r) >= 1 + C.BLE_STATE_LEN and
                                     not any(r[1 + C.BLE_STATE_LEN:])
                                     for _t, r in raw if r[0] == C.RPT_BT_STATE))

    if ctx.want("stream.cadence"):
        with ctx.check(GROUP, "stream.cadence") as c:
            base.need(rig, "stream")
            _cadence(c, path, states, interval_us)

    if ctx.want("stream.sequence"):
        with ctx.check(GROUP, "stream.sequence") as c:
            base.need(rig, "stream")
            _sequence(c, path, states, _lost(path, cnt0, cnt1, ble0, ble1))

    if ctx.want("stream.counters"):
        with ctx.check(GROUP, "stream.counters") as c:
            base.need(rig, "stream")
            rig.cnt_reset()
            rig.flush()
            b0 = _ble_counts(rig) if path == "B" else None
            got = rig.read_stream(duration_s=2.0)
            # Bluetooth's count first, so its window lies inside the frame counters' one (each
            # request over Bluetooth takes tens of milliseconds, several reports' worth).
            b1 = _ble_counts(rig) if path == "B" else None
            cnt = rig.cnt_read()
            tail = rig.read_stream(duration_s=0.05)
            received = sum(1 for _t, r in got + tail if r[0] == C.RPT_STATE)
            c.observe(frames=cnt.get("frames"), usb_sent=cnt.get("usb_sent"),
                      radio_sent=cnt.get("radio_sent"), received=received)
            if path == "B":
                sent = b1[1] - b0[1]
                c.observe(ble_sent=sent, ble_dropped=b1[0] - b0[0])
                c.true("Bluetooth sent %d <= frames %d (+ the one in progress)"
                       % (sent, cnt.get("frames", 0)), sent <= cnt.get("frames", 0) + 1)
                if received < 0.9 * sent - 24:
                    c.warn("received %d of the %d state reports sent" % (received, sent))
            elif path == "W":
                # CNT_RESET runs in the USB stack's thread, which can preempt the report thread
                # between a frame's count and its usb_sent count: one frame either way
                gap = cnt.get("frames", 0) - (cnt.get("usb_sent", 0) + cnt.get("usb_dropped", 0))
                c.true("frames - (usb_sent + usb_dropped) is -1, 0 or 1 (%d)" % gap,
                       gap in (-1, 0, 1))
                c.true("received %d of usb_sent %d (in flight 8, the flush 16)"
                       % (received, cnt.get("usb_sent", 0)),
                       received >= cnt.get("usb_sent", 0) - 24)
            else:
                sent = cnt.get("radio_sent", 0)
                c.true("radio_sent %d <= frames %d" % (sent, cnt.get("frames", 0)),
                       sent <= cnt.get("frames", 0))
                if received < 0.9 * sent - 24:
                    c.warn("received %d of radio_sent %d" % (received, sent))

    if ctx.want("stream.soak"):
        with ctx.check(GROUP, "stream.soak") as c:
            soak = ctx.opt.get("soak") or 0
            if not soak:
                c.skip("off by default (--soak S)")
            base.need(rig, "stream")
            cnt0 = rig.cnt_read()
            s0 = _ble_counts(rig) if path == "B" else ble0
            rig.flush()
            reps = rig.read_stream(duration_s=float(soak))
            cnt1 = rig.cnt_read()
            s1 = _ble_counts(rig) if path == "B" else ble1
            states = [(ts, r) for ts, r in reps if r[0] == C.RPT_STATE]
            _cadence(c, path, states, s1[2] or s0[2])
            _sequence(c, path, states, _lost(path, cnt0, cnt1, s0, s1))


def passive(ctx):
    """The S form (Steam open): read-only, nothing sent to the board; reported only."""
    rig = ctx.rig
    with ctx.check(GROUP, "stream.cadence") as c:
        base.need(rig, "stream")
        reps = rig.read_stream(duration_s=10.0)
        states = [(ts, r) for ts, r in reps if r[0] == C.RPT_STATE]
        _cadence(c, rig.path, states)
    with ctx.check(GROUP, "stream.sequence") as c:
        base.need(rig, "stream")
        g = stats.sequence_gaps([r[1] for _t, r in states])
        c.true("reported", True)
        c.note("%d transitions, %d holes (read only: reported, not judged)"
               % (g["transitions"], g["missing"]))
