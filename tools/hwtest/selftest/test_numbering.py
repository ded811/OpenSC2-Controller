# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# test_numbering.py: one numbering for the test channel. The ops, ring entry kinds and caps build
# flags of the firmware's headers (lib/opensc2/test_core.h, test_sys.h) against the runner's
# (config.py, proto/haptics.py, proto/sysproto.py): each number used once, the same numbers on both
# sides, and each op and ring kind in docs/test-protocol.md's tables exactly once, by its name.
import os
import re
import unittest

from hwtest import config as C
from hwtest.proto import haptics as H
from hwtest.proto import sysproto as S

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
LIB = os.path.join(ROOT, "lib", "opensc2")
HEADERS = ("test_core.h", "test_sys.h")


def _read(*parts):
    with open(os.path.join(ROOT, *parts), encoding="utf-8") as f:
        return f.read()


def c_numbers(kind):
    """{NAME: value} of OSC2_TEST_<kind>_NAME in the headers (enum members and #defines), without
    the range markers *_FIRST and *_LAST."""
    out = {}
    for h in HEADERS:
        text = _read("lib", "opensc2", h)
        for m in re.finditer(r"\bOSC2_TEST_%s_([A-Z0-9_]+)\s*(?:=\s*|\s+)(0x[0-9A-Fa-f]+)\b" % kind,
                             text):
            name, value = m.group(1), int(m.group(2), 16)
            if name.endswith("_FIRST") or name.endswith("_LAST"):
                continue
            if name in out:
                raise AssertionError("OSC2_TEST_%s_%s is defined twice" % (kind, name))
            out[name] = value
    return out


def c_flags():
    out = {}
    for h in HEADERS:
        for m in re.finditer(r"#define OSC2_TEST_FLAG_([A-Z0-9_]+)\s+\(1u << (\d+)\)",
                             _read("lib", "opensc2", h)):
            out[m.group(1)] = int(m.group(2))
    return out


def py_numbers(prefix, modules, skip=()):
    out = {}
    for mod in modules:
        for k, v in vars(mod).items():
            if k.startswith(prefix) and isinstance(v, int) and not isinstance(v, bool) and \
                    not any(k.startswith(s) for s in skip):
                out.setdefault(v, set()).add(k[len(prefix):])
    return out


def doc_rows(start, end):
    """{name: [values]} of the table rows "| `0xNN` | NAME |" between two headings."""
    text = _read("docs", "test-protocol.md")
    a = text.index(start)
    b = text.index(end, a)
    out = {}
    for m in re.finditer(r"^\| `(0x[0-9A-Fa-f]{2})` \| ([A-Z0-9_]+) \|", text[a:b], re.M):
        out.setdefault(m.group(2), []).append(int(m.group(1), 16))
    return out


class Numbering(unittest.TestCase):
    def check_unique(self, what, numbers):
        seen = {}
        for name, value in numbers.items():
            self.assertNotIn(value, seen, "%s 0x%02x: %s and %s" % (what, value, seen.get(value),
                                                                   name))
            seen[value] = name

    def test_ops(self):
        c = c_numbers("OP")
        self.check_unique("op", c)
        py = py_numbers("OP_", (C, H))
        for value, names in py.items():
            # one number, one op (the same op may have a second name, as OP_HAP_* in config)
            self.assertEqual(len(names), 1, "op 0x%02x has the names %s" % (value, sorted(names)))
        self.assertEqual(sorted(c.values()), sorted(py),
                         "the firmware's ops and the runner's differ")
        docs = doc_rows("## The test channel", "## The ring")
        for name, value in c.items():
            self.assertEqual(docs.get(name), [value],
                             "docs/test-protocol.md: op %s (0x%02x) once in the table" % (name,
                                                                                          value))
        self.assertEqual(sorted(docs), sorted(c), "docs/test-protocol.md lists other ops")

    def test_ring_kinds(self):
        c = c_numbers("KIND")
        self.check_unique("ring kind", c)
        py = py_numbers("RING_", (C, H), skip=("RING_LINK_",))
        for value, names in py.items():
            self.assertEqual(len(names), 1, "ring kind 0x%02x has the names %s" % (value,
                                                                                 sorted(names)))
        self.assertEqual(sorted(c.values()), sorted(py),
                         "the firmware's ring kinds and the runner's differ")
        self.assertEqual(sorted(S.KINDS), sorted(v for k, v in c.items()
                                                 if 0x50 <= v <= 0x57))
        self.assertEqual(sorted(H.RING_KINDS), sorted(v for k, v in c.items()
                                                      if 0x60 <= v <= 0x63))
        docs = doc_rows("## The ring", "## The console")
        for name, value in c.items():
            self.assertEqual(docs.get(name), [value],
                             "docs/test-protocol.md: ring kind %s (0x%02x) once in the table"
                             % (name, value))
        self.assertEqual(sorted(docs), sorted(c), "docs/test-protocol.md lists other kinds")

    def test_caps_flags(self):
        c = c_flags()
        self.check_unique("caps bit", c)
        py = py_numbers("CAP_", (C, H))
        for bit, names in py.items():
            self.assertEqual(len(names), 1, "caps bit %d has the names %s" % (bit, sorted(names)))
        self.assertEqual(sorted(c.values()), sorted(py),
                         "the firmware's caps bits and the runner's differ")
        self.assertEqual(H.CAP_HAPTICS_SYNTH, C.CAP_HAPTICS_SYNTH)

    def test_links(self):
        text = _read("lib", "opensc2", "test_core.h")
        links = {m.group(1): int(m.group(2)) for m in
                 re.finditer(r"\bOSC2_TEST_LINK_([A-Z]+) = (\d+)", text)}
        self.assertEqual(links, {"RADIO": 0, "USB": 1, "TEST": 2, "NONE": 3, "BLE": 4, "DOCK": 5})
        self.assertEqual(C.RING_LINK_NAMES, {0: "radio", 1: "usb", 2: "test", 3: "none",
                                             4: "ble", 5: "dock"})
        self.assertEqual((C.RING_LINK_BLE, C.RING_LINK_DOCK), (links["BLE"], links["DOCK"]))
