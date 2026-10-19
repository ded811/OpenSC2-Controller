# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# test_console.py: synthetic transcripts through the console parser: VT100 sequences,
# a prompt before HWT, Zephyr log lines in between, CR LF and lone LF, and the parsers of the
# existing commands' output.
import unittest

from hwtest.proto import console as c


class StreamFilter(unittest.TestCase):
    def test_strip_and_find_hwt(self):
        line = "opensc2:~$ \x1b[1;32mHWT 5 END 0\x1b[0m"
        clean = c.strip_vt100(line)
        found = c.find_hwt(clean)
        self.assertEqual(found, ("5", "END", "0"))

    def test_log_line(self):
        self.assertTrue(c.is_log_line("[00:00:01.234,567] <inf> opensc2: hi"))
        self.assertFalse(c.is_log_line("HWT 1 END 0"))

    def test_split_crlf_and_lf(self):
        lines = c.split_stream("a\r\nb\nc")
        self.assertEqual(lines, ["a", "b", "c"])

    def test_end_and_status(self):
        self.assertEqual(c.parse_end("0"), 0)
        self.assertEqual(c.parse_end("-22 trailing"), -22)
        kv = c.parse_status_kv("armed=1 guard=1 lizard=0 mode=wireless ring=3..9")
        self.assertTrue(c.status_bool(kv, "armed"))
        self.assertFalse(c.status_bool(kv, "lizard"))
        self.assertEqual(kv["mode"], "wireless")
        self.assertEqual(kv.get("future_key", "x"), "x")   # unknown keys tolerated


class ExistingCommands(unittest.TestCase):
    def test_status(self):
        lines = [
            "mode:      wireless (warm restart), policy auto",
            "link:      connected (last reason 1), bond slot 2",
            "lizard:    off, IMU mode 0x18",
            "identity:  OSC0123456789 / OSB0123456789, pid 0x1302, rev 0x4b, build 0x6a628359, g",
            "last wired: boot 3, 10 requests, 1 stalls, 0 outputs, 248 reports, 0 dropped; "
            "ended: host: reboot into wireless",
        ]
        s = c.parse_status(lines)
        self.assertEqual(s["mode"], "wireless")
        self.assertEqual(s["policy"], "auto")
        self.assertEqual(s["link"], "connected")
        self.assertEqual(s["slot"], 2)
        self.assertFalse(s["lizard"])
        self.assertEqual(s["imu_mode"], 0x18)
        self.assertEqual(s["last_wired_end"], "host: reboot into wireless")

    def test_status_wired(self):
        lines = ["mode:      wired (USB power), policy auto",
                 "usb:       configured, 248 reports, 0 dropped, 5 requests, 1 stalls",
                 "lizard:    on, IMU mode 0x00"]
        s = c.parse_status(lines)
        self.assertEqual(s["usb"], "configured")
        self.assertTrue(s["lizard"])

    def test_id(self):
        lines = ["unit      OSC0123456789", "board     OSB0123456789", "git       osc2-app-0.2",
                 "pid       0x1302", "rev       0x4b", "build     0x6a628359", "blbuild   0x68d2f9f2",
                 "name      Steam Controller"]
        i = c.parse_id(lines)
        self.assertEqual(i["unit"], "OSC0123456789")
        self.assertEqual(i["pid"], 0x1302)
        self.assertEqual(i["build"], 0x6A628359)
        self.assertEqual(i["name"], "Steam Controller")
        # firmware before the product name prints no name line
        self.assertNotIn("name", c.parse_id(lines[:-1]))

    def test_bonds_and_mode(self):
        b = c.parse_bonds(["slot 1: empty", "slot 2 (selected): puck FAKEPUCK00001"])
        self.assertEqual(b[0]["puck_serial"], None)
        self.assertTrue(b[1]["selected"])
        self.assertEqual(b[1]["puck_serial"], "FAKEPUCK00001")
        m = c.parse_mode(["wireless now (warm restart); policy auto"])
        self.assertEqual(m["mode"], "wireless")
        self.assertEqual(m["policy"], "auto")


if __name__ == "__main__":
    unittest.main()
