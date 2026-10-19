# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# puck_write.py: the ONLY code that sends to a puck, made only through safety.puck_writer() (S3):
# with --through-puck, Steam closed and the board's slot proven within the last 10 s. A separate
# handle with GENERIC_WRITE and sharing read and write, so it fits beside the reader (S12). Exactly
# three methods; every buffer for a feature report starts with 1 (S4: feature report 2, the puck's
# own commands, is never built), and output reports are 0x80..0x89.
import ctypes
import time
from ctypes import wintypes

from .. import config as C
from .. import safety
from ..transport import Stall

_GENERIC_WRITE = 0x40000000
_FILE_SHARE_READ = 0x1
_FILE_SHARE_WRITE = 0x2
_OPEN_EXISTING = 3
_FILE_FLAG_OVERLAPPED = 0x40000000
_INVALID = ctypes.c_void_p(-1).value
_ERROR_IO_PENDING = 997


class _OVERLAPPED(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_void_p), ("InternalHigh", ctypes.c_void_p),
                ("Offset", wintypes.DWORD), ("OffsetHigh", wintypes.DWORD),
                ("hEvent", wintypes.HANDLE)]


def _bind():
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    POV = ctypes.POINTER(_OVERLAPPED)
    PDW = ctypes.POINTER(wintypes.DWORD)
    for name, args, res in (
            ("CreateFileW", [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                             wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE], wintypes.HANDLE),
            ("CloseHandle", [wintypes.HANDLE], wintypes.BOOL),
            ("WriteFile", [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, PDW, POV],
             wintypes.BOOL),
            ("DeviceIoControl", [wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                 ctypes.c_void_p, wintypes.DWORD, PDW, POV], wintypes.BOOL),
            ("GetOverlappedResult", [wintypes.HANDLE, POV, PDW, wintypes.BOOL], wintypes.BOOL),
            ("CancelIoEx", [wintypes.HANDLE, POV], wintypes.BOOL),
            ("CreateEventW", [ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR],
             wintypes.HANDLE),
            ("WaitForSingleObject", [wintypes.HANDLE, wintypes.DWORD], wintypes.DWORD)):
        fn = getattr(k, name)
        fn.argtypes, fn.restype = args, res
    return k


class PuckSlotWriter:
    """Through-puck writes to the board's proven slot. Made only by safety.puck_writer(); the Rig
    logs and counts every call. `node` is the exact node of the slot that carried the board's
    signature; `ctx` the Rig's live safety.Context, with which every feature report 1 SET is
    checked again right where it goes out (S5; through the puck only reading test ops)."""

    def __init__(self, node, ctx=None):
        cls, _why = safety.classify(node)
        if cls != safety.PUCK_SLOT:
            raise safety.SafetyError("PuckSlotWriter on a %s node" % cls)
        self.node = node
        self._ctx = ctx
        self._k = _bind()
        self._handle = self._k.CreateFileW(node["interface_id"], _GENERIC_WRITE,
                                           _FILE_SHARE_READ | _FILE_SHARE_WRITE, None,
                                           _OPEN_EXISTING, _FILE_FLAG_OVERLAPPED, None)
        if self._handle in (None, _INVALID):
            err = ctypes.get_last_error()
            raise OSError(err, "the puck slot could not be opened for writing (error %d)" % err)

    def _overlapped(self, call, timeout_ms):
        ev = self._k.CreateEventW(None, True, False, None)
        ov = _OVERLAPPED()
        ov.hEvent = ev
        got = wintypes.DWORD(0)
        try:
            if not call(ctypes.byref(ov)):
                err = ctypes.get_last_error()
                if err != _ERROR_IO_PENDING:
                    raise Stall(err, "the puck refused it (error %d)" % err)
                if self._k.WaitForSingleObject(ev, timeout_ms) != 0:
                    self._k.CancelIoEx(self._handle, ctypes.byref(ov))
                    self._k.GetOverlappedResult(self._handle, ctypes.byref(ov), ctypes.byref(got),
                                                True)
                    raise Stall(1460, "the puck did not answer in time")
            if not self._k.GetOverlappedResult(self._handle, ctypes.byref(ov), ctypes.byref(got),
                                               False):
                err = ctypes.get_last_error()
                raise Stall(err, "the puck refused it (error %d)" % err)
        finally:
            self._k.CloseHandle(ev)

    def _feature(self, code, buf):
        if len(buf) != C.FEATURE_LEN or buf[0] != C.FEATURE_ID:
            raise safety.SafetyError("only 64-byte feature report 1 buffers go to the puck (S4)")
        cbuf = (ctypes.c_ubyte * len(buf)).from_buffer_copy(bytes(buf))
        self._overlapped(lambda pov: self._k.DeviceIoControl(
            self._handle, code, cbuf, len(buf), cbuf, len(buf), None, pov),
            int(C.FEATURE_IOCTL_TIMEOUT_S * 1000))
        return bytes(cbuf)

    def set_feature1(self, buf):
        """Feature report 1 SET: the pass-through to the board (S5 on the exact buffer first)."""
        if len(buf) != C.FEATURE_LEN or buf[0] != C.FEATURE_ID:
            raise safety.SafetyError("only 64-byte feature report 1 buffers go to the puck (S4)")
        safety.check_feature_buffer(bytes(buf), "puck", self._ctx)
        self._feature(C.IOCTL_HID_SET_FEATURE, buf)

    def get_feature1_answer(self):
        """The puck's two-step GET: the first fails with "try again" while it asks the board over
        the radio; retried every 10 ms until it answers or 1.5 s pass (then Stall)."""
        req = bytes([C.FEATURE_ID]) + bytes(C.FEATURE_LEN - 1)
        deadline = time.monotonic() + C.PUCK_GET_TIMEOUT_S
        last = None
        while True:
            try:
                return self._feature(C.IOCTL_HID_GET_FEATURE, req)
            except Stall as e:
                last = e
            if time.monotonic() >= deadline:
                raise last
            time.sleep(C.PUCK_GET_POLL_S)

    def write_output(self, report):
        if not 0x80 <= report[0] <= 0x89:
            raise safety.SafetyError("through-puck output reports are 0x80..0x89 only")
        data = bytes(report) + bytes(max(0, C.FEATURE_LEN - len(report)))
        cbuf = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
        self._overlapped(lambda pov: self._k.WriteFile(self._handle, cbuf, len(data), None, pov),
                         1000)

    def close(self):
        if getattr(self, "_handle", None) not in (None, _INVALID):
            self._k.CloseHandle(self._handle)
            self._handle = None
