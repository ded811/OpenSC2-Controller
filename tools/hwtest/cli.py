# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# cli.py: options, orchestration and exit codes (docs/testing.md). Builds the backend (the
# simulator for --dry-run, the firmware emulator for --dry-run --fw-emu, else the hardware through
# hwtest.win on Windows), runs the suite, writes the results folder and returns the exit code.
# --list and --selftest do their work and exit; neither opens a device.
import argparse
import datetime
import os
import sys

from . import config as C
from . import mask as mask_mod


def _parse(argv):
    p = argparse.ArgumentParser(
        prog="run.py", description="OpenSC2 hardware test suite: every feature the board "
        "emulates, checked with no physical interaction (docs/testing.md)")
    p.add_argument("--list", action="store_true", help="the tests, with their paths and needs")
    p.add_argument("--group", default="", help="only these groups (comma separated)")
    p.add_argument("--test", default="", help="only these tests (comma separated ids)")
    p.add_argument("--wired-only", action="store_true", help="only the wired path")
    p.add_argument("--wireless-only", action="store_true", help="only the wireless path")
    p.add_argument("--through-puck", action="store_true",
                   help="allow writes to the board's slot on the puck (never feature report 2)")
    p.add_argument("--flash", action="store_true",
                   help="allow the value-preserving flash rewrites of group flash")
    p.add_argument("--lizard", action="store_true",
                   help="also run group lizard: lizard mode on injected inputs, the guard alone "
                        "holding its reports back")
    p.add_argument("--dock", action="store_true",
                   help="also run group dock: the board's dock link fed frames through the test "
                        "hooks (nothing wired, never the puck's pads)")
    p.add_argument("--with-steam", action="store_true", help="also run group steam (passive)")
    p.add_argument("--steam-control", action="store_true",
                   help="let the runner close and start Steam")
    p.add_argument("--soak", type=int, default=0, help="run stream.soak for S seconds")
    p.add_argument("--board", type=int, default=None,
                   help="the board to use when several are connected (the listed index)")
    p.add_argument("--timeout-scale", type=float, default=1.0, help="multiply every timeout")
    p.add_argument("--max-minutes", type=int, default=None, help="stop (and restore) after N")
    p.add_argument("--fail-fast", action="store_true", help="stop at the first failure")
    p.add_argument("--results", default=None, help="where to write results")
    p.add_argument("--yes", action="store_true", help="never prompt: fail instead of waiting")
    p.add_argument("--dry-run", action="store_true",
                   help="offline: every group against the simulator, no device opened")
    p.add_argument("--fw-emu", default=None, metavar="EXE",
                   help="with --dry-run: against the firmware emulator built by "
                        "tests/host/run.ps1 (the library's own code)")
    p.add_argument("--sim-fault", default=None, help="with --dry-run: make the simulator "
                   "misbehave in one way")
    p.add_argument("--selftest", action="store_true", help="the runner's own tests, no device")
    p.add_argument("-v", "--verbose", action="store_true", help="print the console transcript")
    return p.parse_args(argv)


def _options(a):
    return {
        "dry_run": a.dry_run, "fault": a.sim_fault, "fw_emu": a.fw_emu,
        "groups": set(g for g in a.group.split(",") if g) or None,
        "tests": set(t for t in a.test.split(",") if t) or None,
        "wired_only": a.wired_only, "wireless_only": a.wireless_only,
        "through_puck": a.through_puck, "flash": a.flash, "lizard": a.lizard, "dock": a.dock,
        "with_steam": a.with_steam, "steam_control": a.steam_control, "soak": a.soak,
        "board": a.board, "timeout_scale": a.timeout_scale, "max_minutes": a.max_minutes,
        "fail_fast": a.fail_fast, "yes": a.yes, "verbose": a.verbose,
        "results_dir": a.results or os.path.join(os.path.dirname(__file__), "results"),
    }


def _do_list():
    from . import runner
    print("%-30s %-6s %s" % ("test", "paths", "needs"))
    for tid, paths, needs in runner.list_tests():
        print("%-30s %-6s %s" % (tid, paths or "-", ",".join(needs) or "-"))
    return C.EXIT_OK


def _do_selftest():
    import unittest
    here = os.path.join(os.path.dirname(__file__), "selftest")
    tools = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    suite = unittest.defaultTestLoader.discover(here, pattern="test_*.py", top_level_dir=tools)
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    return C.EXIT_OK if result.wasSuccessful() else C.EXIT_TEST_FAILED


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    a = _parse(argv)
    if a.wired_only and a.wireless_only:
        print("--wired-only and --wireless-only are exclusive")
        return C.EXIT_USAGE
    if (a.sim_fault or a.fw_emu) and not a.dry_run:
        print("--sim-fault and --fw-emu need --dry-run")
        return C.EXIT_USAGE
    if a.list:
        return _do_list()
    if a.selftest:
        return _do_selftest()

    opt = _options(a)
    if not a.dry_run and sys.platform != "win32":
        print("this runner needs Windows (or --dry-run)")
        return C.EXIT_ENVIRONMENT

    from . import runner as runner_mod
    masker = mask_mod.Masker()

    start_mode = "R" if a.wireless_only else "W"
    if a.dry_run and a.fw_emu:
        from . import fwemu
        if not os.path.isfile(a.fw_emu):
            print("the firmware emulator %s is not there: build it with tests/host/run.ps1"
                  % a.fw_emu)
            return C.EXIT_USAGE
        backend = fwemu.FwEmuBackend(a.fw_emu, start_mode=start_mode)
    elif a.dry_run:
        from . import sim
        try:
            backend = sim.SimBackend(start_mode=start_mode, fault=a.sim_fault)
        except ValueError as e:
            print(e)
            return C.EXIT_USAGE
    else:
        from .board import RealClock
        from .win import backend as win_backend
        backend = win_backend.WinBackend(RealClock(), opt, masker)

    run = runner_mod.Runner(backend, opt, masker)
    code = C.EXIT_OK
    try:
        if not a.dry_run:
            backend.keep_awake(True)
        code = run.run()
    except KeyboardInterrupt:
        try:
            run.rig.restore()
        except Exception:
            pass
        code = C.EXIT_INTERRUPTED
    finally:
        try:
            run.rig.restore()
        except Exception:
            pass
        close = getattr(backend, "close", None)
        if close:
            close()
        if not a.dry_run:
            backend.keep_awake(False)

    out_dir = os.path.join(opt["results_dir"],
                           datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    if a.verbose:
        for ln in run.results.console_lines_raw:
            from .results import decode_transcript_line
            print(masker.text(decode_transcript_line(ln), transcript=True))
    run.results.write(out_dir, opt, run.host, run.board_info, run.puck_info, run.steam_info, code)
    run.results.print_summary(code)
    print("results in %s" % masker.text(out_dir))
    return code
