# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# rawinput.py: the leak detector (S13). A message-only window registers for raw mouse and keyboard
# input with RIDEV_INPUTSINK and, for each WM_INPUT, reads ONLY the event's header (RID_HEADER) to
# learn which device it came from. RID_INPUT is never requested, so no key or motion of any device
# is read. An event from the board's own mouse or keyboard collection (or the proven puck slot's)
# while armed is a leak. The name rule is the pure function is_leak(), tested on any OS.
#
# The watched collections must be listed as raw-input devices (GetRawInputDeviceList,
# names only, no device opened); a watch list that is empty or not listed is a detector that is
# off, and the runner refuses --lizard on that path. Each event's time is kept, so that the
# runner counts only what came while it was armed (fired_since), and quiet windows (suppress)
# leave out the runner's own link drops, at which the puck sends empty keyboard and mouse
# reports on the slot so that nothing stays held.
import collections
import threading
import time


def _normalise(name):
    return (name or "").lower().replace("#", "\\").replace("{", "").replace("}", "")


def is_leak(device_name, watched_names):
    """True when a raw-input device name belongs to one of the watched collections (the board's
    Col01/Col02 on W, the proven slot's MI_0n&Col01/Col02 on R). Raw-input names are device
    interface paths (\\\\?\\HID#VID_..&Col01#<instance>#{class}); the watched names are the
    collections' instance IDs (HID\\VID_..&COL01\\<instance>). Case and the #/{} punctuation are
    ignored. Any other device (your own mouse and keyboard, an unknown handle) is not a leak."""
    n = _normalise(device_name)
    for w in watched_names:
        wn = _normalise(w)
        if wn and wn in n:
            return True
    return False


def listed(watched_names, device_names):
    """Every watched collection appears among the raw-input device names (pure)."""
    watched = [w for w in watched_names if w]
    return bool(watched) and all(any(is_leak(n, [w]) for n in device_names) for w in watched)


def raw_input_device_names():
    """The names of the system's raw-input mice and keyboards (GetRawInputDeviceList and
    GetRawInputDeviceInfoW RIDI_DEVICENAME: names only, no device is opened, nothing is read
    from any). None when the list cannot be read."""
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    RIDI_DEVICENAME = 0x20000007
    RIM_TYPEMOUSE, RIM_TYPEKEYBOARD = 0, 1

    class RAWINPUTDEVICELIST(ctypes.Structure):
        _fields_ = [("hDevice", wintypes.HANDLE), ("dwType", wintypes.DWORD)]

    PUINT = ctypes.POINTER(wintypes.UINT)
    user32.GetRawInputDeviceList.argtypes = [ctypes.POINTER(RAWINPUTDEVICELIST), PUINT,
                                             wintypes.UINT]
    user32.GetRawInputDeviceList.restype = wintypes.UINT
    user32.GetRawInputDeviceInfoW.argtypes = [wintypes.HANDLE, wintypes.UINT, wintypes.LPVOID,
                                              PUINT]
    user32.GetRawInputDeviceInfoW.restype = wintypes.UINT
    size = ctypes.sizeof(RAWINPUTDEVICELIST)
    got = 0xFFFFFFFF
    for _ in range(3):              # the list can change between the two calls
        count = wintypes.UINT(0)
        if user32.GetRawInputDeviceList(None, ctypes.byref(count), size) == 0xFFFFFFFF:
            return None
        arr = (RAWINPUTDEVICELIST * max(1, count.value))()
        got = user32.GetRawInputDeviceList(arr, ctypes.byref(count), size)
        if got != 0xFFFFFFFF:
            break
    if got == 0xFFFFFFFF:
        return None
    names = []
    for i in range(got):
        if arr[i].dwType not in (RIM_TYPEMOUSE, RIM_TYPEKEYBOARD):
            continue
        n = wintypes.UINT(0)
        user32.GetRawInputDeviceInfoW(arr[i].hDevice, RIDI_DEVICENAME, None, ctypes.byref(n))
        if not n.value:
            continue
        buf = ctypes.create_unicode_buffer(n.value + 1)
        if user32.GetRawInputDeviceInfoW(arr[i].hDevice, RIDI_DEVICENAME, buf,
                                         ctypes.byref(n)) != 0xFFFFFFFF:
            names.append(buf.value)
    return names


class LeakDetector:
    """A message-only window that watches the board's mouse and keyboard collections for any
    event. Only event headers are read. fired_since(t) says whether a leak happened after t;
    watching() whether there is anything it can see at all. `lister` lists the raw-input device
    names (raw_input_device_names; a self-test passes its own), `clock` gives event times."""

    def __init__(self, watched_names, lister=None, clock=None):
        self._watched = [n for n in watched_names if n]
        self._lister = lister or raw_input_device_names
        self._clock = clock or time.monotonic
        self._fired = False
        self._events = collections.deque(maxlen=256)
        self._lock = threading.Lock()
        self._ready = threading.Event()
        self._thread = None
        self._names = {}
        self._quiet_depth = 0
        self._quiet_until = 0.0
        self.ignored = 0             # watched events inside a quiet window (not leaks)
        self.listed = False

    def watch(self, names):
        """Watch these collections (instance IDs) from now on; an empty list watches nothing."""
        names = [n for n in names if n]
        if names == self._watched and self.listed:
            return
        self._watched = names
        self._names = {}
        self.listed = self._check_listed()

    def watched(self):
        return list(self._watched)

    def _check_listed(self):
        if not self._watched:
            return False
        try:
            names = self._lister()
        except Exception:
            return False
        return names is not None and listed(self._watched, names)

    def watching(self):
        """True when the window is up and every watched collection is a listed raw-input
        device, checked again at every call (Windows lists a new device a moment after it
        appears, and a device that went away is no longer listed)."""
        if not self._ready.is_set() or not self._watched:
            return False
        self.listed = self._check_listed()
        return self.listed

    def start(self, timeout=2.0):
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        ok = self._ready.wait(timeout)
        self.listed = self._check_listed()
        return ok

    def suppress(self, on, grace_s=1.0):
        """Begin (on) or end a quiet window; after its end, `grace_s` more."""
        with self._lock:
            if on:
                self._quiet_depth += 1
            else:
                self._quiet_depth = max(0, self._quiet_depth - 1)
                self._quiet_until = max(self._quiet_until, self._clock() + grace_s)

    def record(self):
        """An event from a watched collection (the window's thread)."""
        now = self._clock()
        with self._lock:
            if self._quiet_depth or now < self._quiet_until:
                self.ignored += 1
                return
            self._events.append(now)
            self._fired = True

    def fired(self):
        return self._fired

    def fired_since(self, t):
        with self._lock:
            return any(e >= t for e in self._events)

    def _run(self):
        import ctypes
        from ctypes import wintypes
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        LRESULT = ctypes.c_ssize_t
        WM_INPUT = 0x00FF
        RIDEV_INPUTSINK = 0x00000100
        RID_HEADER = 0x10000005
        RIDI_DEVICENAME = 0x20000007
        HWND_MESSAGE = ctypes.c_void_p(-3)

        class RAWINPUTDEVICE(ctypes.Structure):
            _fields_ = [("usUsagePage", wintypes.USHORT), ("usUsage", wintypes.USHORT),
                        ("dwFlags", wintypes.DWORD), ("hwndTarget", wintypes.HWND)]

        class RAWINPUTHEADER(ctypes.Structure):
            _fields_ = [("dwType", wintypes.DWORD), ("dwSize", wintypes.DWORD),
                        ("hDevice", wintypes.HANDLE), ("wParam", wintypes.WPARAM)]

        WNDPROC = ctypes.WINFUNCTYPE(LRESULT, wintypes.HWND, wintypes.UINT, wintypes.WPARAM,
                                     wintypes.LPARAM)

        class WNDCLASSW(ctypes.Structure):
            _fields_ = [("style", wintypes.UINT), ("lpfnWndProc", WNDPROC),
                        ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int),
                        ("hInstance", wintypes.HINSTANCE), ("hIcon", wintypes.HICON),
                        ("hCursor", wintypes.HANDLE), ("hbrBackground", wintypes.HBRUSH),
                        ("lpszMenuName", wintypes.LPCWSTR), ("lpszClassName", wintypes.LPCWSTR)]

        PUINT = ctypes.POINTER(wintypes.UINT)
        user32.DefWindowProcW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM,
                                          wintypes.LPARAM]
        user32.DefWindowProcW.restype = LRESULT
        user32.RegisterClassW.argtypes = [ctypes.POINTER(WNDCLASSW)]
        user32.RegisterClassW.restype = wintypes.ATOM
        user32.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR,
                                           wintypes.DWORD, ctypes.c_int, ctypes.c_int,
                                           ctypes.c_int, ctypes.c_int, wintypes.HWND,
                                           wintypes.HMENU, wintypes.HINSTANCE, wintypes.LPVOID]
        user32.CreateWindowExW.restype = wintypes.HWND
        user32.RegisterRawInputDevices.argtypes = [ctypes.POINTER(RAWINPUTDEVICE), wintypes.UINT,
                                                   wintypes.UINT]
        user32.RegisterRawInputDevices.restype = wintypes.BOOL
        user32.GetRawInputData.argtypes = [wintypes.HANDLE, wintypes.UINT, wintypes.LPVOID,
                                           PUINT, wintypes.UINT]
        user32.GetRawInputData.restype = wintypes.UINT
        user32.GetRawInputDeviceInfoW.argtypes = [wintypes.HANDLE, wintypes.UINT,
                                                  wintypes.LPVOID, PUINT]
        user32.GetRawInputDeviceInfoW.restype = wintypes.UINT
        user32.GetMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), wintypes.HWND,
                                       wintypes.UINT, wintypes.UINT]
        user32.GetMessageW.restype = wintypes.BOOL
        user32.TranslateMessage.argtypes = [ctypes.POINTER(wintypes.MSG)]
        user32.TranslateMessage.restype = wintypes.BOOL
        user32.DispatchMessageW.argtypes = [ctypes.POINTER(wintypes.MSG)]
        user32.DispatchMessageW.restype = LRESULT
        kernel32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
        kernel32.GetModuleHandleW.restype = wintypes.HMODULE

        def name_of(hdevice):
            key = hdevice or 0
            if key in self._names:
                return self._names[key]
            size = wintypes.UINT(0)
            user32.GetRawInputDeviceInfoW(hdevice, RIDI_DEVICENAME, None, ctypes.byref(size))
            buf = ctypes.create_unicode_buffer(size.value + 1)
            user32.GetRawInputDeviceInfoW(hdevice, RIDI_DEVICENAME, buf, ctypes.byref(size))
            self._names[key] = buf.value
            return buf.value

        def wndproc(hwnd, msg, wparam, lparam):
            if msg == WM_INPUT:
                hdr = RAWINPUTHEADER()
                size = wintypes.UINT(ctypes.sizeof(hdr))
                # RID_HEADER only: which device, never what it sent
                if user32.GetRawInputData(lparam, RID_HEADER, ctypes.byref(hdr), ctypes.byref(size),
                                          ctypes.sizeof(RAWINPUTHEADER)) != 0xFFFFFFFF:
                    if is_leak(name_of(hdr.hDevice), self._watched):
                        self.record()
            return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

        self._wndproc = WNDPROC(wndproc)
        wc = WNDCLASSW()
        wc.lpfnWndProc = self._wndproc
        wc.lpszClassName = "OpenSC2HwtestLeakDetector"
        wc.hInstance = kernel32.GetModuleHandleW(None)
        user32.RegisterClassW(ctypes.byref(wc))
        hwnd = user32.CreateWindowExW(0, wc.lpszClassName, wc.lpszClassName, 0, 0, 0, 0, 0,
                                      HWND_MESSAGE, None, wc.hInstance, None)
        if not hwnd:
            return
        rid = (RAWINPUTDEVICE * 2)(RAWINPUTDEVICE(1, 2, RIDEV_INPUTSINK, hwnd),
                                   RAWINPUTDEVICE(1, 6, RIDEV_INPUTSINK, hwnd))
        if not user32.RegisterRawInputDevices(rid, 2, ctypes.sizeof(RAWINPUTDEVICE)):
            return
        self._ready.set()
        msg = wintypes.MSG()
        while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
