# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# hid.py: the board's vendor collection on Windows (docs/testing.md, "How the PC talks to the
# board"). BoardHid opens 28DE:1302 Col03 of a node safety.classify() called BOARD_HID: read/write
# with sharing 0 (the sole reader, S12) while Steam is closed, read-only and shared while it is
# open. Before the first write the HID serial string must equal the serial in the instance ID
# (R1). One reader thread keeps one overlapped ReadFile pending at a time; feature reports go
# through overlapped DeviceIoControl on the same handle, each with an OVERLAPPED and event of its
# own (HidD_SetFeature/GetFeature pass none, which on an overlapped handle can miss its own
# completion). Only the board is ever opened here.
import collections
import ctypes
import threading
import time
from ctypes import wintypes

from .. import config as C
from .. import safety
from ..transport import Stall

_GENERIC_READ = 0x80000000
_GENERIC_WRITE = 0x40000000
_FILE_SHARE_READ = 0x1
_FILE_SHARE_WRITE = 0x2
_OPEN_EXISTING = 3
_FILE_FLAG_OVERLAPPED = 0x40000000
_INVALID = ctypes.c_void_p(-1).value
_ERROR_IO_PENDING = 997
_WAIT_OBJECT_0 = 0
IOCTL_HID_SET_OUTPUT_REPORT = 0xB0195    # HID_IN_CTL_CODE(101)
IOCTL_HID_GET_INPUT_REPORT = 0xB01A2     # HID_OUT_CTL_CODE(104)
HIDP_STATUS_SUCCESS = 0x00110000


class OVERLAPPED(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_void_p), ("InternalHigh", ctypes.c_void_p),
                ("Offset", wintypes.DWORD), ("OffsetHigh", wintypes.DWORD),
                ("hEvent", wintypes.HANDLE)]


class HIDP_CAPS(ctypes.Structure):
    _fields_ = [("Usage", wintypes.USHORT), ("UsagePage", wintypes.USHORT),
                ("InputReportByteLength", wintypes.USHORT),
                ("OutputReportByteLength", wintypes.USHORT),
                ("FeatureReportByteLength", wintypes.USHORT),
                ("Reserved", wintypes.USHORT * 17),
                ("NumberLinkCollectionNodes", wintypes.USHORT),
                ("NumberInputButtonCaps", wintypes.USHORT),
                ("NumberInputValueCaps", wintypes.USHORT),
                ("NumberInputDataIndices", wintypes.USHORT),
                ("NumberOutputButtonCaps", wintypes.USHORT),
                ("NumberOutputValueCaps", wintypes.USHORT),
                ("NumberOutputDataIndices", wintypes.USHORT),
                ("NumberFeatureButtonCaps", wintypes.USHORT),
                ("NumberFeatureValueCaps", wintypes.USHORT),
                ("NumberFeatureDataIndices", wintypes.USHORT)]


class HIDP_VALUE_CAPS(ctypes.Structure):
    _fields_ = [("UsagePage", wintypes.USHORT), ("ReportID", ctypes.c_ubyte),
                ("IsAlias", ctypes.c_ubyte), ("BitField", wintypes.USHORT),
                ("LinkCollection", wintypes.USHORT), ("LinkUsage", wintypes.USHORT),
                ("LinkUsagePage", wintypes.USHORT), ("IsRange", ctypes.c_ubyte),
                ("IsStringRange", ctypes.c_ubyte), ("IsDesignatorRange", ctypes.c_ubyte),
                ("IsAbsolute", ctypes.c_ubyte), ("HasNull", ctypes.c_ubyte),
                ("Reserved", ctypes.c_ubyte), ("BitSize", wintypes.USHORT),
                ("ReportCount", wintypes.USHORT), ("Reserved2", wintypes.USHORT * 5),
                ("UnitsExp", wintypes.ULONG), ("Units", wintypes.ULONG),
                ("LogicalMin", wintypes.LONG), ("LogicalMax", wintypes.LONG),
                ("PhysicalMin", wintypes.LONG), ("PhysicalMax", wintypes.LONG),
                ("Union", wintypes.USHORT * 8)]


class HIDD_ATTRIBUTES(ctypes.Structure):
    _fields_ = [("Size", wintypes.ULONG), ("VendorID", wintypes.USHORT),
                ("ProductID", wintypes.USHORT), ("VersionNumber", wintypes.USHORT)]


def _bind():
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    h = ctypes.WinDLL("hid", use_last_error=True)
    POV = ctypes.POINTER(OVERLAPPED)
    PDW = ctypes.POINTER(wintypes.DWORD)
    sig = {
        (k, "CreateFileW"): ([wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                              wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE], wintypes.HANDLE),
        (k, "CloseHandle"): ([wintypes.HANDLE], wintypes.BOOL),
        (k, "ReadFile"): ([wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, PDW, POV],
                          wintypes.BOOL),
        (k, "WriteFile"): ([wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, PDW, POV],
                           wintypes.BOOL),
        (k, "DeviceIoControl"): ([wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p,
                                  wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, PDW, POV],
                                 wintypes.BOOL),
        (k, "GetOverlappedResult"): ([wintypes.HANDLE, POV, PDW, wintypes.BOOL], wintypes.BOOL),
        (k, "CancelIoEx"): ([wintypes.HANDLE, POV], wintypes.BOOL),
        (k, "CreateEventW"): ([ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR],
                              wintypes.HANDLE),
        (k, "WaitForSingleObject"): ([wintypes.HANDLE, wintypes.DWORD], wintypes.DWORD),
        (h, "HidD_SetNumInputBuffers"): ([wintypes.HANDLE, wintypes.ULONG], wintypes.BOOLEAN),
        (h, "HidD_GetAttributes"): ([wintypes.HANDLE, ctypes.POINTER(HIDD_ATTRIBUTES)],
                                    wintypes.BOOLEAN),
        (h, "HidD_GetSerialNumberString"): ([wintypes.HANDLE, ctypes.c_void_p, wintypes.ULONG],
                                            wintypes.BOOLEAN),
        (h, "HidD_GetManufacturerString"): ([wintypes.HANDLE, ctypes.c_void_p, wintypes.ULONG],
                                            wintypes.BOOLEAN),
        (h, "HidD_GetProductString"): ([wintypes.HANDLE, ctypes.c_void_p, wintypes.ULONG],
                                       wintypes.BOOLEAN),
        (h, "HidD_GetPreparsedData"): ([wintypes.HANDLE, ctypes.POINTER(ctypes.c_void_p)],
                                       wintypes.BOOLEAN),
        (h, "HidD_FreePreparsedData"): ([ctypes.c_void_p], wintypes.BOOLEAN),
        (h, "HidP_GetCaps"): ([ctypes.c_void_p, ctypes.POINTER(HIDP_CAPS)], wintypes.LONG),
        (h, "HidP_GetValueCaps"): ([ctypes.c_int, ctypes.POINTER(HIDP_VALUE_CAPS),
                                    ctypes.POINTER(wintypes.USHORT), ctypes.c_void_p],
                                   wintypes.LONG),
    }
    for (dll, name), (args, res) in sig.items():
        fn = getattr(dll, name)
        fn.argtypes = args
        fn.restype = res
    return k, h


class BoardHid:
    """The board's Col03. `writable`: read/write and sharing 0 (Steam closed); else read-only
    and shared. `ble_expect` (a safety.BleExpect): the board over Bluetooth (path B), whose node
    safety.classify_ble() must call the board's, by the address and serial its console gave."""

    def __init__(self, node, writable, retry_s=2.0, ble_expect=None):
        if ble_expect is not None:
            cls, _why = safety.classify_ble(node, ble_expect)
            if cls != safety.BOARD_BLE:
                raise safety.SafetyError("BoardHid on a Bluetooth %s node" % cls)
        else:
            cls, _why = safety.classify(node)
            if cls != safety.BOARD_HID:
                raise safety.SafetyError("BoardHid on a %s node" % cls)
        self._ble_expect = ble_expect
        self._node = node
        self._k, self._h = _bind()
        self.writable = writable
        access = (_GENERIC_READ | _GENERIC_WRITE) if writable else _GENERIC_READ
        share = 0 if writable else (_FILE_SHARE_READ | _FILE_SHARE_WRITE)
        deadline = time.monotonic() + retry_s
        while True:
            self._handle = self._k.CreateFileW(node["interface_id"], access, share, None,
                                               _OPEN_EXISTING, _FILE_FLAG_OVERLAPPED, None)
            if self._handle not in (None, _INVALID):
                break
            err = ctypes.get_last_error()
            # right after the node appeared the HID stack may not be ready yet; a sharing
            # violation (another reader) is never retried (S12)
            if err in (2, 5, 1167) and time.monotonic() < deadline:
                time.sleep(0.2)
                continue
            raise OSError(err, "the board's collection could not be opened (error %d)" % err)
        self._h.HidD_SetNumInputBuffers(self._handle, 512)
        self.serial = self._string(self._h.HidD_GetSerialNumberString)
        self.caps = self._caps()
        self._verified = False
        if writable:
            self.verify_serial()
        self._stop = threading.Event()
        self._queue = collections.deque(maxlen=50000)
        self._qlock = threading.Lock()
        self._thread = None
        self.gone = False

    # ---- identity ----
    def _string(self, fn):
        buf = ctypes.create_unicode_buffer(128)
        if fn(self._handle, buf, ctypes.sizeof(buf)):
            return buf.value
        return ""

    def verify_serial(self):
        """R1: the HID serial string must equal the instance ID's serial before the first write;
        if it does not, the handle is closed unused."""
        try:
            if self._ble_expect is not None:
                safety.assert_ble_write(self._node, self.serial, self._ble_expect)
            else:
                safety.assert_board_write(self._node, self.serial)
        except safety.SafetyError:
            self.close()
            raise
        if (self.caps["usage_page"], self.caps["usage"], self.caps["input_len"],
                self.caps["output_len"], self.caps["feature_len"]) != (0xFF00, 1, 54, 64, 64):
            self.close()
            raise safety.SafetyError("the board's collection has other capabilities than the "
                                     "Steam Controller 2's: not used")
        self._verified = True

    def _caps(self):
        pp = ctypes.c_void_p()
        caps = HIDP_CAPS()
        out = {"usage_page": 0, "usage": 0, "input_len": 0, "output_len": 0, "feature_len": 0,
               "value_caps": {}}
        if not self._h.HidD_GetPreparsedData(self._handle, ctypes.byref(pp)):
            return out
        try:
            if self._h.HidP_GetCaps(pp, ctypes.byref(caps)) == HIDP_STATUS_SUCCESS:
                out.update(usage_page=caps.UsagePage, usage=caps.Usage,
                           input_len=caps.InputReportByteLength,
                           output_len=caps.OutputReportByteLength,
                           feature_len=caps.FeatureReportByteLength)
                for kind, rtype, count in (("input", 0, caps.NumberInputValueCaps),
                                           ("output", 1, caps.NumberOutputValueCaps),
                                           ("feature", 2, caps.NumberFeatureValueCaps)):
                    arr = (HIDP_VALUE_CAPS * max(1, count))()
                    n = wintypes.USHORT(count)
                    found = {}
                    if count and self._h.HidP_GetValueCaps(rtype, arr, ctypes.byref(n), pp) == \
                            HIDP_STATUS_SUCCESS:
                        for i in range(n.value):
                            v = arr[i]
                            if v.BitSize == 8:
                                found[v.ReportID] = found.get(v.ReportID, 0) + v.ReportCount
                    out["value_caps"][kind] = found
        finally:
            self._h.HidD_FreePreparsedData(pp)
        return out

    def info(self):
        a = HIDD_ATTRIBUTES()
        a.Size = ctypes.sizeof(a)
        self._h.HidD_GetAttributes(self._handle, ctypes.byref(a))
        return {"vid": a.VendorID, "pid": a.ProductID, "version": a.VersionNumber,
                "manufacturer": self._string(self._h.HidD_GetManufacturerString),
                "product": self._string(self._h.HidD_GetProductString),
                "serial": self.serial, **self.caps}

    # ---- reading: one overlapped ReadFile pending at a time ----
    def start_reader(self):
        if self._thread is None:
            self._thread = threading.Thread(target=self._run, daemon=True)
            self._thread.start()

    def _run(self):
        n = self.caps["input_len"] or C.PADDED_INPUT_LEN
        buf = (ctypes.c_ubyte * n)()
        ev = self._k.CreateEventW(None, True, False, None)
        ov = OVERLAPPED()
        ov.hEvent = ev
        got = wintypes.DWORD(0)
        try:
            while not self._stop.is_set():
                ok = self._k.ReadFile(self._handle, buf, n, None, ctypes.byref(ov))
                if not ok:
                    err = ctypes.get_last_error()
                    if err != _ERROR_IO_PENDING:
                        self.gone = True
                        return
                    while self._k.WaitForSingleObject(ev, 100) != _WAIT_OBJECT_0:
                        if self._stop.is_set():
                            self._k.CancelIoEx(self._handle, ctypes.byref(ov))
                            self._k.GetOverlappedResult(self._handle, ctypes.byref(ov),
                                                        ctypes.byref(got), True)
                            return
                if not self._k.GetOverlappedResult(self._handle, ctypes.byref(ov),
                                                   ctypes.byref(got), False):
                    err = ctypes.get_last_error()
                    if err in (C.ERROR_DEVICE_NOT_CONNECTED, C.ERROR_OPERATION_ABORTED, 6):
                        self.gone = True
                        return
                    continue
                ts = time.perf_counter_ns()
                with self._qlock:
                    self._queue.append((ts, bytes(buf[:got.value])))
        finally:
            self._k.CloseHandle(ev)

    def drain(self):
        with self._qlock:
            out = list(self._queue)
            self._queue.clear()
        return out

    # ---- requests: overlapped IOCTLs on the one handle ----
    def _overlapped(self, call, timeout_ms):
        ev = self._k.CreateEventW(None, True, False, None)
        ov = OVERLAPPED()
        ov.hEvent = ev
        got = wintypes.DWORD(0)
        try:
            ok = call(ctypes.byref(ov))
            if not ok:
                err = ctypes.get_last_error()
                if err != _ERROR_IO_PENDING:
                    raise Stall(err, "refused (error %d)" % err)
                if self._k.WaitForSingleObject(ev, timeout_ms) != _WAIT_OBJECT_0:
                    self._k.CancelIoEx(self._handle, ctypes.byref(ov))
                    self._k.GetOverlappedResult(self._handle, ctypes.byref(ov), ctypes.byref(got),
                                                True)
                    raise TimeoutError("the request did not complete within %d ms" % timeout_ms)
            if not self._k.GetOverlappedResult(self._handle, ctypes.byref(ov), ctypes.byref(got),
                                               False):
                err = ctypes.get_last_error()
                raise Stall(err, "refused (error %d)" % err)
            return got.value
        finally:
            self._k.CloseHandle(ev)

    def _ioctl(self, code, data):
        if not self._verified:
            raise safety.SafetyError("no write before the serial check (R1)")
        cbuf = (ctypes.c_ubyte * len(data)).from_buffer_copy(bytes(data))
        self._overlapped(lambda pov: self._k.DeviceIoControl(
            self._handle, code, cbuf, len(data), cbuf, len(data), None, pov),
            int(C.FEATURE_IOCTL_TIMEOUT_S * 1000))
        return bytes(cbuf)

    def feature_set(self, buf, ctx=None):
        """A feature report 1 SET. S5 (and S7 for an injection on the test channel) on the exact
        buffer, with the Rig's live Context `ctx` (None: a fresh one, which refuses every flash
        write and every injection), right before it goes out."""
        if len(buf) != C.FEATURE_LEN or buf[0] != C.FEATURE_ID:
            raise safety.SafetyError("only 64-byte feature report 1 buffers")
        safety.check_feature_buffer(bytes(buf), "ble" if self._ble_expect else "usb", ctx)
        self._ioctl(C.IOCTL_HID_SET_FEATURE, buf)

    def feature_get(self, request=None):
        req = request if request is not None else bytes([C.FEATURE_ID]) + bytes(C.FEATURE_LEN - 1)
        if self._ble_expect is not None and len(req) == C.FEATURE_LEN:
            # over Bluetooth the characteristic holds 64 bytes, one more than the report map
            # declares (as on the real controller), and Windows refuses a buffer that cannot
            # take them all (error 122): read 65, hand on the 64 the callers know
            return self._ioctl(C.IOCTL_HID_GET_FEATURE, bytes(req) + b"\0")[:C.FEATURE_LEN]
        return self._ioctl(C.IOCTL_HID_GET_FEATURE, req)

    def feature_raw(self, report_id, buf=None):
        """cmd.framing only: a SET (buf) or GET of another feature report ID (the board must
        stall it): exactly the probes safety.check_framing_probe() builds."""
        data = bytes(buf) if buf is not None else bytes([report_id]) + bytes(C.FEATURE_LEN - 1)
        safety.check_framing_buffer("set2" if buf is not None else "get2", data)
        code = C.IOCTL_HID_SET_FEATURE if buf is not None else C.IOCTL_HID_GET_FEATURE
        self._ioctl(code, data)

    def output_raw(self, kind, buf):
        """cmd.framing only: HidD_SetOutputReport / HidD_GetInputReport through their IOCTLs,
        exactly the two probes of safety.check_framing_probe()."""
        safety.check_framing_buffer(kind, buf)
        code = IOCTL_HID_SET_OUTPUT_REPORT if kind == "set_output" else IOCTL_HID_GET_INPUT_REPORT
        self._ioctl(code, buf)

    def write_output(self, report):
        if not self._verified:
            raise safety.SafetyError("no write before the serial check (R1)")
        safety.check_output_report(report)
        data = bytes(report) + bytes(max(0, C.FEATURE_LEN - len(report)))
        cbuf = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
        try:
            self._overlapped(lambda pov: self._k.WriteFile(self._handle, cbuf, len(data), None,
                                                           pov), 1000)
        except Stall as e:
            raise OSError(e.win_error, "WriteFile failed (error %d)" % e.win_error)

    def close(self):
        if getattr(self, "_handle", None) in (None, _INVALID):
            return
        stop = getattr(self, "_stop", None)
        if stop is not None:
            stop.set()
        self._k.CancelIoEx(self._handle, None)
        if getattr(self, "_thread", None):
            self._thread.join(timeout=1.0)
        self._k.CloseHandle(self._handle)
        self._handle = None
