# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# base.py: what the group modules share. A group exposes TESTS (for --list and the selection) and
# run(ctx). `with ctx.check(group, id) as c:` times one test and files its Result: the test adds
# what it compared to `c` (c.eq, c.true, c.warn) and the verdict follows from that, so a test that
# checked nothing cannot pass; a test the backend cannot show is SKIP with the reason (c.skip). An
# unexpected exception is an ERROR, a safety refusal a precondition (exit 2).
import time

from .. import config as C
from .. import results as results_mod
from .. import safety
from ..board import BoardGone, DeviceError, LeaseLost, NoLink, Refused, RigError
from ..proto import testproto as T
from ..transport import ConsoleError, Stall

PASS = results_mod.PASS
FAIL = results_mod.FAIL
WARN = results_mod.WARN
SKIP = results_mod.SKIP
ERROR = results_mod.ERROR


class Skip(Exception):
    """Raised inside a check to file it as SKIP with the reason."""


class GroupCtx:
    """Passed to each group's run(): the rig and results, the options and the selection."""

    def __init__(self, rig, results, opt, selected, path, paths_map=None, runner=None):
        self.rig = rig
        self.results = results
        self.opt = opt
        self.selected = selected          # None = all; else a set of test ids
        self.path = path
        self.paths_map = paths_map or {}   # test id -> the paths it runs on ("WR", "W", ...)
        self.runner = runner
        self.precondition = None          # an exit code when a safety rule refused
        self.not_as_found = False         # restore found the board changed (exit 6)
        self.board_reset = False
        self.environment = None           # why the run cannot go on with the device (exit 3)

    def want(self, test_id):
        if self.selected is not None and test_id not in self.selected:
            return False
        paths = self.paths_map.get(test_id, "")
        if paths and not set(paths) & set("WRB"):
            return True
        if paths and self.path not in paths:
            return False
        return True

    def check(self, group, test_id):
        return _Check(self, group, test_id)

    def record(self, group, test_id, status, notes="", expected=None, observed=None, ring=None):
        r = results_mod.Result(test_id, group, self.path)
        r.set(status, notes, expected, observed)
        if ring:
            r.ring_excerpt = ring
        self.results.add(r)
        return r


class _Check:
    """A test in progress. `c.eq(what, got, want)` and `c.true(what, cond)` record a failure when
    they do not hold; `c.warn(text)` a warning; `c.note(text)` what the PASS line says;
    `c.skip(why)` ends the test as SKIP. Values that could identify the board never go in."""

    def __init__(self, ctx, group, test_id):
        self.ctx = ctx
        self.group = group
        self.id = test_id
        self.result = results_mod.Result(test_id, group, ctx.path)
        self.fails = []
        self.warns = []
        self.notes = []
        self.checks = 0
        self.observed = {}
        self._t0 = 0.0
        self._status = None

    # ---- what a test reports ----
    def eq(self, what, got, want):
        self.checks += 1
        if got != want:
            self.fails.append("%s: %s, expected %s" % (what, _short(got), _short(want)))
            return False
        return True

    def true(self, what, cond):
        self.checks += 1
        if not cond:
            self.fails.append(what)
            return False
        return True

    def fail(self, what):
        self.checks += 1
        self.fails.append(what)

    def warn(self, what):
        self.warns.append(what)

    def note(self, what):
        self.notes.append(what)

    def observe(self, **kw):
        self.observed.update(kw)

    def skip(self, why):
        raise Skip(why)

    def error(self, why):
        self._status = (ERROR, why)

    # ---- the context manager ----
    def __enter__(self):
        self._t0 = time.monotonic()
        started = getattr(self.ctx.rig.backend, "test_started", None)
        if started:
            started(self.id, self.ctx.path)   # the simulator's faults hook into a named test
        return self

    def __exit__(self, exc_type, exc, tb):
        r = self.result
        r.duration_s = time.monotonic() - self._t0
        if exc_type is not None and not issubclass(exc_type, Exception):
            return False                  # Ctrl-C and the like: not a test result
        if exc_type is not None and self.fails and not isinstance(exc, Skip):
            # A check failed and the test then could not go on: the failure is the result (the
            # exception, usually its consequence, is noted), and the exception's side effects
            # (a precondition, a board reset) still apply.
            _effects(self.ctx, exc)
            r.set(FAIL, "; ".join(self.fails[:4]) + "; then: %s" % _describe(exc))
            if self.observed:
                r.observed = self.observed
            self.ctx.results.add(r)
            return _swallow(exc)
        if exc_type is None:
            if self._status:
                r.set(*self._status)
            elif self.fails:
                r.set(FAIL, "; ".join(self.fails[:4]) + (" (+%d more)" % (len(self.fails) - 4)
                                                        if len(self.fails) > 4 else ""))
            elif self.checks == 0:
                r.set(ERROR, "the test compared nothing (a bug in the runner)")
            elif self.warns:
                r.set(WARN, "; ".join(self.warns[:4]))
            else:
                r.set(PASS, "; ".join(self.notes) if self.notes else "%d checks" % self.checks)
        elif isinstance(exc, Skip):
            r.set(SKIP, str(exc))
        elif isinstance(exc, safety.InjectionBlocked):
            r.set(SKIP, "no injection in this run: %s" % exc)
        elif isinstance(exc, NoLink):
            r.set(FAIL, _describe(exc))     # the link is down; no safety refusal
        else:
            _effects(self.ctx, exc)
            r.set(ERROR, _describe(exc))
        if self.observed:
            r.observed = self.observed
        self.ctx.results.add(r)
        return exc is None or _swallow(exc)


# What ends the whole group, not only the test (after the test's result is filed): the board reset
# or gone (the runner looks for it and goes on with the next group), the device unusable (the run
# stops), Steam started (the runner waits for it to close, or stops).
GROUP_ENDERS = (BoardGone, DeviceError, safety.SteamOpened, safety.LeakFired)


def _swallow(exc):
    if isinstance(exc, GROUP_ENDERS):
        exc.recorded = True
        return False
    return True


def describe(exc):
    """How an exception reads in a result."""
    return _describe(exc)


def _describe(exc):
    if isinstance(exc, safety.InjectionBlocked):
        return "no injection in this run: %s" % exc
    if isinstance(exc, (RigError, safety.SafetyError)):
        return "safety/precondition: %s" % exc
    if isinstance(exc, BoardGone):
        return "board reset: %s" % exc
    if isinstance(exc, DeviceError):
        return "device: %s" % exc
    if isinstance(exc, NoLink):
        return "no link: %s" % exc
    if isinstance(exc, LeaseLost):
        return "lease lost: %s" % exc
    if isinstance(exc, (Stall, Refused)):
        return "unexpected refusal: %s" % exc
    if isinstance(exc, (T.ProtocolError, ConsoleError)):
        return "protocol: %s" % exc
    return "exception: %s: %s" % (type(exc).__name__, exc)


def _effects(ctx, exc):
    """What an exception in a test means for the run: a safety refusal or a precondition is exit
    code 2 (InjectionBlocked was already counted when the layer failed); the board gone starts
    the runner's recovery (it looks for the board again)."""
    if isinstance(exc, safety.InjectionBlocked):
        return
    if isinstance(exc, safety.SteamOpened):
        ctx.precondition = C.EXIT_PRECONDITION
        ctx.rig.ctx.steam_running = True
    elif isinstance(exc, (RigError, safety.SafetyError)):
        ctx.precondition = C.EXIT_PRECONDITION
    elif isinstance(exc, BoardGone):
        ctx.board_reset = True
    elif isinstance(exc, DeviceError):
        ctx.environment = str(exc)


def _short(v):
    s = repr(v)
    return s if len(s) <= 60 else s[:57] + "..."


def byte_lists(reports):
    """Reports as lists of numbers, for comparisons whose failure line shows them: a hex string
    of 16 or more digits would be masked to <hex:N> by the writers' backstop (R25)."""
    return [list(bytes(r)) for r in reports]


# ---- ring helpers ----------------------------------------------------------------------------

def of_kind(entries, *kinds):
    return [e for e in entries if e["kind"] in kinds]


def without_vendor(entries):
    """The entries a test compares exactly: the VENDOR records of the runner's own requests and
    its lizard keep-off requests left out, and over Bluetooth the link's own events (a host's
    parameter update, say), which are never what such a test is about."""
    return [e for e in entries if e["kind"] not in (C.RING_VENDOR, C.RING_BLE)]


def kinds(entries):
    return [e["kind"] for e in entries]


def summary(entries, limit=12):
    """A short, safe description of entries for a failure line (kinds and the main field)."""
    out = []
    for e in entries[:limit]:
        k = e["kind"]
        name = {C.RING_LED: "LED", C.RING_SETTING: "SET%d=%s" % (e.get("number", 0),
                                                                    e.get("value")),
                C.RING_LIZARD: "LZ%d" % e.get("what", 0), C.RING_LIZARD_RPT: "RPT",
                C.RING_RUMBLE: "RUM", C.RING_HAPTIC: "HAP", C.RING_OUTPUT: "OUT",
                C.RING_POWER: "PWR%d" % e.get("request", 0), C.RING_LINK: "LINK%d" % e.get(
                    "state", 0), C.RING_MARK: "MARK", C.RING_SEQ_STEP: "STEP",
                C.RING_SEQ_END: "END", C.RING_INJECT_STOP: "STOP", C.RING_ARMED: "ARMED",
                C.RING_VENDOR: "VEN%02X" % e.get("opcode", 0)}.get(k, "K%02X" % k)
        out.append(name)
    return " ".join(out) + (" ..." if len(entries) > limit else "")


def need(rig, what):
    """Skip the running test when the backend cannot show `what` (dry runs only)."""
    if not rig.models(what):
        raise Skip("not modelled by the %s backend (checked on hardware)" % rig.backend.kind)
