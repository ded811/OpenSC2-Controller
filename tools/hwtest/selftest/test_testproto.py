# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# test_testproto.py: request building, answer parsing, caps and status layouts, every
# ring entry kind (round-trip, unknown kinds skipped by length), counters and settings pages.
import struct
import unittest

from hwtest import config as C
from hwtest.proto import sysproto as S
from hwtest.proto import testproto as T


class RequestsAnswers(unittest.TestCase):
    def test_build_request(self):
        req = T.build_request(C.OP_ARM, struct.pack("<IHB", 0x12345678, 15, 1), request_id=5)
        self.assertEqual(req.hex(), "1eab577e010501785634120f0001")

    def test_parse_answer(self):
        a = T.parse_answer(bytes([1, 5, C.OP_ARM, 0]) + bytes(32))
        self.assertEqual(a["request_id"], 5)
        self.assertEqual(a["op"], C.OP_ARM)
        self.assertEqual(a["status"], 0)
        self.assertEqual(len(a["result"]), 32)

    def test_caps_status_round_trip(self):
        caps = {"version": 1, "mode": 1, "build_flags": 0x2A3F, "ring_size": 4096,
                "git_revision": "osc2-app-0.2", "policy": 1, "sequence_depth": 8,
                "lease_s": 15}
        self.assertEqual(T.decode_caps(T.encode_caps(caps))["git_revision"], "osc2-app-0.2")
        self.assertEqual(T.decode_caps(T.encode_caps(caps))["policy"], 1)
        # 36 bytes: the product name's CRC-32 at 32 (the firmware's, tests/host/hooks_cases.h)
        raw = T.encode_caps(caps)
        self.assertEqual(len(raw), 36)
        self.assertEqual(T.decode_caps(raw)["product_crc"], 0x8A55859E)   # "Steam Controller"
        named = T.decode_caps(T.encode_caps(dict(caps, product_name="Bench Pad")))
        self.assertEqual(named["product_crc"], T.crc32(b"Bench Pad"))
        # firmware before the product name: 32 bytes, no CRC
        self.assertIsNone(T.decode_caps(raw[:32])["product_crc"])
        st = {"flags": 0x83, "nonce": 0xA5A50001, "frames": 5, "mode": 1}
        d = T.decode_status(T.encode_status(st))
        self.assertTrue(d["armed"] and d["guard"] and d["snapshot"])
        self.assertEqual(d["nonce"], 0xA5A50001)


class RingEntries(unittest.TestCase):
    SAMPLES = [
        "0000000001010478563412", "e803000011010a1027204ee80300000000",
        "8813000016010e0105000000410000280000000000", "1027000018010a008703000000002d6400",
        "0000000018010900ee21000000000118", "000000001202090687000404cdfb3cb6",
        "0000000010020a81080100000000000000", "0000000012020a00810002000000000000",
        "00000000140303ff0000", "000000001301066400320a0032",
    ]

    def test_round_trip(self):
        for hx in self.SAMPLES:
            b = bytes.fromhex(hx)
            self.assertEqual(T.encode_entry(T.decode_entry(b)).hex(), hx)

    def test_haptic_kind_not_clobbered(self):
        e = T.decode_entry(bytes.fromhex("000000001202090687000404cdfb3cb6"))
        self.assertEqual(e["kind"], C.RING_HAPTIC)
        self.assertEqual(e["haptic_kind"], 6)
        self.assertEqual(e["crc32"], 0xB63CFBCD)

    def test_crc32(self):
        self.assertEqual(T.crc32(bytes.fromhex("01020304")), 0xB63CFBCD)
        self.assertEqual(T.crc32(bytes.fromhex("0A0B0C")), 0x1894C924)
        self.assertEqual(T.crc32(bytes(24)), 0xA3C1CA20)

    def test_parse_ring_read(self):
        entries = [bytes.fromhex(h) for h in self.SAMPLES[:3]]
        body = struct.pack("<IIB", 0, 3, 3) + b"".join(entries)
        first, nxt, got = T.parse_ring_read(body)
        self.assertEqual((first, nxt, len(got)), (0, 3, 3))

    def test_unknown_kind_skipped_by_length(self):
        entry = struct.pack("<IBBB", 0, 0x7F, 1, 3) + bytes([1, 2, 3])
        d = T.decode_entry(entry)
        self.assertEqual(d["kind"], 0x7F)
        self.assertEqual(d["len"], 3)


class CountersSettings(unittest.TestCase):
    def test_counters(self):
        data = struct.pack("<BB", 0, 3) + struct.pack("<III", 10, 20, 30)
        c = T.decode_counters(data)
        self.assertEqual(c["frames"], 10)
        self.assertEqual(c["usb_sent"], 20)
        self.assertEqual(c["usb_dropped"], 30)

    def test_settings(self):
        data = struct.pack("<BB", 9, 2) + struct.pack("<hh", 1, 200)
        s = T.decode_settings(data)
        self.assertEqual(s[9], 1)
        self.assertEqual(s[10], 200)



class SystemOps(unittest.TestCase):
    """proto/sysproto.py: SYS_INJECT's and SYS_BOOT's arguments, the 30-byte SYS_STATUS and the
    machine's names, as lib/opensc2/test_sys.h and sys_rules.h have them."""

    def test_args(self):
        self.assertEqual(S.sys_args(S.INJECT_BUTTONS | S.INJECT_SOURCE, 0x00010201, 7,
                                    S.SRC_BATTERY, 30).hex(), "05010201000700" + "1e")
        self.assertEqual(len(S.sys_args(0)), 8)

    def test_status_round_trip(self):
        st = {"state": S.STATE["battery"], "transport": 2, "source": S.SRC_BATTERY,
              "led_mode": S.LED_PUCK, "flags": 0x1C, "idle_history": 0xFFE0,
              "end_valid": True, "end_state": S.STATE["shutdown"],
              "end_cause": S.EVENT["steam-held"], "end_how": S.HOW_REBOOT,
              "end_uptime_ms": 123456, "idle_timeout_s": 2}
        d = S.decode_sys_status(S.encode_sys_status(st))
        self.assertEqual(d["state_name"], "battery")
        self.assertTrue(d["level_injected"] and d["source_injected"] and d["boot_injected"])
        self.assertFalse(d["buttons_injected"])
        self.assertEqual((d["end_state"], d["end_cause"], d["end_how"], d["end_uptime_ms"]),
                         (12, 4, 2, 123456))
        self.assertEqual(d["idle_timeout_s"], 2)
        with self.assertRaises(ValueError):
            S.decode_sys_status(bytes(29))

    def test_names(self):
        # the firmware's numbering (lib/opensc2/sys_rules.h)
        self.assertEqual(S.STATE["usb-link-off"], 5)
        self.assertEqual(S.STATE["reboot"], 16)
        self.assertEqual(S.EVENT["unplugged"], 19)
        self.assertEqual(len(S.STATES), 17)
        self.assertEqual(S.KEYLOCK_CHORD, 0x00070180)

    def test_short_entry_kept_raw(self):
        e = T.decode_entry(bytes.fromhex("0000000050030102"))
        self.assertNotIn("from", e)
        self.assertEqual(T.encode_entry(e).hex(), "0000000050030102")

if __name__ == "__main__":
    unittest.main()
