# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# haptics: the haptics synthesiser (docs/testing.md, group "haptics"), on a board built with it
# (app/haptics.conf; the caps' build flag 19), checked through the test hooks with nothing wired:
# the CRCs of the blocks it renders for the shared vectors (tests/vectors/hwtest/haptics.txt,
# rendered on the PC by the library's own code), the generators each output report starts and
# stops (0x81's special cases, the rumble emulation's gains), how long sounds last, the script
# players and the cues, the PCM streams' flow control (the 0x44 flags, read from the ring and, where
# the board's reports can be read, as reports), the drop sounds' timing, and the amplifier mode.
# Every setting it changes (27, 75, 76) is written back; the synthesiser is reset at the end.
import struct

from . import base
from .. import config as C
from ..proto import haptics as H
from ..proto import testproto as T

GROUP = "haptics"
TESTS = [("haptics.present", "WR", []), ("haptics.vectors", "WR", []),
         ("haptics.pulse_cases", "WR", []), ("haptics.lifecycle", "WR", []),
         ("haptics.rumble", "WR", []), ("haptics.scripts", "WR", []),
         ("haptics.cues", "WR", []), ("haptics.pcm", "WR", []), ("haptics.drop", "WR", []),
         ("haptics.amp_mode", "WR", [])]

SEED = 0x14CB                 # the rumble's boot seed (5323), as the vectors use
QUIET_S = 2.0                 # how long the synthesiser may take to fall quiet


def _ok(a, what):
    if a is None or a["status"] != C.ST_OK:
        raise T.ProtocolError("%s answered %s" % (what, C.STATUS_NAMES.get(
            a["status"] if a else -1, "a STALL")))
    return a["result"]


def _status(rig):
    return H.decode_status(_ok(rig.test(H.OP_HAP_STATUS), "HAP_STATUS"))


def _quiet(rig, timeout=QUIET_S):
    """Until nothing plays, no script runs and both paths' threads are idle: the status."""
    deadline = rig.clock.monotonic() + rig.t(timeout)
    while True:
        st = _status(rig)
        if not st["busy"] and not st["running"] and not st["players"]:
            return st
        if rig.clock.monotonic() >= deadline:
            return st
        rig.sleep(0.02)


def _reset(rig, seed=SEED):
    _ok(rig.test(H.OP_HAP_RESET, struct.pack("<I", seed)), "HAP_RESET")
    return _quiet(rig)


def _play(rig, what, *args):
    res = _ok(rig.test(H.OP_HAP_PLAY, bytes([what]) + bytes(a & 0xFF for a in args)), "HAP_PLAY")
    return struct.unpack_from("<i", res, 0)[0]


def _setting(rig, number, value):
    rig.write_settings([(number, value)])


def _wait(rig, start, kind, count, timeout, **match):
    """Ring entries from `start` until `count` of `kind` matching `match` are there."""
    _f, _n, es = rig.ring_wait(start, lambda es: len(H.entries(es, kind, **match)) >= count,
                               timeout)
    return es


def _ms(a, b):
    return (b["time_us"] - a["time_us"]) / 1000.0


def _await_reports(rig, wants, timeout):
    """Read the board's reports until each of `wants` (0x44 reports, 6 bytes) has come, in this
    order, others in between: how many came."""
    left = list(wants)

    def want(r):
        if left and bytes(r[:len(left[0])]) == left[0]:
            left.pop(0)
        return not left

    rig.wait_report(want, timeout)
    return len(wants) - len(left)


def run(ctx):
    rig = ctx.rig
    if not rig.ctx.armed:
        rig.arm()
    caps = rig.caps or {}
    if not rig.models("haptics") or not (caps and T.cap_flag(caps, H.CAP_HAPTICS_SYNTH)):
        # one line for the group, not one per test
        if ctx.want("haptics.present"):
            ctx.record(GROUP, "haptics.present", base.SKIP,
                       "not modelled by the %s backend" % rig.backend.kind
                       if not rig.models("haptics") else
                       "the board is built without the haptics synthesiser "
                       "(CONFIG_OPENSC2_HAPTICS_SYNTH; app/haptics.conf)")
        return
    snap = rig.settings_read()
    try:
        _run(ctx, rig)
    finally:
        try:
            _reset(rig)
        except Exception:   # noqa: BLE001 - the settings and the restore group still run
            pass
        now = rig.settings_read()
        back = [(n, snap[n]) for n in (27, 75, 76) if now.get(n) != snap.get(n)]
        if back:
            rig.write_settings(back)


def _run(ctx, rig):
    if ctx.want("haptics.present"):
        with ctx.check(GROUP, "haptics.present") as c:
            st = _reset(rig)
            c.eq("quiet after HAP_RESET (busy, running, scripts)",
                 (st["busy"], st["running"], st["players"]), (0, 0, 0))
            c.eq("every stream without a format after the reset", st["format"], (0xFF,) * 4)
            c.eq("HAP_CAPTURE with one byte", rig.test(H.OP_HAP_CAPTURE, b"\x01")["status"],
                 C.ST_BAD_LEN)
            c.eq("HAP_CAPTURE of path 3", rig.test(H.OP_HAP_CAPTURE, b"\x03\x01")["status"],
                 C.ST_BAD_ARG)
            c.eq("HAP_PLAY of an unknown kind", rig.test(H.OP_HAP_PLAY, b"\x09")["status"],
                 C.ST_BAD_ARG)
            c.note("drop sounds %s, outputs %s, master gain %d dB" % (
                "built" if st["drop_sounds"] else "not built", st["outputs"] or "none",
                st["master_db"]))

    if ctx.want("haptics.vectors"):
        with ctx.check(GROUP, "haptics.vectors") as c:
            vectors = H.read_vectors()
            for v in vectors:
                _setting(rig, 76, v["master"])
                _setting(rig, 27, v["s27"])
                _reset(rig, v["seed"])
                _ok(rig.test(H.OP_HAP_CAPTURE, bytes([v["path"], v["blocks"]])), "HAP_CAPTURE")
                start = rig.ring_next()
                rig.output(v["report"])
                es = _wait(rig, start, H.RING_HAP_BLOCK, v["blocks"],
                           0.5 + v["blocks"] * 0.008, path=v["path"])
                blocks = H.entries(es, H.RING_HAP_BLOCK, path=v["path"])[:v["blocks"]]
                c.eq("%s: block indices" % v["name"], [b["index"] for b in blocks],
                     list(range(v["blocks"])))
                c.eq("%s: CRCs" % v["name"], [b["crc"] for b in blocks], v["crcs"])
            c.note("%d vectors, %d blocks" % (len(vectors), sum(v["blocks"] for v in vectors)))

    if ctx.want("haptics.pulse_cases"):
        with ctx.check(GROUP, "haptics.pulse_cases") as c:
            _reset(rig)
            # 0x81 target 0 is the right pad
            cases = [("off time 0: click A at +8 dB", (0, 100, 0, 5),
                      dict(channel=H.CH_RIGHT_PAD, type=H.GEN_CLICK_A, what=H.GEN_START), 8),
                     ("count 3: click B at +4 dB", (0, 100, 100, 3),
                      dict(channel=H.CH_RIGHT_PAD, type=H.GEN_CLICK_B, what=H.GEN_START), 4),
                     ("count 4: click B at +4 dB", (1, 100, 100, 4),
                      dict(channel=H.CH_LEFT_PAD, type=H.GEN_CLICK_B, what=H.GEN_START), 4),
                     ("a train", (0, 1000, 1000, 5),
                      dict(channel=H.CH_RIGHT_PAD, type=H.GEN_PULSE, what=H.GEN_START), 0),
                     ("a grip refuses click A", (3, 100, 0, 5),
                      dict(channel=H.CH_LEFT_GRIP, type=H.GEN_CLICK_A, what=H.GEN_REFUSED), 8)]
            for what, (target, on, off, count), match, gain in cases:
                start = rig.ring_next()
                rig.output(bytes([0x81, target]) + struct.pack("<HHH", on, off, count))
                found = H.entries(_wait(rig, start, H.RING_HAP_GEN, 1, 0.5, **match),
                                  H.RING_HAP_GEN, **match)
                if c.true("%s: a HAP_GEN entry" % what, len(found) == 1):
                    c.eq("%s: gain" % what, found[0]["gain"], gain)
                    if match["type"] == H.GEN_PULSE:
                        c.eq("%s: on, off, count" % what, found[0]["p"][:3], (on, off, count))
                _quiet(rig)
            start = rig.ring_next()
            rig.output(bytes([0x81, 0]) + struct.pack("<HHH", 100, 100, 0))
            rig.sleep(0.15)
            c.eq("count 0 starts nothing",
                 H.entries(rig.ring_all(start)[2], H.RING_HAP_GEN, what=H.GEN_START), [])
            # Steam's all-zero pulse (on time 0): a stop-all, which empties the stream ring
            start = rig.ring_next()
            rig.output(bytes([0x81, 0]) + bytes(6))
            found = H.entries(_wait(rig, start, H.RING_HAP_STREAM, 1, 0.5,
                                    what=H.STREAM_EMPTIED), H.RING_HAP_STREAM,
                              what=H.STREAM_EMPTIED)
            c.eq("on time 0: a stop-all on the right pad (its stream emptied)",
                 [f["channel"] for f in found], [H.CH_RIGHT_PAD])

    if ctx.want("haptics.lifecycle"):
        with ctx.check(GROUP, "haptics.lifecycle") as c:
            _setting(rig, 76, -3)
            _reset(rig)
            _ok(rig.test(H.OP_HAP_CAPTURE, bytes([H.GRIPS, 20])), "HAP_CAPTURE")
            start = rig.ring_next()
            rig.output(bytes([0x83, 3, 0xFA]) + struct.pack("<HHHB", 200, 100, 0, 0))
            es = _wait(rig, start, H.RING_HAP_GEN, 1, 1.5, what=H.GEN_END)
            gens = H.entries(es, H.RING_HAP_GEN, channel=H.CH_LEFT_GRIP, type=H.GEN_TONE)
            if c.eq("tone: started, then ended", [g["what"] for g in gens],
                    [H.GEN_START, H.GEN_END]):
                dt = _ms(gens[0], gens[1])
                # rendered one block ahead: 100 ms and the 4 ms lead-in, in 8 ms blocks, less
                # the blocks rendered at once
                c.true("ended %.0f ms after it started (60..140)" % dt, 60 <= dt <= 140)
                c.eq("its parameters (Hz, ms, LFO, depth)", gens[0]["p"], (200, 100, 0, 0))
            es = _wait(rig, start, H.RING_HAP_BLOCK, 14, 1.0, path=H.GRIPS)
            blocks = H.entries(es, H.RING_HAP_BLOCK, path=H.GRIPS)
            # 32 frames of lead-in and 800 of tone: blocks 0..12, 64 frames each; the tone ends
            # with block 12 but stays in the mix for 4 more samples, as on the real controller,
            # so block 13 (silent) is the last before the drain
            c.eq("blocks 0..13, the first with the lead-in, the last before the drain",
                 [(b["index"], b["flags"]) for b in blocks],
                 [(0, H.BLOCK_FIRST)] + [(i, 0) for i in range(1, 13)] + [(13, H.BLOCK_LAST)])
            st = _quiet(rig)
            c.eq("quiet again", (st["busy"], st["running"]), (0, 0))

    if ctx.want("haptics.rumble"):
        with ctx.check(GROUP, "haptics.rumble") as c:
            _setting(rig, 27, -10)
            _reset(rig)
            left, right = 30000, 50000
            start = rig.ring_next()
            rig.output(bytes([0x80, 0, 0, 0]) + struct.pack("<HbHb", left, 0, right, 0))
            es = _wait(rig, start, H.RING_HAP_GEN, 2, 0.5, what=H.GEN_START)
            starts = H.entries(es, H.RING_HAP_GEN, what=H.GEN_START)
            if c.eq("the right grip's rumble, then the left grip's tone",
                    [(g["channel"], g["type"]) for g in starts],
                    [(H.CH_RIGHT_GRIP, H.GEN_RUMBLE), (H.CH_LEFT_GRIP, H.GEN_TONE)]):
                c.eq("rumble: gain", starts[0]["gain"], H.rumble_right_db(right))
                c.eq("rumble: 50 Hz started downwards, 500 ms, bursts at 12 Hz",
                     starts[0]["p"], (50, 500, 1200, 0x8000))
                c.eq("tone: gain (setting 27 = -10)", starts[1]["gain"],
                     H.rumble_left_db(left, -10))
                c.eq("tone: 50 Hz, 500 ms, the LFO, full depth", starts[1]["p"],
                     (50, 500, H.rumble_lfo_mhz(left) // 10, 100))
            _setting(rig, 27, 0)
            start = rig.ring_next()
            rig.output(bytes([0x80, 0, 0x10, 0]) + struct.pack("<HbHb", left, 0, right, 0))
            es = _wait(rig, start, H.RING_HAP_GEN, 2, 0.5, what=H.GEN_START)
            tones = H.entries(es, H.RING_HAP_GEN, what=H.GEN_START, type=H.GEN_TONE)
            if c.eq("the tone again", len(tones), 1):
                c.eq("tone: gain with setting 27 = 0 and the level step 16 (+16 dB)",
                     tones[0]["gain"], H.rumble_left_db(left, 0, 16))
            start = rig.ring_next()
            rig.output(bytes([0x80]) + bytes(9))
            es = _wait(rig, start, H.RING_HAP_GEN, 2, 0.5, what=H.GEN_STOP)
            c.eq("speeds 0: both grips stopped, the right one first",
                 [(g["channel"], g["type"]) for g in
                  H.entries(es, H.RING_HAP_GEN, what=H.GEN_STOP)],
                 [(H.CH_RIGHT_GRIP, H.GEN_RUMBLE), (H.CH_LEFT_GRIP, H.GEN_TONE)])

    if ctx.want("haptics.scripts"):
        with ctx.check(GROUP, "haptics.scripts") as c:
            _reset(rig)
            start = rig.ring_next()
            rig.output(bytes([0x85, 3, 8, 0xF4]))      # script 8 on the left grip at -12 dB
            rig.output(bytes([0x85, 3, 9, 0xF4]))      # refused: the channel's script runs
            rig.output(bytes([0x85, 4, 0, 0]))         # id 0: refused
            es = _wait(rig, start, H.RING_HAP_SCRIPT, 1, 1.5, what=H.SCRIPT_DONE)
            sc = H.entries(es, H.RING_HAP_SCRIPT)
            c.eq("start, refused, refused",
                 [(s["what"], s["channel"], s["id"]) for s in sc
                  if s["what"] in (H.SCRIPT_START, H.SCRIPT_REFUSED)],
                 [(H.SCRIPT_START, H.CH_LEFT_GRIP, 8), (H.SCRIPT_REFUSED, H.CH_LEFT_GRIP, 9),
                  (H.SCRIPT_REFUSED, H.CH_RIGHT_GRIP, 0)])
            steps = [s for s in sc if s["what"] == H.SCRIPT_STEP]
            if c.eq("three steps", [s["extra"] for s in steps], [0, 1, 2]):
                gaps = [_ms(steps[i], steps[i + 1]) for i in range(2)]
                c.true("120 ms apart (%s)" % ", ".join("%.0f" % g for g in gaps),
                       all(100 <= g <= 160 for g in gaps))
            tones = H.entries(es, H.RING_HAP_GEN, what=H.GEN_START, type=H.GEN_TONE)
            c.eq("the steps' tones: 640 Hz at -12 dB", [(t["p"][0], t["gain"]) for t in tones],
                 [(640, -12)] * 3)
            c.eq("then the channel is free", [s["what"] for s in sc][-1], H.SCRIPT_DONE)

    if ctx.want("haptics.cues"):
        with ctx.check(GROUP, "haptics.cues") as c:
            _reset(rig)
            for cue, level, gain, script, rc in ((5, 2, -12, 5, 2), (6, 1, -19, 8, 2),
                                                 (0, 0, 0, 0, -1), (8, 0, 2, 10, 2)):
                start = rig.ring_next()
                got = _play(rig, H.PLAY_CUE, cue, level)
                c.eq("cue %d at level %d: grips started" % (cue, level), got, rc)
                es = rig.ring_all(start)[2]
                cues = H.entries(es, H.RING_HAP_SCRIPT, what=H.SCRIPT_CUE)
                if c.eq("cue %d: one CUE entry" % cue, len(cues), 1):
                    c.eq("cue %d at level %d: script and gain" % (cue, level),
                         (cues[0]["id"], cues[0]["channel"], cues[0]["extra"], cues[0]["gain"]),
                         (cue, level, script, gain))
                if rc > 0:
                    c.eq("cue %d: on both grips" % cue,
                         [(s["channel"], s["id"], s["gain"]) for s in
                          H.entries(es, H.RING_HAP_SCRIPT, what=H.SCRIPT_START)],
                         [(H.CH_LEFT_GRIP, script, gain), (H.CH_RIGHT_GRIP, script, gain)])
                _quiet(rig)

    if ctx.want("haptics.pcm"):
        with ctx.check(GROUP, "haptics.pcm") as c:
            _reset(rig)
            reads = rig.path == "W" or rig.slot_proven_at is not None
            n = 62 if rig.path == "W" else 52          # Windows pads 0x87 to 64 bytes
            rig.flush()
            # formats: 8 kHz on a pad refused (0x10), 16-bit at 8 kHz on the left grip (0x20)
            start = rig.ring_next()
            rig.output(bytes([0x86, 2, 3, 0]))
            rig.output(bytes([0x86, 2, 0, 0]))
            es = _wait(rig, start, H.RING_HAP_STREAM, 2, 0.5, what=H.STREAM_STATUS)
            c.eq("format answers (left pad 0x10, left grip 0x20)",
                 [(s["channel"], s["flags"]) for s in
                  H.entries(es, H.RING_HAP_STREAM, what=H.STREAM_STATUS)],
                 [(H.CH_LEFT_PAD, H.FLAG_NEED_8K), (H.CH_LEFT_GRIP, H.FLAG_FORMAT)])
            if reads:
                c.eq("the two 0x44 reports arrived (44 03 10.., 44 00 20..)",
                     _await_reports(rig, [H.status_report(H.CH_LEFT_PAD, H.FLAG_NEED_8K),
                                          H.status_report(H.CH_LEFT_GRIP, H.FLAG_FORMAT)], 0.5), 2)
            # six writes: the fifth leaves less than 256 bytes free (0x08); the sixth finds more
            # than 256 held and starts the stream, which plays out: 0x04, then 0x02 when dry
            start = rig.ring_next()
            body = bytes((i * 7) & 0xFF for i in range(n))
            for _ in range(6):
                rig.output(bytes([0x87, 0]) + body)
            es = _wait(rig, start, H.RING_HAP_STREAM, 1, 1.0, what=H.STREAM_STATUS,
                       flags=H.FLAG_STOPPED)
            writes = H.entries(es, H.RING_HAP_STREAM, what=H.STREAM_WRITE, channel=H.CH_LEFT_GRIP)
            c.eq("the writes' flags", [w["flags"] for w in writes],
                 [0, 0, 0, 0, H.FLAG_HIGH, H.FLAG_HIGH])
            c.true("the stream started",
                   len(H.entries(es, H.RING_HAP_STREAM, what=H.STREAM_START)) == 1)
            flags = [H.FLAG_HIGH, H.FLAG_HIGH, H.FLAG_LOW, H.FLAG_STOPPED]
            c.eq("0x44 sent: 0x08 for each of the two writes, then 0x04 and 0x02 as it played out",
                 [s["flags"] for s in H.entries(es, H.RING_HAP_STREAM, what=H.STREAM_STATUS,
                                                channel=H.CH_LEFT_GRIP)], flags)
            if reads:
                c.eq("those four 0x44 reports arrived, in order",
                     _await_reports(rig, [H.status_report(H.CH_LEFT_GRIP, f) for f in flags], 0.5),
                     4)
            # a full ring (the left pad, no format: it never plays) drops what does not fit
            start = rig.ring_next()
            for _ in range(512 // n + 1):
                rig.output(bytes([0x87, 3]) + body)
            es = rig.ring_all(start)[2]
            writes = H.entries(es, H.RING_HAP_STREAM, what=H.STREAM_WRITE, channel=H.CH_LEFT_PAD)
            c.eq("the last write did not fit: 0x01, the ring full",
                 (writes[-1]["flags"], writes[-1]["fill"]) if writes else None,
                 (H.FLAG_DROPPED, 512))
            # 0x86's stop empties it
            start = rig.ring_next()
            rig.output(bytes([0x86, 1, 3, 0]))
            es = _wait(rig, start, H.RING_HAP_STREAM, 1, 0.5, what=H.STREAM_EMPTIED)
            c.eq("0x86 stop: the left pad's ring emptied",
                 [s["channel"] for s in H.entries(es, H.RING_HAP_STREAM, what=H.STREAM_EMPTIED)],
                 [H.CH_LEFT_PAD])
            if not reads:
                c.note("the board's reports were not read (its slot is not proved): the ring only")

    if ctx.want("haptics.drop"):
        with ctx.check(GROUP, "haptics.drop") as c:
            st = _reset(rig)
            if not st["drop_sounds"]:
                c.skip("built without CONFIG_OPENSC2_HAPTICS_DROP_SOUNDS")
            start = rig.ring_next()
            c.eq("a free fall is taken", _play(rig, H.PLAY_FALL, 100), 1)
            c.eq("a second one is not (until the re-arm)", _play(rig, H.PLAY_FALL, 10), 0)
            es = _wait(rig, start, H.RING_HAP_SCRIPT, 1, 1.0, what=H.SCRIPT_DROP)
            falls = H.entries(es, H.RING_HAP_SCRIPT, what=H.SCRIPT_FALL)
            drops = H.entries(es, H.RING_HAP_SCRIPT, what=H.SCRIPT_DROP)
            if c.eq("FALL entries (taken, ignored)", [f["extra"] for f in falls], [1, 0]) and \
                    c.eq("one DROP entry", len(drops), 1):
                dt = _ms(falls[0], drops[0])
                c.true("the sound %.0f ms after the fall (250 +- 60)" % dt, abs(dt - 250) <= 60)
                c.eq("byte 100 picks drop sound 2 (script 17)", (drops[0]["id"],
                                                                 drops[0]["extra"]), (17, 100))
                c.eq("on the left grip at 0 dB",
                     [(s["channel"], s["id"], s["gain"]) for s in
                      H.entries(es, H.RING_HAP_SCRIPT, what=H.SCRIPT_START)],
                     [(H.CH_LEFT_GRIP, 17, 0)])
            es = _wait(rig, start, H.RING_HAP_SCRIPT, 1, 6.0, what=H.SCRIPT_REARM)
            re = H.entries(es, H.RING_HAP_SCRIPT, what=H.SCRIPT_REARM)
            if c.eq("re-armed", len(re), 1) and falls:
                dt = _ms(falls[0], re[0])
                c.true("re-armed %.0f ms after the fall (5000 +- 100)" % dt, abs(dt - 5000) <= 100)
            _quiet(rig)
            start = rig.ring_next()
            c.eq("a free fall after the re-arm is taken", _play(rig, H.PLAY_FALL, 200), 1)
            es = _wait(rig, start, H.RING_HAP_SCRIPT, 1, 1.0, what=H.SCRIPT_DROP)
            drops = H.entries(es, H.RING_HAP_SCRIPT, what=H.SCRIPT_DROP)
            c.eq("byte 200 picks no sound", [(d["id"], d["extra"]) for d in drops], [(0, 200)])

    if ctx.want("haptics.amp_mode"):
        with ctx.check(GROUP, "haptics.amp_mode") as c:
            _setting(rig, 75, 1)
            c.eq("setting 75 = 1: amplifier held on", _status(rig)["amp_mode"], 1)
            _setting(rig, 75, 0)
            c.eq("setting 75 = 0: the amplifier follows playback", _status(rig)["amp_mode"], 0)
            _setting(rig, 76, 0)
            c.eq("setting 76 = 0: the master gain", _status(rig)["master_db"], 0)
