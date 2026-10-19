# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# puck_read.py: the puck's slot collections, opened READ-ONLY (S2). Missing write access does not by
# itself stop feature or output reports through the HID IOCTLs, so this module is the rule in code:
# it binds, from hid.dll and kernel32.dll, only the functions in _BINDINGS below (reading, sizing
# this handle's own input queue, the capabilities) and has no method that sends. test_safety.py
# parses this file and fails on any other bound name. It imports nothing from win/hid.py or
# win/puck_write.py. The only code that sends to a puck is win/puck_write.py, reached only through
# safety.puck_writer().
import collections
import ctypes
import threading
import time
from ctypes import wintypes

from .. import config as C
from .. import safety

_GENERIC_READ = 0x80000000
_FILE_SHARE_READ = 0x1
_FILE_SHARE_WRITE = 0x2
_OPEN_EXISTING = 3
_FILE_FLAG_OVERLAPPED = 0x40000000
_INVALID = ctypes.c_void_p(-1).value
_ERROR_IO_PENDING = 997

# Every function this module binds. Nothing that sends is allowed here (S2).
_BINDINGS = {
    "kernel32": ("CreateFileW", "ReadFile", "GetOverlappedResult", "CancelIoEx", "CloseHandle",
                 "CreateEventW", "WaitForSingleObject"),
    "hid": ("HidD_GetAttributes", "HidD_GetPreparsedData", "HidD_FreePreparsedData",
            "HidP_GetCaps", "HidP_GetValueCaps", "HidD_SetNumInputBuffers"),
}


class _OVERLAPPED(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_void_p), ("InternalHigh", ctypes.c_void_p),
                ("Offset", wintypes.DWORD), ("OffsetHigh", wintypes.DWORD),
                ("hEvent", wintypes.HANDLE)]


class _Caps(ctypes.Structure):
    _fields_ = [("Usage", wintypes.USHORT), ("UsagePage", wintypes.USHORT),
                ("InputReportByteLength", wintypes.USHORT),
                ("OutputReportByteLength", wintypes.USHORT),
                ("FeatureReportByteLength", wintypes.USHORT),
                ("Reserved", wintypes.USHORT * 17), ("Counts", wintypes.USHORT * 10)]


def _bind():
    """The bound functions, by name, with their prototypes."""
    POV = ctypes.POINTER(_OVERLAPPED)
    PDW = ctypes.POINTER(wintypes.DWORD)
    protos = {
        "CreateFileW": ([wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                         wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE], wintypes.HANDLE),
        "ReadFile": ([wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, PDW, POV], wintypes.BOOL),
        "GetOverlappedResult": ([wintypes.HANDLE, POV, PDW, wintypes.BOOL], wintypes.BOOL),
        "CancelIoEx": ([wintypes.HANDLE, POV], wintypes.BOOL),
        "CloseHandle": ([wintypes.HANDLE], wintypes.BOOL),
        "CreateEventW": ([ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR],
                         wintypes.HANDLE),
        "WaitForSingleObject": ([wintypes.HANDLE, wintypes.DWORD], wintypes.DWORD),
        "HidD_GetAttributes": ([wintypes.HANDLE, ctypes.c_void_p], wintypes.BOOLEAN),
        "HidD_GetPreparsedData": ([wintypes.HANDLE, ctypes.POINTER(ctypes.c_void_p)],
                                  wintypes.BOOLEAN),
        "HidD_FreePreparsedData": ([ctypes.c_void_p], wintypes.BOOLEAN),
        "HidP_GetCaps": ([ctypes.c_void_p, ctypes.POINTER(_Caps)], wintypes.LONG),
        "HidP_GetValueCaps": ([ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p],
                              wintypes.LONG),
        "HidD_SetNumInputBuffers": ([wintypes.HANDLE, wintypes.ULONG], wintypes.BOOLEAN),
    }
    fns = {}
    for dll_name, names in _BINDINGS.items():
        dll = ctypes.WinDLL(dll_name, use_last_error=True)
        for name in names:
            fn = getattr(dll, name)
            fn.argtypes, fn.restype = protos[name]
            fns[name] = fn
    return fns


class PuckSlotReader:
    """Reads the input reports of one puck slot's vendor collection. Read-only: the handle carries
    GENERIC_READ only (asserted) and nothing bound here can send. `sole`: sharing
    FILE_SHARE_WRITE only, so no other program can read the slot while the runner injects (S12)
    while the through-puck writer still fits beside it; else shared read and write (Steam open)."""

    ACCESS = _GENERIC_READ

    def __init__(self, node, sole=True):
        cls, _why = safety.classify(node)
        if cls != safety.PUCK_SLOT:
            raise safety.SafetyError("PuckSlotReader on a %s node" % cls)
        assert self.ACCESS == _GENERIC_READ, "the puck reader is read-only"
        self._fn = _bind()
        self.node = node
        share = _FILE_SHARE_WRITE if sole else (_FILE_SHARE_READ | _FILE_SHARE_WRITE)
        self._handle = self._fn["CreateFileW"](node["interface_id"], self.ACCESS, share, None,
                                               _OPEN_EXISTING, _FILE_FLAG_OVERLAPPED, None)
        if self._handle in (None, _INVALID):
            err = ctypes.get_last_error()
            raise OSError(err, "the puck slot could not be opened read-only (error %d)" % err)
        self._fn["HidD_SetNumInputBuffers"](self._handle, 512)
        self.input_len = self._input_len()
        self._stop = threading.Event()
        self._queue = collections.deque(maxlen=50000)
        self._lock = threading.Lock()
        self._thread = None
        self.gone = False

    def _input_len(self):
        pp = ctypes.c_void_p()
        caps = _Caps()
        if not self._fn["HidD_GetPreparsedData"](self._handle, ctypes.byref(pp)):
            return C.PADDED_INPUT_LEN
        try:
            self._fn["HidP_GetCaps"](pp, ctypes.byref(caps))
        finally:
            self._fn["HidD_FreePreparsedData"](pp)
        return caps.InputReportByteLength or C.PADDED_INPUT_LEN

    def start(self):
        if self._thread is None:
            self._thread = threading.Thread(target=self._run, daemon=True)
            self._thread.start()

    def _run(self):
        f = self._fn
        n = self.input_len
        buf = (ctypes.c_ubyte * n)()
        ev = f["CreateEventW"](None, True, False, None)
        ov = _OVERLAPPED()
        ov.hEvent = ev
        got = wintypes.DWORD(0)
        try:
            while not self._stop.is_set():
                if not f["ReadFile"](self._handle, buf, n, None, ctypes.byref(ov)):
                    if ctypes.get_last_error() != _ERROR_IO_PENDING:
                        self.gone = True
                        return
                    while f["WaitForSingleObject"](ev, 100) != 0:
                        if self._stop.is_set():
                            f["CancelIoEx"](self._handle, ctypes.byref(ov))
                            f["GetOverlappedResult"](self._handle, ctypes.byref(ov),
                                                     ctypes.byref(got), True)
                            return
                if not f["GetOverlappedResult"](self._handle, ctypes.byref(ov),
                                                ctypes.byref(got), False):
                    if ctypes.get_last_error() in (C.ERROR_DEVICE_NOT_CONNECTED,
                                                   C.ERROR_OPERATION_ABORTED, 6):
                        self.gone = True
                        return
                    continue
                ts = time.perf_counter_ns()
                with self._lock:
                    self._queue.append((ts, bytes(buf[:got.value])))
        finally:
            f["CloseHandle"](ev)

    def drain(self):
        with self._lock:
            out = list(self._queue)
            self._queue.clear()
        return out

    def close(self):
        if self._handle in (None, _INVALID):
            return
        self._stop.set()
        self._fn["CancelIoEx"](self._handle, None)
        if self._thread:
            self._thread.join(timeout=1.0)
        self._fn["CloseHandle"](self._handle)
        self._handle = None
