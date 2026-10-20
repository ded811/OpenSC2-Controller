# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# test_dock.py: the dock's part of the runner. tests/vectors/hwtest/dock.txt through proto/dock.py
# (the C host tests read the same file through the library's rules), the codec of the dock ops,
# the safety rules for dock frames, the opt-in, and the dock group against the firmware emulator
# (the library's own dock link) when tests/host/run.ps1 built it.
import os
import unittest

from hwtest import config as C
from hwtest import mask as mask_mod
from hwtest import results as results_mod
from hwtest import runner as runner_mod
from hwtest import safety
from hwtest import sim
from hwtest.proto import dock as D
from hwtest.proto import testproto as T

from .test_vectors import hex_after, records

PASS = results_mod.PASS
SKIP = results_mod.SKIP


def _bytes(tok, i):
    """The bytes from tok[i] on ("-" for none) and the index after them."""
    if tok[i] == "-":
        return b"", i + 1
    return hex_after(tok, i)


class DockVectors(unittest.TestCase):
    def test_dock_txt(self):
        count = 0
        for tok, line in records("dock.txt"):
            eq = tok.index("=")
            kind, want = tok[0], tok[eq + 1:]
            if kind == "CRC":
                data, _ = _bytes(tok, 1)
                self.assertEqual(D.crc16(data), int(want[0], 16), line)
            elif kind == "REQ":
                payload, _ = _bytes(tok, 2)
                frame, _ = hex_after(want, 0)
                self.assertEqual(D.build_request(int(tok[1], 16), payload), frame, line)
            elif kind == "CHECK":
                frame, _ = _bytes(tok, 1)
                self.assertEqual(D.check(frame), want[0], line)
            elif kind == "MV":
                self.assertEqual(D.pilot_mv(int(tok[1])), int(want[0]), line)
            elif kind == "PILOT":
                mv = [int(v) for v in tok[1:4]]
                self.assertEqual(D.pilot_present([v if v >= 0 else None for v in mv]),
                                 (want[0] == "1", int(want[1], 16)), line)
            elif kind == "POWER":
                rule = [tok[1] == "1", tok[2] == "1", int(tok[3])]
                ev = D.power_step(rule, tok[5] == "1", tok[6] == "1", tok[7] == "1")
                self.assertEqual((ev, rule[1], rule[2]),
                                 (None if want[0] == "-" else int(want[0]), want[1] == "1",
                                  int(want[2])), line)
            elif kind == "ENTER":
                sm = D.DockStates()
                frm, to, actions, _h = sm.enter(D.ENTRIES.index(tok[1]), tok[2] == "1")
                self.assertEqual((D.STATES[to], sm.selected, sorted(D.action_names(actions))),
                                 (want[0], want[1] == "1", sorted(want[2:])), line)
            elif kind == "SM":
                sm = D.DockStates()
                sm.state = D.STATES.index(tok[1])
                sm.selected = tok[2] == "1"
                frm, to, actions, handled = sm.step(D.EVENTS.index(tok[3]), int(tok[4]))
                self.assertEqual((D.STATES[to], sm.selected, handled,
                                  sorted(D.action_names(actions))),
                                 (want[0], want[1] == "1", want[2] == "1", sorted(want[3:])),
                                 line)
            else:
                self.fail("an unknown dock record: %s" % line)
            count += 1
        self.assertGreater(count, 80)


class DockCodec(unittest.TestCase):
    def test_frames(self):
        probe = D.build_request(D.PROBE)
        self.assertEqual(probe[71:], bytes([0xD0, 0x9B]))
        self.assertEqual(D.check(probe), "OK")
        self.assertEqual(D.check(probe[:72]), "BAD_SIZE")
        s = D.set_request(0x87, bytes([45, 50, 0]))
        self.assertEqual(s[1], D.SET)
        self.assertEqual(s[8:13], bytes([0x87, 3, 45, 50, 0]))
        ans = D.answer(D.GET, 63, bytes([0x89, 3, 45, 50, 0]))
        a = D.parse_answer(ans)
        self.assertEqual((a["start"], a["type"], a["result"], a["crc_ok"]),
                         (0xAC, D.GET, 63, True))
        with self.assertRaises(ValueError):
            D.build_request(D.SET, bytes(64))

    def test_ops(self):
        parts = D.load_args(bytes(range(73)))
        self.assertEqual([len(p) for p in parts], [54, 21])
        self.assertEqual((parts[0][0], parts[1][0], parts[1][1]), (0, 53, 53))
        run = {"outcome": 0, "session": True, "start": 0xAC, "type": 3, "result": -95,
               "crc_ok": True, "masked": False, "crc32": 0x12345678, "masked_crc32": 0,
               "us": 150}
        self.assertEqual(D.parse_run(D.encode_run(run)), run)
        st = {"active": True, "alone": False, "test": True, "uart": False, "last_end": 2,
              "sessions": 3, "frames": 7, "errors": 1, "slowest_us": 900, "sm_state": 4,
              "supply": 2, "stored": 0x89, "cache_valid": True}
        self.assertEqual(D.parse_status(D.encode_status(st)), st)
        self.assertEqual(D.pilot_args([2500, None, 3300]),
                         bytes([D.CTRL_PILOT, 0xC4, 0x09, 0xFF, 0xFF, 0xE4, 0x0C]))

    def test_masks(self):
        ident = {"pid": 0x1302, "build": 1, "blbuild": 0, "rev": 0x48, "unit": "OSC0123456789",
                 "board": "OSB0123456789", "git": "osc2-0.2.0"}
        probe = D.answer(D.PROBE, 0, D.identity_record(ident))
        hidden = D.masked_bytes(probe, False)
        self.assertEqual(sorted(hidden), list(range(24, 52)))
        shown = D.mask(probe, hidden)
        self.assertEqual(shown[24:52], bytes(28))
        self.assertEqual(shown[8:24], probe[8:24])
        get = D.answer(D.GET, 63, bytes([0xAE, 20, 1]) + b"OSC0123456789")
        self.assertEqual(sorted(D.masked_bytes(get, True)), list(range(10, 30)))
        self.assertEqual(D.masked_bytes(get, False), set())


class DockSafety(unittest.TestCase):
    def test_frames(self):
        for opcode, payload in ((0x83, b""), (0x87, bytes([45, 50, 0])), (0x89, b"\x2d"),
                                (0x95, (0xABDF9710).to_bytes(4, "little"))):
            safety.check_dock_frame(D.set_request(opcode, payload))
        for opcode, payload in ((0x86, (0xEFC1A917).to_bytes(4, "little")), (0x90, b""),
                                (0x95, (0xECAABAC0).to_bytes(4, "little")),
                                (0x95, (0xA427AF52).to_bytes(4, "little")), (0xEE, b"esb/bond\0"),
                                (0xEF, b"esb/bond\0"), (0xA2, bytes(24)), (0xDC, b"\x01\x02"),
                                (0xD8, b""), (0xC1, b""), (0x7E, b"")):
            with self.assertRaises(safety.SafetyError, msg="0x%02x" % opcode):
                safety.check_dock_frame(D.set_request(opcode, payload))
        # other types run no command, whatever their payload says
        safety.check_dock_frame(D.build_request(D.GET, bytes([0x86, 4])))
        with self.assertRaises(safety.SafetyError):
            safety.check_dock_frame(D.build_request(D.PROBE)[:72])

    def test_loads_at_the_send(self):
        bad = D.load_args(D.set_request(0x90))
        good = D.load_args(D.set_request(0x83))
        for args, ok in ((good[0], True), (good[1], True), (bad[0], False),
                         (bytes([20]) + bytes(10), False), (bytes([0]) + bytes(13), False),
                         (bytes([60]) + bytes(5), True)):
            req = T.build_request(C.OP_DOCK_LOAD, args, 1)
            if ok:
                safety.check_test_request(req, "console")
            else:
                with self.assertRaises(safety.SafetyError, msg=args[:4].hex()):
                    safety.check_test_request(req, "console")
        # never through the puck
        with self.assertRaises(safety.SafetyError):
            safety.check_test_request(T.build_request(C.OP_DOCK_LOAD, good[1], 1), "puck")


def _opt(**extra):
    opt = {"dry_run": True, "fault": None, "yes": True, "flash": False, "lizard": False,
           "through_puck": False, "with_steam": False, "soak": 0, "groups": None, "tests": None,
           "steam_control": False, "wired_only": False, "wireless_only": False, "quiet": True}
    opt.update(extra)
    return opt


def _results(run):
    return [(t.label(), t.status, t.notes) for g in run.results.groups for t in g["tests"]
            if t.group == "dock"]


class DockGroup(unittest.TestCase):
    def test_opt_in(self):
        run = runner_mod.Runner(sim.SimBackend(start_mode="W"), _opt(groups={"dock"}),
                                mask_mod.Masker())
        self.assertEqual(run.run(), C.EXIT_OK)
        res = _results(run)
        self.assertTrue(res)
        self.assertTrue(all(s == SKIP and n == "opt-in (--dock)" for _l, s, n in res), res)

    def test_against_the_emulator(self):
        exe = os.environ.get("OPENSC2_FWEMU")
        if not exe or not os.path.isfile(exe):
            self.skipTest("OPENSC2_FWEMU is not set (tests/host/run.ps1 sets it)")
        from hwtest import fwemu
        for start in ("W", "R"):
            backend = fwemu.FwEmuBackend(exe, start_mode=start)
            try:
                run = runner_mod.Runner(backend, _opt(groups={"dock"}, dock=True),
                                        mask_mod.Masker())
                code = run.run()
            finally:
                backend.close()
            res = _results(run)
            self.assertEqual(code, C.EXIT_OK, (start, res))
            self.assertEqual(sorted(s for _l, s, _n in res), [PASS] * 17, (start, res))

    def test_input_alone_against_the_emulator(self):
        # dock.input on its own, with no test before it that left a session running: the copy
        # of the stream's reports is kept only while a session runs
        exe = os.environ.get("OPENSC2_FWEMU")
        if not exe or not os.path.isfile(exe):
            self.skipTest("OPENSC2_FWEMU is not set (tests/host/run.ps1 sets it)")
        from hwtest import fwemu
        backend = fwemu.FwEmuBackend(exe, start_mode="W")
        try:
            run = runner_mod.Runner(backend, _opt(groups={"dock"}, tests={"dock.input"},
                                                  dock=True), mask_mod.Masker())
            code = run.run()
        finally:
            backend.close()
        res = _results(run)
        self.assertEqual(code, C.EXIT_OK, res)
        self.assertEqual(sorted(s for _l, s, _n in res), [PASS] * 2, res)
