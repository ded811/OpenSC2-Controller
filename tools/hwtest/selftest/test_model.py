# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# test_model.py: the runner's copies of the library's answer rules (proto/model.py), case for case
# as the C host tests hold the library's own (tests/host/proto_cases.h, keys_cases.h): 0x89's
# answers, 0xA1's reset flags, the charge window 0xED reads. A dry run checks most of them against
# the firmware emulator, but not these cases, which no group sends.
import struct
import unittest

from hwtest.proto import model as M


class AnswerRules(unittest.TestCase):
    def test_setting_answer(self):
        # the IMU's three settings answer their defaults, every other one its value
        self.assertEqual([M.setting_answer(n, 7) for n in M.IMU_SETTINGS], [0, 1, 100])
        self.assertEqual(M.setting_answer(9, 0), 0)
        self.assertEqual(M.setting_answer(47, -5), -5)
        self.assertEqual(M.setting_answer(86, 9), 9)

    def test_reset_flags(self):
        # Zephyr's RESET_* bits: pin 0, software 1, brown-out 2, power-on 3, watchdog 4, the rest
        # (debug 5, ..., flash 16) one "other" flag
        self.assertEqual(M.reset_flags(0), 0)
        for bit, flag in ((3, 0x01), (2, 0x02), (0, 0x10), (4, 0x20), (1, 0x40)):
            self.assertEqual(M.reset_flags(1 << bit), flag, bit)
        for bit in range(5, 17):
            self.assertEqual(M.reset_flags(1 << bit), 0x08, bit)
        self.assertEqual(M.reset_flags((1 << 0) | (1 << 1) | (1 << 4)), 0x70)
        self.assertEqual(M.reset_flags(0x1FFFF), 0x7B)

    def test_charge_window(self):
        # mte/charge_level: both 0, or 3400..4120 mV in 10 mV steps (key_rules.c)
        cases = (((0, 0), True), ((3400, 4120), True), ((3800, 4000), True),
                 ((4120, 3400), True), ((3400, 0), True), ((3390, 4120), False),
                 ((3400, 4130), False), ((3405, 4100), False), ((3400, 4115), False),
                 ((0, 4000), False), ((0xFFFFFFFF, 0xFFFFFFFF), False))
        for (lower, upper), valid in cases:
            self.assertEqual(M.charge_window_valid(struct.pack("<II", lower, upper)), valid,
                             (lower, upper))
        self.assertFalse(M.charge_window_valid(bytes(7)))


if __name__ == "__main__":
    unittest.main()
