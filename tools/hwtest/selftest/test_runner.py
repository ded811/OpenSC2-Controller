# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# test_runner.py: the catalogue and the exit-code precedence.
import unittest

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
