# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# procs.py: is Steam running, and where is its folder. A Toolhelp32 snapshot names the
# processes; Steam counts as open when steam.exe, steamwebhelper.exe or hardwareupdater.exe is
# there. The folder comes from HKCU\Software\Valve\Steam, value SteamPath.
#
# The snapshot functions are bound with their prototypes (a HANDLE result, compared with the real
# INVALID_HANDLE_VALUE), and the check fails closed: a process list that cannot be read counts as
# "Steam may be running", since S7, S8 and every Steam guard rely on it.
import ctypes
from ctypes import wintypes

STEAM_PROCESSES = {"steam.exe", "steamwebhelper.exe", "hardwareupdater.exe"}
_TH32CS_SNAPPROCESS = 0x2
_INVALID = ctypes.c_void_p(-1).value      # INVALID_HANDLE_VALUE as a HANDLE result reads


class _PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                ("th32ProcessID", wintypes.DWORD), ("th32DefaultHeapID", ctypes.c_void_p),
                ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", ctypes.c_long),
                ("dwFlags", wintypes.DWORD), ("szExeFile", ctypes.c_wchar * 260)]


_K = None


def _kernel32():
    """kernel32 with the four snapshot functions' prototypes (a WinDLL of this module's own: its
    function objects, so nothing else's argtypes are changed)."""
    global _K
    if _K is None:
        k = ctypes.WinDLL("kernel32", use_last_error=True)
        PENTRY = ctypes.POINTER(_PROCESSENTRY32W)
        k.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
        k.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
        k.Process32FirstW.argtypes = [wintypes.HANDLE, PENTRY]
        k.Process32FirstW.restype = wintypes.BOOL
        k.Process32NextW.argtypes = [wintypes.HANDLE, PENTRY]
        k.Process32NextW.restype = wintypes.BOOL
        k.CloseHandle.argtypes = [wintypes.HANDLE]
        k.CloseHandle.restype = wintypes.BOOL
        _K = k
    return _K


def process_names(kernel32=None):
    """The running processes' names, lower case. OSError when the list cannot be read."""
    k = kernel32 or _kernel32()
    snap = k.CreateToolhelp32Snapshot(_TH32CS_SNAPPROCESS, 0)
    if snap in (None, 0, _INVALID):
        raise OSError(ctypes.get_last_error(), "the process list could not be read")
    names = set()
    try:
        entry = _PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        if not k.Process32FirstW(snap, ctypes.byref(entry)):
            raise OSError(ctypes.get_last_error(), "the process list could not be read")
        while True:
            names.add(entry.szExeFile.lower())
            if not k.Process32NextW(snap, ctypes.byref(entry)):
                break
    finally:
        k.CloseHandle(snap)
    return names


def steam_running(kernel32=None):
    """True when a Steam process runs, and also when the process list cannot be read (fail
    closed: the runner then waits for Steam to close or stops, and injects nothing)."""
    try:
        return bool(process_names(kernel32) & STEAM_PROCESSES)
    except OSError:
        return True


def is_admin():
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def steam_folder():
    import winreg
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
            path, _ = winreg.QueryValueEx(k, "SteamPath")
            return path.replace("/", "\\")
    except OSError:
        return r"C:\Program Files (x86)\Steam"
