# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# test_shellfmt.py: the console lines the offline backends print and the runner's parsers read
# (proto/shellfmt.py) are the ones lib/opensc2/shell.c prints. Every C format in shellfmt must be a
# string literal of shell.c (adjacent literals joined, as the compiler joins them), its Python
# template must be the same format with C's conversions mapped (%u -> %d, %c -> %s), and the
# parsers must read the rendered lines back. A change of a format in shell.c fails here until
# shellfmt, and with it the parsers, follow.
import os
import re
import unittest

from hwtest.proto import console as cproto
from hwtest.proto import shellfmt as SF

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
SHELL_C = os.path.join(ROOT, "lib", "opensc2", "shell.c")

_LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
_BETWEEN = re.compile(r"^\s*$")
_CONV = re.compile(r"%(-?\d*)l?([udxXcs])")


def c_literals(path):
    """Every string literal of a C file, with adjacent literals joined."""
    text = open(path, encoding="utf-8").read()
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    out = []
    last_end = None
    for m in _LITERAL.finditer(text):
        value = m.group(1).replace('\\"', '"').replace("\\\\", "\\")
        if last_end is not None and _BETWEEN.match(text[last_end:m.start()]):
            out[-1] += value
        else:
            out.append(value)
        last_end = m.end()
    return out


def c_to_python(fmt):
    def conv(m):
        kind = {"u": "d", "c": "s"}.get(m.group(2), m.group(2))
        return "%" + m.group(1) + kind
    return _CONV.sub(conv, fmt)


class Formats(unittest.TestCase):
    def test_every_format_is_in_shell_c(self):
        literals = set(c_literals(SHELL_C))
        missing = [name for name, (cfmt, _py) in SF.FORMATS.items() if cfmt not in literals]
        self.assertEqual(missing, [], "formats not in shell.c (changed there?)")

    def test_python_templates_match(self):
        for name, (cfmt, py) in SF.FORMATS.items():
            self.assertEqual(c_to_python(cfmt), py, name)

    def test_mode_reason_texts_are_shell_c(self):
        literals = set(c_literals(SHELL_C))
        for text in SF.MODE_REASON_TEXT.values():
            self.assertIn(text, literals)
        for text in SF.LINK_STATE_TEXT + SF.USB_STATE_TEXT:
            self.assertIn(text, literals)

    def test_the_scanner_joins_adjacent_literals(self):
        self.assertIn("last wired: boot %u, %u requests, %u stalls, %u outputs, %u reports, "
                      "%u dropped; ended: %s", c_literals(SHELL_C))


class Parsers(unittest.TestCase):
    """The rendered lines read back by console.py."""

    IDENT = {"unit": "OSC0123456789", "board": "OSB0123456789", "git": "osc2-app-0.2",
             "pid": 0x1302, "rev": 0x48, "build": 0x6A628345, "blbuild": 0x68D2F92E,
             "name": "Bench Pad (2) x"}   # a stored name: spaces inside are part of it

    def test_status_wireless(self):
        lines = SF.status_lines(False, 2, "auto", False, 0x18, self.IDENT,
                                link=(4, 1, 1, 27, (47, 63, 11), 2, -42, 1000, 900, 3, 30),
                                last=(3, 10, 1, 0, 248, 0, "host: reboot into wireless"))
        s = cproto.parse_status(lines)
        self.assertEqual((s["mode"], s["mode_reason"], s["policy"]),
                         ("wireless", "warm restart", "auto"))
        self.assertEqual((s["link"], s["link_reason"], s["slot"]), ("connected", 1, 2))
        self.assertEqual((s["channel"], s["alternates"], s["protocol"], s["rssi_dbm"]),
                         (27, [47, 63, 11], 2, -42))
        self.assertEqual(s["traffic"], [1000, 900, 3, 30])
        self.assertEqual((s["lizard"], s["imu_mode"]), (False, 0x18))
        self.assertEqual(s["last_wired"], [3, 10, 1, 0, 248, 0])
        self.assertEqual(s["last_wired_end"], "host: reboot into wireless")

    def test_status_wired_and_no_session(self):
        lines = SF.status_lines(True, 1, "wireless", True, 0, self.IDENT,
                                usb=(3, True, 248, 0, 5, 1), last=None)
        s = cproto.parse_status(lines)
        self.assertEqual((s["mode"], s["policy"]), ("wired", "wireless"))
        self.assertEqual((s["usb"], s["usb_suspended"]), ("configured", True))
        self.assertTrue(s["lizard"])
        self.assertIn("last_wired_end", s)
        self.assertIsNone(s["last_wired_end"])

    def test_id_bonds_mode(self):
        i = cproto.parse_id(SF.id_lines(self.IDENT))
        self.assertEqual(i, self.IDENT)
        b = cproto.parse_bonds(SF.bond_lines(["FAKEPUCK00001", None], 0))
        self.assertEqual([(x["slot"], x["selected"], x["puck_serial"]) for x in b],
                         [(1, True, "FAKEPUCK00001"), (2, False, None)])
        m = cproto.parse_mode([SF.mode_line(False, 2, "auto")])
        self.assertEqual(m, {"mode": "wireless", "mode_reason": "warm restart", "policy": "auto"})

    def test_policy_stored(self):
        self.assertEqual(SF.line("mode_stored", "auto"), "policy auto stored")


if __name__ == "__main__":
    unittest.main()
