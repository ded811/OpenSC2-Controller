# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# test_stats.py: cadence and gap statistics on synthetic timestamps, and sequence
# gaps with 8-bit wrap and duplicates.
import unittest

from hwtest import stats


class Cadence(unittest.TestCase):
    def test_mean_and_gap(self):
        ts = [int(i * 4.0283e6) for i in range(1000)]   # 4.0283 ms apart, ns
        c = stats.cadence(ts)
        self.assertAlmostEqual(c["mean_period_ms"], 4.0283, places=3)
        self.assertAlmostEqual(c["per_s"], 248.24, delta=0.5)

    def test_gap_verdict(self):
        self.assertEqual(stats.gap_verdict(10, 15, None), "pass")
        self.assertEqual(stats.gap_verdict(20, 15, None), "warn")
        self.assertEqual(stats.gap_verdict(300, 50, 200), "fail")


class Sequence(unittest.TestCase):
    def test_clean(self):
        g = stats.sequence_gaps(list(range(256)) + [0, 1, 2])
        self.assertEqual(g["missing"], 0)
        self.assertEqual(g["duplicates"], 0)

    def test_missing_and_dup(self):
        g = stats.sequence_gaps([1, 2, 4, 4, 5])   # a gap of 1 (3 missing), then a duplicate
        self.assertEqual(g["missing"], 1)
        self.assertEqual(g["duplicates"], 1)

    def test_wrap(self):
        g = stats.sequence_gaps([254, 255, 0, 1])
        self.assertEqual(g["missing"], 0)

    def test_verdict(self):
        v, unexp = stats.sequence_verdict(5, 5, 2000, None, None)   # all explained, W
        self.assertEqual(v, "pass")
        v, unexp = stats.sequence_verdict(10, 0, 2000, None, None)  # unexplained on W -> fail
        self.assertEqual(v, "fail")
        v, unexp = stats.sequence_verdict(10, 0, 2000, 0.01, 0.05)  # 0.5 % on R -> pass
        self.assertEqual(v, "pass")


if __name__ == "__main__":
    unittest.main()
