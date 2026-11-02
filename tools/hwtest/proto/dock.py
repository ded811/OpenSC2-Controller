# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# dock.py: the dock's frames (docs/protocol.md, "The dock") and the test hooks' dock ops
# (docs/test-protocol.md): CRC-16/KERMIT, building and checking 73-byte frames, the arguments and
# results of the ops 0x90 to 0x93, and a model of what the board answers to each request, of the
# pilot check, the supply rule and the docked states. The dock group, the simulator and
# tools/fake_puck.py use it; tests/vectors/hwtest/dock.txt ties it to the C rules.
import struct

FRAME_LEN = 73
TYPE_OFF = 1
RESULT_OFF = 4
PAYLOAD_OFF = 8
PAYLOAD_LEN = 63
CRC_OFF = 71
START_PUCK = 0x5A
START_CONTROLLER = 0xAC
INTERVAL_US = 64000          # attribute 11 while the dock is the controller's current link
BAUD = 115200
GAP_TIMEOUT_MS = 64

PROBE, GET, SET, INPUT = 0, 1, 2, 3
TYPE_NAMES = {PROBE: "probe", GET: "GET", SET: "SET", INPUT: "GET input"}
RESULT_GET_OK = 63
RESULT_NOTHING_TO_READ = -95
RESULT_INVALID = -22

# the probe's identity record inside an answer frame: the two serials, masked by the board
SERIALS_OFF = PAYLOAD_OFF + 16
SERIALS_LEN = 28
GET_DATA_OFF = PAYLOAD_OFF + 2

# ---- frames ----------------------------------------------------------------------------------


def crc16(data, crc=0):
    """CRC-16/KERMIT: polynomial 0x1021 bit-reversed (0x8408), initial value `crc`, no final XOR;
    "123456789" gives 0x2189."""
    for b in bytes(data):
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc


def seal(frame, start):
    """The start byte at 0 and the CRC of bytes 0..70 at 71..72 (low byte first)."""
    f = bytearray(frame)
    f[0] = start
    c = crc16(f[:CRC_OFF])
    f[CRC_OFF] = c & 0xFF
    f[CRC_OFF + 1] = c >> 8
    return bytes(f)


def build_request(type_, payload=b""):
    """A request as the puck builds it: 0x5A, the type, zeros, the payload (at most 63 bytes),
    the CRC."""
    payload = bytes(payload)
    if len(payload) > PAYLOAD_LEN:
        raise ValueError("a dock payload is at most 63 bytes")
    f = bytearray(FRAME_LEN)
    f[TYPE_OFF] = type_ & 0xFF
    f[PAYLOAD_OFF:PAYLOAD_OFF + len(payload)] = payload
    return seal(f, START_PUCK)


def set_request(opcode, payload=b""):
    """A SET (type 2) of the vendor command {opcode, n, payload}."""
    payload = bytes(payload)
    return build_request(SET, bytes([opcode, len(payload)]) + payload)


def check(frame):
    """"OK", "BAD_SIZE" or "BAD_CRC", as the controller checks a received frame."""
    frame = bytes(frame)
    if len(frame) != FRAME_LEN:
        return "BAD_SIZE"
    return "OK" if crc16(frame) == 0 else "BAD_CRC"


def parse_answer(frame):
    """An answer frame: {start, type, result, payload, crc_ok}."""
    frame = bytes(frame)
    return {"start": frame[0], "type": frame[TYPE_OFF],
            "result": struct.unpack_from("<i", frame, RESULT_OFF)[0],
            "payload": frame[PAYLOAD_OFF:PAYLOAD_OFF + PAYLOAD_LEN], "crc_ok": check(frame) == "OK"}


def answer(type_, result=0, payload=b""):
    """An answer frame over zeros: 0xAC, the type, zeros, the result, the payload zero padded, the
    CRC. The board builds each answer over its previous one (AnswerModel); this is one whose every
    other byte happens to be zero."""
    f = bytearray(FRAME_LEN)
    f[TYPE_OFF] = type_ & 0xFF
    struct.pack_into("<i", f, RESULT_OFF, result)
    payload = bytes(payload)[:PAYLOAD_LEN]
    f[PAYLOAD_OFF:PAYLOAD_OFF + len(payload)] = payload
    return seal(f, START_CONTROLLER)


class AnswerModel:
    """The board's answers as the real controller builds them, and OpenSC2 with it: each over the
    previous answer (zeros after a boot), writing only what the request defines (dock_rules.h):
    a probe the record over the payload's first 57 bytes (type and result stay); a SET its type
    and result (the payload stays); a GET its type and result and, unless there was nothing to
    read, the stored request {opcode, length, data} and then the answer {opcode, n, data} over the
    payload; GET input its type, result 0 and 53 bytes; any other type nothing."""

    def __init__(self, last=None):
        self.last = bytearray(last if last is not None else bytes(FRAME_LEN))

    def _out(self):
        self.last[:] = seal(self.last, START_CONTROLLER)
        return bytes(self.last)

    def _head(self, type_, result):
        self.last[TYPE_OFF] = type_ & 0xFF
        struct.pack_into("<i", self.last, RESULT_OFF, result)

    def _over(self, data):
        data = bytes(data)[:PAYLOAD_LEN]
        self.last[PAYLOAD_OFF:PAYLOAD_OFF + len(data)] = data

    def probe(self, record):
        self._over(bytes(record)[:57])
        return self._out()

    def set(self, result=0):
        self._head(SET, result)
        return self._out()

    def get(self, result, stored=b"", answer_rec=b""):
        self._head(GET, result)
        if result != RESULT_NOTHING_TO_READ:
            self._over(stored)
            self._over(answer_rec)
        return self._out()

    def input(self, body):
        self._head(INPUT, 0)
        self._over(bytes(body)[:53])
        return self._out()

    def other(self):
        return self._out()

    def request(self, frame, record=b"", get=(RESULT_GET_OK, b"", b""), set_result=0,
                body=b""):
        """The answer to `frame` (a request), with what the board would supply for it."""
        t = bytes(frame)[TYPE_OFF]
        if t == PROBE:
            return self.probe(record)
        if t == SET:
            return self.set(set_result)
        if t == GET:
            return self.get(*get)
        if t == INPUT:
            return self.input(body)
        return self.other()


def identity_record(ident):
    """The 57-byte identity record from a dict {pid, build, blbuild, rev, unit, board, git}."""
    def field(text, size):
        b = (text or "").encode("ascii", "replace")[:size - 1]
        return b + bytes(size - len(b))
    return (struct.pack("<IIII", ident.get("pid", 0x1302), ident.get("build", 0),
                        ident.get("blbuild", 0), ident.get("rev", 0)) +
            field(ident.get("unit"), 14) + field(ident.get("board"), 14) +
            field(ident.get("git"), 13))


# ---- the test hooks' dock ops (docs/test-protocol.md) -----------------------------------------

OP_LOAD, OP_RUN, OP_READ, OP_CTRL = 0x90, 0x91, 0x92, 0x93
RUN_START_SESSION = 0x01
RUN_ANSWERED, RUN_SHORT, RUN_BAD_CRC, RUN_NOTHING = 0, 1, 2, 3
RUN_OUTCOMES = {0: "answered", 1: "short", 2: "bad CRC", 3: "nothing"}
ANS_CRC_OK, ANS_MASKED = 0x01, 0x02
READ_MAX = 55
CTRL_STATUS, CTRL_END, CTRL_PILOT, CTRL_SUPPLY, CTRL_EVENT, CTRL_RESET = 0, 1, 2, 3, 4, 5
ST_ACTIVE, ST_ALONE, ST_TEST, ST_UART = 0x01, 0x02, 0x04, 0x08
SESSION_STARTERS = {0: "pilot", 1: "test", 2: "application"}
SESSION_ENDS = {0: "none", 1: "bad size", 2: "bad CRC", 3: "receive error", 4: "arm failed",
                5: "stopped"}


def load_args(frame):
    """The two DOCK_LOAD argument blocks of a 73-byte frame: 53 bytes at 0, 20 at 53."""
    frame = bytes(frame)
    if len(frame) != FRAME_LEN:
        raise ValueError("a dock frame is 73 bytes")
    return [bytes([0]) + frame[:53], bytes([53]) + frame[53:]]


def parse_run(result):
    r = bytes(result)
    if len(r) < 21:
        raise ValueError("short DOCK_RUN result")
    return {"outcome": r[0], "session": bool(r[1]), "start": r[2], "type": r[3],
            "result": struct.unpack_from("<i", r, 4)[0], "crc_ok": bool(r[8] & ANS_CRC_OK),
            "masked": bool(r[8] & ANS_MASKED), "crc32": struct.unpack_from("<I", r, 9)[0],
            "masked_crc32": struct.unpack_from("<I", r, 13)[0],
            "us": struct.unpack_from("<I", r, 17)[0]}


def encode_run(d):
    return (bytes([d["outcome"], 1 if d["session"] else 0, d.get("start", 0), d.get("type", 0)]) +
            struct.pack("<i", d.get("result", 0)) +
            bytes([(ANS_CRC_OK if d.get("crc_ok") else 0) | (ANS_MASKED if d.get("masked")
                                                              else 0)]) +
            struct.pack("<III", d.get("crc32", 0), d.get("masked_crc32", 0), d.get("us", 0)))


def parse_status(result):
    r = bytes(result)
    if len(r) < 22:
        raise ValueError("short dock STATUS result")
    return {"active": bool(r[0] & ST_ACTIVE), "alone": bool(r[0] & ST_ALONE),
            "test": bool(r[0] & ST_TEST), "uart": bool(r[0] & ST_UART), "last_end": r[1],
            "sessions": struct.unpack_from("<I", r, 2)[0],
            "frames": struct.unpack_from("<I", r, 6)[0],
            "errors": struct.unpack_from("<I", r, 10)[0],
            "slowest_us": struct.unpack_from("<I", r, 14)[0], "sm_state": r[18],
            "supply": r[19], "stored": r[20], "cache_valid": bool(r[21]),
            # byte 22 (later firmware): the docked stick offsets in effect, the supply rule's view
            "offsets": bool(r[22] & 1) if len(r) > 22 else None,
            "rule_supply": (r[22] >> 1) & 3 if len(r) > 22 else None}


def encode_status(d):
    flags = ((ST_ACTIVE if d.get("active") else 0) | (ST_ALONE if d.get("alone") else 0) |
             (ST_TEST if d.get("test") else 0) | (ST_UART if d.get("uart") else 0))
    return (bytes([flags, d.get("last_end", 0)]) +
            struct.pack("<IIII", d.get("sessions", 0), d.get("frames", 0), d.get("errors", 0),
                        d.get("slowest_us", 0)) +
            bytes([d.get("sm_state", 0), d.get("supply", 0), d.get("stored", 0x83),
                   1 if d.get("cache_valid") else 0,
                   (1 if d.get("offsets") else 0) | ((d.get("rule_supply") or 0) << 1)]))


def pilot_args(mv):
    """DOCK_CTRL PILOT: three readings in mV (None: a conversion that failed)."""
    return bytes([CTRL_PILOT]) + b"".join(
        struct.pack("<H", 0xFFFF if v is None else v) for v in mv)


def mask(frame, masked):
    """What DOCK_READ shows of an answer: the masked bytes as zeros."""
    return bytes(0 if i in masked else b for i, b in enumerate(bytes(frame)))


def masked_bytes(frame, masked_get, req_type=None):
    """The positions an answer to a request of `req_type` (by default the answer's own type) adds
    to what the board masks: a probe's serials, or (`masked_get`, a GET of a bond, a serial or the
    chip ID) the data of a GET's answer. The board keeps a masked byte masked in later answers
    until a request writes it again (they are built over each other)."""
    frame = bytes(frame)
    t = frame[TYPE_OFF] if req_type is None else req_type
    if t == PROBE:
        return set(range(SERIALS_OFF, SERIALS_OFF + SERIALS_LEN))
    if t == GET and masked_get and \
            struct.unpack_from("<i", frame, RESULT_OFF)[0] == RESULT_GET_OK:
        n = frame[PAYLOAD_OFF + 1]
        return set(range(GET_DATA_OFF, GET_DATA_OFF + n))
    return set()


# ---- the pure rules, as dock_rules.c has them (and dock.txt checks) ---------------------------

PILOT_MIN_MV, PILOT_MAX_MV = 2000, 3300


def pilot_mv(count):
    return -1 if count < 0 else count * 3600 // 4096


def pilot_present(mv):
    """(present, inside bits) of three readings (a negative or None reading: outside)."""
    inside = 0
    for i, v in enumerate(mv):
        if v is not None and PILOT_MIN_MV <= v <= PILOT_MAX_MV:
            inside |= 1 << i
    return bin(inside).count("1") >= 2, inside


SUPPLY_NONE, SUPPLY_USB, SUPPLY_DOCK = 0, 1, 2


def supply_kind(usb, plus, pilot):
    """A supply (VBUS or the + on its own input) that appears with the pilot is the dock's, any
    other USB power (the real controller's + and USB-C port share one input)."""
    if not (usb or plus):
        return SUPPLY_NONE
    return SUPPLY_DOCK if pilot else SUPPLY_USB


def power_step(rule, usb, plus, pilot):
    """rule = [reported, present, kind]; returns the event (0, 1, 2) or None."""
    present = bool(usb or plus)
    if rule[0] and present == rule[1]:
        return None
    rule[0] = True
    rule[1] = present
    rule[2] = supply_kind(usb, plus, pilot) if present else SUPPLY_NONE
    return rule[2]


STATES = ["AWAY", "RADIO_OFF", "LIFTED", "CHORD", "LINK_UP"]
EVENTS = ["POWER_GONE", "POWER_USB", "POWER_DOCK", "CHORD", "CHORD_UP", "TIMER", "STEAM_7S",
          "LINK_OFF", "TURN_OFF", "WARM_REBOOT", "REBOOT", "BOOTLOADER", "OTHER"]
ENTRIES = ["POWER_ON", "BATTERY_WAIT", "ON_BATTERY"]
ACTIONS = ["STOP_LINK", "START_LINK", "PAIRING", "SELECT", "CUE", "CHIME_OFF", "TIMER_4S",
           "TIMER_5S", "TIMER_STOP", "PAUSE", "REBOOT", "SHUTDOWN", "UPDATE_WINDOW",
           "TO_BATTERY", "TO_CHORD", "LED_CHARGING", "LED_TRANSPORT", "LED_COLOUR", "LED_OFF",
           "KEYLOCK_CLEAR", "OFFSETS_ON", "OFFSETS_OFF"]
A = {name: 1 << i for i, name in enumerate(ACTIONS)}
AWAY, RADIO_OFF, LIFTED, CHORD, LINK_UP = range(5)


def action_names(bits):
    return [n for n in ACTIONS if bits & A[n]] or ["NONE"]


class DockStates:
    """The docked states (dock_rules.c's osc2_dock_sm_*), for the simulator and the model."""

    def __init__(self):
        self.state = AWAY
        self.chord = 0
        self.selected = False

    def _go(self, to, actions):
        if self.state == CHORD:
            actions |= A["LED_TRANSPORT"] | A["TIMER_STOP"]
        elif self.state == LIFTED:
            actions |= A["TIMER_STOP"]
        if to == RADIO_OFF:
            actions |= A["LED_CHARGING"] | A["OFFSETS_ON"] | (
                A["STOP_LINK"] if self.selected else 0)
        elif to == LIFTED:
            actions |= A["LED_OFF"] | A["OFFSETS_OFF"] | A["TIMER_5S"]
        elif to == CHORD:
            actions |= A["SELECT"] | A["CUE"] | A["LED_COLOUR"] | A["TIMER_4S"] | A["PAUSE"]
        elif to == LINK_UP:
            actions |= A["LED_TRANSPORT"] | A["OFFSETS_ON"]
        if actions & (A["START_LINK"] | A["SELECT"]):
            self.selected = True
        frm, self.state = self.state, to
        return (frm, to, actions, True)

    def enter(self, how, warm):
        if self.state != AWAY:
            return (self.state, self.state, 0, False)
        if how == 0:
            return self._go(LINK_UP, A["KEYLOCK_CLEAR"] | A["START_LINK"]) if warm else \
                self._go(RADIO_OFF, A["KEYLOCK_CLEAR"])
        if how == 1:
            self.selected = True
            return self._go(RADIO_OFF, 0)
        self.selected = True
        return self._go(LINK_UP, 0)

    def step(self, ev, chord=0):
        s = self.state
        nothing = (s, s, 0, False)
        if s == RADIO_OFF:
            if ev == 3 and chord < 5:
                self.chord = chord
                return self._go(CHORD, 0)
            if ev == 0:
                return self._go(LIFTED, 0)
            if ev == 9:
                return self._go(AWAY, A["REBOOT"])
        elif s == LIFTED:
            if ev == 2:
                return self._go(RADIO_OFF, 0)
            if ev == 1:
                return self._go(AWAY, A["REBOOT"])
            if ev == 5:
                return self._go(AWAY, A["SHUTDOWN"])
            if ev == 3 and chord < 5:
                self.chord = chord
                return self._go(AWAY, A["TO_CHORD"])
        elif s == CHORD:
            if ev == 5:
                return self._go(LINK_UP, A["START_LINK"] | A["PAIRING"])
            if ev == 4 and chord == self.chord:
                return self._go(LINK_UP, A["START_LINK"])
        elif s == LINK_UP:
            if ev == 11:
                return (s, s, A["UPDATE_WINDOW"], True)
            if ev in (9, 10):
                return self._go(AWAY, A["REBOOT"])
            if ev in (6, 7, 8):
                return self._go(RADIO_OFF, A["CHIME_OFF"])
            if ev == 0:
                return self._go(AWAY, A["TO_BATTERY"] | A["OFFSETS_OFF"])
        return nothing
