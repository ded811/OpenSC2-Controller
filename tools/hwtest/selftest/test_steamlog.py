# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# test_steamlog.py: the tail reader, the phrase filter, the Controller Info parser and
# masking, on synthetic lines only.
import unittest

from hwtest import mask as mask_mod
from hwtest.proto import steamlog


class SteamLog(unittest.TestCase):
    def test_filter(self):
        text = "\n".join([
            "boring line",
            "Local Device Found: type: 28de 1302",
            "Controller Info: HWID: 72, FWTimestamp: 0x6A628345",
            "... wireless_established ...",
            "another boring line",
        ])
        kept = steamlog.filter_lines(text)
        self.assertEqual(len(kept), 3)

    def test_controller_info(self):
        got = steamlog.parse_controller_info("Controller Info: HWID: 72, FWTimestamp: 0x6A628345")
        self.assertEqual(got, (72, 0x6A628345))
        self.assertIsNone(steamlog.parse_controller_info("no match"))

    def test_rotation(self):
        self.assertTrue(steamlog.Tail.rotated(1000, 10))
        self.assertFalse(steamlog.Tail.rotated(10, 1000))

    def test_masking_of_kept_line(self):
        m = mask_mod.Masker()
        m.add_secret("OSC0123456789", "<unit>")
        line = "Got bond for slot 0 (1): 0A1B2C3D 4E5F6071 OSC0123456789"
        masked = m.text(line, transcript=True)
        self.assertIn("<unit>", masked)
        self.assertIn("<id>", masked)
        self.assertNotIn("0A1B2C3D", masked)

    def test_bond_lines_keep_no_bond(self):
        """safety-HW-3: whatever base Steam prints the IDs in (decimal, unpadded hex), a kept
        bond line holds no ID and no serial; an empty bond reads "empty"."""
        text = "\n".join([
            "[t] Got bond for slot 0 (1): 3735928559 305419896 FAKEPUCK00001",
            "[t] Got bond for slot 1 (1): 0 0",
            "[t] Got bond for slot 0 (8): 2a3b4c5 1f FAKEPUCK00001",
        ])
        kept = steamlog.filter_lines(text)
        self.assertEqual(kept, ["[t] Got bond for slot 0 (1): <bond>",
                                "[t] Got bond for slot 1 (1): empty",
                                "[t] Got bond for slot 0 (8): <bond>"])
        self.assertEqual([steamlog.redact(ln) for ln in kept], kept)    # idempotent
        m = mask_mod.Masker()
        for ln in kept:
            out = m.text(ln, transcript=True)
            for secret in ("3735928559", "305419896", "2a3b4c5", "FAKEPUCK"):
                self.assertNotIn(secret, out)

    def test_board_lines(self):
        """A line that names another device's serial is not about the board."""
        lines = ["Controller Info: HWID: 72, FWTimestamp: 0x6A628345",
                 "opened for index 5 OSC0123456789",
                 "opened for index 6 NOTOSC000SIM1",
                 "Controller Connected to Dongle FAKEPUCK00001 (wireless_established)"]
        kept = steamlog.board_lines(lines, {"OSC0123456789", "FAKEPUCK00001"})
        self.assertEqual(kept, [lines[0], lines[1], lines[3]])


if __name__ == "__main__":
    unittest.main()
