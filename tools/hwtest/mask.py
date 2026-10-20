# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# mask.py: nothing identifying is ever printed or saved (rule S9). Every string goes through a
# Masker before it reaches the console or a results file, after the sources have already masked.
# The known secrets of a run (serials, bond IDs, the chip ID, the console's USB serial, the board's
# Bluetooth address) are replaced by tags; general patterns catch anything the run did not know it
# held (any Bluetooth address, as text or in a Windows device name, among them). As a backstop,
# any run of 16+ hex digits becomes <hex:N> and is counted (a non-zero count is a WARN: some
# decoder missed a field). Bond bytes and chip IDs never leave memory; results keep only a CRC-32
# or the word "equal".
import re

# Order matters: specific known secrets first, then the general patterns.
_OSC_SERIAL = re.compile(r"\b(OSC|OSB)[0-9A-Z]{10}\b")
# any other 13-char word of 0-9A-Z that has at least one letter and one digit (a serial)
_GENERIC_SERIAL = re.compile(r"\b(?=[0-9A-Z]{13}\b)(?=[0-9A-Z]*[A-Z])(?=[0-9A-Z]*[0-9])[0-9A-Z]{13}\b")
_CHIP16 = re.compile(r"\b[0-9A-Fa-f]{16}\b")
_STEAMID = re.compile(r"\b7656119\d{10}\b")
_ACCOUNT = re.compile(r"\[U:1:\d+\]")
# a user folder: a drive letter (or MSYS's /c), then Users, then the account's folder, with either
# slash and also with the doubled separators an exception's repr of a path shows; the folder's
# name ends at a separator, white space or a quote
_USERPATH = re.compile(r"((?:\b[A-Za-z]:|/[A-Za-z])[\\/]+Users[\\/]+)[^\\/\s'\"]+", re.IGNORECASE)
# the instance part of device paths: \\?\HID#VID_..&COL03#<instance>#{class} (an interface
# path, also with doubled backslashes), and USB\VID_xxxx&PID_xxxx[&MI_nn]\<instance> or
# HID\VID_..&COLnn\<instance> (instance IDs)
_IFACEPATH = re.compile(r"((?:\\{1,4}|/{1,2})\?(?:\\{1,2}|/)[A-Za-z0-9_]+#[^#\s'\"]+#)"
                        r"([^#\s'\"]+)(?=#)")
_INSTANCEID = re.compile(r"(\b(?:USB|HID)(?:\\{1,2}|/)VID_[0-9A-Fa-f]{4}&PID_[0-9A-Fa-f]{4}"
                         r"[^\\/\s'\"]*(?:\\{1,2}|/))([^\\/\s'\"#]+)", re.IGNORECASE)
# the same for a Bluetooth device's nodes: HID\{service}_DEV_...\<instance>,
# BTHLEDEVICE\{service}_DEV_...\<instance>, BTHLE\DEV_<address>\<instance>
_BTINSTANCE = re.compile(r"(\b(?:HID|BTHLEDEVICE)(?:\\{1,2}|/)\{[0-9A-Fa-f-]{36}\}[^\\/\s'\"]*"
                         r"(?:\\{1,2}|/)|\bBTHLE(?:\\{1,2}|/)DEV_[^\\/\s'\"]*(?:\\{1,2}|/))"
                         r"([^\\/\s'\"#]+)", re.IGNORECASE)
_HEXRUN = re.compile(r"[0-9A-Fa-f]{16,}")
# Bluetooth addresses: as text (C3:4F:12:AB:CD:EF, the console's form) and as Windows names a
# Bluetooth device's nodes (BTHLE\DEV_<12 hex>, ..._REV&0100_<12 hex>&COL03, also in interface
# paths), which identify the board and the PC's other devices
_BTADDR = re.compile(r"(?<![0-9A-Fa-f:])[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}(?![0-9A-Fa-f:])")
_BTNODE = re.compile(r"((?:_REV&[0-9A-F]{4}_|\bDEV_))([0-9A-F]{12})(?![0-9A-F])", re.IGNORECASE)
# an 8-hex-digit word not preceded by 0x (a bond ID printed as %08x in a debug build), used in the
# console transcript and Steam's log only: masking a nonce now and then costs nothing.
_ID8 = re.compile(r"(?<![0-9A-Za-zx])[0-9A-Fa-f]{8}(?![0-9A-Fa-f])")


class Masker:
    """Holds a run's known secrets and masks every string that passes through it."""

    def __init__(self):
        self._secrets = []          # (compiled-or-literal, replacement), longest literals first
        self._words = []            # (compiled word pattern, replacement): IDs in their forms
        self.hex_dropped = 0        # the 16-hex backstop fired this many times (a WARN if > 0)

    def add_id_secret(self, value, tag="<id>"):
        """A 32-bit ID (a bond's puck or controller ID) in every form a log may print it: decimal,
        unpadded hex in either case and 8-digit hex, each as a whole word (an ID never masks
        part of another number). Forms shorter than 4 characters are left out: they would mask
        ordinary numbers, and an ID that small is not one a run can tell apart anyway."""
        try:
            v = int(value) & 0xFFFFFFFF
        except (TypeError, ValueError):
            return
        if v == 0:
            return
        forms = {str(v), "%x" % v, "%X" % v, "%08x" % v, "%08X" % v}
        for f in sorted(forms, key=len, reverse=True):
            if len(f) < 4:
                continue
            pat = re.compile(r"(?<![0-9A-Za-z])%s(?![0-9A-Za-z])" % re.escape(f))
            if all(p.pattern != pat.pattern for p, _t in self._words):
                self._words.append((pat, tag))

    def add_secret(self, value, tag):
        """Register a literal secret (both hex cases for IDs) to be replaced by `tag`."""
        if not value:
            return
        v = str(value)
        if v not in [s for s, _ in self._literals()]:
            self._secrets.append((v, tag))
            # bond IDs and the chip ID appear in either hex case
            if re.fullmatch(r"[0-9A-Fa-f]+", v) and not v.isdigit():
                self._secrets.append((v.lower(), tag))
                self._secrets.append((v.upper(), tag))
        self._secrets.sort(key=lambda p: len(p[0]), reverse=True)

    def add_ble_address(self, address):
        """The board's own Bluetooth address (any form: C3:4F:..., 12 hex digits) in every form
        a log or a device name prints it."""
        hexes = re.sub(r"[^0-9A-Fa-f]", "", str(address or ""))
        if len(hexes) != 12:
            return
        colons = ":".join(hexes[i:i + 2] for i in range(0, 12, 2))
        for form in (hexes, colons):
            self.add_secret(form, "<bt-addr>")

    def _literals(self):
        return self._secrets

    def text(self, s, transcript=False):
        """Mask `s`. `transcript`: also mask lone 8-hex words (console log, Steam log)."""
        if s is None:
            return s
        out = str(s)
        for literal, tag in self._secrets:
            if literal:
                out = out.replace(literal, tag)
        for pat, tag in self._words:
            out = pat.sub(tag, out)
        out = _BTNODE.sub(r"\1<bt-addr>", out)
        out = _BTADDR.sub("<bt-addr>", out)
        out = _IFACEPATH.sub(r"\1<masked>", out)
        out = _INSTANCEID.sub(r"\1<masked>", out)
        out = _BTINSTANCE.sub(r"\1<masked>", out)
        out = _OSC_SERIAL.sub(lambda m: m.group(1) + "*" * 10, out)
        out = _GENERIC_SERIAL.sub("<serial>", out)
        out = _STEAMID.sub("<steamid>", out)
        out = _ACCOUNT.sub("[U:1:<account>]", out)
        out = _USERPATH.sub(r"\1<user>", out)
        out = _CHIP16.sub("<chip>", out)
        if transcript:
            out = _ID8.sub("<id>", out)
        # backstop: any remaining run of 16+ hex digits (some decoder missed a field)
        out, n = _HEXRUN.subn(lambda m: "<hex:%d>" % len(m.group(0)), out)
        self.hex_dropped += n
        return out

    def value(self, obj, transcript=False):
        """Mask recursively through lists and dicts; numbers and bools pass unchanged."""
        if isinstance(obj, str):
            return self.text(obj, transcript=transcript)
        if isinstance(obj, dict):
            return {k: self.value(v, transcript=transcript) for k, v in obj.items()}
        if isinstance(obj, (list, tuple)):
            return [self.value(v, transcript=transcript) for v in obj]
        return obj
