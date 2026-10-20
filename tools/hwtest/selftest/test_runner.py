# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# test_runner.py: the catalogue and the exit-code precedence.
import unittest

from hwtest import board as board_mod
from hwtest import config as C
from hwtest import mask as mask_mod
from hwtest import results as results_mod
from hwtest import runner


class Catalog(unittest.TestCase):
    def test_all_groups_present(self):
        names = [n for n, _m in runner.CATALOG]
        for g in ("hooks", "desc", "cmd", "inputs", "battery", "stream", "reports", "outputs",
                  "settings", "flash", "lizard", "wireless", "puck", "steam", "modes", "restore"):
            self.assertIn(g, names)

    def test_list_tests_has_matrix_ids(self):
        ids = {t[0] for t in runner.list_tests()}
        for tid in ("inputs.buttons", "battery.values", "stream.cadence", "outputs.rumble",
                    "settings.clamps", "cmd.attributes", "wireless.reconnect", "modes.to_wireless",
                    "restore.as_found", "hooks.guard", "lizard.no_leak"):
            self.assertIn(tid, ids)


class RuleWord(unittest.TestCase):
    """A firmware without the rule word (caps bit 15) has OpenSC2 0.2.0's rules: the runner
    models it so, never sends RULES or RULES_SET to it, and its new tests skip."""

    def test_without_the_rule_word(self):
        from hwtest.proto import model as M
        rig = object.__new__(board_mod.Rig)

        def no_request(*_a, **_k):
            raise AssertionError("a test-channel request to a firmware without RULES")
        rig.test = no_request
        rig.caps = {"build_flags": 1 << C.CAP_TRIGGER_CLICK_AUTO}
        self.assertEqual(rig.rules(), (M.RULES_V0_2, M.RULES_V0_2))
        rig.caps = {"build_flags": 0}
        self.assertEqual(rig.rules(), (0, 0))
        self.assertIsNone(rig.rules_set(M.RULES_DEFAULT))

    def test_with_the_rule_word(self):
        from hwtest.proto import model as M
        from hwtest.proto import testproto as T
        rig = object.__new__(board_mod.Rig)
        sent = []

        def answer(op, args=b"", **_k):
            sent.append((op, bytes(args)))
            if op == C.OP_RULES:
                return {"status": C.ST_OK, "result": (M.RULES_DEFAULT).to_bytes(4, "little") +
                        (M.RULES_DEFAULT).to_bytes(4, "little") +
                        M.RULES_KNOWN.to_bytes(4, "little")}
            return {"status": C.ST_OK, "result": args[:4]}
        rig.test = answer
        rig.caps = {"build_flags": 1 << C.CAP_RULES}
        self.assertEqual(rig.rules(), (M.RULES_DEFAULT, M.RULES_DEFAULT))
        self.assertEqual(rig.rules_set(M.RULE_PAD_CLICK), M.RULE_PAD_CLICK)
        self.assertEqual(sent[-1], (C.OP_RULES_SET, M.RULE_PAD_CLICK.to_bytes(4, "little")))
        self.assertTrue(T.cap_flag(rig.caps, C.CAP_RULES))


class ExitCodes(unittest.TestCase):
    def _results(self, statuses):
        res = results_mod.Results(mask_mod.Masker(), quiet=True)
        for i, s in enumerate(statuses):
            res.add(results_mod.Result("t%d" % i, "g", "W").set(s))
        return res

    def test_pass(self):
        res = self._results([results_mod.PASS, results_mod.WARN, results_mod.SKIP])
        self.assertEqual(res.exit_code(0), C.EXIT_OK)

    def test_fail(self):
        res = self._results([results_mod.PASS, results_mod.FAIL])
        self.assertEqual(res.exit_code(0), C.EXIT_TEST_FAILED)

    def test_error_is_at_least_one(self):
        res = self._results([results_mod.ERROR])
        self.assertEqual(res.exit_code(0), C.EXIT_TEST_FAILED)

    def test_precondition_base(self):
        res = self._results([results_mod.PASS])
        self.assertEqual(res.exit_code(C.EXIT_PRECONDITION), C.EXIT_PRECONDITION)


if __name__ == "__main__":
    unittest.main()
