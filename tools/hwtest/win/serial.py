# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# serial.py: the board's Zephyr console as a Win32 serial port (docs/testing.md). Opened only for a
# node safety.classify() called BOARD_CONSOLE (2FE3:0004, "OpenSC2 console"), then the Rig's
# handshake decides whether it is used. 115200 baud, never 1200 (no "1200 bps touch"), DTR and RTS
# on (the shell sends nothing until DTR is set). Overlapped I/O, so a write never waits for the
# reader's pending read. A reader thread splits the stream into lines (LF; CR and VT100 removed).
# A failing read or write (the board rebooted) marks the port gone.
import ctypes
import threading
from ctypes import wintypes

from .. import safety
from ..proto import console as proto

_GENERIC_READ = 0x80000000
_GENERIC_WRITE = 0x40000000
_OPEN_EXISTING = 3
_FILE_FLAG_OVERLAPPED = 0x40000000
_INVALID = ctypes.c_void_p(-1).value
_ERROR_IO_PENDING = 997
_PURGE_RXCLEAR, _PURGE_TXCLEAR = 0x0008, 0x0004
_SETRTS, _SETDTR = 3, 5
_MAXDWORD = 0xFFFFFFFF
ERROR_ACCESS_DENIED = 5


class _OVERLAPPED(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_void_p), ("InternalHigh", ctypes.c_void_p),
                ("Offset", wintypes.DWORD), ("OffsetHigh", wintypes.DWORD),
                ("hEvent", wintypes.HANDLE)]


class _DCB(ctypes.Structure):
    _fields_ = [("DCBlength", wintypes.DWORD), ("BaudRate", wintypes.DWORD),
                ("fFlags", wintypes.DWORD), ("wReserved", wintypes.WORD),
                ("XonLim", wintypes.WORD), ("XoffLim", wintypes.WORD),
                ("ByteSize", ctypes.c_ubyte), ("Parity", ctypes.c_ubyte),
                ("StopBits", ctypes.c_ubyte), ("XonChar", ctypes.c_char),
                ("XoffChar", ctypes.c_char), ("ErrorChar", ctypes.c_char),
                ("EofChar", ctypes.c_char), ("EvtChar", ctypes.c_char),
                ("wReserved1", wintypes.WORD)]


class _TIMEOUTS(ctypes.Structure):
    _fields_ = [("ReadIntervalTimeout", wintypes.DWORD),
                ("ReadTotalTimeoutMultiplier", wintypes.DWORD),
                ("ReadTotalTimeoutConstant", wintypes.DWORD),
                ("WriteTotalTimeoutMultiplier", wintypes.DWORD),
                ("WriteTotalTimeoutConstant", wintypes.DWORD)]


def _bind():
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    POV = ctypes.POINTER(_OVERLAPPED)
    PDW = ctypes.POINTER(wintypes.DWORD)
    for name, args, res in (
            ("CreateFileW", [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                             wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE], wintypes.HANDLE),
            ("CloseHandle", [wintypes.HANDLE], wintypes.BOOL),
            ("ReadFile", [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, PDW, POV],
             wintypes.BOOL),
            ("WriteFile", [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, PDW, POV],
             wintypes.BOOL),
            ("GetOverlappedResult", [wintypes.HANDLE, POV, PDW, wintypes.BOOL], wintypes.BOOL),
            ("CancelIoEx", [wintypes.HANDLE, POV], wintypes.BOOL),
            ("CreateEventW", [ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR],
             wintypes.HANDLE),
            ("WaitForSingleObject", [wintypes.HANDLE, wintypes.DWORD], wintypes.DWORD),
            ("SetupComm", [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD], wintypes.BOOL),
            ("GetCommState", [wintypes.HANDLE, ctypes.POINTER(_DCB)], wintypes.BOOL),
            ("SetCommState", [wintypes.HANDLE, ctypes.POINTER(_DCB)], wintypes.BOOL),
            ("SetCommTimeouts", [wintypes.HANDLE, ctypes.POINTER(_TIMEOUTS)], wintypes.BOOL),
            ("EscapeCommFunction", [wintypes.HANDLE, wintypes.DWORD], wintypes.BOOL),
            ("PurgeComm", [wintypes.HANDLE, wintypes.DWORD], wintypes.BOOL)):
        fn = getattr(k, name)
        fn.argtypes, fn.restype = args, res
    return k


class ConsolePort:
    """The port interface the Rig's ConsoleSession uses: write(text), drain_lines(), gone."""

    def __init__(self, node):
        cls, _why = safety.classify(node)
        if cls != safety.BOARD_CONSOLE:
            raise safety.SafetyError("ConsolePort on a %s node" % cls)
        self._k = _bind()
        self._handle = self._k.CreateFileW("\\\\.\\" + node["com"], _GENERIC_READ | _GENERIC_WRITE,
                                           0, None, _OPEN_EXISTING, _FILE_FLAG_OVERLAPPED, None)
        if self._handle in (None, _INVALID):
            err = ctypes.get_last_error()
            raise OSError(err, "the board's console could not be opened (error %d)%s" % (
                err, ": close the program that holds it" if err == ERROR_ACCESS_DENIED else ""))
        self._k.SetupComm(self._handle, 4096, 4096)
        self._configure()
        self._k.PurgeComm(self._handle, _PURGE_RXCLEAR | _PURGE_TXCLEAR)
        self.gone = False
        self._buf = ""
        self._lines = []
        self._lock = threading.Lock()
        self._wlock = threading.Lock()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _configure(self):
        dcb = _DCB()
        dcb.DCBlength = ctypes.sizeof(dcb)
        self._k.GetCommState(self._handle, ctypes.byref(dcb))
        dcb.BaudRate = 115200          # ignored by CDC-ACM, but never 1200
        dcb.ByteSize = 8
        dcb.Parity = 0
        dcb.StopBits = 0
        # fBinary 1, fDtrControl DTR_CONTROL_ENABLE (bits 4-5 = 1), fRtsControl RTS_CONTROL_ENABLE
        # (bits 12-13 = 1); no hardware or XON/XOFF flow control
        dcb.fFlags = 0x1 | (0x1 << 4) | (0x1 << 12)
        self._k.SetCommState(self._handle, ctypes.byref(dcb))
        to = _TIMEOUTS(_MAXDWORD, _MAXDWORD, 50, 0, 1000)
        self._k.SetCommTimeouts(self._handle, ctypes.byref(to))
        self._k.EscapeCommFunction(self._handle, _SETDTR)
        self._k.EscapeCommFunction(self._handle, _SETRTS)

    def _io(self, call, timeout_ms, keep_waiting=False):
        """One overlapped read or write: the bytes moved, 0 when stopped, None on a failure (the
        port went away). `keep_waiting`: wait in `timeout_ms` steps until done or stopped."""
        ev = self._k.CreateEventW(None, True, False, None)
        ov = _OVERLAPPED()
        ov.hEvent = ev
        got = wintypes.DWORD(0)
        try:
            if not call(ctypes.byref(ov)):
                if ctypes.get_last_error() != _ERROR_IO_PENDING:
                    return None
                while self._k.WaitForSingleObject(ev, timeout_ms) != 0:
                    if keep_waiting and not self._stop.is_set():
                        continue
                    self._k.CancelIoEx(self._handle, ctypes.byref(ov))
                    self._k.GetOverlappedResult(self._handle, ctypes.byref(ov),
                                                ctypes.byref(got), True)
                    return 0 if self._stop.is_set() else None
            if not self._k.GetOverlappedResult(self._handle, ctypes.byref(ov), ctypes.byref(got),
                                               False):
                return None
            return got.value
        finally:
            self._k.CloseHandle(ev)

    def _run(self):
        buf = (ctypes.c_ubyte * 512)()
        while not self._stop.is_set():
            n = self._io(lambda pov: self._k.ReadFile(self._handle, buf, 512, None, pov), 100,
                         keep_waiting=True)
            if n is None:
                self.gone = True
                return
            if n:
                self._feed(bytes(buf[:n]).decode("ascii", "replace"))

    def _feed(self, text):
        with self._lock:
            self._buf += text
            while "\n" in self._buf:
                line, self._buf = self._buf.split("\n", 1)
                self._lines.append(proto.strip_vt100(line))

    def write(self, text):
        data = text.encode("ascii", "replace")
        cbuf = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
        with self._wlock:
            n = self._io(lambda pov: self._k.WriteFile(self._handle, cbuf, len(data), None, pov),
                         1000)
        if n is None:
            self.gone = True
            raise OSError(1167, "the console went away")

    def drain_lines(self):
        with self._lock:
            out, self._lines = self._lines, []
        return out

    def close(self):
        if self._handle in (None, _INVALID):
            return
        self._stop.set()
        self._k.CancelIoEx(self._handle, None)
        self._thread.join(timeout=1.0)
        self._k.CloseHandle(self._handle)
        self._handle = None
