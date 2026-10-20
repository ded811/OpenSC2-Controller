# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# desc: the board as Windows sees it on USB (docs/testing.md, group "desc"), W, also with Steam open
# (read-only). HidD_GetAttributes, the three strings, the collections under the board's USB device
# (no handle opened for the mouse and keyboard ones) and HidP_GetCaps / HidP_GetValueCaps of the
# vendor collection, against the real controller's descriptors. The product string is the board's
# configured name ("Steam Controller" unless one is stored): the CRC-32 the caps carry, and the
# name `opensc2 id` showed when the run has read the console.
#
# B: the HID device Windows made of the board over Bluetooth: its IDs from the Device Information
# Service's PnP ID (28DE:1303, version 0x0100), the same three collections and capabilities (the
# same report map); the strings are what Windows takes from the Device Information Service, which
# is not documented (BLE-31), so they are reported, and only checked not to name another device.
import re

from . import base
from .. import config as C
from ..proto import console as cproto
from ..proto import testproto as T

GROUP = "desc"
TESTS = [("desc.ids", "WB", []), ("desc.strings", "WB", []), ("desc.collections", "WB", []),
         ("desc.caps", "WB", [])]

VALUE_CAPS = {
    "input": {0x42: 53, 0x44: 5, 0x79: 1, 0x43: 14, 0x7B: 12, 0x45: 45},
    "output": {0x80: 9, 0x81: 7, 0x82: 3, 0x83: 9, 0x84: 8, 0x85: 3, 0x86: 3, 0x87: 63,
               0x88: 63, 0x89: 63},
    "feature": {0x01: 63, 0x02: 63},
}

_NAME_RE = re.compile(r"^[\x20-\x7e]{1,%d}$" % C.PRODUCT_NAME_MAX)


def name_valid(text):
    """A product name the firmware accepts: 1 to 31 printable ASCII characters, no space at
    either end (opensc2_product_name_valid())."""
    return bool(text) and bool(_NAME_RE.match(text)) and text[0] != " " and text[-1] != " "


def check_product(c, rig, product):
    """The product string against the configured name, by every source this run has. The name
    itself is never written into the results (it may be personal): only the checks, its CRC-32s
    and its length."""
    product = product or ""
    crc = (rig.caps or {}).get("product_crc")
    shown = (getattr(rig, "identity_r", None) or {}).get("name")
    c.true("product: a valid name (1 to %d printable characters)" % C.PRODUCT_NAME_MAX,
           name_valid(product))
    if crc is not None:
        c.eq("product = the configured name (its CRC-32 in the caps)",
             "0x%08x" % T.crc32(product.encode("ascii", "replace")), "0x%08x" % crc)
    if shown is not None:
        c.true("product = the name `opensc2 id` shows", product == shown)
    if crc is None and shown is None:
        c.note("the configured name is not known to this run (no caps, no console yet): "
               "checked as a valid name only")
    c.note("the default name" if product == C.DEFAULT_PRODUCT_NAME else
           "a custom name (%d characters): the wired device differs from the real controller"
           % len(product))


def _shown(text):
    """How a Bluetooth string is reported: the default ones by name, anything else by length
    (a name may be personal)."""
    if not text:
        return "empty"
    if text in (C.DEFAULT_PRODUCT_NAME, "Valve Software", "Valve Corporation"):
        return '"%s"' % text
    return "%d characters" % len(text)


def run(ctx):
    rig = ctx.rig
    info = None
    if rig.models("desc"):
        info = rig.backend.hid_info()
    caps = rig.caps or {}
    october = bool(caps.get("build_flags", 0) & (1 << C.CAP_USB_PROFILE_OCTOBER))
    wired_console = bool(caps.get("build_flags", 0) & (1 << C.CAP_USB_WIRED_CONSOLE))

    if ctx.want("desc.ids") and rig.path == "B":
        with ctx.check(GROUP, "desc.ids") as c:
            base.need(rig, "desc")
            c.eq("VID:PID (the PnP ID)", (info["vid"], info["pid"]), (C.BOARD_VID, C.BLE_PID))
            c.eq("version (the PnP ID's)", info["version"], 0x0100)
    elif ctx.want("desc.ids"):
        with ctx.check(GROUP, "desc.ids") as c:
            base.need(rig, "desc")
            c.eq("VID:PID", (info["vid"], info["pid"]), (0x28DE, 0x1302))
            c.true("version 0x0307 (July) or 0x0404 (October): 0x%04x" % info["version"],
                   info["version"] in (0x0307, 0x0404))
            if rig.caps:
                c.eq("the version matches the USB profile in the caps", info["version"],
                     0x0404 if october else 0x0307)
            c.note("%s USB form" % ("October" if info["version"] == 0x0404 else "July"))

    if ctx.want("desc.strings") and rig.path == "B":
        with ctx.check(GROUP, "desc.strings") as c:
            base.need(rig, "desc")
            serial = info["serial"] or ""
            c.true("the serial string names no other device (empty, the board's address or its "
                   "unit serial)", not serial or serial == rig.unit_serial or
                   cproto.address_hex(serial) == getattr(rig.ctx.ble_expect, "address", None))
            c.note("Windows' strings over Bluetooth: manufacturer %s, product %s, serial %s"
                   % (_shown(info["manufacturer"]), _shown(info["product"]),
                      "empty" if not serial else "the unit serial" if serial == rig.unit_serial
                      else "the address"))
    elif ctx.want("desc.strings"):
        with ctx.check(GROUP, "desc.strings") as c:
            base.need(rig, "desc")
            c.eq("manufacturer", info["manufacturer"], "Valve Software")
            check_product(c, rig, info["product"])
            serial = info["serial"] or ""
            c.true("the serial: 13 characters starting OSC, equal to the devnode's (masked)",
                   len(serial) == 13 and serial.startswith("OSC") and serial == rig.unit_serial)

    if ctx.want("desc.collections"):
        with ctx.check(GROUP, "desc.collections") as c:
            base.need(rig, "desc")
            want = ["COL01", "COL02", "COL03"]
            c.eq("collections under the board's %s device" % ("Bluetooth" if rig.path == "B"
                                                               else "USB"),
                 sorted(info["collections"]), want)
            c.eq("composite (MI_00)", info["composite"], wired_console and rig.path == "W")

    if ctx.want("desc.caps"):
        with ctx.check(GROUP, "desc.caps") as c:
            base.need(rig, "desc")
            c.eq("usage page and usage", (info["usage_page"], info["usage"]), (0xFF00, 1))
            c.eq("report lengths (input, output, feature)",
                 (info["input_len"], info["output_len"], info["feature_len"]), (54, 64, 64))
            got = info.get("value_caps") or {}
            for kind, want in VALUE_CAPS.items():
                c.eq("%s value caps (report ID: count)" % kind,
                     {"0x%02x" % k: v for k, v in sorted(got.get(kind, {}).items())},
                     {"0x%02x" % k: v for k, v in sorted(want.items())})
