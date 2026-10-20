# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# modes: the mode switches and reboots (docs/testing.md, group "modes"), Steam closed. The runner
# calls these functions at the points of its order (after the W groups, after the R groups, at the
# end). Each switch checks how the board left (the last reports), that it came back on the expected
# path within the timeout, and identifies it again by its unit serial before anything is written.
from . import base
from . import cmd as cmd_group
from .. import config as C
from ..proto import console as cproto
from ..proto import model as M
from ..proto import reports as R
from ..proto import vendor as V

GROUP = "modes"
TESTS = [("modes.plain_reboot", "W", []), ("modes.to_wireless", "W", []),
         ("modes.session_log", "R", []), ("modes.warm_wireless", "RB", []),
         ("modes.to_wired", "R", []), ("modes.identity", "WRB", []), ("modes.return", "", [])]


def _last_reports(rig, seconds=0.3):
    try:
        return rig.backend.read_stream("W", duration_s=seconds)
    except Exception:
        return []


def _check_neutral(c, reps, what):
    states = [r for _t, r in reps if r and r[0] == C.RPT_STATE]
    if not states:
        c.warn("%s: no report read before the board left" % what)
        return
    c.eq("%s: the last 0x42 is all zero, sequence 0 too" % what, states[-1][:C.STATE_REPORT_LEN],
         bytes([C.RPT_STATE]) + bytes(C.STATE_REPORT_LEN - 1))


def _rearm(rig):
    """After a reboot the hooks are unarmed: arm again (a new nonce) before anything else.
    True when armed (a board without the hooks STALLs the keyed ARM)."""
    if rig.hooks is False:
        return False
    try:
        rig.arm()
    except base.Stall:
        rig.hooks = False
        return False
    rig.hooks = True
    return True


def plain_reboot(ctx):
    rig = ctx.rig
    with ctx.check(GROUP, "modes.plain_reboot") as c:
        base.need(rig, "reboot")
        policy = (rig.caps or {}).get("policy", 0)
        rig.flush()
        rig.vendor_set(C.VC_POWER_REBOOT, bytes(4))
        reps = _last_reports(rig)
        if rig.models("detach_report"):
            _check_neutral(c, reps, "plain reboot")
        rig.did_plain_reboot = True
        rig.wait_back("W", None, rig.t(C.REENUM_TIMEOUT_S))
        rig._after_boot()
        ctx.path = rig.path
        want = "R" if policy else "W"
        c.eq("came back %s (the stored policy %s)" % ("wireless" if policy else "wired",
                                                    "wireless" if policy else "auto"),
             rig.path, want)
        if rig.path == "W":
            if ctx.want("cmd.first_get"):
                cmd_group.first_get(ctx)       # before any SET: its own row
            a = rig.vendor(C.VC_DEVICE_INFO, b"\x02")
            up = V.decode_device_info2(a.data)["uptime_ms"]
            c.true("0xF2 02: uptime below 15 s (%d ms)" % up, up < rig.t(15.0) * 1000)
            if _rearm(rig):
                s = rig.settings_read()
                c.eq("settings not at their defaults after the reboot",
                     [n for n in range(86) if s[n] != M.setting_default(n)], [])
                c.eq("setting 9 (lizard mode on at boot)", s[9], 1)
    ctx.path = rig.path


def to_wireless(ctx):
    rig = ctx.rig
    with ctx.check(GROUP, "modes.to_wireless") as c:
        base.need(rig, "reboot")
        rig.flush()
        t0 = rig.clock.monotonic()
        rig.vendor_set(C.VC_POWER_REBOOT, C.KEY_REBOOT_WIRELESS.to_bytes(4, "little"))
        rig.stats_before_switch = dict(rig.stats)
        reps = _last_reports(rig)
        if rig.models("detach_report"):
            _check_neutral(c, reps, "reboot into wireless")
        rig.wait_back("W", "R", rig.t(C.REENUM_FIRST_INSTALL_S))
        rig._after_boot()
        if rig.path != "R":
            c.fail("came back wired (the warm mark did not survive)")
            ctx.path = rig.path
            return
        st = cproto.parse_status(rig.console_run("opensc2 status"))
        c.eq("opensc2 status: mode", (st.get("mode"), st.get("mode_reason")),
             ("wireless", "warm restart"))
        c.eq("opensc2 status: the wired session's end", st.get("last_wired_end"),
             "host: reboot into wireless")
        c.note("console after %.1f s" % (rig.clock.monotonic() - t0))
    ctx.path = rig.path


def session_log(ctx):
    rig = ctx.rig
    if not ctx.want("modes.session_log"):
        return
    with ctx.check(GROUP, "modes.session_log") as c:
        base.need(rig, "session_log")
        if rig.path != "R":
            c.skip("the board is not wireless")
        lines = rig.console_run("opensc2 usb log", timeout=5.0)
        text = [ln.strip() for ln in lines]
        head = [ln for ln in text if ln.startswith("session log:")]
        c.true("the session log header", bool(head))
        body = [ln.split(" ", 1)[1].strip() if " " in ln else ln for ln in text
                if ln[:1] == " " or ln[:1].isdigit()]
        c.true("the wired session was configured", any("configured" in ln for ln in body))
        last = cproto.parse_status(text).get("last_wired_end")
        c.eq("the end reason", last, "host: reboot into wireless")
        stalls = [ln for ln in body if "STALL" in ln]
        c.observe(lines=len(body), stalls=len(stalls))
        summary = [ln for ln in text if ln.startswith("last wired: boot")]
        if summary:
            parts = summary[0].replace(",", "").split()
            try:
                requests = int(parts[parts.index("requests") - 1])
                stalled = int(parts[parts.index("stalls") - 1])
                mine = rig.stats_before_switch or {}
                if requests != mine.get("usb_requests") or stalled != mine.get("usb_stalls"):
                    c.warn("the board counted %d requests, %d stalls; the runner sent %s, saw %s "
                           "stall (Windows may add requests)" % (requests, stalled,
                                                                mine.get("usb_requests"),
                                                                mine.get("usb_stalls")))
                else:
                    c.note("%d requests, %d stalls, as the runner counted" % (requests, stalled))
            except (ValueError, IndexError):
                c.fail("the summary line does not parse")


def warm_wireless(ctx):
    rig = ctx.rig
    if not ctx.want("modes.warm_wireless"):
        return
    if rig.path == "B":
        _warm_bluetooth(ctx)
        return
    with ctx.check(GROUP, "modes.warm_wireless") as c:
        base.need(rig, "reboot")
        rig.warm_wireless()
        c.eq("came back wireless", rig.path, "R")
        if rig.path == "R":
            st = cproto.parse_status(rig.console_run("opensc2 status"))
            c.eq("opensc2 status", (st.get("mode"), st.get("mode_reason")),
                 ("wireless", "warm restart"))
            armed = _rearm(rig)
            if armed and rig.models("puck") and rig.puck_present and not rig.injection_blocked:
                # a reboot closes every slot reader: prove the board's slot again, then read it
                rig.ctx.sole_reader = True      # the slots are held as they are opened (S12)
                found = rig.prove_slot()
                c.true("the board's slot on the puck carries it again",
                       found is not None and found["count"] == 1)
                if found:
                    c.true("the board's slot streams again",
                           rig.wait_report(lambda r: r[0] == C.RPT_STATE, rig.t(5.0)) is not None)
            elif rig.models("puck"):
                c.note("the link is not checked (no puck, no hooks, or no injection in this run "
                       "to prove the slot)")
            else:
                c.true("the board streams again",
                       rig.wait_report(lambda r: r[0] == C.RPT_STATE, rig.t(5.0)) is not None)
    ctx.path = rig.path


def _warm_bluetooth(ctx):
    """B: `opensc2 reboot wireless` comes back on the stored transport, Bluetooth, and Windows
    connects the board again (directed advertising to its bonded host): its reports flow."""
    rig = ctx.rig
    with ctx.check(GROUP, "modes.warm_wireless") as c:
        base.need(rig, "reboot")
        t0 = rig.clock.monotonic()
        rig.warm_wireless()
        c.eq("came back wireless over Bluetooth", rig.path, "B")
        if rig.path == "B":
            st = cproto.parse_status(rig.console_run("opensc2 status"))
            c.eq("opensc2 status", (st.get("mode"), st.get("mode_reason"), st.get("bluetooth")),
                 ("wireless", "warm restart", True))
            c.true("the board streams again",
                   rig.wait_report(lambda r: r[0] == C.RPT_STATE, rig.t(5.0)) is not None)
            c.note("Windows had the board back after %.1f s" % (rig.clock.monotonic() - t0))
            _rearm(rig)
    ctx.path = rig.path


def to_wired(ctx):
    rig = ctx.rig
    with ctx.check(GROUP, "modes.to_wired") as c:
        base.need(rig, "reboot")
        rig.to_wired()
        c.eq("came back wired", rig.path, "W")
        if rig.path == "W":
            c.true("the stream flows", rig.wait_report(lambda r: r[0] == C.RPT_STATE, 1.0)
                   is not None)
            a = rig.vendor(C.VC_DEVICE_INFO, b"\x02")
            up = V.decode_device_info2(a.data)["uptime_ms"]
            c.true("0xF2 02: uptime below 15 s (%d ms)" % up, up < rig.t(15.0) * 1000)
            _rearm(rig)
    ctx.path = rig.path


def identity(ctx):
    """modes.identity, after a W phase that followed an R one (or the other way round), or on
    B: the identity the board shows on its own HID device (wired, or over Bluetooth) equals what
    its console shows; compared in memory."""
    rig = ctx.rig
    if not ctx.want("modes.identity"):
        return
    with ctx.check(GROUP, "modes.identity") as c:
        ident = getattr(rig, "identity_r", None)
        if not ident:
            c.skip("no wireless phase in this run")
        if not rig.own_hid:
            c.skip("the board is not on its own HID device (wired or Bluetooth)")
        attrs = dict(V.decode_attributes(rig.vendor(C.VC_ATTRIBUTES).data))
        c.eq("0x83 = opensc2 id", (attrs.get(1), attrs.get(10), attrs.get(4), attrs.get(9)),
             (ident.get("pid"), ident.get("blbuild"), ident.get("build"), ident.get("rev")))
        texts = {}
        for tag in (0, 1, 3):
            a = rig.vendor(C.VC_STRING_ATTR, bytes([tag]))
            texts[tag] = a.data[1:20].split(b"\0", 1)[0].decode("ascii", "replace")
        c.true("0xAE 1 = opensc2 id's unit serial", texts[1] == ident.get("unit"))
        c.true("0xAE 0 = opensc2 id's board serial", texts[0] == ident.get("board"))
        c.eq("0xAE 3 = opensc2 id's git revision", texts[3], ident.get("git"))
        if rig.path == "W":
            c.true("the HID serial string = 0xAE 1", texts[1] == rig.unit_serial)
        if rig.models("desc") and ident.get("name") is not None and rig.path == "W":
            c.true("the HID product string = opensc2 id's name",
                   (rig.backend.hid_info() or {}).get("product") == ident.get("name"))
        c.note("equal (compared in memory)")


def mode_return(ctx, start_mode):
    rig = ctx.rig
    with ctx.check(GROUP, "modes.return") as c:
        if rig.path != start_mode:
            if start_mode == "W":
                rig.to_wired()
            elif rig.path == "W":
                rig.to_wireless()      # back on the stored transport: Bluetooth comes back as B
            else:
                rig.warm_wireless()
            _rearm(rig)
        c.eq("the starting mode", rig.path, start_mode)
    ctx.path = rig.path


def run(ctx):
    """--group modes alone: the runner calls the switches itself (runner.py)."""
    return
