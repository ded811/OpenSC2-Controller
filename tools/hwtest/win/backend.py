# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# backend.py: the hardware backend (Windows). It lists the devices without opening them
# (devices.py), lets safety.classify() decide what each one is, and opens only the board's vendor
# collection (hid.py), the board's console (serial.py, after the Rig's handshake), the puck's slots
# read-only (puck_read.py) and, with --through-puck, the board's proven slot for writing
# (puck_write.py, made only through safety.puck_writer()). Device paths and COM names are looked up
# again after every reboot and never printed.
#
# The puck: one 28DE:1304 device only (two are refused: their slots could not be told apart, S3).
# The board's slot is the reader that carried the signature; the leak detector, the through-puck
# writer and the handles of the Steam phase are bound to that exact node, never looked up again
# by slot number.
#
# Bluetooth (--bluetooth, path B): the board paired with Windows shows up as a Bluetooth HID device
# (28DE:1303) next to its console. Its vendor collection is "the board" only once
# safety.classify_ble() matched it to the address and serial the board's console gave (the Rig puts
# them in the safety Context); the board's HID handle is then that collection's, the same code as
# on USB (feature report 1, output reports, the input reader).
import os
import time

from .. import config as C
from .. import safety
from ..transport import Stall
from . import devices


class WinBackend:
    kind = "hw"
    models = set()

    def __init__(self, clock, options, masker):
        self.clock = clock
        self.opt = options
        self.mask = masker
        self.safety_ctx = None      # the Rig's live safety.Context (bound by the Rig)
        self._board = None          # hid.BoardHid
        self._console = None        # serial.ConsolePort
        self._slots = {}            # slot number -> puck_read.PuckSlotReader
        self._proven = None         # the board's slot number
        self._writer = None
        self._leak = None
        self._leak_epoch = float("inf")   # S13 counts events from the last fresh ARM on
        self._nodes = []
        self._nodes_at = 0.0
        self._last_mode = None
        self._last_board_node = None
        self._path = "W"            # the Rig's path: the board's handle is USB's (W) or Bluetooth's

    def use_path(self, path):
        """The Rig's path changed: the board's own handle is the USB one for W, the Bluetooth
        one for B (a handle of the other is closed)."""
        if path != self._path and path in ("W", "B"):
            self._close_board()
        self._path = path

    def _ble_expect(self):
        return getattr(self.safety_ctx, "ble_expect", None)

    def ble_nodes(self):
        """For messages: the Bluetooth 28DE:1303 collections Windows lists, as (is the board's,
        why). Nothing opened."""
        expect = self._ble_expect()
        out = []
        for n in self._enum():
            f = safety.ble_node_fields(n)
            if f and (f[0], f[1]) == (C.BOARD_VID, C.BLE_PID) and f[3] == "COL03":
                cls, why = safety.classify_ble(n, expect)
                out.append((cls == safety.BOARD_BLE, why))
        return out

    # ---- discovery (nothing opened) ----
    def _enum(self, fresh=False):
        if fresh or time.monotonic() - self._nodes_at > 0.2:
            self._nodes = devices.enumerate_nodes()
            self._nodes_at = time.monotonic()
        return self._nodes

    def discover(self):
        nodes = self._enum(fresh=True)
        boards, consoles, slots, uf2 = [], [], [], False
        for n in nodes:
            cls, _why = safety.classify(n)
            if cls == safety.BOARD_HID:
                boards.append(n)
            elif cls == safety.BOARD_CONSOLE:
                consoles.append(n)
            elif cls == safety.PUCK_SLOT:
                slots.append(n)
            elif "VID_239A" in ((n.get("instance_id") or "") + (n.get("interface_id") or "")).upper():
                uf2 = True
        pick = self.opt.get("board")
        extra = 0
        if len(boards) > 1 or len(consoles) > 1:
            if pick is None:
                extra = max(len(boards), len(consoles)) - 1
                for i, n in enumerate(boards + consoles):
                    print("  board %d: %s at %s" % (i, "wired" if n in boards else "wireless",
                                                    n.get("location") or "?"))
            else:
                both = boards + consoles
                if pick >= len(both):
                    extra = len(both)
                else:
                    chosen = both[pick]
                    boards = [chosen] if chosen in boards else []
                    consoles = [chosen] if chosen in consoles else []
        for s in slots:
            own = (s.get("own_instance_id") or s.get("interface_id") or "").upper()
            for mi in (2, 3, 4, 5):
                if "MI_0%d" % mi in own:
                    s["slot"] = mi
        pucks = {}
        for s in slots:
            pucks.setdefault((s.get("instance_id") or "").upper(), s.get("location") or "?")
        board = boards[0] if boards else None
        # B: the board's Bluetooth vendor collection, only by classify_ble (S1), only with the
        # console there (whose identity it was matched against) and --bluetooth
        ble_board = None
        expect = self._ble_expect()
        if self.opt.get("bluetooth") and consoles and expect is not None:
            ble = [n for n in nodes if safety.classify_ble(n, expect)[0] == safety.BOARD_BLE]
            ble_board = ble[0] if len(ble) == 1 else None
        mode = "W" if board else ("B" if ble_board else ("R" if consoles else None))
        self._last_mode = mode
        self._last_board_node = board or ble_board
        if (board is not None or ble_board is not None) and self._leak is not None:
            self._leak.watch(self._board_collections())
        return {"board": board, "console": consoles[0] if consoles else None,
                "ble_board": ble_board, "puck_slots": slots, "mode": mode,
                "extra_boards": extra, "uf2": uf2, "pucks": sorted(pucks.values())}

    def steam_running(self):
        from . import procs
        return procs.steam_running()

    def keep_awake(self, on):
        from . import power
        (power.keep_awake if on else power.allow_sleep)()

    # ---- the board's HID (W; B over Bluetooth) ----
    def _open_board(self):
        from . import hid
        if self._board is not None and not self._board.gone:
            return self._board
        self._close_board()
        if self._path == "B":
            node = self.discover().get("ble_board")
            if node is None:
                raise Stall(1167, "the board's Bluetooth device is not there")
            self._board = hid.BoardHid(node, writable=not self.steam_running(),
                                       ble_expect=self._ble_expect())
        else:
            node = self.discover().get("board")
            if node is None:
                raise Stall(1167, "the board is not there")
            self._board = hid.BoardHid(node, writable=not self.steam_running())
        self._board.start_reader()
        return self._board

    def _close_board(self):
        if self._board is not None:
            self._board.close()
            self._board = None

    def feature_set(self, buf):
        # S5/S7 with the live Context in BoardHid.feature_set, where the bytes go out
        self._open_board().feature_set(buf, self.safety_ctx)

    def feature_get(self):
        return self._open_board().feature_get()

    def feature_raw(self, report_id, buf=None):
        self._open_board().feature_raw(report_id, buf)

    def output_raw(self, kind, buf):
        self._open_board().output_raw(kind, buf)

    def write_output(self, report):
        self._open_board().write_output(report)     # checked there (safety.check_output_report)

    def hid_info(self):
        b = self._open_board()
        info = b.info()
        node = self._last_board_node or {}
        cols, composite = devices.collections_of(self._enum(), node.get("instance_id") or "")
        info.update(collections=cols, composite=composite)
        return info

    # ---- the console (R) ----
    def console_open_ok(self):
        try:
            self.console_port()
            return True
        except OSError as e:
            return e.errno != 5

    def console_port(self):
        from . import serial
        if self._console is not None and not self._console.gone:
            return self._console
        if self._console is not None:
            self._console.close()
            self._console = None
        node = self.discover().get("console")
        if node is None or not node.get("com"):
            raise OSError(2, "the board's console is not there")
        self._console = serial.ConsolePort(node)
        return self._console

    # ---- the stream ----
    def _reader(self, path):
        if path in ("W", "B"):
            return self._open_board()
        if self._proven is None or self._proven not in self._slots:
            raise Stall(1167, "the board's slot on the puck is not proven")
        return self._slots[self._proven]

    def read_stream(self, path, duration_s=None, count=None):
        """The reports as they arrive (over Bluetooth the state report is 0x45: the Rig reads it
        as 0x42 without the quaternion)."""
        reader = self._reader(path)
        got = []
        if duration_s is None:
            duration_s = (count or 0) * (0.012 if path == "B" else 0.006) + 1.0
        end = time.monotonic() + duration_s
        states = (C.RPT_STATE, C.RPT_BT_STATE) if path == "B" else (C.RPT_STATE,)
        while True:
            got += [(ts, r) for ts, r in reader.drain() if r]
            if count is not None and sum(1 for _t, r in got if r[0] in states) >= count:
                break
            if time.monotonic() >= end or reader.gone:
                break
            time.sleep(0.002)
        return got

    def flush(self, path):
        try:
            self._reader(path).drain()
        except Stall:
            pass

    # ---- the puck's slots (R, P) ----
    def _one_puck(self, d):
        """S3: the board's slot is told apart by its MI number on ONE puck. With two 28DE:1304
        devices (Valve's puck and a second puck or a puck-like dev board) a slot number names two
        devices: refused, the pucks listed by their USB location (no serial)."""
        pucks = d.get("pucks") or []
        if len(pucks) > 1:
            raise safety.SafetyError("%d pucks (28DE:1304) are connected, at %s: unplug every one "
                                     "but the puck the board is paired with (its slot could not "
                                     "be told apart, S3)" % (len(pucks), ", ".join(pucks)))

    def _drop_writer(self):
        if self._writer is not None:
            try:
                self._writer.close()
            finally:
                self._writer = None

    def _open_slots(self, sole):
        from . import puck_read
        self._close_slots()
        self._drop_writer()               # a new proof follows: the old writer is not proven
        d = self.discover()
        self._one_puck(d)
        for node in d.get("puck_slots", []):
            slot = node.get("slot")
            if slot is None:
                continue
            old = self._slots.pop(slot, None)
            if old is not None:
                old.close()               # never a reader left open behind a replaced one
            self._slots[slot] = puck_read.PuckSlotReader(node, sole=sole)
            self._slots[slot].start()
        return self._slots

    def _close_slots(self, keep=None):
        for n, r in list(self._slots.items()):
            if n != keep:
                r.close()
                del self._slots[n]

    def _proven_node(self):
        """The node of the reader that carried the board's signature (it keeps its "slot")."""
        r = self._slots.get(self._proven) if self._proven is not None else None
        return getattr(r, "node", None)

    def read_slots(self, duration_s, before=None):
        """Every slot opened as its only reader (S12), `before()` run (the signature injected),
        then all read for `duration_s`. None when the puck is not there; SafetyError for two
        pucks."""
        try:
            slots = self._open_slots(sole=not self.steam_running())
        except OSError as e:
            self._close_slots()
            if e.errno == C.ERROR_SHARING_VIOLATION:
                raise safety.SafetyError("another program reads one of the puck's slots (S12)")
            return None
        if not slots:
            return None
        if not self._slot_selfcheck(next(iter(slots.values())).node):
            self._close_slots()
            raise safety.SafetyError("Windows does not enforce the sharing of the puck's slots "
                                     "(S12)")
        for r in slots.values():
            r.drain()
        try:
            if before:
                before()
            end = time.monotonic() + duration_s
            out = {n: [] for n in slots}
            while time.monotonic() < end:
                for n, r in slots.items():
                    out[n] += r.drain()
                time.sleep(0.01)
        except BaseException:
            self._close_slots()           # no proof: no slot stays held
            raise
        if all(r.gone for r in slots.values()):
            return None
        return out

    def _slot_selfcheck(self, node):
        """A second, shared, read-only open of a held slot must fail with a sharing violation."""
        from . import puck_read
        try:
            probe = puck_read.PuckSlotReader(node, sole=False)
        except OSError as e:
            return e.errno == C.ERROR_SHARING_VIOLATION
        probe.close()
        return False

    def set_board_slot(self, slot):
        """The proof found the board on `slot`: keep that reader (the one that carried the
        signature), close the others, and watch that node's mouse and keyboard collections
        (S13). The node is the reader's own (it keeps its "slot"): a fresh enumeration's node
        dicts have none."""
        if slot != self._proven:
            self._drop_writer()
        self._proven = slot
        self._close_slots(keep=slot)
        if self._leak is not None:
            self._leak.watch(self._slot_collections(self._proven_node()))

    def puck_writer(self):
        """Called only by safety.puck_writer() (S3): the writer of the exact node that carried
        the signature (never another device's slot with the same MI number)."""
        from . import puck_write
        node = self._proven_node()
        if self._writer is not None and getattr(self._writer, "node", None) is not node:
            self._drop_writer()
        if self._writer is None:
            if node is None:
                raise Stall(1167, "the board's slot on the puck is not there")
            self._writer = puck_write.PuckSlotWriter(node, self.safety_ctx)
        return self._writer

    def share_for_steam(self, on):
        """[R12] before Steam starts (on) the sole-reader handles are closed, so that Steam can
        open the board's collection or its slot, and the slot is opened again shared and
        read-only; after Steam closed (off) they are taken back. The board's own handle is opened
        again on its next use, shared and read-only while Steam runs (_open_board). The slot is
        the proven reader's node (not a slot number looked up again)."""
        from . import puck_read
        self._close_board()
        self._drop_writer()
        slot = self._proven
        if slot is None:
            return
        node = self._proven_node()
        self._close_slots()
        if node is None:
            self._proven = None
            return
        try:
            self._slots[slot] = puck_read.PuckSlotReader(node, sole=not on)
            self._slots[slot].start()
        except OSError:
            self._proven = None           # the slot is proved again before it is read
            if self._leak is not None:
                self._leak.watch([])

    # ---- S12: the sole reader ----
    def _own_node(self):
        """The board's own HID node on the current path (W: USB, B: Bluetooth), or None (R)."""
        d = self.discover()
        return d.get("ble_board") if self._path == "B" else d.get("board")

    def sole_reader_ok(self):
        if self._own_node() is None:
            return True     # R: the slots are held when read_slots opens them
        try:
            self._open_board()
            return True
        except OSError as e:
            return e.errno != C.ERROR_SHARING_VIOLATION

    def sole_reader_selfcheck_ok(self):
        from . import hid
        node = self._own_node()
        if node is None:
            return True
        try:
            probe = hid.BoardHid(node, writable=False, retry_s=0,
                                 ble_expect=self._ble_expect() if self._path == "B" else None)
        except OSError as e:
            return e.errno == C.ERROR_SHARING_VIOLATION
        probe.close()
        return False

    # ---- S13: the leak detector ----
    def _board_collections(self):
        """The board's mouse and keyboard collections (Col01, Col02) on USB (W) or over
        Bluetooth (B): the nodes of the same device as its vendor collection."""
        node = self._last_board_node or self.discover().get("board") or {}
        usb = (node.get("instance_id") or "").upper()
        return [n["own_instance_id"] for n in self._enum()
                if n["kind"] == "hid" and (n.get("instance_id") or "").upper() == usb and
                n.get("own_instance_id") and
                any(c in n["own_instance_id"].upper() for c in ("&COL01", "&COL02"))]

    def _slot_collections(self, node):
        if not node:
            return []
        usb = (node.get("instance_id") or "").upper()
        mi = "MI_0%d" % node.get("slot", 0)
        return [n["own_instance_id"] for n in self._enum()
                if n["kind"] == "hid" and (n.get("instance_id") or "").upper() == usb and
                n.get("own_instance_id") and mi in n["own_instance_id"].upper() and
                any(c in n["own_instance_id"].upper() for c in ("&COL01", "&COL02"))]

    def start_leak_detector(self):
        from . import rawinput
        self._leak = rawinput.LeakDetector(self._board_collections())
        return self._leak.start(C.LEAK_WINDOW_READY_S)

    def leak_watching(self):
        """S13 can see a leak on the current path: the window is up, and the board's two
        collections (W) or the proven slot's two (R), Col01 and Col02, are watched and listed as
        raw-input devices. False means the detector is off there: --lizard is refused."""
        if self._leak is None or not self._leak.watching():
            return False
        names = [n.upper() for n in self._leak.watched()]
        return any("&COL01" in n for n in names) and any("&COL02" in n for n in names)

    def leak_fired(self):
        """An event from a watched collection since the last fresh ARM (leak_epoch)."""
        return bool(self._leak and self._leak.fired_since(self._leak_epoch))

    def leak_epoch(self, grace_s=0.1):
        """A fresh ARM: from now on (after `grace_s`, for the two reports the ARM itself releases
        before its guard, R6) an event from a watched collection is a leak. What came while the
        runner was not armed is lizard mode's own, on the PC by right."""
        self._leak_epoch = time.monotonic() + grace_s

    def leak_quiet(self, on, grace_s=1.0):
        """The runner's own link drop begins (on) or ends: the puck's empty keyboard and mouse
        reports on the slot are not leaks (counted as leak_ignored)."""
        if self._leak is not None:
            self._leak.suppress(on, grace_s)

    def leak_ignored(self):
        return self._leak.ignored if self._leak is not None else 0

    # ---- reboots ----
    def _close_all(self):
        self._close_board()
        if self._console is not None:
            self._console.close()
            self._console = None
        self._close_slots()
        self._proven = None
        self._drop_writer()
        if self._leak is not None:
            self._leak.watch([])          # watched again once the board is found (W) or proved (R)

    def find_board(self, timeout):
        """After an unplanned reset: every handle closed, the board looked for on both
        paths every 200 ms until it is back (its mode). Nothing is opened meanwhile; the UF2
        bootloader appearing stops the run (S11)."""
        self._close_all()
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            d = self.discover()
            if d.get("uf2"):
                raise safety.SafetyError("the UF2 bootloader appeared: double-tap reset or replug "
                                         "the board to leave it (exit 3)")
            if d.get("mode") is not None:
                time.sleep(0.3)        # let Windows finish installing the interfaces
                return self.discover().get("mode")
            time.sleep(C.POLL_INTERVAL_S)
        raise TimeoutError("the board did not come back within %.0f s: unplug and replug it"
                           % timeout)

    def wait_reboot(self, from_mode, to_mode, timeout, gone_timeout=None):
        """The board's device of `from_mode` gone (within `gone_timeout`, default
        C.GONE_TIMEOUT_S * 2, from the call), then the board back on any path: its mode. The UF2
        bootloader appearing stops the run (S11). Nothing is opened meanwhile."""
        t_start = time.monotonic()
        self._close_all()
        t_closed = time.monotonic()
        trace = os.environ.get("HWTEST_TRACE")
        gone_s = float(os.environ.get("HWTEST_GONE_S") or gone_timeout or C.GONE_TIMEOUT_S * 2)
        gone_by = t_closed + gone_s
        gone = False
        polls = 0
        while time.monotonic() < gone_by:
            d = self.discover()
            polls += 1
            if d.get("mode") != from_mode:
                gone = True
                break
            time.sleep(C.POLL_INTERVAL_S)
        if trace:
            print("  [trace] wait_reboot %s->%s: close %.2f s, gone=%s after %.2f s (%d polls)"
                  % (from_mode, to_mode, t_closed - t_start, gone, time.monotonic() - t_start,
                     polls), flush=True)
        if not gone:
            raise TimeoutError("the board did not leave within %.0f s (no reboot; closing the "
                               "handles took %.2f s, %d polls all saw it)"
                               % (gone_s, t_closed - t_start, polls))
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            d = self.discover()
            if d.get("uf2"):
                raise safety.SafetyError("the UF2 bootloader appeared: double-tap reset or replug "
                                         "the board to leave it (exit 3)")
            if d.get("mode") is not None:
                time.sleep(0.3)        # let Windows finish installing the interfaces
                if trace:
                    print("  [trace] wait_reboot: back as %s after %.2f s"
                          % (d.get("mode"), time.monotonic() - t_start), flush=True)
                return self.discover().get("mode")
            time.sleep(C.POLL_INTERVAL_S)
        raise TimeoutError("the board did not come back within %.0f s: unplug and replug it"
                           % timeout)

    # ---- Steam (opt-in) ----
    def _steam_log(self):
        from . import procs
        return os.path.join(procs.steam_folder(), "logs", "controller.txt")

    def steam_log_mark(self):
        path = self._steam_log()
        return os.path.getsize(path) if os.path.isfile(path) else None

    def steam_log_since(self, mark):
        from ..proto import steamlog
        path = self._steam_log()
        size = os.path.getsize(path)
        start = 0 if steamlog.Tail.rotated(mark, size) else mark
        with open(path, "rb") as f:
            f.seek(start)
            data = f.read()
        return steamlog.Tail().read_added(data)

    def steam_start(self):
        os.startfile("steam://open/main")
        end = time.monotonic() + 60
        while time.monotonic() < end and not self.steam_running():
            time.sleep(1.0)
        time.sleep(20.0)

    def steam_stop(self):
        import subprocess
        from . import procs
        exe = os.path.join(procs.steam_folder(), "steam.exe")
        if os.path.isfile(exe):
            subprocess.Popen([exe, "-shutdown"])
        end = time.monotonic() + 60
        while time.monotonic() < end and self.steam_running():
            time.sleep(1.0)

    def close(self):
        self._close_all()
