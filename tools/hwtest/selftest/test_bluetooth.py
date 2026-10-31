# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# test_bluetooth.py: the Bluetooth path (--bluetooth, B). How a Windows Bluetooth HID node becomes
# the board's (safety.classify_ble, S1) and nothing else ever does; the check after opening; the
# masking of Bluetooth addresses (S9); the console's Bluetooth lines and BLE_STATUS; the shell
# allowlist's one Bluetooth exception; and whole dry runs against the simulator's Bluetooth link,
# with the board found, not on Bluetooth, or not connected by Windows. Synthetic data only: no
# address here is a real device's.
import unittest

from hwtest import config as C
from hwtest import mask as mask_mod
from hwtest import results as results_mod
from hwtest import runner as runner_mod
from hwtest import safety
from hwtest import sim
from hwtest.proto import console as cproto
from hwtest.proto import shellfmt as SF
from hwtest.proto import testproto as T

PASS = results_mod.PASS
FAIL = results_mod.FAIL
ERROR = results_mod.ERROR
SKIP = results_mod.SKIP

ADDR = "C34F12ABCDEF"
OTHER = "D2A1B2C3D4E5"
SERIAL = "OSC0123456789"
SVC = "{00001812-0000-1000-8000-00805F9B34FB}_DEV_VID&0228DE_PID&1303_REV&0100_"


def ble_node(address=ADDR, col="COL03", names=None, parent=None, vidpid="0228DE_PID&1303"):
    svc = SVC.replace("0228DE_PID&1303", vidpid) + address
    return {"kind": "hid", "own_instance_id": "HID\\%s&%s\\9&1a2b3c&0&0000" % (svc, col),
            "instance_id": "BTHLEDEVICE\\%s\\8&1a2b3c" % svc,
            "interface_id": "\\\\?\\hid#%s&%s#9&1a2b3c#{4d1e55b2}" % (svc.lower(), col.lower()),
            "serial": None, "usb_product": None, "location": None,
            "ble_address": parent if parent is not None else address,
            "ble_names": names if names is not None else [C.BLE_NAME_PREFIX + SERIAL]}


EXPECT = safety.BleExpect(ADDR, SERIAL)


class ClassifyBle(unittest.TestCase):
    def test_the_boards_vendor_collection(self):
        self.assertEqual(safety.classify_ble(ble_node(), EXPECT)[0], safety.BOARD_BLE)

    def test_its_mouse_and_keyboard_collections(self):
        for col in ("COL01", "COL02"):
            self.assertEqual(safety.classify_ble(ble_node(col=col), EXPECT)[0], safety.FORBIDDEN)

    def test_a_real_controller_paired_with_the_pc(self):
        real = ble_node(OTHER, names=[C.BLE_NAME_PREFIX + "NOTOSC000SIM1"])
        self.assertEqual(safety.classify_ble(real, EXPECT)[0], safety.FORBIDDEN)

    def test_the_boards_name_at_another_address(self):
        # the name alone never makes a node the board's
        self.assertEqual(safety.classify_ble(ble_node(OTHER), EXPECT)[0], safety.FORBIDDEN)

    def test_the_boards_address_under_another_name(self):
        for names in ([], ["My pad"], [C.BLE_NAME_PREFIX + "OSC9999999999"]):
            self.assertEqual(safety.classify_ble(ble_node(names=names), EXPECT)[0],
                             safety.FORBIDDEN, names)

    def test_a_device_node_with_another_address(self):
        self.assertEqual(safety.classify_ble(ble_node(parent=OTHER), EXPECT)[0],
                         safety.FORBIDDEN)

    def test_another_product(self):
        n = ble_node(vidpid="0228DE_PID&1302")
        self.assertEqual(safety.classify_ble(n, EXPECT)[0], safety.FORBIDDEN)

    def test_without_the_boards_identity(self):
        self.assertEqual(safety.classify_ble(ble_node(), None)[0], safety.FORBIDDEN)
        self.assertEqual(safety.classify_ble(ble_node(), safety.BleExpect(None, SERIAL))[0],
                         safety.FORBIDDEN)
        # a serial without the OSC prefix (a real controller's) is never the board's
        not_osc = safety.BleExpect(ADDR, "NOTOSC000SIM1")
        n = ble_node(names=[C.BLE_NAME_PREFIX + "NOTOSC000SIM1"])
        self.assertEqual(safety.classify_ble(n, not_osc)[0], safety.FORBIDDEN)

    def test_usb_nodes_are_not_bluetooth(self):
        usb = {"kind": "hid", "own_instance_id": r"HID\VID_28DE&PID_1302&COL03\7&x&0&0002",
               "instance_id": r"USB\VID_28DE&PID_1302\OSC0123456789",
               "interface_id": r"\\?\hid#vid_28de&pid_1302&col03#x", "serial": SERIAL}
        self.assertEqual(safety.classify_ble(usb, EXPECT)[0], safety.FORBIDDEN)

    def test_classify_never_takes_a_bluetooth_node(self):
        for n in (ble_node(), ble_node(col="COL01"), ble_node(OTHER)):
            self.assertEqual(safety.classify(n)[0], safety.FORBIDDEN)

    def test_after_opening(self):
        node = ble_node()
        for serial in ("", SERIAL, ADDR, ADDR.lower(), "C3:4F:12:AB:CD:EF"):
            safety.assert_ble_write(node, serial, EXPECT)
        with self.assertRaises(safety.SafetyError):
            safety.assert_ble_write(node, OTHER, EXPECT)
        with self.assertRaises(safety.SafetyError):
            safety.assert_ble_write(node, "NOTOSC000SIM1", EXPECT)
        with self.assertRaises(safety.SafetyError):
            safety.assert_ble_write(ble_node(OTHER), "", EXPECT)


class Masking(unittest.TestCase):
    def test_addresses_everywhere(self):
        m = mask_mod.Masker()
        line = ("bluetooth: on; slot 1, identity 1 C3:4F:12:AB:CD:EF (random) and "
                + ble_node()["own_instance_id"] + " " + ble_node()["interface_id"] + " "
                + "BTHLE\\DEV_" + OTHER + "\\7&2f0a1b3c&0&" + OTHER.lower())
        out = m.text(line)
        for secret in (ADDR, ADDR.lower(), "C3:4F:12:AB:CD:EF", OTHER, "1a2b3c", "2f0a1b3c"):
            self.assertNotIn(secret, out)
        self.assertIn("<bt-addr>", out)

    def test_the_boards_address_as_a_secret(self):
        m = mask_mod.Masker()
        m.add_ble_address("C3:4F:12:AB:CD:EF")
        self.assertEqual(m.text("a c34f12abcdef b C34F12ABCDEF"), "a <bt-addr> b <bt-addr>")

    def test_ordinary_text_stays(self):
        m = mask_mod.Masker()
        for text in ("interval 7500 us, ATT MTU 67", "0x1303 version 0x0100",
                     "12:34:56 a time", "{4D1E55B2-F16F-11CF-88CB-001111000030}"):
            self.assertEqual(m.text(text), text)


class Console(unittest.TestCase):
    def test_ble_status_lines_read_back(self):
        lines = SF.ble_status_lines({
            "enabled": True, "running": True, "slot": 0, "identity": 1,
            "address": "C3:4F:12:AB:CD:EF (random)", "name": C.BLE_NAME_PREFIX + SERIAL,
            "bonded": [True, False, False, False], "connected": True, "active": True,
            "security": 2, "mtu": 67, "interval_us": 7500, "reports": 10, "dropped": 0,
            "last_reason": 8})
        got = cproto.parse_ble_status(lines)
        self.assertEqual((got["on"], got["transport"], got["slot"], got["identity"]),
                         (True, True, 1, 1))
        self.assertEqual(cproto.address_hex(got["address"]), ADDR)
        self.assertEqual(got["name"], C.BLE_NAME_PREFIX + SERIAL)
        self.assertEqual(got["bonded"], [True, False, False, False])
        self.assertEqual((got["connected"], got["active"], got["last_reason"]), (True, True, 8))

    def test_advertising_and_off(self):
        for form, word in ((0, "pairing"), (1, "allow-listed"), (2, "directed")):
            lines = SF.ble_status_lines({"enabled": True, "advertising": True, "form": form,
                                         "window_left_s": 42, "address": None})
            got = cproto.parse_ble_status(lines)
            self.assertEqual((got["advertising"], got["form"], got["window_left_s"]),
                             (True, word, 42))
        got = cproto.parse_ble_status(SF.ble_status_lines({"enabled": True, "idle": "host"}))
        self.assertTrue(got["wireless_off"])
        got = cproto.parse_ble_status(SF.ble_status_lines({"enabled": False, "running": False}))
        self.assertEqual((got["on"], got["transport"], got["address"]), (False, False, None))

    def test_status_and_transport(self):
        lines = SF.status_lines(False, 2, "auto", False, 0, {
            "unit": SERIAL, "board": "OSB0123456789", "git": "x", "pid": 0x1302, "rev": 0x48,
            "build": 0x6A628359, "blbuild": 0}, transport=("Bluetooth 1", "Bluetooth 1"),
            ble_link=(4, 0, 0))
        got = cproto.parse_status(lines)
        self.assertEqual((got["link"], got["slot"], got.get("bluetooth")), ("connected", 1, True))
        self.assertEqual(got["transport"], "Bluetooth 1")
        got = cproto.parse_transport([SF.line("transport", "ble", "Bluetooth 1", "ble", "ble")])
        self.assertEqual((got["running"], got["stored"]), ("ble", "ble"))

    def test_the_shell_allowlist(self):
        safety.check_shell("opensc2 ble status")
        safety.check_shell("opensc2 transport")
        with self.assertRaises(safety.SafetyError):
            safety.check_shell("opensc2 ble pair")
        safety.check_shell("opensc2 ble pair", flash_allowed=True)
        ctx = safety.Context()
        ctx.flash_allowed = True
        safety.check_shell("opensc2 ble pair", ctx=ctx)
        for bad in ("opensc2 transport ble", "opensc2 transport puck save", "opensc2 ble unpair"):
            with self.assertRaises(safety.SafetyError):
                safety.check_shell(bad, flash_allowed=True)


class BleStatus(unittest.TestCase):
    # the bytes the library's own packer gives (tests/host/ble_cases.h, ble_status_pack)
    VECTOR = bytes([0x2D, 0xFF, 1, 3, 0x08, 5, 2, 0x08, 4, 16, 67, 0, 0x4C, 0x1D, 0, 0,
                    0x04, 0x03, 0x02, 0x01, 0xE8, 0x03, 0, 0, 3, 0, 0, 0])

    def test_the_librarys_bytes(self):
        st = T.decode_ble_status(self.VECTOR)
        self.assertEqual((st["enabled"], st["advertising"], st["connected"], st["active"],
                          st["trigger"], st["running"], st["pair_request"], st["wireless_off"]),
                         (True, False, True, True, False, True, False, False))
        self.assertEqual((st["form"], st["slot"], st["identity"], st["bonded"], st["identities"],
                          st["security"], st["last_reason"], st["link_state"], st["reason"]),
                         (0xFF, 1, 3, 0x08, 5, 2, 8, 4, 16))
        self.assertEqual((st["mtu"], st["interval_us"], st["window_left_ms"], st["reports"],
                          st["dropped"]), (67, 7500, 0x01020304, 1000, 3))
        self.assertEqual(T.encode_ble_status(st), self.VECTOR)

    def test_short(self):
        with self.assertRaises(ValueError):
            T.decode_ble_status(self.VECTOR[:27])


def _opt(**extra):
    opt = {"dry_run": True, "fault": None, "yes": True, "flash": False, "lizard": False,
           "through_puck": False, "with_steam": False, "soak": 0, "groups": None, "tests": None,
           "steam_control": False, "wired_only": False, "wireless_only": False, "quiet": True,
           "bluetooth": True}
    opt.update(extra)
    return opt


class Run:
    def __init__(self, start="B", setup=None, **extra):
        self.backend = sim.SimBackend(start_mode=start)
        if setup:
            setup(self.backend)
        self.runner = runner_mod.Runner(self.backend, _opt(**extra), mask_mod.Masker())
        self.code = self.runner.run()
        self.by_id = {}
        for g in self.runner.results.groups:
            for t in g["tests"]:
                self.by_id.setdefault(t.id, []).append(t)

    def statuses(self, tid):
        return [t.status for t in self.by_id.get(tid, [])]

    def bad(self):
        return sorted({(t.id, t.status) for ts in self.by_id.values() for t in ts
                       if t.status in (FAIL, ERROR)})


class DryRuns(unittest.TestCase):
    def test_the_bluetooth_path(self):
        r = Run()
        self.assertEqual(r.code, 0, r.bad())
        for tid in ("hooks.arm", "desc.ids", "cmd.attributes", "cmd.stored_request",
                    "inputs.imu", "inputs.full", "battery.values", "stream.ids",
                    "stream.cadence", "reports.app_send_raw", "outputs.haptics", "outputs.pcm",
                    "outputs.rumble_linkdown", "settings.invalid", "ble.link", "ble.cut", "ble.feature",
                    "ble.reconnect", "ble.wireless_off", "modes.warm_wireless", "modes.identity",
                    "restore.as_found"):
            self.assertEqual(r.statuses(tid), [PASS], tid)
        self.assertEqual(r.statuses("ble.window"), [SKIP])
        # an unwired board's state over Bluetooth (0x45, the quaternion cut) is no baseline WARN
        self.assertEqual(r.statuses("env.baseline"), [])
        # nothing of the wired or puck paths, and never 0x9F over Bluetooth
        for tid in ("cmd.turn_off_ignored", "cmd.framing", "wireless.slot", "modes.to_wired"):
            self.assertEqual(r.statuses(tid), [], tid)
        self.assertEqual(r.runner.results.safety["puck_writes"], 0)

    def test_opt_ins(self):
        r = Run(flash=True, lizard=True)
        self.assertEqual(r.code, 0, r.bad())
        self.assertEqual(r.statuses("ble.window"), [PASS])
        self.assertEqual(r.statuses("lizard.pointer"), [PASS])

    def test_board_not_on_bluetooth(self):
        r = Run(start="W")
        self.assertEqual(r.code, C.EXIT_PRECONDITION)
        self.assertEqual(r.statuses("env.bluetooth"), [ERROR])

    def test_windows_does_not_connect_the_board(self):
        def no_host(backend):
            backend.ble_host_on = False
        r = Run(setup=no_host)
        self.assertEqual(r.code, C.EXIT_PRECONDITION)
        notes = " ".join(t.notes for t in r.by_id.get("env.devices", []))
        self.assertIn("Bluetooth HID device is not there", notes)

    def test_a_build_without_bluetooth(self):
        def puck_build(backend):
            backend.ble_build = False
        r = Run(setup=puck_build)
        self.assertEqual(r.code, C.EXIT_PRECONDITION)
        notes = " ".join(t.notes for t in r.by_id.get("env.devices", []))
        self.assertIn("no Bluetooth", notes)


if __name__ == "__main__":
    unittest.main()
