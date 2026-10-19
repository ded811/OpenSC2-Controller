# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# test_reports.py: decode and encode of 0x42/state52, 0x43, 0x44, 0x79, 0x7B and
# padding to 54 bytes.
import unittest

from hwtest import config as C
from hwtest.proto import reports as R


class Reports(unittest.TestCase):
    def test_state52_round_trip(self):
        s = R.make_state52(buttons=0x12345678, lstick_x=-30000, right_trigger=20000,
                           rpad_x=-1, imu_timestamp=0xDEADBEEF, gyro_z=-32768)
        self.assertEqual(len(s), C.STATE52_LEN)
        f = R.decode_state52(s)
        self.assertEqual(f["buttons"], 0x12345678)
        self.assertEqual(f["lstick_x"], -30000)
        self.assertEqual(f["right_trigger"], 20000)
        self.assertEqual(f["rpad_x"], -1)
        self.assertEqual(f["imu_timestamp"], 0xDEADBEEF)
        self.assertEqual(f["gyro_z"], -32768)

    def test_state_report_nonce_and_seq(self):
        s = R.make_state52(imu_timestamp=0xA5A50001)
        rep = bytes([C.RPT_STATE, 7]) + s
        seq, f = R.decode_state_report(R.pad_input(rep))
        self.assertEqual(seq, 7)
        self.assertEqual(R.nonce_of_report(R.pad_input(rep)), 0xA5A50001)
        self.assertEqual(f["imu_timestamp"], 0xA5A50001)

    def test_battery_default(self):
        d = R.decode_battery(R.BATTERY_DEFAULT)
        self.assertEqual(d["charge_state"], 4)
        self.assertEqual(d["level"], 100)
        self.assertEqual(d["battery_mv"], 4100)
        self.assertEqual(d["system_mv"], 3300)
        self.assertEqual(d["input_mv"], 5000)

    def test_be_decode(self):
        be = bytes.fromhex("34123c0fe40c881306fff40137a502")
        d = R.decode_be(be)
        self.assertEqual(d["level"], 55)
        self.assertEqual(d["gauge_flags"], 0xA5)
        self.assertEqual(d["charge_state"], 2)
        self.assertEqual(d["current_ma"], -250)

    def test_0x44_0x79_0x7b(self):
        self.assertEqual(R.decode_haptic_status(bytes([0x44, 3, 2, 0, 0, 0])),
                         {"channel": 3, "flags": 2})
        self.assertEqual(R.decode_link_status(bytes([0x79, 2]))["status"], 2)
        q = R.decode_link_quality(bytes([0x7B, 0xF4, 1, 10, 0, 0, 0, 5, 0, 0xCE, 0, 5, 80]))
        self.assertEqual(q["window_ms"], 500)
        self.assertEqual(q["exchanges"], 10)
        self.assertEqual(q["longest_gap_ms"], 5)
        self.assertEqual(q["rssi_dbm"], -50)
        self.assertEqual(q["reason"], 0)
        self.assertEqual(q["channel"], 5)
        self.assertEqual(q["first_alternate"], 80)

    def test_pad_input(self):
        self.assertEqual(len(R.pad_input(bytes([0x42, 0, 1, 2]))), 54)
        self.assertEqual(len(R.pad_input(bytes(range(60)))), 54)

    def test_mouse_keyboard(self):
        self.assertEqual(R.decode_mouse(bytes([0x40, 1, 0xF6, 0, 0, 0]))["x"], -10)
        kb = R.decode_keyboard(bytes([0x41, 0, 0, 0x28, 0, 0, 0, 0, 0]))
        self.assertEqual(kb["keys"][0], 0x28)


if __name__ == "__main__":
    unittest.main()
