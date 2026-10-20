# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# test_safety.py: classify() on synthetic device lists, the read-only puck reader's
# source scan (S2), the vendor denylist on every transport with the flash-token rules, the shell
# allowlist, and injection preconditions. Synthetic data only.
import ast
import os
import unittest

from hwtest import config as C
from hwtest import safety
from hwtest.proto import testproto as T
from hwtest.proto import vendor as V


def node(kind, instance, iface, serial=None, product=None):
    return {"kind": kind, "instance_id": instance, "interface_id": iface, "serial": serial,
            "usb_product": product}


class Classify(unittest.TestCase):
    def test_board(self):
        n = node("hid", r"USB\VID_28DE&PID_1302\OSC0123456789",
                 r"\\?\hid#vid_28de&pid_1302&col03#x", "OSC0123456789", "Steam Controller")
        self.assertEqual(safety.classify(n)[0], safety.BOARD_HID)

    def test_non_hex_osc_serial_allowed(self):
        n = node("hid", r"USB\VID_28DE&PID_1302\OSC2DEV000001",
                 r"\\?\hid#vid_28de&pid_1302&col03#x", "OSC2DEV000001")
        self.assertEqual(safety.classify(n)[0], safety.BOARD_HID)

    def test_real_controller_forbidden(self):
        n = node("hid", r"USB\VID_28DE&PID_1302\NOTOSC123456A",
                 r"\\?\hid#vid_28de&pid_1302&col03#x", "NOTOSC123456A")
        self.assertEqual(safety.classify(n)[0], safety.FORBIDDEN)

    def test_no_serial_forbidden(self):
        n = node("hid", r"USB\VID_28DE&PID_1302\6&2f3a5e1&0&3",
                 r"\\?\hid#vid_28de&pid_1302&col03#x", None)
        self.assertEqual(safety.classify(n)[0], safety.FORBIDDEN)

    def test_1303_forbidden(self):
        n = node("hid", r"USB\VID_28DE&PID_1303\X", r"...&col03", "X")
        self.assertEqual(safety.classify(n)[0], safety.FORBIDDEN)

    def test_puck_slots_and_shell(self):
        for mi in (2, 3, 4, 5):
            n = node("hid", r"USB\VID_28DE&PID_1304\FAKEPUCK00001",
                     r"\\?\hid#vid_28de&pid_1304&mi_0%d&col03#x" % mi, "FAKEPUCK00001")
            self.assertEqual(safety.classify(n)[0], safety.PUCK_SLOT)
        for mi in (0, 1, 6):
            n = node("hid", r"USB\VID_28DE&PID_1304\FAKEPUCK00001",
                     r"\\?\hid#vid_28de&pid_1304&mi_0%d&col03#x" % mi, "FAKEPUCK00001")
            self.assertEqual(safety.classify(n)[0], safety.FORBIDDEN)

    def test_console_and_wrong_product(self):
        ok = node("port", r"USB\VID_2FE3&PID_0004\X", r"\\?\usb#vid_2fe3&pid_0004&mi_00",
                  product=C.CONSOLE_PRODUCT)
        self.assertEqual(safety.classify(ok)[0], safety.BOARD_CONSOLE)
        bad = node("port", r"USB\VID_2FE3&PID_0004\X", r"...", product="Some other port")
        self.assertEqual(safety.classify(bad)[0], safety.FORBIDDEN)

    def test_col01_col02_and_uf2(self):
        self.assertEqual(safety.classify(node("hid", r"USB\VID_28DE&PID_1302\OSC0123456789",
                         r"...&col01#x", "OSC0123456789"))[0], safety.FORBIDDEN)
        self.assertEqual(safety.classify(node("hid", r"USB\VID_239A&PID_0029\X", r"..."))[0],
                         safety.FORBIDDEN)

    def test_serial_check_before_write(self):
        n = node("hid", r"USB\VID_28DE&PID_1302\OSC0123456789", r"...&col03", "OSC0123456789")
        with self.assertRaises(safety.SafetyError):
            safety.assert_board_write(n, "OSCDIFFERENT9")   # the HID string differs: refuse
        safety.assert_board_write(n, "OSC0123456789")


class PuckReadSource(unittest.TestCase):
    FORBIDDEN_NAMES = {"HidD_SetFeature", "HidD_GetFeature", "HidD_SetOutputReport",
                       "HidD_GetInputReport", "WriteFile", "DeviceIoControl",
                       "HidD_GetSerialNumberString", "HidD_GetManufacturerString",
                       "HidD_GetProductString"}

    def _names(self, source):
        names = set()
        for n in ast.walk(ast.parse(source)):
            if isinstance(n, ast.Attribute):
                names.add(n.attr)
            elif isinstance(n, ast.Name):
                names.add(n.id)
            elif isinstance(n, ast.Constant) and isinstance(n.value, str):
                names.add(n.value)
        return names

    # S2: the only functions win/puck_read.py may bind
    S2_ALLOWED = {"HidD_GetAttributes", "HidD_GetPreparsedData", "HidD_FreePreparsedData",
                  "HidP_GetCaps", "HidP_GetValueCaps", "HidD_SetNumInputBuffers", "CreateFileW",
                  "ReadFile", "GetOverlappedResult", "CancelIoEx", "CloseHandle", "CreateEventW",
                  "WaitForSingleObject"}

    @staticmethod
    def _source():
        path = os.path.join(os.path.dirname(os.path.dirname(__file__)), "win", "puck_read.py")
        with open(path, encoding="utf-8") as f:
            return f.read()

    def scan(self, source):
        """What is wrong with a puck reader's source: a bound name outside S2, a HID or kernel32
        function reached another way, a forbidden name anywhere, an import of the sending
        modules."""
        tree = ast.parse(source)
        problems = []
        bindings = None
        for n in ast.walk(tree):
            if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "_BINDINGS"
                                                 for t in n.targets):
                bindings = ast.literal_eval(n.value)
            if isinstance(n, ast.ImportFrom) and (n.module or "").split(".")[-1] in (
                    "hid", "puck_write"):
                problems.append("imports %s" % n.module)
            if isinstance(n, ast.ImportFrom) and any(a.name in ("hid", "puck_write")
                                                     for a in n.names):
                problems.append("imports %s" % [a.name for a in n.names])
            if isinstance(n, ast.Attribute) and (
                    (isinstance(n.value, ast.Call) and getattr(n.value.func, "attr", None) in
                     ("WinDLL", "CDLL", "OleDLL")) or
                    (isinstance(n.value, ast.Attribute) and n.value.attr in ("windll", "oledll",
                                                                             "cdll"))):
                if n.attr not in self.S2_ALLOWED:
                    problems.append("reaches %s outside _BINDINGS" % n.attr)
            if isinstance(n, ast.Attribute) and n.attr in ("windll", "oledll", "cdll"):
                problems.append("uses ctypes.%s" % n.attr)
        if bindings is None:
            problems.append("no _BINDINGS")
        else:
            if set(bindings) - {"kernel32", "hid"}:
                problems.append("binds from %s" % sorted(set(bindings) - {"kernel32", "hid"}))
            for names in bindings.values():
                for name in names:
                    if name not in self.S2_ALLOWED:
                        problems.append("binds %s" % name)
        names = self._names(source)
        for name in names:
            if name in self.FORBIDDEN_NAMES:
                problems.append("names %s" % name)
            elif (name.startswith("HidD_") or name.startswith("HidP_")) and \
                    name not in self.S2_ALLOWED:
                problems.append("names %s" % name)
        return problems

    def test_puck_read_binds_only_s2(self):
        self.assertEqual(self.scan(self._source()), [])

    def test_planted_bindings_fail(self):
        src = self._source()
        planted = src.replace('"HidD_SetNumInputBuffers"),', '"HidD_SetNumInputBuffers", '
                              '"HidD_SetFeature"),', 1)
        self.assertNotEqual(planted, src)
        self.assertTrue(self.scan(planted))
        for line in ("x = ctypes.windll.kernel32.DeviceIoControl\n",
                     "x = ctypes.WinDLL('hid').HidD_GetFeature\n",
                     "from .hid import BoardHid\n",
                     "y = 'WriteFile'\n",
                     "z = 'HidD_GetSerialNumberString'\n"):
            self.assertTrue(self.scan(src + "\n" + line), line)

    def test_reader_has_no_sending_method(self):
        tree = ast.parse(self._source())
        for cls in [n for n in ast.walk(tree) if isinstance(n, ast.ClassDef)]:
            methods = {m.name for m in cls.body if isinstance(m, ast.FunctionDef)}
            for bad in ("write", "write_output", "set_feature", "feature_set", "set_feature1",
                        "send", "get_feature"):
                self.assertNotIn(bad, methods, "%s.%s" % (cls.name, bad))


class VendorDenylist(unittest.TestCase):
    def test_hard_refusals(self):
        for op in (0x90, 0x86, 0xFE, 0xC0, 0xC3, 0xD8):
            with self.assertRaises(safety.SafetyError):
                safety.check_vendor(op, b"", "usb")

    def test_bootloader_key_behind_n0(self):
        # 0x95 with n=0 but the bootloader key sitting in buffer bytes 3..6 is still refused
        buf = bytearray(C.FEATURE_LEN)
        buf[0], buf[1], buf[2] = 1, 0x95, 0
        buf[3:7] = C.KEY_BOOTLOADER.to_bytes(4, "little")
        with self.assertRaises(safety.SafetyError):
            safety.check_feature_buffer(bytes(buf), "usb")

    def test_reboot_keys_allowed(self):
        safety.check_vendor(0x95, b"\x00\x00\x00\x00", "usb")          # plain reboot
        safety.check_vendor(0x95, C.KEY_REBOOT_WIRELESS.to_bytes(4, "little"), "usb")

    def test_e2_forms(self):
        for p in (b"\x00", b"\x01\x20", b"", b"\x02"):
            safety.check_vendor(0xE2, p, "usb")
        with self.assertRaises(safety.SafetyError):
            safety.check_vendor(0xE2, b"\x03\x04", "usb")

    def test_flash_needs_token(self):
        ctx = safety.Context()
        with self.assertRaises(safety.SafetyError):
            safety.check_vendor(C.VC_BOND_WRITE, bytes(24), "usb", ctx=ctx)
        ctx.flash_allowed = True
        with self.assertRaises(safety.SafetyError):
            safety.check_vendor(C.VC_BOND_WRITE, bytes(24), "usb", ctx=ctx)   # no token
        ctx.add_token(C.VC_BOND_WRITE, None, bytes(24))
        safety.check_vendor(C.VC_BOND_WRITE, bytes(24), "usb", ctx=ctx)       # now allowed, once
        with self.assertRaises(safety.SafetyError):
            safety.check_vendor(C.VC_BOND_WRITE, bytes(24), "usb", ctx=ctx)   # token spent

    def test_token_bound_to_value(self):
        ctx = safety.Context()
        ctx.flash_allowed = True
        ctx.add_token(C.VC_BOND_WRITE, None, bytes(24))
        with self.assertRaises(safety.SafetyError):
            safety.check_vendor(C.VC_BOND_WRITE, b"\x01" + bytes(23), "usb", ctx=ctx)

    def test_named_refusal_forms_allowed_without_flash(self):
        ctx = safety.Context()
        ctx.empty_slots = {"esb/bond_2"}
        # 0xDC type 2 (refused by firmware), 0xF0 on a name that is no key, 0xEE bond length != 24
        safety.check_vendor(C.VC_USER_STORE_SET, b"\x02\x01", "usb", ctx=ctx)
        safety.check_vendor(C.VC_BY_NAME_DELETE, C.NO_KEY.encode() + b"\x00", "usb", ctx=ctx)
        safety.check_vendor(C.VC_BY_NAME_SET, b"esb/bond\x00\x00", "usb", ctx=ctx)

    def test_crash_record_read(self):
        """0xF2 selector 1 marks a new crash record seen (a flash write): only after 0xED showed
        no new record, or with a token; never on the test link."""
        ctx = safety.Context()
        safety.check_vendor(C.VC_DEVICE_INFO, b"\x00", "usb", ctx=ctx)
        safety.check_vendor(C.VC_DEVICE_INFO, b"\x02", "usb", ctx=ctx)
        with self.assertRaises(safety.SafetyError):
            safety.check_vendor(C.VC_DEVICE_INFO, b"\x01", "usb", ctx=ctx)   # not read first
        ctx.read_values["debug/resetreason"] = b"\x01" + bytes(32)
        with self.assertRaises(safety.SafetyError):
            safety.check_vendor(C.VC_DEVICE_INFO, b"\x01", "usb", ctx=ctx)   # a new record
        ctx.read_values["debug/resetreason"] = bytes(33)
        safety.check_vendor(C.VC_DEVICE_INFO, b"\x01", "usb", ctx=ctx)
        ctx.read_values["debug/resetreason"] = b"\x01" + bytes(32)
        ctx.flash_allowed = True
        ctx.add_token(C.VC_DEVICE_INFO, "debug/resetreason", b"\x01")
        safety.check_vendor(C.VC_DEVICE_INFO, b"\x01", "usb", ctx=ctx)
        with self.assertRaises(safety.SafetyError):
            safety.check_host_set(C.VC_DEVICE_INFO, b"\x01")
        safety.check_host_set(C.VC_DEVICE_INFO, b"\x02")

    def test_calibration_keys_every_key(self):
        """S5 on every calibration key: never written, saved or deleted, whatever the tokens; only
        the forms every firmware refuses first (another size for 0xEE, a byte after the NUL for
        0xEF)."""
        ctx = safety.Context()
        ctx.flash_allowed = True
        for name, size in C.CAL_KEYS.items():
            key = name.encode() + b"\x00"
            safety.check_vendor(C.VC_BY_NAME_SET, key + bytes(size + 1), "usb", ctx=ctx)
            safety.check_vendor(C.VC_BY_NAME_SAVE, key + b"\x01", "usb", ctx=ctx)
            for opcode, value in ((C.VC_BY_NAME_SET, bytes(size)), (C.VC_BY_NAME_SAVE, b""),
                                  (C.VC_BY_NAME_DELETE, b"")):
                ctx.add_token(opcode, name, value)
                with self.assertRaises(safety.SafetyError):
                    safety.check_vendor(opcode, key + value, "usb", ctx=ctx)

    def test_kept_keys(self):
        """The keys the library keeps (keylock, charge window, crash record, ...): 0xEE of another
        size or of the value just read back; everything else with a token only."""
        ctx = safety.Context()
        lock = b"user/keylock\x00"
        safety.check_vendor(C.VC_BY_NAME_SET, lock + bytes(3), "usb", ctx=ctx)
        with self.assertRaises(safety.SafetyError):
            safety.check_vendor(C.VC_BY_NAME_SET, lock + bytes(4), "usb", ctx=ctx)
        ctx.read_values["user/keylock"] = bytes(4)
        safety.check_vendor(C.VC_BY_NAME_SET, lock + bytes(4), "usb", ctx=ctx)
        with self.assertRaises(safety.SafetyError):
            safety.check_vendor(C.VC_BY_NAME_SET, lock + b"\xC3\x0A\xFB\x41", "usb", ctx=ctx)
        # nothing read back yet: only another size without a token
        led = b"user/fully_charged_led_intensity\x00"
        safety.check_vendor(C.VC_BY_NAME_SET, led + b"\x01\x02", "usb", ctx=ctx)
        with self.assertRaises(safety.SafetyError):
            safety.check_vendor(C.VC_BY_NAME_SET, led + b"\x03", "usb", ctx=ctx)
        safety.check_vendor(C.VC_BY_NAME_SAVE, lock + b"\x01", "usb", ctx=ctx)   # refused form
        for opcode in (C.VC_BY_NAME_SAVE, C.VC_BY_NAME_DELETE):
            with self.assertRaises(safety.SafetyError):
                safety.check_vendor(opcode, lock, "usb", ctx=ctx)
        ctx.flash_allowed = True
        ctx.add_token(C.VC_BY_NAME_SAVE, "user/keylock", b"")
        safety.check_vendor(C.VC_BY_NAME_SAVE, lock, "usb", ctx=ctx)

    def test_calibration_keys_never_written(self):
        """S5: a calibration key is never written, saved or deleted, not even with --flash and a
        token; only the forms the firmware refuses go out."""
        ctx = safety.Context()
        ctx.flash_allowed = True
        joy = b"cal/joy_l\x00"
        ctx.add_token(C.VC_BY_NAME_SET, "cal/joy_l", bytes(18))
        ctx.add_token(C.VC_BY_NAME_DELETE, "cal/joy_l", b"")
        for opcode, payload in ((C.VC_BY_NAME_SET, joy + bytes(18)),
                                (C.VC_BY_NAME_SAVE, joy),
                                (C.VC_BY_NAME_DELETE, joy),
                                (C.VC_BY_NAME_DELETE, b"cal/foo\x00"),
                                (C.VC_BY_NAME_SET, b"cal/foo\x00\x01"),
                                (C.VC_BY_NAME_SET, b"cal/rgbw_r\x00\x5a"),
                                (C.VC_BY_NAME_DELETE, b"cal/sensors/gyroscope/bias\x00"),
                                # a cal/ name the library does not know goes to the application,
                                # which may take it: not even the forms the library refuses
                                (C.VC_BY_NAME_SAVE, b"cal/foo\x00\x00"),
                                (C.VC_BY_NAME_DELETE, b"cal/foo\x00\x00"),
                                (C.VC_BY_NAME_SET, b"cal/joy_x\x00" + bytes(17))):
            with self.assertRaises(safety.SafetyError):
                safety.check_vendor(opcode, payload, "usb", ctx=ctx)
        # refused by the firmware: a wrong size, a byte after the NUL
        safety.check_vendor(C.VC_BY_NAME_SET, joy + b"\x00", "usb", ctx=ctx)
        safety.check_vendor(C.VC_BY_NAME_SET, joy + bytes(17), "usb", ctx=ctx)
        safety.check_vendor(C.VC_BY_NAME_SAVE, joy + b"\x00", "usb", ctx=ctx)
        safety.check_vendor(C.VC_BY_NAME_DELETE, joy + b"\x00", "usb", ctx=ctx)
        # the calibration commands themselves stay hard-refused
        for opcode in (0xD8, 0xC0, 0xC3, 0xFE):
            with self.assertRaises(safety.SafetyError):
                safety.check_vendor(opcode, b"\x00\x01", "usb", ctx=ctx)

    def test_transport_and_chime_values(self):
        """safety-HW-6: 0x01 is ignored only for the transport key (Bluetooth); the chime key
        stores any one-byte value, so there only the refused lengths or the level read back."""
        ctx = safety.Context()
        transport = b"user/wireless_transport\x00"
        chime = b"user/haptic_boot_level\x00"
        safety.check_vendor(C.VC_BY_NAME_SET, transport + b"\x01", "usb", ctx=ctx)
        with self.assertRaises(safety.SafetyError):
            safety.check_vendor(C.VC_BY_NAME_SET, chime + b"\x01", "usb", ctx=ctx)
        for refused in (b"", b"\x02\x02"):
            safety.check_vendor(C.VC_BY_NAME_SET, chime + refused, "usb", ctx=ctx)
        ctx.read_values["user/haptic_boot_level"] = b"\x03"
        safety.check_vendor(C.VC_BY_NAME_SET, chime + b"\x03", "usb", ctx=ctx)
        with self.assertRaises(safety.SafetyError):
            safety.check_vendor(C.VC_BY_NAME_SET, chime + b"\x01", "usb", ctx=ctx)

    def test_transport_in_a_bluetooth_build(self):
        """safety-HW-6: in a Bluetooth build (caps bit 17) 0x01 selects Bluetooth slot 2, so
        only the value read back (or a token) passes."""
        ctx = safety.Context()
        ctx.caps_ble = True
        transport = b"user/wireless_transport\x00"
        with self.assertRaises(safety.SafetyError):
            safety.check_vendor(C.VC_BY_NAME_SET, transport + b"\x01", "usb", ctx=ctx)
        ctx.read_values["user/wireless_transport"] = b"\x01"
        safety.check_vendor(C.VC_BY_NAME_SET, transport + b"\x01", "usb", ctx=ctx)

    def test_feature_report_2_never(self):
        buf = bytearray(C.FEATURE_LEN)
        buf[0] = 2
        with self.assertRaises(safety.SafetyError):
            safety.check_feature_buffer(bytes(buf), "usb")


class PuckGate(unittest.TestCase):
    """S3, S4: the puck is written only through safety.puck_writer(), never with feature report 2,
    and its reader is read-only."""

    HERE = os.path.dirname(os.path.dirname(__file__))

    def test_writer_refusals(self):
        made = []

        def make():
            made.append(1)
            return "writer"

        for kw in ({"through_puck": False, "steam_running": False, "slot_proven_s": 1.0},
                   {"through_puck": True, "steam_running": True, "slot_proven_s": 1.0},
                   {"through_puck": True, "steam_running": False, "slot_proven_s": None},
                   {"through_puck": True, "steam_running": False, "slot_proven_s": 10.5}):
            with self.assertRaises(safety.SafetyError):
                safety.puck_writer(make, **kw)
        self.assertEqual(made, [])
        self.assertEqual(safety.puck_writer(make, through_puck=True, steam_running=False,
                                            slot_proven_s=2.0), "writer")

    def test_no_feature_report_2(self):
        for transport in ("puck", "usb"):
            buf = bytearray(C.FEATURE_LEN)
            buf[0], buf[1] = 2, C.VC_ATTRIBUTES
            with self.assertRaises(safety.SafetyError):
                safety.check_feature_buffer(bytes(buf), transport)

    def test_reader_access_is_read_only(self):
        with open(os.path.join(self.HERE, "win", "puck_read.py"), encoding="utf-8") as f:
            tree = ast.parse(f.read())
        consts = {}
        for n in ast.walk(tree):
            if isinstance(n, ast.Assign) and isinstance(n.value, ast.Constant):
                for t in n.targets:
                    if isinstance(t, ast.Name):
                        consts[t.id] = n.value.value
        self.assertEqual(consts.get("_GENERIC_READ"), 0x80000000)
        values = {n.value for n in ast.walk(tree) if isinstance(n, ast.Constant)}
        self.assertFalse(values & {0x40000000 | 0x80000000, 0x10000000},
                         "GENERIC_WRITE/ALL in the reader")
        self.assertNotIn("_GENERIC_WRITE", consts)
        access = [n for n in ast.walk(tree) if isinstance(n, ast.Assign) and
                  any(isinstance(t, ast.Name) and t.id == "ACCESS" for t in n.targets)]
        self.assertEqual(len(access), 1)
        self.assertEqual(getattr(access[0].value, "id", None), "_GENERIC_READ")

    def test_only_the_gate_makes_a_writer(self):
        """Every call of a backend's puck_writer() is the factory handed to safety.puck_writer()
        (board.py), and PuckSlotWriter is built only by win/backend.py's puck_writer()."""
        hits = []
        for folder, _dirs, files in os.walk(self.HERE):
            if os.path.basename(folder) in ("selftest", "__pycache__"):
                continue
            for name in files:
                if not name.endswith(".py"):
                    continue
                path = os.path.join(folder, name)
                with open(path, encoding="utf-8") as f:
                    tree = ast.parse(f.read())
                rel = os.path.relpath(path, self.HERE).replace(os.sep, "/")
                for n in ast.walk(tree):
                    if isinstance(n, ast.Call):
                        fn = n.func
                        fname = getattr(fn, "attr", None) or getattr(fn, "id", None)
                        if fname == "PuckSlotWriter":
                            hits.append((rel, "PuckSlotWriter()"))
                        if fname == "puck_writer" and isinstance(fn, ast.Attribute) and \
                                getattr(fn.value, "id", None) != "safety":
                            hits.append((rel, "backend.puck_writer()"))
                    if isinstance(n, ast.Attribute) and n.attr == "puck_writer" and \
                            getattr(n.value, "attr", None) == "backend":
                        hits.append((rel, "backend.puck_writer"))
        self.assertEqual(sorted(set(hits)), [("board.py", "backend.puck_writer"),
                                             ("win/backend.py", "PuckSlotWriter()")])


class Probes(unittest.TestCase):
    """The requests cmd.framing and hooks.invisible send to be refused: exactly these, never a
    keyed request that does something, never to the puck."""

    def test_test_probes(self):
        key = C.TEST_KEY_BYTES
        safety.check_test_probe(bytes([0x1E, 0xAB, 0x57, 0x7F, 1, 9, C.OP_STATUS]))   # wrong key
        safety.check_test_probe(key + bytes([2, 9, C.OP_STATUS]))                     # version 2
        safety.check_test_probe(key + bytes([1, 9]))                                  # n = 6
        safety.check_test_probe(key + bytes([1, 9, C.OP_STATUS]))                     # STATUS
        for op in (C.OP_ARM, C.OP_INJECT, C.OP_HOST_SET, C.OP_SEQ_RUN, C.OP_DISARM):
            with self.assertRaises(safety.SafetyError):
                safety.check_test_probe(key + bytes([1, 9, op]))
        with self.assertRaises(safety.SafetyError):
            safety.check_test_probe(bytes(C.ANSWER_MAX + 1))

    def test_framing_probes(self):
        self.assertEqual(safety.check_framing_probe("set2")[:3], bytes([2, C.VC_ATTRIBUTES, 0]))
        self.assertEqual(safety.check_framing_probe("set_output"),
                         bytes([0x80]) + bytes(C.FEATURE_LEN - 1))
        self.assertEqual(set(safety.check_framing_probe("get2")[1:]), {0})
        self.assertEqual(safety.check_framing_probe("get_input")[0], C.RPT_STATE)
        for kind in ("set1", "", "set2 ", "puck"):
            with self.assertRaises(safety.SafetyError):
                safety.check_framing_probe(kind)


class HostSetAllowlist(unittest.TestCase):
    def test_allow_and_refuse(self):
        for op in (0x81, 0x83, 0x87, 0x89, 0xAE, 0xBE, 0xC5, 0xF2):
            safety.check_host_set(op, b"")
        for op in (0x90, 0x86, 0xA2, 0xEE, 0xEF, 0xF0, C.TEST_OPCODE):
            with self.assertRaises(safety.SafetyError):
                safety.check_host_set(op, b"")

    def test_host_set_0x95_turn_off_only(self):
        safety.check_host_set(0x95, C.KEY_TURN_OFF.to_bytes(4, "little"))
        with self.assertRaises(safety.SafetyError):
            safety.check_host_set(0x95, C.KEY_BOOTLOADER.to_bytes(4, "little"))


class ShellAllowlist(unittest.TestCase):
    def test_allow(self):
        safety.check_shell("opensc2 status")
        safety.check_shell("opensc2 test 1 raw 1eab577e010603")
        safety.check_shell("shell echo off")

    def test_refuse(self):
        for cmd in ("uf2", "opensc2 forget 1", "opensc2 id set unit X", "opensc2 mode wireless",
                    "opensc2 slot 2", "kernel reboot"):
            with self.assertRaises(safety.SafetyError):
                safety.check_shell(cmd)

    def test_policy_token(self):
        tok = safety.FlashToken(None, None, 0)
        safety.check_shell("opensc2 mode auto", flash_allowed=True, policy_token=tok,
                           policy_in_use="auto")
        self.assertTrue(tok.used)
        with self.assertRaises(safety.SafetyError):
            safety.check_shell("opensc2 mode auto", flash_allowed=True,
                               policy_token=safety.FlashToken(None, None, 0), policy_in_use="wireless")

    def test_pre_check_keeps_the_token(self):
        tok = safety.FlashToken(None, None, 0)
        safety.check_shell("opensc2 mode auto", flash_allowed=True, policy_token=tok,
                           policy_in_use="auto", consume=False)
        self.assertFalse(tok.used)

    def test_control_characters_and_white_space(self):
        """safety-HW-9: a line that would run a second command never goes out."""
        for cmd in ("opensc2 test 5 sync\ruf2", "opensc2 test 5 sync\nuf2", "opensc2 status\r",
                    "opensc2 status\x00", "opensc2 status\x1b[A", " opensc2 status",
                    "opensc2  status", "opensc2 status ", "opensc2 test 5 sync\tuf2"):
            with self.assertRaises(safety.SafetyError, msg=repr(cmd)):
                safety.check_shell(cmd)

    def test_test_line_grammar(self):
        for cmd in ("opensc2 test 1 sync", "opensc2 test 65535 status", "opensc2 test 9 cnt",
                    "opensc2 test 9 ring", "opensc2 test 9 ring 12", "opensc2 test 9 settings",
                    "opensc2 test 9 inject off", "opensc2 test 9 seq stop",
                    "opensc2 test 9 disarm"):
            safety.check_shell(cmd)
        for cmd in ("opensc2 test 0 sync", "opensc2 test 65536 sync", "opensc2 test sync",
                    "opensc2 test 9 arm 7e57ab1e", "opensc2 test 9 frobnicate",
                    "opensc2 test 9 raw 1eab577e01060", "opensc2 test 9 raw zz",
                    "opensc2 test 9 raw abababababababab",          # no test key
                    "opensc2 test 9 sync extra"):
            with self.assertRaises(safety.SafetyError, msg=cmd):
                safety.check_shell(cmd)

    def test_raw_requests_are_checked(self):
        """A `raw` line is checked like a 0x7E request: the HOST_SET allowlist, and S7 on the
        live Context for an injection (no Context: refused)."""
        def raw(op, args):
            return "opensc2 test 7 raw " + T.build_request(op, args, 7).hex()

        safety.check_shell(raw(C.OP_HOST_SET, bytes([0x81, 0])))
        with self.assertRaises(safety.SafetyError):
            safety.check_shell(raw(C.OP_HOST_SET, bytes([0x90, 0])))
        with self.assertRaises(safety.SafetyError):
            safety.check_shell(raw(C.OP_HOST_SET, bytes([0x95, 4]) +
                                   C.KEY_BOOTLOADER.to_bytes(4, "little")))
        inject = raw(C.OP_INJECT, bytes(53))
        with self.assertRaises(safety.SafetyError):
            safety.check_shell(inject)                              # no Context: fail closed
        ctx = Injection._ok_ctx()
        safety.check_shell(inject, ctx=ctx)
        ctx.lizard_off = False
        with self.assertRaises(safety.SafetyError):
            safety.check_shell(inject, ctx=ctx)


class SendTimeChecks(unittest.TestCase):
    """safety-HW-9: S5 and S7 again where the bytes go out (the feature buffer of USB and of the
    puck, output reports), the Rig's checks being pre-checks that keep the token."""

    def _buf(self, opcode, payload):
        return V.build_set(opcode, payload)

    def test_test_channel_buffers(self):
        ctx = Injection._ok_ctx()
        status = T.build_request(C.OP_STATUS, b"", 3)
        safety.check_feature_buffer(self._buf(C.TEST_OPCODE, status), "usb", ctx)
        safety.check_feature_buffer(self._buf(C.TEST_OPCODE, status), "puck", ctx)
        # the probes the firmware must refuse (USB only)
        safety.check_feature_buffer(self._buf(C.TEST_OPCODE, bytes([0, 0, 0, 0, 1, 5, 3])),
                                    "usb", ctx)
        with self.assertRaises(safety.SafetyError):
            safety.check_feature_buffer(self._buf(C.TEST_OPCODE, bytes([0, 0, 0, 0, 1, 5, 3])),
                                        "puck", ctx)
        # through the puck only reading ops
        mark = T.build_request(C.OP_MARK, bytes(4), 3)
        safety.check_feature_buffer(self._buf(C.TEST_OPCODE, mark), "usb", ctx)
        with self.assertRaises(safety.SafetyError):
            safety.check_feature_buffer(self._buf(C.TEST_OPCODE, mark), "puck", ctx)
        # HOST_SET's allowlist
        with self.assertRaises(safety.SafetyError):
            safety.check_feature_buffer(self._buf(C.TEST_OPCODE, T.build_request(
                C.OP_HOST_SET, bytes([0x86, 0]), 3)), "usb", ctx)

    def test_injection_needs_s7_at_the_send(self):
        inject = self._buf(C.TEST_OPCODE, T.build_request(C.OP_INJECT, bytes(53), 4))
        ctx = Injection._ok_ctx()
        safety.check_feature_buffer(inject, "usb", ctx)
        for attr in ("armed", "canary_passed", "sole_reader", "lizard_off"):
            bad = Injection._ok_ctx()
            setattr(bad, attr, False)
            with self.assertRaises(safety.SafetyError, msg=attr):
                safety.check_feature_buffer(inject, "usb", bad)
        with self.assertRaises(safety.SafetyError):
            safety.check_feature_buffer(inject, "usb", None)          # fail closed
        probe = Injection._ok_ctx()
        probe.lizard_off = False
        probe.lizard_probe = True            # inputs.lizard_lock's one DENIED probe
        safety.check_feature_buffer(inject, "usb", probe)
        lizard_flag = self._buf(C.TEST_OPCODE, T.build_request(
            C.OP_INJECT, bytes([C.INJECT_FLAG_LIZARD]) + bytes(52), 4))
        with self.assertRaises(safety.SafetyError):
            safety.check_feature_buffer(lizard_flag, "usb", ctx)
        seq = self._buf(C.TEST_OPCODE, T.build_request(C.OP_SEQ_RUN, bytes([1, 0]), 4))
        with self.assertRaises(safety.SafetyError):
            safety.check_feature_buffer(seq, "usb", safety.Context())
        state45 = self._buf(C.TEST_OPCODE, T.build_request(C.OP_APP_SEND,
                                                           bytes([0, C.RPT_BT_STATE]) + bytes(8),
                                                           4))
        with self.assertRaises(safety.SafetyError):
            safety.check_feature_buffer(state45, "usb", safety.Context())
        canary = self._buf(C.TEST_OPCODE, T.build_request(C.OP_APP_SEND,
                                                          bytes([0, C.RPT_MOUSE]) + bytes(5), 4))
        safety.check_feature_buffer(canary, "usb", safety.Context())   # not an injection

    def test_pre_check_keeps_the_flash_token(self):
        ctx = safety.Context()
        ctx.flash_allowed = True
        ctx.add_token(C.VC_BOND_WRITE, None, bytes(24))
        buf = self._buf(C.VC_BOND_WRITE, bytes(24))
        safety.check_feature_buffer(buf, "usb", ctx, consume=False)
        safety.check_feature_buffer(buf, "usb", ctx, consume=False)
        safety.check_feature_buffer(buf, "usb", ctx)                  # the send spends it
        with self.assertRaises(safety.SafetyError):
            safety.check_feature_buffer(buf, "usb", ctx)

    def test_output_reports(self):
        for rid in range(0x80, 0x8B):
            safety.check_output_report(bytes([rid, 0]))
        for report in (b"", bytes([0x7F, 0]), bytes([0x8B]), bytes([0x80]) + bytes(64)):
            with self.assertRaises(safety.SafetyError):
                safety.check_output_report(report)

    def test_framing_buffers(self):
        for kind in ("set2", "get2", "set_output", "get_input"):
            safety.check_framing_buffer(kind, safety.check_framing_probe(kind))
        with self.assertRaises(safety.SafetyError):
            safety.check_framing_buffer("set2", bytes([2, 0x90, 0]) + bytes(61))

    def test_the_send_functions_check(self):
        """The functions that send call the checks themselves (a source scan, no device)."""
        here = os.path.dirname(os.path.dirname(__file__))
        want = {("win/hid.py", "BoardHid", "feature_set"): "check_feature_buffer",
                ("win/hid.py", "BoardHid", "write_output"): "check_output_report",
                ("win/hid.py", "BoardHid", "feature_raw"): "check_framing_buffer",
                ("win/hid.py", "BoardHid", "output_raw"): "check_framing_buffer",
                ("win/puck_write.py", "PuckSlotWriter", "set_feature1"): "check_feature_buffer",
                ("transport.py", "ConsoleSession", "send_line"): "check_shell",
                ("sim.py", "SimBackend", "feature_set"): "check_feature_buffer",
                ("fwemu.py", "FwEmuBackend", "feature_set"): "check_feature_buffer"}
        for (rel, cls_name, fn_name), check in want.items():
            with open(os.path.join(here, *rel.split("/")), encoding="utf-8") as f:
                tree = ast.parse(f.read())
            cls = next(n for n in ast.walk(tree) if isinstance(n, ast.ClassDef) and
                       n.name == cls_name)
            fn = next(n for n in cls.body if isinstance(n, ast.FunctionDef) and
                      n.name == fn_name)
            calls = {getattr(n.func, "attr", None) for n in ast.walk(fn) if isinstance(n, ast.Call)}
            self.assertIn(check, calls, "%s %s.%s" % (rel, cls_name, fn_name))


class Injection(unittest.TestCase):
    @staticmethod
    def _ok_ctx():
        ctx = safety.Context()
        ctx.armed = ctx.guard = ctx.canary_passed = ctx.sole_reader = ctx.lizard_off = True
        ctx.steam_running = False
        return ctx

    def _ctx(self):
        return self._ok_ctx()

    def test_ok(self):
        safety.check_injection(self._ctx())

    def test_each_precondition(self):
        for attr in ("armed", "canary_passed", "sole_reader"):
            ctx = self._ctx()
            setattr(ctx, attr, False)
            with self.assertRaises(safety.SafetyError):
                safety.check_injection(ctx)
        ctx = self._ctx()
        ctx.steam_running = True
        with self.assertRaises(safety.SafetyError):
            safety.check_injection(ctx)
        ctx = self._ctx()
        ctx.lizard_off = False
        with self.assertRaises(safety.SafetyError):
            safety.check_injection(ctx)
        ctx = self._ctx()
        ctx.lizard_off = False
        ctx.lizard_group = True       # the lizard group may inject with lizard mode on
        safety.check_injection(ctx)


class SystemInjection(unittest.TestCase):
    """The system ops (SYS_INJECT, SYS_BOOT, SYS_EVENT) reach only the firmware's rules: armed with
    the guard, the canary passed, Steam closed; neither the sole reader nor lizard mode counts."""

    @staticmethod
    def _ctx():
        ctx = safety.Context()
        ctx.armed = ctx.guard = ctx.canary_passed = True
        return ctx

    def test_preconditions(self):
        safety.check_system_injection(self._ctx())       # no sole reader, lizard mode on: fine
        for attr in ("armed", "guard", "canary_passed"):
            ctx = self._ctx()
            setattr(ctx, attr, False)
            with self.assertRaises(safety.SafetyError):
                safety.check_system_injection(ctx)
        ctx = self._ctx()
        ctx.steam_running = True
        with self.assertRaises(safety.SafetyError):
            safety.check_system_injection(ctx)

    def test_at_the_send(self):
        for op, args in ((C.OP_SYS_INJECT, bytes(8)), (C.OP_SYS_BOOT, bytes([4]) + bytes(7)),
                         (C.OP_SYS_BOOT, bytes(7) + bytes([1])), (C.OP_SYS_EVENT, bytes(1))):
            req = T.build_request(op, args)
            with self.assertRaises(safety.SafetyError):
                safety.check_test_request(req, "console", safety.Context())
            safety.check_test_request(req, "console", self._ctx())
            safety.check_test_request(req, "usb", self._ctx())
            with self.assertRaises(safety.SafetyError):
                safety.check_test_request(req, "puck", self._ctx())   # not a reading op
        # the reading and stopping ones need nothing, nor SYS_BOOT's clear (all eight bytes 0),
        # which the restore sends: it injects nothing
        for op, args in ((C.OP_SYS_STATUS, b""), (C.OP_SYS_INJECT_OFF, b""),
                         (C.OP_SYS_BOOT, bytes(8))):
            safety.check_test_request(T.build_request(op, args), "console", safety.Context())
            safety.check_test_request(T.build_request(op, args), "usb", safety.Context())
        self.assertTrue(safety.system_clear(C.OP_SYS_BOOT, bytes(8)))
        self.assertFalse(safety.system_clear(C.OP_SYS_BOOT, bytes(7)))
        self.assertFalse(safety.system_clear(C.OP_SYS_INJECT, bytes(8)))


if __name__ == "__main__":
    unittest.main()
