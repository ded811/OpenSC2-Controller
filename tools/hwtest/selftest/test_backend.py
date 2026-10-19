# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# test_backend.py (S3, S13): the Windows backend's handling of the puck's slots and of
# the leak detector's watch list, on synthetic device lists. Nothing is opened: the enumeration,
# the slot readers, the through-puck writer and the leak detector are fakes put in their place,
# and the backend's Steam check is replaced. Windows only (win/ imports ctypes.wintypes).
import sys
import time
import unittest

from hwtest import config as C
from hwtest import safety

if sys.platform == "win32":
    from hwtest.win import backend as B
    from hwtest.win import devices as D
    from hwtest.win import puck_read, puck_write

BS = "\\"


def puck_nodes(serial="FAKEPUCK00001", location="Port_#0001.Hub_#0001", inst="8&1111&0"):
    usb = "USB" + BS + "VID_28DE&PID_1304" + BS + serial
    out = []
    for mi in (2, 3, 4, 5):
        for col in ("COL01", "COL02", "COL03"):
            own = "HID" + BS + "VID_28DE&PID_1304&MI_0%d&%s" % (mi, col) + BS + \
                "%s&000%d" % (inst, mi)
            out.append({"kind": "hid", "interface_id": "x#vid_28de&pid_1304&mi_0%d&%s#%s#y"
                        % (mi, col.lower(), inst), "own_instance_id": own, "instance_id": usb,
                        "serial": serial, "usb_product": None, "location": location,
                        "com": None})
    return out


class FakeLeak:
    def __init__(self):
        self.calls = []

    def watch(self, names):
        self.calls.append(list(names))


class FakeReader:
    made = []

    def __init__(self, node, sole=True):
        if not sole:                       # the S12 self-check: Windows enforces the sharing
            raise OSError(C.ERROR_SHARING_VIOLATION, "sharing violation")
        self.node = node
        self.closed = False
        self.gone = False
        FakeReader.made.append(self)

    def start(self):
        pass

    def drain(self):
        return []

    def close(self):
        self.closed = True


class FakeWriter:
    def __init__(self, node, ctx=None):
        self.node = node
        self.ctx = ctx
        self.closed = False

    def close(self):
        self.closed = True


@unittest.skipUnless(sys.platform == "win32", "the Windows backend")
class Slots(unittest.TestCase):
    def setUp(self):
        self._saved = (D.enumerate_nodes, puck_read.PuckSlotReader, puck_write.PuckSlotWriter)
        self.nodes = puck_nodes()
        D.enumerate_nodes = lambda: [dict(n) for n in self.nodes]   # fresh dicts every time
        puck_read.PuckSlotReader = FakeReader
        puck_write.PuckSlotWriter = FakeWriter
        FakeReader.made = []
        self.be = B.WinBackend(clock=None, options={}, masker=None)
        self.be.steam_running = lambda: False

    def tearDown(self):
        D.enumerate_nodes, puck_read.PuckSlotReader, puck_write.PuckSlotWriter = self._saved

    def _prove(self, slot, gap_s=0.3):
        out = self.be.read_slots(0.02)
        self.assertEqual(sorted(out), [2, 3, 4, 5])
        self.be._nodes_at -= gap_s         # the 0.2 s enumeration cache has run out
        self.be.set_board_slot(slot)

    def test_watch_list_after_a_gap(self):
        """safety-HW-1: the proven slot's Col01/Col02 are watched however long the proof took
        (a fresh enumeration's node dicts carry no "slot")."""
        for gap in (0.0, 0.3, 2.1):
            self.be._leak = FakeLeak()
            self._prove(3, gap)
            watched = self.be._leak.calls[-1]
            self.assertEqual(len(watched), 2, gap)
            self.assertTrue(all("MI_03" in w and ("COL01" in w or "COL02" in w)
                                for w in watched), watched)

    def test_two_pucks_are_refused(self):
        """safety-HW-2: two 28DE:1304 devices: no slot proof (exit 2), the pucks named by their
        USB location, and no reader left open."""
        self.nodes = puck_nodes() + puck_nodes("FAKEPUCK00002", "Port_#0004.Hub_#0002",
                                               "8&2222&0")
        with self.assertRaises(safety.SafetyError) as e:
            self.be.read_slots(0.02)
        self.assertIn("2 pucks", str(e.exception))
        self.assertIn("Port_#0004.Hub_#0002", str(e.exception))
        self.assertNotIn("FAKEPUCK", str(e.exception))
        self.assertEqual(self.be._slots, {})
        self.assertTrue(all(r.closed for r in FakeReader.made))

    def test_writer_bound_to_the_proven_node(self):
        """safety-HW-2, correctness-stale-puck-writer: the writer is the proven reader's node,
        and a new proof (the slots reopened) or another slot drops the old writer."""
        self._prove(4)
        proven = self.be._slots[4].node
        w = self.be.puck_writer()
        self.assertIs(w.node, proven)
        self.assertIs(self.be.puck_writer(), w)          # cached while the proof stands
        self._prove(4)
        self.assertTrue(w.closed)
        w2 = self.be.puck_writer()
        self.assertIs(w2.node, self.be._slots[4].node)
        self._prove(2)
        self.assertTrue(w2.closed)
        self.assertEqual(self.be.puck_writer().node["slot"], 2)

    def test_steam_share_reopens_the_proven_node(self):
        self._prove(5)
        proven = self.be._slots[5].node
        self.be._close_board = lambda: None
        self.be.share_for_steam(True)
        # shared while Steam runs: the fake refuses a shared open, so the slot is not held and
        # is proved again before it is read
        self.assertIsNone(self.be._proven)
        self.assertEqual(self.be._slots, {})
        self._prove(5)
        self.be.share_for_steam(False)
        self.assertEqual(self.be._slots[5].node["own_instance_id"], proven["own_instance_id"])

    def test_no_reader_left_behind(self):
        """A failure while the signature is injected closes every slot held (no proof)."""
        def boom():
            raise safety.SafetyError("injection refused")
        with self.assertRaises(safety.SafetyError):
            self.be.read_slots(0.02, before=boom)
        self.assertEqual(self.be._slots, {})
        self.assertTrue(all(r.closed for r in FakeReader.made))

    def test_leak_watching_needs_both_collections(self):
        from hwtest.win import rawinput
        names = [n["own_instance_id"] for n in self.nodes]
        listed = ["\\\\?\\" + n.replace(BS, "#") + "#{guid}" for n in names]
        det = rawinput.LeakDetector([], lister=lambda: listed)
        det._ready.set()                   # no window in a self-test
        self.be._leak = det
        self.assertFalse(self.be.leak_watching())          # nothing watched yet (R, unproved)
        self._prove(3)
        self.assertTrue(self.be.leak_watching())
        det.watch([w for w in det.watched() if "COL01" in w])
        self.assertFalse(self.be.leak_watching())          # the keyboard collection missing
        self.be._close_all()
        self.assertFalse(self.be.leak_watching())          # a reboot: watched again later

    def test_leak_epoch_and_quiet(self):
        from hwtest.win import rawinput
        t = [100.0]
        det = rawinput.LeakDetector(["HID\\VID_28DE&PID_1304&MI_03&COL01\\X"],
                                    lister=lambda: [], clock=lambda: t[0])
        self.be._leak = det
        det.record()                       # while unarmed (no epoch yet): lizard mode's own
        self.assertFalse(self.be.leak_fired())
        self.be._leak_epoch = 101.0
        t[0] = 100.5
        det.record()
        self.assertFalse(self.be.leak_fired())
        self.be.leak_quiet(True)
        t[0] = 102.0
        det.record()                       # the runner's own link drop: ignored
        self.be.leak_quiet(False, 1.0)
        t[0] = 102.5
        det.record()                       # inside the grace after it
        self.assertFalse(self.be.leak_fired())
        self.assertEqual(self.be.leak_ignored(), 2)
        t[0] = 104.0
        det.record()
        self.assertTrue(self.be.leak_fired())


class FakeKernel32:
    """CreateToolhelp32Snapshot and Process32First/NextW as the bound prototypes return them."""

    def __init__(self, snap, names=()):
        self.snap = snap
        self.names = list(names)
        self.closed = []

    def CreateToolhelp32Snapshot(self, flags, pid):
        return self.snap

    def _fill(self, ref):
        if not self.names:
            return False
        ref._obj.szExeFile = self.names.pop(0)
        return True

    def Process32FirstW(self, snap, ref):
        return self._fill(ref)

    def Process32NextW(self, snap, ref):
        return self._fill(ref)

    def CloseHandle(self, h):
        self.closed.append(h)
        return True


@unittest.skipUnless(sys.platform == "win32", "the Windows process list")
class Procs(unittest.TestCase):
    """safety-HW-10: the snapshot result is a HANDLE compared with INVALID_HANDLE_VALUE, and a
    process list that cannot be read counts as Steam running (fail closed)."""

    def test_bound_as_handles(self):
        from ctypes import wintypes
        from hwtest.win import procs
        k = procs._kernel32()
        self.assertIs(k.CreateToolhelp32Snapshot.restype, wintypes.HANDLE)
        self.assertEqual(k.Process32FirstW.argtypes[0], wintypes.HANDLE)

    def test_fails_closed(self):
        from hwtest.win import procs
        self.assertTrue(procs.steam_running(FakeKernel32(procs._INVALID)))
        self.assertTrue(procs.steam_running(FakeKernel32(None)))
        self.assertTrue(procs.steam_running(FakeKernel32(1234, [])))     # no first entry

    def test_names(self):
        from hwtest.win import procs
        k = FakeKernel32(1234, ["System", "Steam.exe"])
        self.assertTrue(procs.steam_running(k))
        self.assertEqual(k.closed, [1234])
        self.assertFalse(procs.steam_running(FakeKernel32(1234, ["System", "explorer.exe"])))


if __name__ == "__main__":
    unittest.main()
