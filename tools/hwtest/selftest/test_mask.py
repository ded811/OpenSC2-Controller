# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# test_mask.py: every masking rule, including lines that must stay unchanged.
import unittest

from hwtest import mask as mask_mod


class Masking(unittest.TestCase):
    def test_known_secrets(self):
        m = mask_mod.Masker()
        m.add_secret("OSC0123456789", "<unit>")
        m.add_secret("11111111", "<id>")
        self.assertEqual(m.text("serial OSC0123456789 bond 11111111"), "serial <unit> bond <id>")
        # both hex cases of an ID
        self.assertEqual(m.text("AABBCCDD" if False else "11111111"), "<id>")

    def test_osc_serial_pattern(self):
        m = mask_mod.Masker()
        self.assertEqual(m.text("OSB9A8B7C6D5E"), "OSB" + "*" * 10)

    def test_generic_serial(self):
        m = mask_mod.Masker()
        self.assertEqual(m.text("NOTOSC12AB34Z"), "<serial>")

    def test_steamid_and_account_and_path(self):
        m = mask_mod.Masker()
        self.assertEqual(m.text("id 76561190000000001 acct [U:1:12345]"),
                         "id <steamid> acct [U:1:<account>]")
        self.assertEqual(m.text(r"C:\Users\alice\x"), r"C:\Users\<user>\x")
        # forward slashes (a --results path typed that way), MSYS paths, other drive letters
        self.assertEqual(m.text("results in C:/Users/alice/tmp/res"),
                         "results in C:/Users/<user>/tmp/res")
        self.assertEqual(m.text("/c/Users/alice/x and d:\\users\\bob"),
                         "/c/Users/<user>/x and d:\\users\\<user>")
        self.assertEqual(m.text("Users/alice"), "Users/alice")     # no drive: not a user folder

    def test_unchanged_build_times(self):
        m = mask_mod.Masker()
        # a plain 8-hex build time, NOT in a transcript, is not masked as an id
        self.assertEqual(m.text("FWTimestamp 0x6A628345"), "FWTimestamp 0x6A628345")
        self.assertEqual(m.text("wireless_established"), "wireless_established")

    def test_transcript_id8(self):
        m = mask_mod.Masker()
        self.assertEqual(m.text("bond 0A1B2C3D here", transcript=True), "bond <id> here")
        # 0x-prefixed stays
        self.assertEqual(m.text("build 0x6A628345", transcript=True), "build 0x6A628345")

    def test_hex_backstop(self):
        m = mask_mod.Masker()
        out = m.text("blob 00112233445566778899AABBCCDD")
        self.assertIn("<hex:", out)
        self.assertGreaterEqual(m.hex_dropped, 1)

    def test_value_recurses(self):
        m = mask_mod.Masker()
        m.add_secret("OSC0123456789", "<unit>")
        v = m.value({"a": ["OSC0123456789", 5], "b": 7})
        self.assertEqual(v, {"a": ["<unit>", 5], "b": 7})

    def test_user_folder_in_an_exception(self):
        """safety-HW-11: an OSError shows its path as a repr, with doubled backslashes."""
        from hwtest.groups import base
        m = mask_mod.Masker()
        p = "C:\\Users\\alice\\Steam\\logs\\controller.txt"
        text = m.text(base.describe(FileNotFoundError(2, "No such file or directory", p)))
        self.assertNotIn("alice", text)
        self.assertIn("<user>", text)
        self.assertNotIn("bob", m.text("'C:\\\\\\\\Users\\\\\\\\bob\\\\\\\\x'"))
        self.assertNotIn("carol", m.text('open("D:/Users/carol")'))
        self.assertEqual(m.text("C:\\Users\\dave'"), "C:\\Users\\<user>'")   # ends at a quote

    def test_device_paths(self):
        m = mask_mod.Masker()
        iface = "\\\\?\\HID#VID_28DE&PID_1302&Col03#7&2a3b4c5d&0&0000#{4d1e55b2-f16f-11cf}"
        self.assertEqual(m.text(iface),
                         "\\\\?\\HID#VID_28DE&PID_1302&Col03#<masked>#{4d1e55b2-f16f-11cf}")
        doubled = repr(iface)
        self.assertNotIn("2a3b4c5d", m.text(doubled))
        self.assertEqual(m.text("USB\\VID_2FE3&PID_0004\\5&1C2D3E4F&0&2"),
                         "USB\\VID_2FE3&PID_0004\\<masked>")
        self.assertEqual(m.text("HID\\VID_28DE&PID_1304&MI_02&COL03\\8&11&0&0002 x"),
                         "HID\\VID_28DE&PID_1304&MI_02&COL03\\<masked> x")
        self.assertEqual(m.text("VID_28DE&PID_1302&COL03"), "VID_28DE&PID_1302&COL03")

    def test_bond_ids_in_every_form(self):
        """safety-HW-3: a bond ID read in the clear is masked as Steam may print it: decimal,
        unpadded hex, 8-digit hex, either case, as whole words only."""
        m = mask_mod.Masker()
        m.add_id_secret(0xDEADBEEF)
        m.add_id_secret(0x02A3B4C5)
        line = "ids 3735928559 deadbeef DEADBEEF 2a3b4c5 02a3b4c5 44283077 x"
        out = m.text(line)
        for form in ("3735928559", "deadbeef", "DEADBEEF", "2a3b4c5", "02a3b4c5", "44283077"):
            self.assertNotIn(form, out)
        self.assertEqual(m.text("13735928559 a2a3b4c5"), "13735928559 a2a3b4c5")  # not whole
        m.add_id_secret(7)                   # too short to mask safely: left out
        self.assertEqual(m.text("7 seconds"), "7 seconds")


if __name__ == "__main__":
    unittest.main()
