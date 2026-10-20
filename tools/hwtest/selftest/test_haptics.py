# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# test_haptics.py: the haptics synthesiser's part of the test protocol (proto/haptics.py): its
# ring entries (the HAP_* lines of ring.txt), its status, the shared vectors of rendered blocks
# (haptics.txt, which the host tests render with the library's code), and the rumble emulation's
# numbers the group expects.
import os
import struct
import tempfile
import unittest

from hwtest.proto import haptics as H
from hwtest.proto import testproto as T

VDIR = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "tests",
                                     "vectors", "hwtest"))


def _ring_entries():
    out = []
    with open(os.path.join(VDIR, "ring.txt"), encoding="utf-8") as f:
        for line in f:
            tok = line.split("#", 1)[0].split()
            if tok and tok[0] == "ENTRY":
                e = T.decode_entry(bytes.fromhex("".join(tok[1:])))
                if e["kind"] in H.RING_KINDS:
                    out.append(e)
    return out


class RingEntries(unittest.TestCase):
    def test_every_kind_has_a_vector(self):
        self.assertEqual(sorted({e["kind"] for e in _ring_entries()}), list(H.RING_KINDS))

    def test_fields(self):
        by_kind = {e["kind"]: H.decode(e) for e in _ring_entries()}
        self.assertEqual(by_kind[H.RING_HAP_GEN],
                         {"channel": H.CH_LEFT_GRIP, "type": H.GEN_TONE, "what": H.GEN_START,
                          "gain": -6, "p": (200, 100, 0, 0)})
        self.assertEqual(by_kind[H.RING_HAP_BLOCK],
                         {"path": H.GRIPS, "index": 0, "crc": 0xAB9DE1FD, "peak": 0x1234,
                          "flags": H.BLOCK_FIRST})
        self.assertEqual(by_kind[H.RING_HAP_STREAM],
                         {"channel": H.CH_LEFT_GRIP, "what": H.STREAM_WRITE, "fill": 310,
                          "flags": H.FLAG_HIGH})
        self.assertEqual(by_kind[H.RING_HAP_SCRIPT],
                         {"what": H.SCRIPT_CUE, "channel": 2, "id": 5, "gain": -12, "extra": 5})

    def test_entries_filter(self):
        es = _ring_entries()
        self.assertEqual(len(H.entries(es, H.RING_HAP_GEN, channel=H.CH_LEFT_GRIP)), 1)
        self.assertEqual(H.entries(es, H.RING_HAP_GEN, channel=H.CH_RIGHT_PAD), [])
        self.assertEqual(H.entries(es, H.RING_HAP_BLOCK)[0]["time_us"], 0x200)


class Status(unittest.TestCase):
    def test_decode(self):
        raw = bytearray(H.STATUS_LEN)
        raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7] = 1, 2, 3, 1, 0xFD, 0x0C, 2, 1
        struct.pack_into("<II", raw, 8, 10, 20)
        struct.pack_into("<HHH", raw, 16, 1, 2, 3)
        raw[22:26] = bytes([0, 1 << H.GEN_TONE, 0, 0])
        struct.pack_into("<HHHH", raw, 26, 0, 310, 0, 512)
        raw[34:38] = bytes([0xFF, 0, 0xFF, 5])
        st = H.decode_status(bytes(raw))
        self.assertTrue(st["drop_sounds"])
        self.assertEqual((st["busy"], st["outputs"], st["amp_mode"], st["master_db"]), (2, 3, 1, -3))
        self.assertEqual((st["players"], st["drop_state"], st["running"]), (0x0C, 2, 1))
        self.assertEqual((st["blocks"], st["underruns"], st["lost"]), ((10, 20), (1, 2), 3))
        self.assertEqual(st["fill"], (0, 310, 0, 512))
        self.assertEqual(st["format"], (0xFF, 0, 0xFF, 5))
        with self.assertRaises(ValueError):
            H.decode_status(bytes(10))

    def test_status_report(self):
        self.assertEqual(H.status_report(H.CH_RIGHT_PAD, H.FLAG_STOPPED), bytes.fromhex("440402000000"))
        self.assertEqual(H.status_report(H.CH_LEFT_GRIP, H.FLAG_FORMAT), bytes.fromhex("440020000000"))


class Vectors(unittest.TestCase):
    def test_file(self):
        vs = H.read_vectors(os.path.join(VDIR, "haptics.txt"))
        self.assertGreaterEqual(len(vs), 10)
        names = set()
        for v in vs:
            self.assertNotIn(v["name"], names)
            names.add(v["name"])
            self.assertIn(v["path"], (H.PADS, H.GRIPS))
            self.assertTrue(1 <= v["blocks"] <= 13, v["name"])   # what one capture reports
            self.assertEqual(len(v["crcs"]), v["blocks"])
            self.assertTrue(0x80 <= v["report"][0] <= 0x89, v["name"])
            self.assertLessEqual(len(v["report"]), 54, v["name"])   # HOST_OUT carries 54 bytes

    def test_bad_record(self):
        fd, path = tempfile.mkstemp(suffix=".txt")
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                f.write("RENDER x PATH 1 SEED 1 MASTER 0 S27 0 BLOCKS 2 OUT 8200 CRC 00000000\n")
            with self.assertRaises(ValueError):    # two blocks, one CRC
                H.read_vectors(path)
        finally:
            os.remove(path)


class RumbleNumbers(unittest.TestCase):
    def test_gains_and_lfo(self):
        self.assertEqual(H.rumble_right_db(999), -20)
        self.assertEqual(H.rumble_right_db(60000), -8)
        self.assertEqual(H.rumble_left_db(999), -26)
        self.assertEqual(H.rumble_left_db(65535), -5)
        self.assertEqual(H.rumble_left_db(1000, 0, 16), 6)
        self.assertEqual(H.rumble_lfo_mhz(13107), 10000)
        self.assertEqual(H.rumble_lfo_mhz(13108), 8800)
        self.assertEqual(H.rumble_lfo_mhz(65535), 13600)


if __name__ == "__main__":
    unittest.main()
