# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# devices.py: the present HID interfaces and COM ports, listed with cfgmgr32 and described WITHOUT
# opening any device (docs/testing.md, "Which devices it touches"). For each interface: its path
# (what would be opened), its own device instance ID (HID\VID_...&COL03\...), the USB device it
# belongs to (USB\VID_xxxx&PID_xxxx\<serial>: the first ancestor that is a USB device and not one
# of its interfaces, &MI_nn), the serial at the end of that instance ID, the USB product string
# read on that device node, its location and, for a port, its COM name. safety.classify() turns
# each node into one class; nothing here opens anything.
import ctypes
import uuid
from ctypes import wintypes

from .. import config as C

_CR_SUCCESS = 0
_CM_GET_DEVICE_INTERFACE_LIST_PRESENT = 0
_KEY_READ = 0x20019
_CLASSES = {"hid": C.GUID_DEVINTERFACE_HID, "port": C.GUID_DEVINTERFACE_COMPORT}

# DEVPKEY_Device_InstanceId, _BusReportedDeviceDesc, _LocationInfo (devpkey.h)
_DEVPKEY_Device_InstanceId = ("78c34fc8-104a-4aca-9ea4-524d52996e57", 256)
_DEVPKEY_Device_BusReportedDeviceDesc = ("540b947e-8b40-45bc-a8a2-6a0b894cbda2", 4)
_DEVPKEY_Device_LocationInfo = ("a45c254e-df1c-4efd-8020-67d146a850e0", 15)


class _GUID(ctypes.Structure):
    _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD), ("Data3", wintypes.WORD),
                ("Data4", ctypes.c_ubyte * 8)]


class _DEVPROPKEY(ctypes.Structure):
    _fields_ = [("fmtid", _GUID), ("pid", wintypes.ULONG)]


def _guid(text):
    g = _GUID()
    ctypes.memmove(ctypes.byref(g), uuid.UUID(text.strip("{}")).bytes_le, 16)
    return g


def _propkey(fmtid, pid):
    k = _DEVPROPKEY()
    k.fmtid = _guid(fmtid)
    k.pid = pid
    return k


class _Cfgmgr:
    def __init__(self):
        cfg = ctypes.WinDLL("cfgmgr32")
        PUL = ctypes.POINTER(wintypes.ULONG)
        PKEY = ctypes.POINTER(_DEVPROPKEY)
        for name, args in (
                ("CM_Get_Device_Interface_List_SizeW",
                 [PUL, ctypes.POINTER(_GUID), wintypes.LPCWSTR, wintypes.ULONG]),
                ("CM_Get_Device_Interface_ListW",
                 [ctypes.POINTER(_GUID), wintypes.LPCWSTR, ctypes.c_void_p, wintypes.ULONG,
                  wintypes.ULONG]),
                ("CM_Get_Device_Interface_PropertyW",
                 [wintypes.LPCWSTR, PKEY, PUL, ctypes.c_void_p, PUL, wintypes.ULONG]),
                ("CM_Locate_DevNodeW", [PUL, wintypes.LPCWSTR, wintypes.ULONG]),
                ("CM_Get_Parent", [PUL, wintypes.ULONG, wintypes.ULONG]),
                ("CM_Get_Device_IDW", [wintypes.ULONG, ctypes.c_void_p, wintypes.ULONG,
                                       wintypes.ULONG]),
                ("CM_Get_DevNode_PropertyW",
                 [wintypes.ULONG, PKEY, PUL, ctypes.c_void_p, PUL, wintypes.ULONG]),
                ("CM_Open_DevNode_Key", [wintypes.ULONG, wintypes.DWORD, wintypes.ULONG,
                                         wintypes.ULONG, ctypes.POINTER(wintypes.HKEY),
                                         wintypes.ULONG])):
            fn = getattr(cfg, name)
            fn.argtypes = args
            fn.restype = wintypes.DWORD
        self.cfg = cfg
        adv = ctypes.WinDLL("advapi32")
        adv.RegQueryValueExW.argtypes = [wintypes.HKEY, wintypes.LPCWSTR, ctypes.c_void_p,
                                         ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p,
                                         ctypes.POINTER(wintypes.DWORD)]
        adv.RegQueryValueExW.restype = wintypes.LONG
        adv.RegCloseKey.argtypes = [wintypes.HKEY]
        adv.RegCloseKey.restype = wintypes.LONG
        self.adv = adv

    def interface_list(self, guid_text):
        g = _guid(guid_text)
        size = wintypes.ULONG(0)
        if self.cfg.CM_Get_Device_Interface_List_SizeW(ctypes.byref(size), ctypes.byref(g), None,
                                                       _CM_GET_DEVICE_INTERFACE_LIST_PRESENT) \
                != _CR_SUCCESS or size.value <= 1:
            return []
        buf = ctypes.create_unicode_buffer(size.value)
        if self.cfg.CM_Get_Device_Interface_ListW(ctypes.byref(g), None, buf, size.value,
                                                  _CM_GET_DEVICE_INTERFACE_LIST_PRESENT) \
                != _CR_SUCCESS:
            return []
        return [s for s in buf[:size.value].split("\0") if s]

    def _string_property(self, call):
        ptype = wintypes.ULONG(0)
        size = wintypes.ULONG(0)
        call(ctypes.byref(ptype), None, ctypes.byref(size))
        if size.value == 0:
            return None
        buf = ctypes.create_unicode_buffer(size.value // 2 + 1)
        if call(ctypes.byref(ptype), buf, ctypes.byref(size)) != _CR_SUCCESS:
            return None
        return buf.value

    def interface_property(self, iface, key):
        return self._string_property(lambda pt, b, sz: self.cfg.CM_Get_Device_Interface_PropertyW(
            iface, ctypes.byref(key), pt, b, sz, 0))

    def devnode_property(self, devnode, key):
        return self._string_property(lambda pt, b, sz: self.cfg.CM_Get_DevNode_PropertyW(
            devnode, ctypes.byref(key), pt, b, sz, 0))

    def locate(self, instance_id):
        dn = wintypes.ULONG(0)
        if self.cfg.CM_Locate_DevNodeW(ctypes.byref(dn), instance_id, 0) != _CR_SUCCESS:
            return None
        return dn.value

    def parent(self, devnode):
        p = wintypes.ULONG(0)
        if self.cfg.CM_Get_Parent(ctypes.byref(p), devnode, 0) != _CR_SUCCESS:
            return None
        return p.value

    def device_id(self, devnode):
        buf = ctypes.create_unicode_buffer(512)
        if self.cfg.CM_Get_Device_IDW(devnode, buf, 512, 0) != _CR_SUCCESS:
            return None
        return buf.value

    def port_name(self, devnode):
        """A COM port's PortName, from its device's hardware key (CM_REGISTRY_HARDWARE)."""
        hkey = wintypes.HKEY()
        if self.cfg.CM_Open_DevNode_Key(devnode, _KEY_READ, 0, 1, ctypes.byref(hkey), 0) \
                != _CR_SUCCESS:
            return None
        try:
            buf = ctypes.create_unicode_buffer(64)
            size = wintypes.DWORD(ctypes.sizeof(buf))
            if self.adv.RegQueryValueExW(hkey, "PortName", None, None, buf,
                                         ctypes.byref(size)) == 0:
                return buf.value
        finally:
            self.adv.RegCloseKey(hkey)
        return None


def serial_from_instance(instance_id):
    """USB\\VID_28DE&PID_1302\\<serial>: the last part is the device's serial, unless Windows
    numbered the device by its port (an & in it): then it has none that can be checked."""
    if not instance_id:
        return None
    tail = instance_id.split("\\")[-1]
    return None if "&" in tail else tail


def usb_device(cfg, devnode):
    """The USB device a node belongs to: the first ancestor whose ID is USB\\VID_... and is not
    an interface of a composite device (&MI_nn). (devnode, instance ID) or (None, None)."""
    dn, hops = devnode, 0
    while dn is not None and hops < 8:
        did = cfg.device_id(dn)
        if did and did.upper().startswith("USB\\VID_") and "&MI_" not in did.upper():
            return dn, did
        dn = cfg.parent(dn)
        hops += 1
    return None, None


def enumerate_nodes():
    """The present HID interfaces and COM ports, described for safety.classify()."""
    cfg = _Cfgmgr()
    iid_key = _propkey(*_DEVPKEY_Device_InstanceId)
    bus_key = _propkey(*_DEVPKEY_Device_BusReportedDeviceDesc)
    loc_key = _propkey(*_DEVPKEY_Device_LocationInfo)
    nodes = []
    for kind, guid in _CLASSES.items():
        for iface in cfg.interface_list(guid):
            own = cfg.interface_property(iface, iid_key)
            own_dn = cfg.locate(own) if own else None
            usb_dn, usb_id = usb_device(cfg, own_dn) if own_dn is not None else (None, None)
            nodes.append({
                "kind": kind,
                "interface_id": iface,
                "own_instance_id": own,
                "instance_id": usb_id or own or iface,
                "serial": serial_from_instance(usb_id),
                "usb_product": cfg.devnode_property(usb_dn, bus_key) if usb_dn is not None
                else None,
                "location": cfg.devnode_property(usb_dn, loc_key) if usb_dn is not None
                else None,
                "com": cfg.port_name(own_dn) if kind == "port" and own_dn is not None else None,
            })
    return nodes


def collections_of(nodes, usb_instance_id):
    """The HID collections (COL01, COL02...) under one USB device, and whether its interfaces
    are numbered (MI_00: a composite device)."""
    cols, composite = [], False
    for n in nodes:
        if n["kind"] != "hid" or (n.get("instance_id") or "").upper() != usb_instance_id.upper():
            continue
        own = (n.get("own_instance_id") or "").upper()
        for part in own.split("\\")[1:2]:
            for tok in part.split("&"):
                if tok.startswith("COL"):
                    cols.append(tok)
                if tok.startswith("MI_"):
                    composite = True
    return sorted(cols), composite
