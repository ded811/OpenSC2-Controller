# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# runner.py: the order of a run and its exit code (docs/testing.md). It discovers and classifies
# the devices, then runs the phases for the path the board starts on: the wired phase (W), the
# switches, the wireless phase (R), the opt-in Steam phases, and always the restore at the end on
# the path the board started on. A phase starts with hooks.present (which arms), the sole-reader
# handles (S12), the application's baseline and, in the first phase, the start snapshot; then its
# groups. A failed guard canary, another reader or a leak stop all injection (exit 2); a board not
# left as found is exit 6.
#
# What stops a run early: the leak detector, Steam starting with --yes, the console
# held by another program, --fail-fast, --max-minutes, a second unplanned reset or a board that does
# not come back. The remaining groups are skipped with the reason; the switch back and the restore
# still run whenever the board can be reached safely.
import datetime
import platform
import sys

from . import config as C
from . import results as results_mod
from . import safety
from .board import BoardGone, DeviceError, NoLink, Rig, RigError
from .groups import base as gbase
from .groups import (battery, cmd, desc, flash, hooks, inputs, lizard, modes, outputs, puck,
                     reports, restore, settings, steam, stream, system, wireless)

CATALOG = [
    ("hooks", hooks), ("desc", desc), ("cmd", cmd), ("inputs", inputs), ("battery", battery),
    ("stream", stream), ("reports", reports), ("outputs", outputs), ("settings", settings),
    ("flash", flash), ("lizard", lizard), ("wireless", wireless), ("puck", puck),
    ("steam", steam), ("modes", modes), ("restore", restore), ("system", system),
]
MODULES = dict(CATALOG)
INJECTION_GROUPS = {"inputs", "reports", "lizard", "wireless", "system"}
STEAM_CLOSED_GROUPS = {"hooks", "cmd", "inputs", "battery", "reports", "outputs", "settings",
                       "flash", "lizard", "modes", "wireless", "puck", "desc", "stream",
                       "system"}
W_GROUPS = ("hooks", "desc", "cmd", "inputs", "battery", "stream", "reports", "outputs",
            "settings", "flash", "lizard", "system")
R_GROUPS = ("hooks", "inputs", "battery", "stream", "reports", "outputs", "settings", "cmd",
            "wireless", "flash", "puck", "lizard", "system")
# the R groups that read the board's reports from its slot on the puck (proved by wireless.slot)
SLOT_GROUPS = {"inputs", "battery", "stream", "reports", "wireless", "lizard", "puck"}
STEAM_WAIT_S = 600.0                 # how long an interactive run waits for Steam to close


def list_tests():
    rows = []
    for _name, mod in CATALOG:
        for tid, paths, needs in getattr(mod, "TESTS", []):
            rows.append((tid, paths, needs))
    return rows


class Runner:
    def __init__(self, backend, options, masker):
        self.backend = backend
        self.opt = options
        self.mask = masker
        self.rig = Rig(backend, masker, options)
        self.results = results_mod.Results(masker, quiet=options.get("quiet", False))
        self.rig.log = self.results.log
        self.exit_base = C.EXIT_OK
        self.exit6 = False
        self.inject_blocked = False
        self.aborted = False          # no more groups and no switch: only the restore
        self.stop_reason = None       # no more groups; the switch back and the restore run
        self.resets = 0
        self.host = {"os": platform.platform(), "python": platform.python_version(),
                     "elevated": False}
        self.board_info = {}
        self.puck_info = {}
        self.steam_info = {"seen_open": False, "controlled": options.get("steam_control", False)}
        self._sel_groups = options.get("groups")
        self._sel_tests = options.get("tests")
        self._snapshot_taken = False
        self._leak_handled = False
        self._t0 = None
        self.board_lost = False       # the board did not come back: nothing more is written
        self.start_mode = None
        self._steam_skips_filed = False

    # ---- selection ----
    def _group_on(self, name):
        if self._sel_groups and name not in self._sel_groups:
            return False
        if self._sel_tests:
            ids = [t[0] for t in getattr(MODULES[name], "TESTS", [])]
            return any(t in ids for t in self._sel_tests)
        return True

    def _wants_path(self, path):
        """Does the selection need this path at all?"""
        if not self._sel_groups and not self._sel_tests:
            return True
        for name, mod in CATALOG:
            if not self._group_on(name):
                continue
            for tid, paths, _n in getattr(mod, "TESTS", []):
                if self._sel_tests and tid not in self._sel_tests:
                    continue
                if not paths or path in paths or set(paths) & set("PS"):
                    return True
        return False

    def _ctx(self, name, path):
        return gbase.GroupCtx(self.rig, self.results, self.opt,
                              set(self._sel_tests) if self._sel_tests else None, path,
                              {t[0]: t[1] for t in getattr(MODULES.get(name), "TESTS", [])},
                              runner=self)

    def _go(self):
        return not self.aborted and self.stop_reason is None

    def _stop(self, why, path=None):
        """No more groups in this run (the switch back and the restore still run)."""
        if self.stop_reason is not None or self.aborted:
            return
        self.stop_reason = why
        self.results.add(results_mod.Result("run.stopped", "env", path or self.rig.path or "")
                         .set(gbase.SKIP, "the remaining groups did not run: %s" % why))

    def _absorb(self, ctx):
        if ctx.precondition:
            self.exit_base = max(self.exit_base, ctx.precondition)
        if ctx.not_as_found:
            self.exit6 = True
        if getattr(ctx, "environment", None):
            self.exit_base = max(self.exit_base, C.EXIT_ENVIRONMENT)
            if not self.aborted:
                self.aborted = True
                self.results.add(results_mod.Result("run.stopped", "env", self.rig.path or "")
                                 .set(gbase.SKIP, "the run stopped: %s" % ctx.environment))
            ctx.board_reset = False
        if ctx.board_reset:
            ctx.board_reset = False
            self._board_reset(rearm=not getattr(ctx, "in_switch", False))

    def _out_of_time(self):
        limit = self.opt.get("max_minutes")
        if limit and self._t0 is not None and \
                self.backend.clock.monotonic() - self._t0 >= limit * 60.0:
            self._stop("--max-minutes %s reached" % limit)
            return True
        return False

    # ---- one group ----
    def _run_group(self, name, path, fn=None, required=False):
        """One group (or one of its functions); `required`: a step the selection needs even when
        its group is not selected (the slot proof)."""
        if not (required or self._group_on(name)) or not self._go() or self._out_of_time():
            return
        if self.rig.path != path:
            return                        # the board is on the other path (after a reset)
        ctx = self._ctx(name, path)
        if name in STEAM_CLOSED_GROUPS and self._steam_guard(name, path):
            return
        if name in INJECTION_GROUPS and self.inject_blocked:
            for tid, _paths, _n in getattr(MODULES[name], "TESTS", []):
                if ctx.want(tid):
                    ctx.record(name, tid, gbase.SKIP, "no injection in this run: %s" % (
                        self.rig.injection_blocked or "the sole reader is not held (S12)"))
            return
        lost_before = self.rig.lease_lost
        self.rig.steam_closed_required = name in STEAM_CLOSED_GROUPS
        try:
            self._call(ctx, name, fn or MODULES[name].run)
        finally:
            self.rig.steam_closed_required = False
        if self.rig.lease_lost - lost_before >= 2:
            ctx.record(name, name + ".lease", gbase.ERROR, "lease lost %d times in this group "
                       "(the keep-alive is being starved)" % (self.rig.lease_lost - lost_before))
        self._absorb(ctx)
        self._after_group(name, path)

    def _call(self, ctx, name, fn, *args):
        """Run a group function; what ends a group is filed once and turned into
        the context's flags for _absorb()."""
        try:
            fn(ctx, *args)
        except RigError as e:
            ctx.record(name, name + ".setup", gbase.ERROR, "precondition: %s" % e)
            ctx.precondition = C.EXIT_PRECONDITION
        except DeviceError as e:
            if not getattr(e, "recorded", False):
                ctx.record(name, name + ".setup", gbase.ERROR, "device: %s" % e)
            ctx.environment = str(e)
        except BoardGone as e:
            if not getattr(e, "recorded", False):
                ctx.record(name, name + ".setup", gbase.ERROR, "board reset: %s" % e)
            ctx.board_reset = True
        except safety.SteamOpened as e:
            if not getattr(e, "recorded", False):
                ctx.record(name, name + ".setup", gbase.ERROR, "safety/precondition: %s" % e)
            ctx.precondition = C.EXIT_PRECONDITION
            self.rig.ctx.steam_running = True
        except safety.LeakFired as e:
            if not getattr(e, "recorded", False):
                ctx.record(name, name + ".setup", gbase.ERROR, "safety/precondition: %s" % e)
            ctx.precondition = C.EXIT_PRECONDITION
        except safety.InjectionBlocked as e:
            ctx.record(name, name + ".setup", gbase.SKIP, "no injection in this run: %s" % e)
        except safety.SafetyError as e:
            ctx.record(name, name + ".setup", gbase.ERROR, "safety/precondition: %s" % e)
            ctx.precondition = C.EXIT_PRECONDITION
        except NoLink as e:               # the radio link is down, no safety refusal
            if not getattr(e, "recorded", False):
                ctx.record(name, name + ".setup", gbase.FAIL, gbase.describe(e))
        except Exception as e:            # a bug or a device error outside a test: the group ends
            ctx.record(name, name + ".setup", gbase.ERROR, gbase.describe(e))

    def _after_group(self, name, path):
        rig = self.rig
        if (rig.leak_seen or (rig.ctx.armed and self.backend.leak_fired())) and \
                not self._leak_handled:
            # S13: a leak while armed stops injection at once and ends the run (restore only)
            self._leak_handled = True
            if not rig.leak_seen:
                with rig._lock:
                    rig.leak_stop()
            rig._stop_keepalive()
            self.inject_blocked = True
            self.exit_base = max(self.exit_base, C.EXIT_PRECONDITION)
            self.results.safety["leak_events"] += 1
            self.results.add(results_mod.Result("leak.detector", "env", path).set(
                gbase.ERROR, "an input event from the board's mouse or keyboard collection while "
                             "armed: INJECT_OFF, DISARM, no more injection (exit 2)"))
            self._stop("the leak detector fired (S13)", path)
        if rig.injection_blocked:
            self.inject_blocked = True
        if rig.link_stuck and path == "R":
            self._stop("no link: the radio link did not come back after one stop and start (a "
                       "stuck link); what is left of the wireless phase needs it", path)
        if rig.ctx.steam_running or self.backend.steam_running():
            self._steam_opened(name, path)
        if self.opt.get("fail_fast"):
            c = self.results.counts()
            if c[gbase.FAIL] or c[gbase.ERROR]:
                self._stop("--fail-fast: a test failed", path)

    def _board_reset(self, rearm=True):
        """An unplanned reset (or a board that did not come back from a switch). Look for
        it for 30 s, read why it reset, re-arm and go on with the next group on whatever path it
        is on (a switch leaves the next phase to arm). A second one in a run: no more groups,
        only the restore."""
        rig = self.rig
        self.resets += 1
        try:
            note = rig.recover()
        except Exception as e:
            self.aborted = True
            self.board_lost = True
            self.exit_base = max(self.exit_base, C.EXIT_ENVIRONMENT)
            self.results.add(results_mod.Result("env.board_reset", "env", rig.path or "").set(
                gbase.ERROR, "the board did not come back (%s): unplug and replug it" % e))
            return
        for g in reversed(self.results.groups):
            hit = [t for t in g["tests"] if t.status == gbase.ERROR and
                   t.notes.startswith("board reset")]
            if hit:
                hit[-1].notes += "; %s" % note
                break
        self.results.add(results_mod.Result("env.board_reset", "env", rig.path).set(
            gbase.WARN, "the board reset unplanned and came back %s; %s" % (
                "wired" if rig.path == "W" else "wireless", note)))
        if self.resets > 1:
            self.aborted = True
            self.results.add(results_mod.Result("run.stopped", "env", rig.path).set(
                gbase.SKIP, "a second unplanned reset: no more groups, only the restore"))
            return
        if rearm:
            self._phase_start(rig.path)

    def _wait_steam_closed(self):
        """Steam is open where it must not be: with --yes (or no terminal) the run stops; else the
        person running the suite is asked to close it, and the run waits (polling every 2 s)."""
        interactive = not self.opt.get("yes") and not self.opt.get("dry_run") and \
            sys.stdin is not None and sys.stdin.isatty()
        if not interactive:
            return False
        print("Steam is running: exit Steam (tray icon, Exit); the run goes on when it is closed "
              "(Ctrl-C stops it)")
        end = self.backend.clock.monotonic() + STEAM_WAIT_S
        while self.backend.clock.monotonic() < end:
            if not self.backend.steam_running():
                self.rig.ctx.steam_running = False
                return True
            self.backend.clock.sleep(2.0)
        return False

    def _wait_steam_open(self):
        """--with-steam without --steam-control: the person running the suite starts Steam when
        asked (an interactive run only)."""
        interactive = not self.opt.get("yes") and not self.opt.get("dry_run") and \
            sys.stdin is not None and sys.stdin.isatty()
        if not interactive:
            return False
        print("Start Steam now (and leave it on its library page); the run goes on when it is "
              "running (Ctrl-C stops it)")
        end = self.backend.clock.monotonic() + STEAM_WAIT_S
        while self.backend.clock.monotonic() < end:
            if self.backend.steam_running():
                self.backend.clock.sleep(20.0)        # let it find the controller
                return True
            self.backend.clock.sleep(2.0)
        return False

    def _steam_opened(self, name, path):
        """S8, Steam started during a group: the group already stopped its injection (the rig
        checks before every injection); the run waits for Steam to close or stops."""
        self.steam_info["seen_open"] = True
        rig = self.rig
        if rig.ctx.armed:
            for fn in (rig.seq_stop, rig.inject_off):
                try:
                    fn()
                except Exception:
                    pass
        self.exit_base = max(self.exit_base, C.EXIT_PRECONDITION)
        if not self._wait_steam_closed():
            rig._stop_keepalive()      # nothing more to the board: the lease ends arming
            self._stop("Steam is running (S8): exit Steam and run again", path)

    def _steam_guard(self, name, path):
        """S8: a group that needs Steam closed does not run while Steam is open."""
        if self.backend.steam_running():
            self.steam_info["seen_open"] = True
            self.rig.ctx.steam_running = True
            if self._wait_steam_closed():
                return False
            self.results.add(results_mod.Result(name + ".steam", name, path).set(
                gbase.ERROR, "Steam is running: the group did not run (S8); exit Steam (tray "
                             "icon, Exit) and run again"))
            self._steam_opened(name, path)
            return True
        return False

    # ---- phases ----
    def _phase_start(self, path):
        """hooks.present (which arms), the sole reader, the baseline, the snapshot."""
        rig = self.rig
        ctx = self._ctx("hooks", path)
        if not self.backend.steam_running():
            try:
                if path == "W" or not rig.models("puck"):
                    rig.acquire_sole_reader()
                else:
                    rig.ctx.sole_reader = True   # the slots are held when they are opened
                self.results.safety["sole_reader"] = True
            except RigError as e:
                self.results.add(results_mod.Result("sole_reader", "env", path).set(
                    gbase.ERROR, str(e)))
                self.exit_base = max(self.exit_base, C.EXIT_PRECONDITION)
                self.inject_blocked = True
                self.results.safety["sole_reader"] = False
        notes = []
        if rig.models("stream") and path == "W":
            notes = rig.record_baseline()      # R: after the slot is proved (below)
        result = {}
        rig.steam_closed_required = True
        try:
            self._call(ctx, "hooks", lambda c: result.update(have=hooks.present(c)))
        finally:
            rig.steam_closed_required = False
        have = result.get("have", False)
        self._absorb(ctx)
        for n in notes:
            self.results.add(results_mod.Result("env.baseline", "env", path).set(gbase.WARN, n))
        if not have or not self._go():
            return have
        if rig.caps:
            self.board_info["git_revision"] = rig.caps.get("git_revision")
            self.board_info["caps"] = {"build_flags": "0x%04x" % rig.caps["build_flags"],
                                       "policy": rig.caps.get("policy"),
                                       "ring_size": rig.caps.get("ring_size")}
        if not self._snapshot_taken:
            sctx = self._ctx("restore", path)
            self._call(sctx, "restore", _snapshot)
            self._snapshot_taken = True
            self._absorb(sctx)
            if not self._go() or rig.path != path:
                return self._go()
        if path == "R" and rig.models("puck") and rig.puck_present and self._needs_slot():
            self._run_group("wireless", path, wireless.slot, required=True)
            if rig.slot_proven_at is not None and rig.models("stream") and self._go():
                for n in rig.record_baseline():      # INJECT_OFF: the application's state
                    self.results.add(results_mod.Result("env.baseline", "env", path).set(
                        gbase.WARN, n))
        return True

    def _phase(self, path):
        if not self._wants_path(path) or not self._go():
            return
        if not self._phase_start(path):
            # no hooks: only what needs none (W: the descriptors and the stream read as it
            # comes; R: nothing, since the board's slot on the puck is proved by an injection)
            if path == "W":
                self._run_group("desc", path)
                self._run_group("stream", path, stream.passive)
            elif self._go():
                self._skip_phase(path, R_GROUPS[0], "no test hooks: the board's slot on the puck "
                                 "cannot be proved without them")
            return
        if path == "R" and self.rig.models("puck") and not self.rig.puck_present:
            # without the puck there is no link: only what needs none runs
            why = ("no puck: plug in Valve's puck (the wireless phase reads the board through it)")
            self._skip_phase(path, R_GROUPS[1], why)
            return
        for name in (W_GROUPS if path == "W" else R_GROUPS):
            if path == "R" and name in SLOT_GROUPS and self.rig.slot_proven_at is None and \
                    self.rig.models("puck") and self._group_on(name):
                why = "the board's slot on the puck is not proved"
                if self.rig.injection_blocked or self.inject_blocked:
                    why += " (no injection in this run: %s)" % (
                        self.rig.injection_blocked or "the sole reader is not held (S12)")
                self.results.add(results_mod.Result(name + ".phase", name, path).set(
                    gbase.SKIP, why))
                continue
            if self.rig.path != path:
                self._skip_phase(path, name, "the board came back %s after a reset" % (
                    "wired" if self.rig.path == "W" else "wireless"))
                return
            if name == "puck" and path != "R":
                continue
            if name == "cmd" and path == "R":
                self._run_group(name, path, _cmd_r)
                continue
            self._run_group(name, path)

    def _needs_slot(self):
        """Does the selection run an R group that reads the board's slot?"""
        return any(self._group_on(name) for name in SLOT_GROUPS)

    def _skip_phase(self, path, from_name, why):
        names = W_GROUPS if path == "W" else R_GROUPS
        for name in names[names.index(from_name):]:
            if self._group_on(name):
                self.results.add(results_mod.Result(name + ".phase", name, path).set(
                    gbase.SKIP, why))

    def _steam_phase(self, path):
        if not self.opt.get("with_steam"):
            if not self._steam_skips_filed and self._group_on("steam"):
                self._steam_skips_filed = True
                steam.run(self._ctx("steam", path))    # files the opt-in SKIPs once
            return
        if not self._go():
            return
        rig = self.rig
        share = getattr(self.backend, "share_for_steam", None)
        if rig.ctx.armed:
            # arming ends now, not with the lease in the middle of Steam's session: the firmware
            # writes its snapshot back before Steam starts and sets its own settings [R7]
            try:
                rig.disarm()
                rig.sleep(0.06)
            except Exception:
                pass
        rig._stop_keepalive()
        if share:
            share(True)                    # [R12] Steam must be able to open the collection
        rig.ctx.sole_reader = False
        if not self.backend.steam_running():
            if self.opt.get("steam_control") and hasattr(self.backend, "steam_start"):
                self.backend.steam_start()
                self.steam_info["controlled"] = True
            elif not self._wait_steam_open():
                self.results.add(results_mod.Result("steam.phase", "steam", path).set(
                    gbase.SKIP, "Steam is not running (start it when asked, or use "
                                "--steam-control)"))
                self._steam_done(path, share)
                return
        self.steam_info["seen_open"] = True
        ctx = self._ctx("steam", path)
        if path == "R":
            self._call(ctx, "steam", steam.wireless)
        else:
            self._call(ctx, "steam", steam.wired)
            if self._group_on("desc") and self._go():
                self._call(ctx, "desc", MODULES["desc"].run)
        self._absorb(ctx)
        if self.opt.get("steam_control") and hasattr(self.backend, "steam_stop"):
            self.backend.steam_stop()
        if self.backend.steam_running() and not self._wait_steam_closed():
            self.exit_base = max(self.exit_base, C.EXIT_PRECONDITION)
            self.rig._stop_keepalive()
            self._stop("Steam is still open after the steam group: close it (or use "
                       "--steam-control) and run again", path)
            return
        self.rig.ctx.steam_running = False
        self._steam_done(path, share)
        # Steam's last 0x81 leaves the watchdog pending; wait it out before going on
        if self.rig.hooks and self._go() and self.rig.path == path:
            wctx = self._ctx("steam", path)
            self._call(wctx, "steam", _steam_watchdog)
            self._absorb(wctx)

    def _steam_done(self, path, share):
        """Steam closed again: the sole-reader handles taken back (S12)."""
        rig = self.rig
        if share:
            share(False)
        try:
            if path == "W" or not rig.models("puck"):
                rig.acquire_sole_reader()
            else:
                rig.ctx.sole_reader = True
        except RigError as e:
            self.results.add(results_mod.Result("sole_reader", "env", path).set(
                gbase.ERROR, str(e)))
            self.exit_base = max(self.exit_base, C.EXIT_PRECONDITION)
            self.inject_blocked = True

    def _switch(self, fn, name):
        """A planned switch (modes.*): the check files its own result; a console that cannot be
        opened afterwards, or a board that does not come back, stops the run."""
        if not self._go():
            return
        if self.backend.steam_running():
            self._steam_guard("modes", self.rig.path)
            return
        ctx = self._ctx("modes", self.rig.path)
        ctx.in_switch = True
        self.rig.steam_closed_required = True
        try:
            self._call(ctx, "modes", fn)
        finally:
            self.rig.steam_closed_required = False
        self._absorb(ctx)
        if self.rig.path == "R" and self.rig.console is None and self._go():
            self._stop("the board is wireless and its console could not be opened: close the "
                       "program that holds it (a terminal, a serial monitor) and run again")

    # ---- the whole run ----
    def run(self):
        self.opt["started"] = datetime.datetime.now().isoformat(timespec="seconds")
        self.opt["suite_version"] = C.SUITE_VERSION
        self._t0 = self.backend.clock.monotonic()
        rig = self.rig
        try:
            d = rig.discover()
        except DeviceError as e:
            self.results.add(results_mod.Result("env.devices", "env", "").set(gbase.ERROR,
                                                                               str(e)))
            return C.EXIT_ENVIRONMENT
        except RigError as e:
            self.results.add(results_mod.Result("env.devices", "env", "").set(gbase.ERROR,
                                                                               str(e)))
            return C.EXIT_PRECONDITION
        self.board_info = {"serial": self.mask.text(rig.unit_serial or ""),
                           "mode_at_start": rig.path}
        self.puck_info = {"present": bool(d.get("puck_slots")), "slot": None}
        self.results.add(results_mod.Result("env.devices", "env", rig.path).set(
            gbase.PASS, "the board (%s), %d puck slot(s)" % (
                "wired" if rig.path == "W" else "wireless", len(d.get("puck_slots", [])))))
        if self.backend.steam_running():
            self.steam_info["seen_open"] = True
            if self.opt.get("steam_control") and hasattr(self.backend, "steam_stop"):
                # --steam-control: the runner closes Steam now and starts it again for the steam
                # phase, as the docs promise (the groups before it need Steam closed, S8)
                self.backend.steam_stop()
            if self.backend.steam_running() and not self.opt.get("with_steam") and \
                    not self._wait_steam_closed():
                self.results.add(results_mod.Result("env.steam", "env", rig.path).set(
                    gbase.ERROR, "Steam is running: exit Steam (tray icon, Exit) and run again "
                                 "(S8)"))
                return C.EXIT_PRECONDITION
        start = getattr(self.backend, "start_leak_detector", None)
        if start:
            ok = start()
            if not ok and self.opt.get("lizard"):
                self.results.add(results_mod.Result("env.leak_detector", "env", rig.path).set(
                    gbase.ERROR, "the leak detector did not start: --lizard refused"))
                self.opt["lizard"] = False
                self.exit_base = max(self.exit_base, C.EXIT_PRECONDITION)
            elif not ok:
                # S13 is a backstop to the firmware's two layers: without it the run goes on,
                # but --lizard (one layer only) would be refused
                self.results.add(results_mod.Result("env.leak_detector", "env", rig.path).set(
                    gbase.WARN, "the leak detector did not start (no raw-input window)"))
        start_mode = self.start_mode = rig.path
        only_w = self.opt.get("wired_only")
        only_r = self.opt.get("wireless_only")
        try:
            if start_mode == "W":
                if not only_r:
                    self._phase("W")
                if not only_w and self._go() and self._wants_path("R"):
                    if not only_r and (self._group_on("modes") or not self._sel_groups):
                        self._switch(modes.plain_reboot, "modes.plain_reboot")
                    if rig.path == "W":
                        self._switch(modes.to_wireless, "modes.to_wireless")
                    if rig.path == "R" and self._go():
                        self._run_group("modes", "R", modes.session_log)
                        self._phase("R")
                        self._steam_phase("R")
                        if self._go():
                            self._switch(modes.warm_wireless, "modes.warm_wireless")
                    elif self._go():
                        self._skip_r("the board came back wired")
            else:
                if not only_w:
                    self._phase("R")
                    self._steam_phase("R")
                if not only_r and self._go() and self._wants_path("W"):
                    self._switch(modes.to_wired, "modes.to_wired")
                    if rig.path == "W" and self._go():
                        self._run_group("modes", "W", modes.identity)
                        self._phase("W")
                        self._steam_phase("W")
            if start_mode == "W" and rig.path == "R" and self._go():
                self._switch(modes.to_wired, "modes.to_wired")
                if rig.path == "W" and self._go():
                    self._run_group("modes", "W", modes.identity)
                    self._steam_phase("W")
        finally:
            self._finish(start_mode)
        code = self.results.exit_code(self.exit_base)
        if self.exit6:
            code = C.EXIT_NOT_AS_FOUND
        return code

    def _skip_r(self, why):
        for name in R_GROUPS:
            if self._group_on(name):
                self.results.add(results_mod.Result(name + ".phase", name, "R").set(
                    gbase.SKIP, why))

    def _finish(self, start_mode):
        """modes.return and restore, always: on the path the board started on. Nothing is
        written while Steam is open (the lease ends arming within 15 s and the firmware writes its
        own snapshot back), nor to a board that did not come back."""
        rig = self.rig
        self.results.safety["puck_writes"] = rig.puck_writes()
        self.results.safety["lease_lost"] = rig.lease_lost
        try:
            self._finish_board(start_mode)
        finally:
            self.results.safety["puck_writes"] = rig.puck_writes()
            self.results.safety["lease_lost"] = rig.lease_lost
            ignored = getattr(self.backend, "leak_ignored", None)
            if ignored:
                # watched-collection events during the runner's own link drops (the puck's empty
                # keyboard and mouse reports): not leaks, counted to show they happen
                self.results.safety["leak_ignored"] = ignored()
            lost = rig.console_input_lost + (rig.console.input_lost if rig.console else 0)
            if lost:
                self.results.add(results_mod.Result("env.console_input", "env", "").set(
                    gbase.WARN, "the board's shell dropped console input %d time(s) (\"RX ring "
                                "buffer full\"); %d request(s) were sent again: raise "
                                "CONFIG_SHELL_BACKEND_SERIAL_RX_RING_BUFFER_SIZE"
                    % (lost, rig.console_retries)))
            if rig.transcript:
                self.results.console_lines_raw = list(rig.transcript)

    def _finish_board(self, start_mode):
        rig = self.rig
        if self.backend.steam_running():
            rig._stop_keepalive()
            self.results.add(results_mod.Result("restore.steam", "restore", rig.path or "").set(
                gbase.ERROR, "Steam is running: nothing written to the board; arming ends with "
                             "the lease (15 s) and the board writes its settings back itself; "
                             "close Steam and run again to check it"))
            self.exit_base = max(self.exit_base, C.EXIT_PRECONDITION)
            if rig.path != start_mode:
                self.exit6 = True
            return
        if self.board_lost:
            self.results.add(results_mod.Result("restore.skipped", "restore", "").set(
                gbase.ERROR, "the board was not found again: nothing written; unplug and replug "
                             "it (the firmware already restored its settings when the lease ran "
                             "out)"))
            self.exit6 = True
            return
        ctx = self._ctx("modes", rig.path)
        if rig.path != start_mode:
            if rig.path == "R" and rig.console is None:
                try:
                    rig.reopen_console()
                except Exception:
                    pass
            if rig.path == "R" and rig.console is None:
                ctx.record("modes", "modes.return", gbase.ERROR,
                           "the board is wireless and its console cannot be opened: close the "
                           "program that holds it and run again, or replug the board (it starts "
                           "wired)")
                self.exit6 = True
                return
            try:
                modes.mode_return(ctx, start_mode)
            except Exception as e:
                ctx.record("modes", "modes.return", gbase.ERROR, "could not switch back: %s" % e)
                self.exit6 = True
        ctx.board_reset = False           # no recovery at the end: the restore reports it
        self._absorb(ctx)
        rctx = self._ctx("restore", rig.path)
        rctx.selected = None              # the restore always runs whole, whatever --test
        try:
            restore.run(rctx, start_mode)
        except Exception as e:
            rctx.record("restore", "restore.error", gbase.ERROR, "restore failed: %s" % e)
            self.exit6 = True
        rctx.board_reset = False
        self._absorb(rctx)
        if rig.path != start_mode:
            self.exit6 = True


def _steam_watchdog(ctx):
    rig = ctx.rig
    # [R7] armed WITHOUT the restore flag after Steam (also for the re-arms after a lost lease,
    # which use the last flag): a snapshot now would hold Steam's settings, and the DISARM of the
    # restore would write them back over the start snapshot. The restore writes that itself.
    rig.arm(restore=False)
    notes = []
    restore.wait_watchdog(rig, notes)
    for n in notes:
        ctx.results.snapshot_notes.append("after Steam: " + n)


def _snapshot(ctx):
    with ctx.check("restore", "restore.snapshot") as c:
        snap = restore.take_snapshot(ctx)
        c.true("the board as found was recorded", bool(snap))
        for n in getattr(ctx.results, "snapshot_notes", []):
            c.note(n)


def _cmd_r(ctx):
    """The one cmd row that runs wireless."""
    sel = ctx.selected
    if sel is not None and "cmd.no_link" not in sel:
        return
    saved = ctx.selected
    ctx.selected = {"cmd.no_link"}
    try:
        cmd.run(ctx)
    finally:
        ctx.selected = saved
