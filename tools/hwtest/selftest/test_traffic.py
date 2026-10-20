# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# test_traffic.py: steam.session's traffic counters (groups/steam.py traffic_counts()) on
# synthetic ring entries: output reports by ID, rumble, haptics by kind, the stops and the 0x44
# replies they call for, other links left out.
import struct
import unittest

from hwtest import config as C
from hwtest.groups import steam
from hwtest.proto import model as M
from hwtest.proto import testproto as T


def _entry(kind, link, data):
    """A ring entry as the board sends it, decoded the way the runner decodes it."""
    return T.decode_entry(struct.pack("<IBBB", 1000, kind, link, len(data)) + data)


def _output(link, rid, result=0):
    return _entry(C.RING_OUTPUT, link, bytes([rid, 64, result & 0xFF]) + bytes(16))


def _pulse(link, target, channels, on_us):
    return _entry(C.RING_HAPTIC, link, bytes([0, 0x81, target, channels]) +
                  struct.pack("<HHH", on_us, 0, 0))


def _rumble(link, left, right):
    return _entry(C.RING_RUMBLE, link, struct.pack("<HHHbbBB", left, right, 0, 0, 0, 0, 0))


def _pcm(link, n):
    return _entry(C.RING_HAPTIC, link, bytes([6, 0x87, 0, M.CH_LPAD, n]) + bytes(4))


def _reply(link, channel):
    return _entry(C.RING_STOP_STATUS, link, bytes([channel]) + struct.pack("<I", 2))


class Traffic(unittest.TestCase):
    def test_steams_zero_pulses(self):
        # the pair seen on the cable: target 0 (the right pad), then target 1 (the left pad)
        entries = [_output(0, 0x81), _pulse(0, 0, M.CH_RPAD, 0), _reply(0, C.STATUS_RIGHT_PAD),
                   _output(0, 0x81), _pulse(0, 1, M.CH_LPAD, 0), _reply(0, C.STATUS_LEFT_PAD)]
        counts, want, got = steam.traffic_counts(entries)
        self.assertEqual(counts["outputs"], {"0x81": 2})
        self.assertEqual(counts["haptics"], {"pulse": 2})
        self.assertEqual(counts["stops"], 2)
        self.assertEqual(counts["stop_replies"], {"right_pad": 1, "left_pad": 1})
        self.assertEqual(want, [C.STATUS_RIGHT_PAD, C.STATUS_LEFT_PAD])
        self.assertEqual(got, want)

    def test_kinds_and_links(self):
        entries = [
            _output(0, 0x80), _rumble(0, 0, 1000), _reply(0, C.STATUS_LEFT_GRIP),
            _output(0, 0x80), _rumble(0, 1000, 1000),                      # no stop
            _output(0, 0x87), _pcm(0, 32), _output(0, 0x87), _pcm(0, 16),
            _output(0, 0x81), _pulse(0, 2, M.CH_LPAD | M.CH_RPAD, 500),     # a pulse, no stop
            _output(0, 0x83, result=-1),                                    # refused
            _output(2, 0x81), _pulse(2, 0, M.CH_RPAD, 0), _reply(2, 4),     # the test link
            _rumble(3, 0, 0),                                               # the rumble timeout
        ]
        counts, want, got = steam.traffic_counts(entries)
        self.assertEqual(counts["outputs"], {"0x80": 2, "0x87": 2, "0x81": 1, "0x83": 1})
        self.assertEqual(counts["refused"], 1)
        self.assertEqual(counts["rumble"], 2)
        self.assertEqual(counts["haptics"], {"pcm_data": 2, "pulse": 1})
        self.assertEqual(counts["pcm_bytes"], 48)
        self.assertEqual(counts["stops"], 1)
        self.assertEqual(counts["stop_replies"], {"left_grip": 1})
        self.assertEqual((want, got), ([C.STATUS_LEFT_GRIP], [C.STATUS_LEFT_GRIP]))

    def test_a_missing_reply_shows(self):
        entries = [_output(0, 0x82), _entry(C.RING_HAPTIC, 0, bytes([1, 0x82, 6, 3, 0, 0]))]
        counts, want, got = steam.traffic_counts(entries)
        self.assertEqual(counts["haptics"], {"command": 1})
        self.assertEqual((want, got), ([C.STATUS_LEFT_PAD, C.STATUS_RIGHT_PAD], []))


if __name__ == "__main__":
    unittest.main()
