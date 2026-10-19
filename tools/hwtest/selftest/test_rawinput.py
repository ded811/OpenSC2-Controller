# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# test_rawinput.py (S13): the leak detector's name filter on synthetic device names,
# and a source check that it never asks for RID_INPUT (only RID_HEADER). Pure; no window is made.
import ast
import os
import unittest

from hwtest.win import rawinput


class LeakFilter(unittest.TestCase):
    def setUp(self):
        self.watched = [
            r"\\?\HID#VID_28DE&PID_1302&Col01#abc",
            r"\\?\HID#VID_28DE&PID_1302&Col02#abc",
        ]

    def test_board_collections_are_leaks(self):
        self.assertTrue(rawinput.is_leak(r"\\?\HID#VID_28DE&PID_1302&Col01#abc#{guid}",
                                         self.watched))
        self.assertTrue(rawinput.is_leak(r"\\?\hid#vid_28de&pid_1302&col02#abc", self.watched))

    def test_others_not_leaks(self):
        self.assertFalse(rawinput.is_leak(r"\\?\HID#VID_046D&PID_C33A&Col01#x", self.watched))
        self.assertFalse(rawinput.is_leak(r"\\?\HID#VID_28DE&PID_1302&Col03#x", self.watched))
        self.assertFalse(rawinput.is_leak("", self.watched))
        self.assertFalse(rawinput.is_leak(None, self.watched))

    def test_source_never_requests_rid_input(self):
        path = os.path.join(os.path.dirname(os.path.dirname(__file__)), "win", "rawinput.py")
        with open(path, encoding="utf-8") as f:
            src = f.read()
        names = set()
        for n in ast.walk(ast.parse(src)):
            if isinstance(n, ast.Name):
                names.add(n.id)
        self.assertIn("RID_HEADER", names)
        self.assertNotIn("RID_INPUT", names)


class Watching(unittest.TestCase):
    """What the detector can see (listed raw-input devices), and what counts."""

    BOARD = [r"HID\VID_28DE&PID_1302&COL01\7&ABC&0&0000",
             r"HID\VID_28DE&PID_1302&COL02\7&ABC&0&0001"]
    RAW = [r"\\?\HID#VID_28DE&PID_1302&Col01#7&abc&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}",
           r"\\?\HID#VID_28DE&PID_1302&Col02#7&abc&0&0001#{884b96c3-56ef-11d1-bc8c-00a0c91405dd}",
           r"\\?\HID#VID_046D&PID_C33A&Col01#8&1&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}"]

    def test_listed(self):
        self.assertTrue(rawinput.listed(self.BOARD, self.RAW))
        self.assertFalse(rawinput.listed(self.BOARD, self.RAW[1:]))     # Col01 not listed
        self.assertFalse(rawinput.listed([], self.RAW))                 # nothing watched

    def test_watching_needs_the_window_and_the_list(self):
        det = rawinput.LeakDetector(self.BOARD, lister=lambda: self.RAW)
        self.assertFalse(det.watching())                # no window yet
        det._ready.set()
        self.assertTrue(det.watching())
        det.watch([])
        self.assertFalse(det.watching())                # an empty watch list is "off"
        det.watch(self.BOARD)
        self.assertTrue(det.watching())
        unlisted = rawinput.LeakDetector(self.BOARD, lister=lambda: self.RAW[2:])
        unlisted._ready.set()
        self.assertFalse(unlisted.watching())
        broken = rawinput.LeakDetector(self.BOARD, lister=lambda: None)
        broken._ready.set()
        self.assertFalse(broken.watching())             # the list could not be read

    def test_events_counted_only_outside_quiet_windows(self):
        t = [10.0]
        det = rawinput.LeakDetector(self.BOARD, lister=lambda: self.RAW, clock=lambda: t[0])
        det.record()
        self.assertTrue(det.fired_since(10.0))
        self.assertFalse(det.fired_since(10.5))         # before the epoch: not a leak
        det.suppress(True)
        t[0] = 11.0
        det.record()
        det.suppress(False, grace_s=1.0)
        t[0] = 11.9
        det.record()
        self.assertFalse(det.fired_since(10.5))
        self.assertEqual(det.ignored, 2)
        t[0] = 12.1
        det.record()
        self.assertTrue(det.fired_since(10.5))


if __name__ == "__main__":
    unittest.main()
