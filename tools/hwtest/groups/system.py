# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# system: the firmware's system state machine (docs/testing.md, group "system"), Steam closed,
# through the test hooks' system injection: buttons, a battery level and a power source that reach
# the firmware's own rules (the chords, STEAM held 7 s, the idle timeout, low battery, the power
# source), never a report. The board has no battery, so every shutdown ends in a reboot the suite
# can see, and setting 77 makes the ordinary shutdown's reboot its own; the firmware keeps how the
# shutdown ended across that reboot (SYS_STATUS). A shutdown test leaves a boot-time injection
# (power source "battery") for the next boot, so the board comes back wireless; it then returns to
# USB power with its link up. The keylock test writes flash (--flash).
from . import base
from .. import config as C
from ..board import BoardGone
from ..proto import sysproto as S
from ..proto import testproto as T

GROUP = "system"
TESTS = [("system.status", "WR", []), ("system.battery_source", "R", []),
         ("system.turn_off", "R", []), ("system.wireless_off", "R", []),
         ("system.host_asleep", "R", []), ("system.steam_hold_usb", "R", []),
         ("system.idle_usb", "R", []), ("system.link_off_steam", "R", []),
         ("system.shutdown_77", "R", []), ("system.idle_battery", "R", []),
         ("system.low_battery", "R", []), ("system.power_on_chord", "R", []),
         ("system.keylock", "R", ["flash"]), ("system.unplug", "W", [])]

STEAM_X = S.BTN_STEAM | S.BTN_X   # STEAM held without being "STEAM alone": no chord, no timers
LINK = {"off": 0, "connected": 4}


def _rearm(rig):
    try:
        rig.arm()
    except base.Stall:
        rig.hooks = False
        return False
    rig.hooks = True
    return True


def _of(entries, kind):
    return [e for e in entries if e["kind"] == kind]


def _states(entries):
    return [(e["from_name"], e["to_name"], e["cause_name"])
            for e in _of(entries, C.RING_SYS_STATE) if "from_name" in e]


def _status(rig, c):
    st = rig.sys_status()
    if st is None:
        c.fail("SYS_STATUS did not answer")
        raise base.Skip("no system status")
    return st


def _wait_state(rig, names, timeout):
    """SYS_STATUS until the state is one of `names`; the last status."""
    deadline = rig.clock.monotonic() + rig.t(timeout)
    while True:
        st = rig.sys_status()
        if st and st["state_name"] in names or rig.clock.monotonic() >= deadline:
            return st
        rig.sleep(0.1)


def _need_system(rig, c):
    base.need(rig, "system")
    caps = rig.caps or rig.arm()
    if not T.cap_flag(caps, C.CAP_SYSTEM):
        c.skip("firmware built without CONFIG_OPENSC2_SYSTEM")


def _back_on_usb(rig, c):
    """After a test on an injected battery: the real power source again, with the link up, so
    the machine goes back to USB power with its link (no reboot). Not during the battery wait of
    a boot on the injected battery: USB power there is the wired start, a reboot."""
    if rig.path == "R":
        _wait_state(rig, ("battery", "usb-link-on"), 3.0)
        rig.wait_link("connected", 5.0)
    rig.sys_inject_off()
    rig.sleep(0.3)
    st = rig.sys_status()
    if st is not None and rig.path == "R":
        c.eq("back on USB power with the link up", st["state_name"], "usb-link-on")


def _reboot_through(rig, request):
    """A request that makes the board reboot: sent with the keep-alive off; the board may go
    before its answer comes. Then the board back (identified again) and armed."""
    rig._stop_keepalive()
    try:
        request()
    except BoardGone:
        pass
    rig.wait_back(rig.path, None, rig.t(C.REENUM_TIMEOUT_S))
    rig._after_boot()
    _rearm(rig)


def _end_ok(c, st, state, cause, hows):
    c.true("the last end was kept across the reboot", st["end_valid"])
    c.eq("the last end", (S.STATES[st["end_state"]] if st["end_state"] < len(S.STATES) else "?",
                          S.EVENTS[st["end_cause"]] if st["end_cause"] < len(S.EVENTS) else "?"),
         (state, cause))
    c.true("it ended in a reboot (how %d)" % st["end_how"], st["end_how"] in hows)


def _setup_battery(rig, c, settings=None):
    """Battery operation with the link up (the machine's view), and the next boot on battery
    too. Returns the ring position after it."""
    if settings:
        rig.write_settings(settings)
    rig.sys_boot(S.INJECT_SOURCE, source=S.SRC_BATTERY)
    rig.sys_inject(S.INJECT_SOURCE, source=S.SRC_BATTERY)
    st = _wait_state(rig, ("battery",), 1.0)
    c.eq("on an injected battery", st["state_name"] if st else None, "battery")
    return rig.ring_next()


def run(ctx):
    rig = ctx.rig
    w = rig.path == "W"
    if not rig.ctx.armed:
        rig.arm()

    if ctx.want("system.status"):
        with ctx.check(GROUP, "system.status") as c:
            _need_system(rig, c)
            st = _status(rig, c)
            c.eq("power source USB", st["source"], S.SRC_USB)
            c.eq("nothing injected", st["flags"] & 0x1E, 0)
            c.eq("setting 50 in use", st["idle_timeout_s"], 900)
            c.eq("its periods (900 s / 16)", st["idle_period_ms"], 56250)
            c.eq("the fully-charged LED level", st["full_level"], 3)
            if w:
                c.eq("state", st["state_name"], "wired")
                c.eq("LED mode 3 (wired)", st["led_mode"], S.LED_WIRED)
                c.eq("the boot: the wired start", S.STATES[st["boot_state"]], "wired-wait")
                c.eq("the boot's cue: 0, the power-on chime", st["boot_cue"], S.CUE_POWER_ON)
            else:
                c.eq("state", st["state_name"], "usb-link-on")
                c.eq("LED mode 2 (the puck) on external power",
                     (st["led_mode"], st["led_flags"] & S.LED_EXTERNAL), (S.LED_PUCK,
                                                                          S.LED_EXTERNAL))
                c.eq("the boot: wireless on USB power", S.STATES[st["boot_state"]],
                     "usb-link-on")
                c.true("the boot's cue: the stored transport's (3 or 4)",
                       st["boot_cue"] in (3, 4))
            c.observe(battery_build=st["battery_build"], bluetooth=st["bluetooth"])

    if w:
        _run_w(ctx)
        return
    if ctx.want("system.battery_source"):
        with ctx.check(GROUP, "system.battery_source") as c:
            _need_system(rig, c)
            start = rig.ring_next()
            rig.sys_inject(S.INJECT_SOURCE, source=S.SRC_BATTERY)
            st = _wait_state(rig, ("battery",), 1.0)
            c.eq("power gone: battery operation, the link kept", st["state_name"], "battery")
            c.eq("LED mode 2 on battery", (st["led_mode"], st["led_flags"] & S.LED_EXTERNAL),
                 (S.LED_PUCK, 0))
            rig.sys_inject_off()
            st = _wait_state(rig, ("usb-link-on",), 1.0)
            c.eq("power back with the link up: USB power, the link kept", st["state_name"],
                 "usb-link-on")
            entries = rig.ring_all(start)[2]
            c.eq("SYS_STATE entries", _states(entries),
                 [("usb-link-on", "battery", "power-none"),
                  ("battery", "usb-link-on", "power-usb")])
            src = [(e["from"], e["to"]) for e in _of(entries, C.RING_SOURCE) if "to" in e]
            c.eq("SOURCE entries", src, [(S.SRC_USB, S.SRC_BATTERY), (S.SRC_BATTERY,
                                                                      S.SRC_USB)])
            c.true("the link stayed up", rig.wait_link("connected", 1.0))

    if ctx.want("system.turn_off"):
        with ctx.check(GROUP, "system.turn_off") as c:
            _need_system(rig, c)
            start = rig.ring_next()
            with rig.own_link_drop():
                c.eq("HOST_SET 0x9F", rig.host_set(C.VC_TURN_OFF), 0)
                st = _wait_state(rig, ("usb-link-off",), 1.0)
                c.eq("on USB power the turn-off is wireless off", st["state_name"],
                     "usb-link-off")
                c.eq("LED mode 4 (amber)", st["led_mode"], S.LED_AMBER)
                c.true("the link went off", rig.wait_link("off", 1.0))
                entries = rig.ring_all(start)[2]
                cues = [(e["cue"], e["script"]) for e in _of(entries, C.RING_CUE) if "cue" in e]
                c.eq("the power-off cue (cue 5, script 5)", cues, [(S.CUE_POWER_OFF, 5)])
                c.true("SYS_STATE usb-link-on -> usb-link-off (turn-off)",
                       ("usb-link-on", "usb-link-off", "turn-off") in _states(entries))
                rig.console_run("opensc2 start")
                c.true("opensc2 start: connected again", rig.wait_link("connected", 3.0))
            st = _wait_state(rig, ("usb-link-on",), 1.0)
            c.eq("the link started from outside: wireless on again", st["state_name"],
                 "usb-link-on")
            rig.lizard_off()

    if ctx.want("system.wireless_off"):
        with ctx.check(GROUP, "system.wireless_off") as c:
            _need_system(rig, c)
            start = rig.ring_next()
            with rig.own_link_drop():
                rig.sys_event(S.EV_WIRELESS_OFF)
                st = _wait_state(rig, ("usb-link-off",), 1.0)
                c.eq("a link that gave up, on USB power: wireless off", st["state_name"],
                     "usb-link-off")
                c.true("SYS_STATE ... (wireless-off)", ("usb-link-on", "usb-link-off",
                                                        "wireless-off")
                       in _states(rig.ring_all(start)[2]))
                rig.console_run("opensc2 start")
                c.true("connected again", rig.wait_link("connected", 3.0))
            rig.lizard_off()

    if ctx.want("system.host_asleep"):
        with ctx.check(GROUP, "system.host_asleep") as c:
            _need_system(rig, c)
            start = rig.ring_next()
            rig.sys_event(S.EV_HOST_ASLEEP)
            rig.sleep(0.1)
            rig.sys_event(S.EV_HOST_ASLEEP)
            rig.sleep(0.1)
            entries = rig.ring_all(start)[2]
            c.eq("HOST_ASLEEP entries", len(_of(entries, C.RING_HOST_ASLEEP)), 2)
            cues = [(e["cue"], e["script"], e["gain"]) for e in _of(entries, C.RING_CUE)
                    if "cue" in e]
            if not cues:
                c.warn("no cue 10: the PC-asleep cue had already played this boot (once per "
                       "boot)")
            else:
                c.eq("one cue 10 (script 4) at the normal chime level, once", cues,
                     [(S.CUE_HOST_ASLEEP, 4, -12)])
            st = _status(rig, c)
            c.eq("the link and the state stay", st["state_name"], "usb-link-on")

    if ctx.want("system.steam_hold_usb"):
        with ctx.check(GROUP, "system.steam_hold_usb") as c:
            _need_system(rig, c)
            start = rig.ring_next()
            with rig.own_link_drop():
                rig.sys_inject(S.INJECT_BUTTONS, buttons=S.BTN_STEAM)
                rig.sleep(7.4)
                st = _status(rig, c)
                c.eq("STEAM held 7 s on USB power: wireless off", st["state_name"],
                     "usb-link-off")
                rig.sys_inject(S.INJECT_BUTTONS, buttons=0)
                entries = rig.ring_all(start)[2]
                chords = [(e["what_name"], e["chord"], e["time_us"])
                          for e in _of(entries, C.RING_CHORD) if "what_name" in e]
                names = [(n, k) for n, k, _t in chords]
                c.true("CHORD down, steam-press, steam-hold (STEAM alone)",
                       names[:3] == [("down", S.CHORD_STEAM), ("steam-press", S.CHORD_STEAM),
                                     ("steam-hold", S.CHORD_STEAM)])
                if len(chords) >= 3:
                    held = (chords[2][2] - chords[0][2]) / 1000.0
                    c.true("held after 7 s (%.0f ms)" % held, 6980 <= held <= 7100)
                    press = (chords[1][2] - chords[0][2]) / 1000.0
                    c.true("pressed after 20 ms (%.0f ms)" % press, 19 <= press <= 80)
                c.true("SYS_STATE usb-link-on -> usb-link-off (steam-held)",
                       ("usb-link-on", "usb-link-off", "steam-held") in _states(entries))
                rig.console_run("opensc2 start")
                c.true("connected again", rig.wait_link("connected", 3.0))
            rig.sys_inject_off()
            rig.lizard_off()

    if ctx.want("system.idle_usb"):
        with ctx.check(GROUP, "system.idle_usb") as c:
            _need_system(rig, c)
            rig.write_settings([(50, 2)])
            start = rig.ring_next()
            rig.sleep(2.6)
            entries = rig.ring_all(start)[2]
            c.true("the idle monitor fired (an IDLE entry)", _of(entries, C.RING_IDLE) != [])
            st = _status(rig, c)
            c.eq("on USB power nothing acts on it", st["state_name"], "usb-link-on")
            c.eq("setting 50 in use", st["idle_timeout_s"], 2)
            rig.write_settings([(50, 900)])

    if ctx.want("system.link_off_steam"):
        with ctx.check(GROUP, "system.link_off_steam") as c:
            _need_system(rig, c)
            with rig.own_link_drop():
                rig.host_set(C.VC_TURN_OFF)
                st = _wait_state(rig, ("usb-link-off",), 1.0)
                c.eq("wireless off", st["state_name"], "usb-link-off")
                # a STEAM press: a reboot without the warm mark, which on USB power is wired
                _reboot_through(rig, lambda: rig.sys_inject(S.INJECT_BUTTONS,
                                                            buttons=S.BTN_STEAM))
            c.eq("came back wired", rig.path, "W")
            st = _status(rig, c)
            _end_ok(c, st, "reboot", "steam-pressed", (S.HOW_REBOOT,))
            if rig.path == "W":
                rig.to_wireless()
                _rearm(rig)
            c.eq("back wireless for the rest", rig.path, "R")

    if ctx.want("system.shutdown_77"):
        with ctx.check(GROUP, "system.shutdown_77") as c:
            _need_system(rig, c)
            start = _setup_battery(rig, c, [(77, 1)])
            with rig.own_link_drop():
                rig.sys_inject(S.INJECT_BUTTONS, buttons=S.BTN_STEAM)
                rig.sleep(7.4)
                st = _status(rig, c)
                c.eq("STEAM held 7 s on battery: the shutdown, waiting for STEAM to be up",
                     (st["state_name"], st["ended"]), ("shutdown", False))
                entries = rig.ring_all(start)[2]
                c.true("SYS_STATE battery -> shutdown (steam-held)",
                       ("battery", "shutdown", "steam-held") in _states(entries))
                c.eq("the power-off cue", [e["cue"] for e in _of(entries, C.RING_CUE)
                                           if "cue" in e], [S.CUE_POWER_OFF])
                leds = [(e["mode"], e["flags"]) for e in _of(entries, C.RING_LED_MODE)
                        if "mode" in e]
                c.true("LED off, shutting down", (S.LED_OFF, S.LED_SHUTTING_DOWN) in leds)
                _reboot_through(rig, lambda: rig.sys_inject(S.INJECT_BUTTONS, buttons=0))
            c.eq("came back wireless (the boot-time injection: a battery)", rig.path, "R")
            st = _status(rig, c)
            _end_ok(c, st, "shutdown", "steam-held", (S.HOW_REBOOT,))
            c.true("this boot came from the boot-time injection", st["boot_injected"])
            c.eq("setting 77 back to 0 after the reboot", rig.settings_read().get(77), 0)
            _back_on_usb(rig, c)

    if ctx.want("system.idle_battery"):
        with ctx.check(GROUP, "system.idle_battery") as c:
            _need_system(rig, c)
            _setup_battery(rig, c, [(77, 1), (50, 2)])
            with rig.own_link_drop():
                marker = rig.mark(0x1D1E0001)
                rig.sys_inject(S.INJECT_BUTTONS, buttons=STEAM_X)   # one press: activity
                st = _wait_state(rig, ("shutdown",), 3.5)
                c.eq("idle 2 s on battery: the shutdown", st["state_name"], "shutdown")
                entries = rig.ring_all(marker)[2]
                c.true("SYS_STATE battery -> shutdown (idle)",
                       ("battery", "shutdown", "idle") in _states(entries))
                idle = _of(entries, C.RING_IDLE)
                if entries and idle:
                    dt = (idle[0]["time_us"] - entries[0]["time_us"]) / 1000.0
                    c.true("16 to 17 periods of 125 ms after the press (%.0f ms)" % dt,
                           1950 <= dt <= 2300)
                _reboot_through(rig, lambda: rig.sys_inject(S.INJECT_BUTTONS, buttons=0))
            st = _status(rig, c)
            _end_ok(c, st, "shutdown", "idle", (S.HOW_REBOOT,))
            _back_on_usb(rig, c)

    if ctx.want("system.low_battery"):
        with ctx.check(GROUP, "system.low_battery") as c:
            _need_system(rig, c)
            _setup_battery(rig, c, [(77, 1)])
            with rig.own_link_drop():
                start = rig.ring_next()
                rig.sys_inject(S.INJECT_BUTTONS, buttons=STEAM_X)
                rig.sys_inject(S.INJECT_LEVEL, level=0)
                st = _wait_state(rig, ("shutdown-low",), 4.5)   # the next battery report
                c.eq("0 % on battery: the low-battery shutdown", st["state_name"],
                     "shutdown-low")
                entries = rig.ring_all(start)[2]
                leds = [e["mode"] for e in _of(entries, C.RING_LED_MODE) if "mode" in e]
                c.true("LED mode 6 (low battery)", S.LED_LOW_BATTERY in leds)
                c.eq("the power-off cue", [e["cue"] for e in _of(entries, C.RING_CUE)
                                           if "cue" in e], [S.CUE_POWER_OFF])
                _reboot_through(rig, lambda: rig.sys_inject(S.INJECT_BUTTONS, buttons=0))
            st = _status(rig, c)
            # it ignores setting 77: a power-off, which a board without a battery (or with its
            # test hooks in use) turns into a reboot
            _end_ok(c, st, "shutdown-low", "level", (S.HOW_NO_BATTERY, S.HOW_TEST))
            _back_on_usb(rig, c)

    if ctx.want("system.power_on_chord"):
        with ctx.check(GROUP, "system.power_on_chord") as c:
            _need_system(rig, c)
            for hold, pairing in ((3, 0), (45, 1)):
                rig.sys_boot(S.INJECT_SOURCE | S.INJECT_BUTTONS, buttons=S.BTN_STEAM,
                             source=S.SRC_BATTERY, hold=hold)
                with rig.own_link_drop():
                    rig.plain_reboot()
                _rearm(rig)
                c.eq("came back wireless (on the injected battery)", rig.path, "R")
                st = _wait_state(rig, ("battery",), hold / 10.0 + 2.0)
                c.eq("STEAM held at a battery power-on: the chord state, then battery",
                     (S.STATES[st["boot_state"]], st["boot_chord"], st["state_name"]),
                     ("chord", S.CHORD_STEAM, "battery"))
                c.true("the stored transport's cue (3 or 4)", st["boot_cue"] in (3, 4))
                c.eq("held %.1f s: the pairing flag %d" % (hold / 10.0, pairing),
                     st["boot_pairing"], pairing)
                _back_on_usb(rig, c)

    if ctx.want("system.keylock"):
        with ctx.check(GROUP, "system.keylock") as c:
            _need_system(rig, c)
            if not ctx.opt.get("flash"):
                c.skip("opt-in (--flash): it stores and deletes user/keylock")
            start = _setup_battery(rig, c, [(77, 1)])
            # the next boot on battery meets the lock: STEAM held there keeps its shutdown
            # waiting long enough to look at it
            rig.sys_boot(S.INJECT_SOURCE | S.INJECT_BUTTONS, buttons=STEAM_X,
                         source=S.SRC_BATTERY, hold=60)
            with rig.own_link_drop():
                rig.sys_inject(S.INJECT_BUTTONS, buttons=S.KEYLOCK_CHORD)
                st = _wait_state(rig, ("shutdown",), 4.0)
                c.eq("the keylock chord held 3 s: stored, then the shutdown", st["state_name"],
                     "shutdown")
                entries = rig.ring_all(start)[2]
                what = [e["what_name"] for e in _of(entries, C.RING_CHORD) if "what_name" in e]
                c.true("CHORD keylock-tap, then keylock-hold",
                       "keylock-tap" in what and "keylock-hold" in what)
                ctx.results.safety["flash_writes"] += 1
                _reboot_through(rig, lambda: rig.sys_inject(S.INJECT_BUTTONS, buttons=0))
                st = _status(rig, c)
                c.eq("the next power-on on battery: the keylock shutdown",
                     (S.STATES[st["boot_state"]], st["state_name"]),
                     ("shutdown-locked", "shutdown-locked"))
                c.eq("LED mode 7 (keylock)", st["led_mode"], S.LED_KEYLOCK)
                c.true("0xED user/keylock holds the lock",
                       rig.read_named(b"user/keylock\0").data[:4] ==
                       (0x41FB0AC3).to_bytes(4, "little"))
                # STEAM lets go (the boot injection's 6 s): off, which reboots; a USB power-on
                # then deletes the lock
                rig._stop_keepalive()
                rig.wait_back("R", None, rig.t(C.REENUM_TIMEOUT_S + 6.0))
                rig._after_boot()
                _rearm(rig)
            ctx.results.safety["flash_writes"] += 1
            c.eq("came back wired (USB power, no injection)", rig.path, "W")
            st = _status(rig, c)
            _end_ok(c, st, "shutdown-locked", "keylock-held", (S.HOW_NO_BATTERY, S.HOW_TEST))
            c.eq("a USB power-on deletes the keylock", rig.read_named(b"user/keylock\0").n, 0)
            if rig.path == "W":
                rig.to_wireless()
                _rearm(rig)

    # the rest of the run expects the board as it came: wireless, on USB power
    if rig.path == "W":
        rig.to_wireless()
        _rearm(rig)


def _run_w(ctx):
    rig = ctx.rig
    if ctx.want("system.unplug"):
        with ctx.check(GROUP, "system.unplug") as c:
            _need_system(rig, c)
            rig.write_settings([(77, 1)])
            # power gone while wired: the shutdown, which leaves the bus at once
            _reboot_through(rig, lambda: rig.sys_inject(S.INJECT_SOURCE,
                                                        source=S.SRC_BATTERY))
            c.eq("came back wired", rig.path, "W")
            st = _status(rig, c)
            _end_ok(c, st, "shutdown", "power-none", (S.HOW_REBOOT,))
            c.eq("wired again", st["state_name"], "wired")
