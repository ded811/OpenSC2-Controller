# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# test_dryrun.py: the whole runner under --dry-run. No fault runs every group and exits 0; each
# --sim-fault (the faults docs/testing.md lists) gives the expected failing test and exit code,
# and the run around it behaves as it should: what must stop stops, what must go on goes on, and
# nothing else fails as a consequence.
import os
import threading
import unittest

from hwtest import board as board_mod
from hwtest import config as C
from hwtest import mask as mask_mod
from hwtest import results as results_mod
from hwtest import runner as runner_mod
from hwtest import sim
from hwtest import simfw
from hwtest.proto import reports as R
from hwtest.proto import testproto as T

PASS = results_mod.PASS
FAIL = results_mod.FAIL
ERROR = results_mod.ERROR
SKIP = results_mod.SKIP
WARN = results_mod.WARN


def _opt(fault, **extra):
    opt = {"dry_run": True, "fault": fault, "yes": True, "flash": False, "lizard": False,
           "through_puck": False, "with_steam": False, "soak": 0, "groups": None, "tests": None,
           "steam_control": False, "wired_only": False, "wireless_only": False, "quiet": True}
    opt.update(extra)
    return opt


class Run:
    """One dry run: its exit code, every result by id and the runner itself."""

    def __init__(self, fault=None, start="W", setup=None, hooks=True, **extra):
        self.backend = sim.SimBackend(start_mode=start, fault=fault, hooks=hooks)
        if setup:
            setup(self.backend)
        self.runner = runner_mod.Runner(self.backend, _opt(fault, **extra), mask_mod.Masker())
        self.code = self.runner.run()
        self.by_id = {}
        for g in self.runner.results.groups:
            for t in g["tests"]:
                self.by_id.setdefault(t.id, []).append(t)

    def statuses(self, tid):
        return [t.status for t in self.by_id.get(tid, [])]

    def bad(self):
        """Every (id, status) that is a FAIL or an ERROR."""
        return sorted({(t.id, t.status) for ts in self.by_id.values() for t in ts
                       if t.status in (FAIL, ERROR)})

    def ran(self, prefix, status=PASS):
        return sorted({t.id for ts in self.by_id.values() for t in ts
                       if t.id.startswith(prefix) and t.status == status})


class CleanRuns(unittest.TestCase):
    def test_no_fault(self):
        r = Run()
        self.assertEqual(r.code, 0, r.bad())
        self.assertEqual(r.bad(), [])
        for tid in ("inputs.buttons", "inputs.lizard_lock", "battery.values", "stream.cadence",
                    "outputs.rumble", "settings.clamps", "cmd.attributes", "wireless.slot",
                    "wireless.reconnect", "modes.to_wireless", "modes.to_wired",
                    "restore.as_found"):
            self.assertIn(PASS, r.statuses(tid), tid)
        self.assertEqual(r.runner.results.safety["puck_writes"], 0)
        self.assertEqual(r.runner.results.safety["lease_lost"], 0)

    def test_board_found_wireless(self):
        r = Run(start="R")
        self.assertEqual(r.code, 0, r.bad())
        self.assertIn(PASS, r.statuses("modes.to_wired"))
        self.assertIn(PASS, r.statuses("modes.return"))
        self.assertIn(PASS, r.statuses("restore.as_found"))

    def test_one_path(self):
        for start, opt in (("W", "wired_only"), ("W", "wireless_only"), ("R", "wired_only"),
                           ("R", "wireless_only")):
            r = Run(start=start, **{opt: True})
            self.assertEqual(r.code, 0, (start, opt, r.bad()))
            self.assertIn(PASS, r.statuses("restore.as_found"), (start, opt))

    def test_opt_ins(self):
        r = Run(lizard=True, flash=True, through_puck=True)
        self.assertEqual(r.code, 0, r.bad())
        for tid in ("lizard.no_leak", "flash.chime", "flash.policy", "puck.output",
                    "puck.test_channel"):
            self.assertIn(PASS, r.statuses(tid), tid)
        writes = r.runner.results.safety["puck_writes"]
        self.assertGreater(writes, 0)
        # every write through the puck is logged, in the console and in console.log
        logged = [ln for ln in r.runner.results.console_lines if ln.startswith("[through-puck]")]
        self.assertEqual(len([ln for ln in logged if "writing to the board's slot" not in ln]),
                         writes)

    def test_without_opt_ins(self):
        r = Run()
        self.assertEqual(r.backend.writes, 0)                    # no puck writer was ever made
        self.assertFalse([ln for ln in r.runner.results.console_lines if "through-puck]" in ln])
        for tid in ("lizard.no_leak", "flash.chime", "puck.output"):
            self.assertEqual(set(r.statuses(tid)), {SKIP}, tid)
        self.assertEqual(set(r.statuses("steam.session")), {SKIP})

    def test_with_steam(self):
        for start in ("W", "R"):
            r = Run(start=start, with_steam=True, steam_control=True)
            self.assertEqual(r.code, 0, (start, r.bad()))
            for tid in ("steam.session", "steam.log_reconnect", "steam.wired_passive"):
                self.assertIn(PASS, r.statuses(tid), (start, tid))
            self.assertFalse(r.backend.steam)                    # closed again at the end
            self.assertIn(PASS, r.statuses("restore.as_found"))

    def test_with_steam_not_started(self):
        # --with-steam, no --steam-control, --yes: nobody can start Steam: the phase is skipped
        r = Run(with_steam=True)
        self.assertEqual(r.code, 0, r.bad())
        self.assertIn(SKIP, r.statuses("steam.phase"))

    def test_no_hooks(self):
        # a firmware built without the hooks: only what needs none runs (W: the descriptors and
        # the stream as it comes), nothing is armed, and nothing fails
        for start in ("W", "R"):
            r = Run(start=start, hooks=False)
            self.assertEqual(r.code, 0, (start, r.bad()))
            self.assertIn(SKIP, r.statuses("hooks.present"))
            self.assertIn(PASS, r.statuses("desc.ids"))
            self.assertEqual(r.ran("inputs."), [])

    def test_custom_name(self):
        # a name stored on the board (`opensc2 id set name`): the descriptors carry it, the caps
        # carry its CRC-32, the console shows it, and desc.strings and modes.identity expect it
        for start in ("W", "R"):
            r = Run(start=start, setup=lambda b: setattr(b.identity, "name", "Bench Pad 2"))
            self.assertEqual(r.code, 0, (start, r.bad()))
            self.assertIn(PASS, r.statuses("desc.strings"), start)
            self.assertIn(PASS, r.statuses("modes.identity"), start)

    def test_product_string_not_the_name(self):
        # a USB product string other than the configured name fails desc.strings, by the caps'
        # CRC-32 on a wired start and also by `opensc2 id` after a wireless one
        def other(b):
            b.hid_product = "Steam Controller 9"
        for start in ("W", "R"):
            r = Run(start=start, setup=other)
            self.assertIn(FAIL, r.statuses("desc.strings"), start)
            self.assertEqual(r.code, 1, (start, r.bad()))
        r = Run(start="R", setup=other)
        self.assertIn(FAIL, r.statuses("modes.identity"))

    def test_charger_test_build_is_flagged(self):
        # the charger test build's caps flag: hooks.present warns, and nothing fails
        def flag(b):
            b.extra_build_flags = 1 << C.CAP_NO_HOST_WIRELESS
        r = Run(setup=flag)
        self.assertEqual(r.code, 0, r.bad())
        self.assertIn(WARN, r.statuses("hooks.present"))

    def test_build_without_stop_replies(self):
        # CONFIG_OPENSC2_HAPTIC_STOP_STATUS off (caps bit 16 clear): no 0x44 replies are expected,
        # in outputs.stop_status and in steam.session's count of Steam's stops
        def off(b):
            b.cleared_build_flags = 1 << C.CAP_HAPTIC_STOP_STATUS
        r = Run(start="R", setup=off, with_steam=True, steam_control=True)
        self.assertEqual(r.code, 0, r.bad())
        for tid in ("outputs.stop_status", "steam.session"):
            self.assertIn(PASS, r.statuses(tid), tid)
        traffic = r.by_id["steam.session"][0].observed["traffic"]
        self.assertEqual(traffic["stops"], 2)              # Steam's pair of zero pulses
        self.assertEqual(traffic["stop_replies"], {})

    def test_steam_session_counts_steams_outputs(self):
        # steam.session counts what Steam sent the outputs through the puck (the simulated Steam
        # sends its pair of zero pulses once) and checks the board's 0x44 reply to each
        r = Run(start="R", with_steam=True, steam_control=True)
        self.assertEqual(r.code, 0, r.bad())
        session = r.by_id["steam.session"][0]
        self.assertEqual(session.status, PASS)
        traffic = session.observed["traffic"]
        self.assertEqual(traffic["outputs"], {"0x81": 2})
        self.assertEqual(traffic["haptics"], {"pulse": 2})
        self.assertEqual(traffic["stop_replies"], {"right_pad": 1, "left_pad": 1})
        self.assertEqual(session.observed["traffic_entries_lost"], 0)

    def test_no_puck(self):
        # no puck: the wireless phase runs only what needs no link; nothing fails
        r = Run(setup=lambda b: setattr(b, "puck_present", False))
        self.assertEqual(r.code, 0, r.bad())
        self.assertIn(SKIP, r.statuses("inputs.phase"))
        self.assertIn(PASS, r.statuses("inputs.buttons"))           # the wired phase ran
        self.assertIn(PASS, r.statuses("restore.as_found"))

    def test_selection(self):
        r = Run(groups={"battery"})
        self.assertEqual(r.code, 0, r.bad())
        self.assertIn(PASS, r.statuses("battery.values"))
        self.assertNotIn("inputs.buttons", r.by_id)
        self.assertIn(PASS, r.statuses("restore.as_found"))   # restore always runs

    def test_max_minutes(self):
        r = Run(max_minutes=1)
        self.assertIn(SKIP, r.statuses("run.stopped"))
        self.assertIn(PASS, r.statuses("restore.as_found"))

    def test_fail_fast(self):
        r = Run("seq_gap", fail_fast=True)
        self.assertEqual(r.code, 1)
        self.assertIn(SKIP, r.statuses("run.stopped"))
        self.assertNotIn("outputs.rumble", r.by_id)            # nothing after the failing group
        self.assertIn(PASS, r.statuses("restore.as_found"))


class Faults(unittest.TestCase):
    """Every --sim-fault, one fault each."""

    def assertOnly(self, r, allowed):
        extra = [b for b in r.bad() if b[0] not in allowed]
        self.assertEqual(extra, [], "failures besides %s" % sorted(allowed))

    def test_guard_leak(self):
        r = Run("guard_leak")
        self.assertEqual(r.code, 2)
        self.assertEqual(r.statuses("hooks.guard")[0], FAIL)
        self.assertEqual(r.ran("inputs."), [])                  # no injection at all
        self.assertEqual(r.ran("lizard."), [])
        self.assertEqual(r.runner.results.safety["canary"], False)
        self.assertOnly(r, {"hooks.guard"})

    def test_guard_leak_lizard(self):
        r = Run("guard_leak", lizard=True)
        self.assertEqual(r.code, 2)
        self.assertEqual(r.ran("lizard."), [])
        self.assertIn(SKIP, r.statuses("lizard.no_leak"))

    def test_seq_gap(self):
        r = Run("seq_gap")
        self.assertEqual(r.code, 1)
        self.assertIn(FAIL, r.statuses("stream.sequence"))
        self.assertOnly(r, {"stream.sequence"})

    def test_trigger_bit(self):
        r = Run("trigger_bit")
        self.assertEqual(r.code, 1)
        self.assertIn(FAIL, r.statuses("inputs.trigger_bits"))
        self.assertOnly(r, {"inputs.trigger_bits"})

    def test_stall_missing(self):
        r = Run("stall_missing")
        self.assertEqual(r.code, 1)
        self.assertIn(FAIL, r.statuses("cmd.unknown"))
        self.assertOnly(r, {"cmd.unknown", "cmd.stored_request"})

    def test_no_reconnect(self):
        r = Run("no_reconnect")
        self.assertEqual(r.code, 1)
        self.assertIn(FAIL, r.statuses("wireless.reconnect"))
        self.assertIn(SKIP, r.statuses("wireless.link_loss"))   # "no link"
        self.assertIn(SKIP, r.statuses("run.stopped"))
        self.assertIn(PASS, r.statuses("modes.return"))
        self.assertIn(PASS, r.statuses("restore.as_found"))
        self.assertOnly(r, {"wireless.reconnect"})

    def test_stale_battery(self):
        r = Run("stale_battery")
        self.assertEqual(r.code, 1)
        self.assertIn(FAIL, r.statuses("battery.values"))
        self.assertOnly(r, {"battery.values"})

    def test_lease_ignored(self):
        r = Run("lease_ignored")
        self.assertEqual(r.code, 1)
        self.assertIn(FAIL, r.statuses("hooks.lease"))
        self.assertOnly(r, {"hooks.lease"})

    def test_not_as_found(self):
        r = Run("not_as_found")
        self.assertEqual(r.code, 6)
        self.assertIn(FAIL, r.statuses("restore.as_found"))

    def test_steam_appears(self):
        r = Run("steam_appears")
        self.assertEqual(r.code, 2)
        self.assertIn(ERROR, r.statuses("inputs.triggers"))     # the running injection group
        self.assertNotIn("inputs.pads", r.by_id)                 # it ended at once
        self.assertIn(SKIP, r.statuses("run.stopped"))           # --yes: no waiting
        self.assertIn(ERROR, r.statuses("restore.steam"))        # nothing written with Steam open
        self.assertOnly(r, {"inputs.triggers", "restore.steam"})

    def test_lizard_ignored(self):
        r = Run("lizard_ignored")
        self.assertEqual(r.code, 2)
        self.assertIn(FAIL, r.statuses("inputs.lizard_lock"))
        self.assertIn(SKIP, r.statuses("inputs.decoupled"))      # S7 refused it
        self.assertEqual(r.ran("reports."), [])                  # and every later injection
        self.assertEqual(r.ran("lizard."), [])
        self.assertOnly(r, {"inputs.lizard_lock"})

    def test_second_reader(self):
        r = Run("second_reader")
        self.assertEqual(r.code, 2)
        self.assertIn(ERROR, r.statuses("sole_reader"))
        self.assertEqual(r.ran("inputs."), [])

    def test_share_not_enforced(self):
        r = Run("share_not_enforced")
        self.assertEqual(r.code, 2)
        self.assertIn(ERROR, r.statuses("sole_reader"))
        self.assertEqual(r.ran("inputs."), [])

    def test_leak_event(self):
        r = Run("leak_event")
        self.assertEqual(r.code, 2)
        self.assertIn(ERROR, r.statuses("leak.detector"))
        self.assertEqual(r.ran("inputs."), [])
        self.assertEqual(r.runner.results.safety["leak_events"], 1)
        self.assertOnly(r, {"leak.detector"})

    def test_board_reset(self):
        r = Run("board_reset")
        self.assertEqual(r.code, 1)
        full = r.by_id["inputs.full"][0]
        self.assertEqual(full.status, ERROR)
        self.assertTrue(full.notes.startswith("board reset"), full.notes)
        self.assertIn("watchdog", full.notes)                    # the reset cause, from 0xA1
        self.assertIn(WARN, r.statuses("env.board_reset"))
        self.assertIn(PASS, r.statuses("battery.values"))        # the run went on
        self.assertIn(PASS, r.statuses("restore.as_found"))
        self.assertOnly(r, {"inputs.full"})

    def test_wrong_mode(self):
        r = Run("wrong_mode")
        self.assertEqual(r.code, 1)
        self.assertIn(FAIL, r.statuses("modes.to_wireless"))
        self.assertIn(PASS, r.statuses("restore.as_found"))
        self.assertOnly(r, {"modes.to_wireless"})

    def test_puck_reset(self):
        r = Run("puck_reset")
        self.assertEqual(r.code, 1)
        self.assertIn(ERROR, r.statuses("wireless.quality"))
        self.assertIn("proved again", r.by_id["wireless.quality"][0].notes)
        self.assertIn(PASS, r.statuses("wireless.reconnect"))    # the group went on
        self.assertOnly(r, {"wireless.quality"})

    def test_com_renumbered(self):
        r = Run("com_renumbered")
        self.assertEqual(r.code, 0, r.bad())

    def test_console_busy(self):
        r = Run("console_busy", start="R")
        self.assertEqual(r.code, 2)
        self.assertIn("close the program that holds the board's console",
                      r.by_id["env.devices"][0].notes)

    def test_console_busy_after_a_switch(self):
        # found wired, the console held once the board is wireless: the run stops, and the board
        # cannot be switched back without its console (exit 6, with what to do)
        r = Run("console_busy")
        self.assertEqual(r.code, 6)
        self.assertIn(ERROR, r.statuses("modes.to_wireless"))
        self.assertIn("replug", r.by_id["modes.return"][0].notes)
        self.assertOnly(r, {"modes.to_wireless", "modes.return"})

    def test_keepalive_race(self):
        r = Run("keepalive_race")
        self.assertEqual(r.code, 0, r.bad())
        self.assertGreater(r.backend._races, 0)
        self.assertEqual(r.backend.race_slipped, 0)              # the lock held every one off

    def test_keepalive_race_without_the_lock(self):
        """The same fault with the link lock taken out: the keep-alive's STATUS lands between a
        SET and its GET, and the runner sees an answer to another request."""

        class NoLock:
            def __enter__(self):
                return self

            def __exit__(self, *exc):
                return False

        backend = sim.SimBackend(start_mode="W", fault="keepalive_race")
        rig = board_mod.Rig(backend, mask_mod.Masker(), _opt("keepalive_race"))
        rig.discover()
        rig.arm()
        rig._lock = NoLock()
        with self.assertRaises(T.ProtocolError):
            for _ in range(5):
                rig.cnt_read()
        self.assertGreater(backend.race_slipped, 0)

    def test_answer_dropped(self):
        r = Run("answer_dropped")
        self.assertEqual(r.code, 0, r.bad())
        self.assertEqual(r.runner.results.safety["lease_lost"], 1)   # re-armed once


def _rig(start="W"):
    backend = sim.SimBackend(start_mode=start)
    rig = board_mod.Rig(backend, mask_mod.Masker(), _opt(None))
    rig.discover()
    rig.arm()
    if start == "W":
        rig.acquire_sole_reader()
    else:
        rig.ctx.sole_reader = True
    return backend, rig


class Fixes(unittest.TestCase):
    """The review findings of 2026-10-18, each against the simulator."""

    def _spy(self, rig, before_op=None, hook=None):
        ops = []
        orig = rig._channel

        def spy(op, args, rid, path):
            ops.append(op)
            if hook and op == before_op and ops.count(op) == 1:
                hook()
            return orig(op, args, rid, path)

        rig._channel = spy
        return ops

    def test_lease_found_ended_while_preparing(self):
        """safety-HW-5: S7's own STATUS finds the lease ended: armed again, and the canary of the
        new ARM runs before the INJECT goes out (not a safety refusal)."""
        backend, rig = _rig()
        rig.ready_to_inject()
        ops = self._spy(rig)
        backend.board.hooks.end_arming()            # the lease ran out unseen
        a, _n = rig.inject(R.make_state52(buttons=1))
        self.assertEqual(a["status"], C.ST_OK)
        self.assertEqual(rig.lease_lost, 1)
        i_arm = ops.index(C.OP_ARM)
        self.assertIn(C.OP_APP_SEND, ops[i_arm:ops.index(C.OP_INJECT)])

    def test_injection_not_resent_under_a_fresh_arm(self):
        """safety-HW-5: the INJECT itself meets the ended lease: it is not sent again under the
        new ARM until S7 (the canary first) ran again."""
        backend, rig = _rig()
        rig.ready_to_inject()
        ops = self._spy(rig, C.OP_INJECT, backend.board.hooks.end_arming)
        a, _n = rig.inject(R.make_state52(buttons=1))
        self.assertEqual(a["status"], C.ST_OK)
        self.assertEqual(ops.count(C.OP_INJECT), 2)
        first, second = [i for i, op in enumerate(ops) if op == C.OP_INJECT]
        between = ops[first:second]
        self.assertIn(C.OP_ARM, between)
        self.assertIn(C.OP_APP_SEND, between[between.index(C.OP_ARM):])   # the canary

    def test_exclusive_holds_the_keepalive_off(self):
        """correctness-keepalive-splits-vendor-set-get: a SET and its later GET under
        rig.exclusive(): the keep-alive (from its own thread) waits until the block ends."""
        _backend, rig = _rig()
        with rig.exclusive():
            rig.vendor_set(C.VC_ATTRIBUTES)
            t = threading.Thread(target=rig._race_tick, daemon=True)
            t.start()
            t.join(0.1)
            self.assertTrue(t.is_alive())             # held off by the link lock
            a = rig.vendor_get()
        t.join(2.0)
        self.assertFalse(t.is_alive())
        self.assertEqual((a.opcode, a.n), (C.VC_ATTRIBUTES, 30))

    def test_no_keep_off_in_the_lizard_group(self):
        """correctness-keepalive-lizard-off-in-lizard-group: the keep-alive sends no 0x81 while
        the lizard group's sequences run."""
        _backend, rig = _rig()
        rig.inject_active = True
        rig._last_lizard_off = -1e9
        rig.ctx.lizard_group = True
        n0 = rig.stats["usb_requests"]
        with rig._lock:
            rig._tick()
        self.assertEqual(rig.stats["usb_requests"] - n0, 2)          # the STATUS (SET + GET)
        rig.ctx.lizard_group = False
        n1 = rig.stats["usb_requests"]
        with rig._lock:
            rig._tick()
        self.assertEqual(rig.stats["usb_requests"] - n1, 3)          # and the 0x81 outside it

    def test_no_link_at_the_proof(self):
        """correctness-wireless-boot-no-link-wait: a board found wireless whose link does not
        connect: wireless.slot FAILs "no link" (no safety refusal), the run goes to the restore."""
        def setup(b):
            b.board.link_state = 3
            b._no_reconnect = True
        r = Run(start="R", setup=setup)
        self.assertEqual(r.code, 1, r.bad())
        slot = r.by_id["wireless.slot"][0]
        self.assertEqual(slot.status, FAIL)
        self.assertIn("no link", slot.notes)
        self.assertIn(SKIP, r.statuses("run.stopped"))
        self.assertIn(PASS, r.statuses("restore.as_found"))
        self.assertEqual([b for b in r.bad() if b[0] != "wireless.slot"], [])

    def test_link_back_before_the_proof(self):
        """The same with a link that comes back after one stop and start: proved (a WARN that
        it took the stop and start), and the run goes on."""
        r = Run(start="R", setup=lambda b: setattr(b.board, "link_state", 3))
        self.assertEqual(r.code, 0, r.bad())
        slot = r.by_id["wireless.slot"][0]
        self.assertEqual(slot.status, WARN)
        self.assertIn("stop and start", slot.notes)
        self.assertIn(PASS, r.statuses("inputs.buttons"))

    def test_restore_after_steam(self):
        """safety-HW-8, safety-HW-7: after the Steam phase the board ends with the start
        snapshot's settings, and the restore's through-puck 0x83 goes out before its DISARM."""
        for extra in ({}, {"through_puck": True}):
            r = Run(start="R", wireless_only=True, with_steam=True, steam_control=True, **extra)
            self.assertEqual(r.code, 0, (extra, r.bad()))
            want = r.runner.rig.start_snapshot["settings"]
            self.assertEqual([n for n in range(86) if r.backend.board.settings[n] != want[n]], [],
                             extra)
            self.assertIn(PASS, r.statuses("restore.disarm"), extra)
            self.assertEqual(r.runner.results.left_as_found.get("settings"), "equal")

    def test_restore_runs_whatever_the_selection(self):
        r = Run(tests={"inputs.buttons"})
        self.assertEqual(r.code, 0, r.bad())
        for tid in ("restore.settings", "restore.disarm", "restore.as_found"):
            self.assertIn(PASS, r.statuses(tid), tid)

    def test_steam_log_keeps_no_bond_and_only_the_board(self):
        """safety-HW-3: the bond lines are kept without their IDs (decimal in the simulator's
        synthetic log), the empty one as "empty", and a line about another controller is
        dropped."""
        r = Run(with_steam=True, steam_control=True)
        self.assertEqual(r.code, 0, r.bad())
        log = r.runner.results.steam_log
        self.assertTrue(any(ln.endswith(": <bond>") for ln in log), log)
        self.assertTrue(any(ln.endswith(": empty") for ln in log), log)
        whole = "\n".join(log)
        for secret in ("286331153", "572662306", "11111111", "FAKEPUCK", "NOTOSC000SIM1",
                       "<serial>"):
            self.assertNotIn(secret, whole)

    def test_leak_detector_off_refuses_lizard(self):
        """safety-HW-1: a path where the detector watches nothing refuses --lizard (exit 2)."""
        r = Run(lizard=True, setup=lambda b: setattr(b, "leak_watching", lambda: False))
        self.assertEqual(r.code, 2)
        self.assertEqual(r.ran("lizard."), [])
        w = [t for t in r.by_id["lizard.keyboard"] if t.path == "W"][0]
        self.assertEqual(w.status, ERROR)
        self.assertIn("--lizard refused", w.notes)
        # the R phase comes after that ERROR: R4 keeps the group off there as well
        self.assertEqual({t.status for t in r.by_id["lizard.keyboard"] if t.path == "R"}, {SKIP})

    def test_lizard_needs_lizard_lock(self):
        """safety-HW-4, R4: without inputs.lizard_lock passing on the path, no lizard group."""
        r = Run(lizard=True, groups={"hooks", "lizard"})
        self.assertEqual(r.code, 0, r.bad())
        self.assertEqual(r.ran("lizard."), [])
        self.assertIn("inputs.lizard_lock", r.by_id["lizard.keyboard"][0].notes)

    def test_lizard_after_an_earlier_failure(self):
        r = Run("seq_gap", lizard=True)
        self.assertEqual(r.code, 1)
        self.assertEqual(r.ran("lizard."), [])
        self.assertIn("an earlier test failed", r.by_id["lizard.keyboard"][0].notes)

    def test_lizard_lock_second_half(self):
        """safety-HW-4: the firmware refuses the INJECT while lizard mode is on but does not
        stop a running one when lizard mode comes on: FAIL, injection stopped and blocked for the
        rest of the run, exit 2."""
        original = simfw.Hooks.input

        def no_stop(hooks, lizard_on):
            return original(hooks, False)

        simfw.Hooks.input = no_stop
        try:
            r = Run()
        finally:
            simfw.Hooks.input = original
        self.assertEqual(r.code, 2)
        self.assertIn(FAIL, r.statuses("inputs.lizard_lock"))
        self.assertIn(SKIP, r.statuses("inputs.decoupled"))
        self.assertEqual(r.ran("reports."), [])
        self.assertEqual([b for b in r.bad() if b[0] != "inputs.lizard_lock"], [])


class FwEmu(unittest.TestCase):
    """The same runner against the firmware emulator (the library's own code), when
    tests/host/run.ps1 built it and set OPENSC2_FWEMU."""

    def test_fwemu_run(self):
        exe = os.environ.get("OPENSC2_FWEMU")
        if not exe or not os.path.isfile(exe):
            self.skipTest("OPENSC2_FWEMU is not set (tests/host/run.ps1 sets it)")
        from hwtest import fwemu
        for start, extra in (("W", {"lizard": True}), ("R", {})):
            backend = fwemu.FwEmuBackend(exe, start_mode=start)
            try:
                run = runner_mod.Runner(backend, _opt(None, **extra), mask_mod.Masker())
                code = run.run()
            finally:
                backend.close()
            bad = [(t.id, t.status, t.notes) for g in run.results.groups for t in g["tests"]
                   if t.status in (FAIL, ERROR)]
            self.assertEqual((code, bad), (C.EXIT_OK, []), start)
            # both phases ran against the library's code, not just one
            passed = {t.label() for g in run.results.groups for t in g["tests"]
                      if t.status == PASS}
            for label in ("inputs.buttons@W", "inputs.buttons@R", "battery.values@R",
                          "outputs.rumble@R", "settings.watchdog@W", "hooks.lease@R",
                          "restore.as_found@" + start):
                self.assertIn(label, passed, start)
            if extra.get("lizard"):
                self.assertIn("lizard.no_leak@W", passed)


if __name__ == "__main__":
    unittest.main()
