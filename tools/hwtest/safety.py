# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# safety.py: the suite's hard safety rules, as code. Nothing that writes to a device
# passes without going through here first. Device classification (S1), the puck stays read-only
# (S2) and writes only through a gated writer (S3, S4), the vendor denylist with one-shot flash
# tokens (S5), the shell allowlist (S6), injection preconditions (S7). Every decision is one
# function, covered by test_safety.py. SafetyError is raised to refuse; the caller turns it into
# exit code 2.
#
# The checks run where the bytes leave the runner, not only in the Rig: every board feature SET
# (win/hid.py BoardHid.feature_set, and the dry-run backends' feature_set), every through-puck
# feature SET (win/puck_write.py), every output report (BoardHid.write_output) and every console
# line (transport.ConsoleSession.send_line). The Rig's own calls are early pre-checks that never
# spend a flash token (consume=False); the one at the send spends it.
import re

from . import config as C
from .proto import testproto as _tp

# ---- device classes -------------------------------------------------------------------------

BOARD_HID = "BOARD_HID"
BOARD_CONSOLE = "BOARD_CONSOLE"
PUCK_SLOT = "PUCK_SLOT"
FORBIDDEN = "FORBIDDEN"

_SERIAL_RE = re.compile(C.SERIAL_RE)


class SafetyError(Exception):
    """A request the suite must never make. The runner turns it into exit code 2."""


class SteamOpened(SafetyError):
    """Steam started while a group that needs it closed was running (S8): the injection was
    stopped at once, and the group ends (the runner then waits for Steam to close, or stops)."""


class LeakFired(SafetyError):
    """The leak detector saw an input event from the board's mouse or keyboard collection while
    armed (S13): injection stopped, the hooks disarmed, and the run ends (exit 2)."""


class InjectionBlocked(SafetyError):
    """An injection after a safety layer failed earlier in this run (the guard canary, the
    firmware's lizard lock, the leak detector): refused, and the test SKIPs with the reason (the
    failure itself was already recorded, with its exit code)."""


def serial_ok(serial):
    return bool(serial) and bool(_SERIAL_RE.match(serial))


def classify(node):
    """Classify one enumerated device node into exactly one class (S1). `node` is a dict:
    kind ('hid'/'port'), instance_id (the USB device instance ID), interface_id (the HID interface
    path or the COM port's interface), serial (parsed from the instance ID, may be None),
    usb_product (the bus-reported product string, for the console). Returns (class, reason)."""
    iid = (node.get("instance_id") or "").upper()
    iface = (node.get("interface_id") or "").upper()
    both = iid + " " + iface
    serial = node.get("serial")

    if "VID_239A" in both:
        return FORBIDDEN, "UF2 bootloader"

    if node.get("kind") == "port":
        if "VID_2FE3&PID_0004" in both and (node.get("usb_product") == C.CONSOLE_PRODUCT):
            return BOARD_CONSOLE, "OpenSC2 console"
        return FORBIDDEN, "not the board's console"

    # HID from here on
    if "VID_28DE&PID_1304" in both:
        if re.search(r"MI_0[2-5]", both) and "COL03" in both:
            return PUCK_SLOT, "puck slot vendor collection"
        return FORBIDDEN, "puck non-slot interface"
    if "VID_28DE&PID_1303" in both:
        return FORBIDDEN, "another Valve controller"
    if "VID_28DE&PID_1302" in both:
        if "COL01" in both or "COL02" in both:
            return FORBIDDEN, "mouse/keyboard collection"
        if "COL03" not in both:
            return FORBIDDEN, "not the vendor collection"
        if not serial:
            return FORBIDDEN, "28DE:1302 with no serial (cannot be checked before opening)"
        if not serial_ok(serial):
            return FORBIDDEN, "28DE:1302 without an OSC serial (the real controller)"
        return BOARD_HID, "the board"
    return FORBIDDEN, "unrecognised device"


def assert_board_write(node, open_serial):
    """R1: a board writer may only act once the HID serial string read after opening equals the
    serial in the instance ID, and the node is really a BOARD_HID."""
    cls, _ = classify(node)
    if cls != BOARD_HID:
        raise SafetyError("write attempted on a %s node" % cls)
    if open_serial != node.get("serial") or not serial_ok(open_serial):
        raise SafetyError("HID serial string does not match the instance ID: closing unused")


# ---- the vendor denylist (S5) ---------------------------------------------------------------

_HARD_REFUSE = {C.VC_BOOTLOADER, C.VC_FACTORY_RESET, 0xFE, 0xC0, 0xC3, 0xD8}
_FLASH_OPCODES = {C.VC_BOND_WRITE, C.VC_USER_STORE_SET, C.VC_BY_NAME_SET, C.VC_BY_NAME_SAVE,
                  C.VC_BY_NAME_DELETE}
# the exact calibration forms cmd.calibration is allowed to send (0xE2)
_E2_ALLOWED = {b"\x00", b"\x01\x20", b"", b"\x02"}


class FlashToken:
    """A one-shot permit for exactly one flash-writing request: that opcode, key and value (R11).
    A request without a matching unused token is refused even with --flash."""

    __slots__ = ("opcode", "key", "value_crc", "used")

    def __init__(self, opcode, key, value_crc):
        self.opcode = opcode
        self.key = key
        self.value_crc = value_crc
        self.used = False


class Context:
    """What S5 needs to judge a 0xEE/0xEF/0xF0 or an injection: what 0xED last read, which slots
    read empty, whether the build has a real identity write path (caps bit 7), the flash tokens,
    and the live arming / Steam / sole-reader / lizard state for S7."""

    def __init__(self):
        self.read_values = {}        # key name -> the exact bytes 0xED last returned this session
        self.empty_slots = set()     # key names whose 0xED read empty this session
        self.caps_identity_write = False
        self.flash_allowed = False
        self.tokens = []             # FlashToken list
        # S7 live state
        self.armed = False
        self.guard = False
        self.canary_passed = False
        self.steam_running = False
        self.sole_reader = False
        self.lizard_off = False      # setting 9 == 0 confirmed within 5 s
        self.lizard_group = False    # the opt-in lizard group (allows lizard mode on)
        self.lizard_probe = False    # inputs.lizard_lock's one INJECT that must be DENIED

    def add_token(self, opcode, key, value):
        t = FlashToken(opcode, key, _tp.crc32(value))
        self.tokens.append(t)
        return t

    def _take_token(self, opcode, key, value, consume=True):
        """A matching unused token: True (and, with `consume`, spent). The Rig's pre-checks pass
        consume=False; the check at the send spends it, so one token allows exactly one request."""
        crc = _tp.crc32(value)
        for t in self.tokens:
            if not t.used and t.opcode == opcode and t.key == key and t.value_crc == crc:
                if consume:
                    t.used = True
                return True
        return False


def _split_named(payload):
    """A 0xEE/0xEF/0xF0 payload is name\\0value. Returns (name str or None, value bytes)."""
    nul = payload.find(b"\0")
    if nul < 0:
        return None, b""
    return payload[:nul].decode("ascii", "replace"), payload[nul + 1:]


def _check_named(opcode, payload, ctx, consume=True):
    """0xEE/0xEF/0xF0: the allowed forms of S5. Raises SafetyError when refused."""
    name, value = _split_named(payload)
    known = name in set(C.KEY_NAMES.values())
    is_bond = name in ("esb/bond", "esb/bond_2")
    kept = C.KEPT_KEYS.get(name)
    # calibration, before any token: never written, saved or deleted (the board's own once the
    # library keeps calibration); only the forms every firmware refuses before it writes: 0xEE of
    # another size, 0xEF with a byte after the NUL
    if name in C.CAL_KEYS:
        if opcode == C.VC_BY_NAME_SET and len(value) != C.CAL_KEYS[name]:
            return
        if opcode == C.VC_BY_NAME_SAVE and len(value) > 0:
            return
        raise SafetyError("0x%02X on %s: calibration is never written, saved or deleted"
                          % (opcode, name))
    # a matching one-shot token (with --flash) lets a real flash write through
    if ctx.flash_allowed and ctx._take_token(opcode, name, value, consume):
        return
    if kept and opcode == C.VC_BY_NAME_SET:
        size, readable = kept
        if len(value) != size:
            return   # another size: refused
        if readable and value == ctx.read_values.get(name):
            return   # the value 0xED just read back, in RAM only
        raise SafetyError("0xEE %s with a new value needs --flash and a token" % name)
    if opcode == C.VC_BY_NAME_DELETE:   # 0xF0
        if name is None or not known:
            return   # deleting an unknown key: the firmware refuses before any flash access
        raise SafetyError("0xF0 on a known key needs --flash and a token")
    if opcode == C.VC_BY_NAME_SAVE:     # 0xEF
        if name is None:
            return
        if known and len(value) > 0:
            return   # a byte after the NUL: refused by the firmware before writing
        if is_bond and name in ctx.empty_slots and len(value) == 0:
            return   # an empty bond: refused before writing
        raise SafetyError("0xEF on %r needs --flash and a token" % name)
    if opcode == C.VC_BY_NAME_SET:      # 0xEE
        if name is None:
            raise SafetyError("0xEE with no NUL")
        if is_bond:
            if len(value) != 24:
                return   # a length other than 24: refused
            if value == ctx.read_values.get(name):
                return   # the 24 bytes just read back
            if name in ctx.empty_slots and value == bytes(24):
                return   # 24 zero bytes for a slot that read empty
            raise SafetyError("0xEE %s with a non-matching bond needs --flash and a token" % name)
        if name == "user/wireless_transport":
            if len(value) in (0, 2):
                return   # refused forms
            if value == b"\x01":
                return   # Bluetooth: accepted, ignored (key_set stores nothing for 0 and 1)
            if value == ctx.read_values.get(name):
                return   # the value read back
            raise SafetyError("0xEE %s with a new value needs --flash and a token" % name)
        if name == "user/haptic_boot_level":
            # key_set stores any one-byte value as the chime level (and flash.chime would then
            # persist it): only the refused lengths, or exactly the level 0xED just read back
            if len(value) in (0, 2):
                return   # refused forms
            if value == ctx.read_values.get(name):
                return   # the value read back
            raise SafetyError("0xEE %s with a new value needs --flash and a token" % name)
        if name is not None and name.startswith("opensc2/"):
            if value == b"bad!" and not ctx.caps_identity_write:
                return
            raise SafetyError("0xEE %s: identity write refused" % name)
        # any other name (e.g. cal/joy_l 00): the library does not know it, the firmware refuses
        return
    raise SafetyError("unreachable named opcode")


def check_vendor(opcode, payload, transport, ctx=None, key_u32=None, consume=True):
    """The whole check for a vendor SET about to go to a device or the test link. `payload` is the
    command payload (the bytes after n). `key_u32`, when given, is the 0x95/0x86 key read as the
    firmware reads it (buffer bytes 3..6 zero padded); the caller passes it so a key behind n=0
    still counts (R11). `consume`: spend the flash token that allows it (the check at the send);
    False for a pre-check. Raises SafetyError when the request must not be sent."""
    ctx = ctx or Context()
    if opcode == C.TEST_OPCODE:
        raise SafetyError("0x7E must go through the test-channel path, not as a raw vendor SET")
    if opcode == C.VC_BOOTLOADER:
        raise SafetyError("0x90 (bootloader) is never sent")
    if opcode == C.VC_FACTORY_RESET:
        raise SafetyError("0x86 (factory reset) is never sent")
    if opcode in (0xFE, 0xC0, 0xC3, 0xD8):
        raise SafetyError("calibration/erase opcode 0x%02X is never sent" % opcode)
    if opcode == C.VC_POWER_REBOOT:   # 0x95
        k = key_u32 if key_u32 is not None else int.from_bytes((payload + bytes(4))[:4], "little")
        if k == C.KEY_BOOTLOADER:
            raise SafetyError("0x95 with the bootloader key is never sent")
        return   # plain reboot, reboot-into-wireless and turn-off keys are allowed
    if opcode == C.VC_CALIBRATION:    # 0xE2
        if bytes(payload) not in _E2_ALLOWED:
            raise SafetyError("0xE2 only in the exact cmd.calibration forms")
        return
    if opcode in _FLASH_OPCODES:
        if opcode == C.VC_BOND_WRITE:   # 0xA2
            if ctx.flash_allowed and ctx._take_token(opcode, None, payload, consume):
                return
            raise SafetyError("0xA2 (write bond) needs --flash and a token")
        if opcode == C.VC_USER_STORE_SET:   # 0xDC
            if len(payload) >= 1 and payload[0] != 1:
                return   # a type other than 1: refused by the firmware before writing
            if ctx.flash_allowed and ctx._take_token(opcode, None, payload, consume):
                return
            raise SafetyError("0xDC type 1 (chime) needs --flash and a token")
        _check_named(opcode, bytes(payload), ctx, consume)
        return
    if opcode == C.VC_DEVICE_INFO and bytes(payload)[:1] == b"\x01":   # 0xF2 selector 1
        # its GET marks a new crash record seen, a flash write: allowed when the record 0xED read
        # last is not new (or there is none), or with --flash and a token
        record = ctx.read_values.get("debug/resetreason")
        if record is not None and (len(record) == 0 or record[0] == 0):
            return
        if ctx.flash_allowed and ctx._take_token(opcode, "debug/resetreason", b"\x01", consume):
            return
        raise SafetyError("0xF2 selector 1 with a new or unread crash record writes flash: read "
                          "debug/resetreason first, or --flash and a token")
    # everything else (reads, settings, lizard, LED, reboots already handled) is allowed


def check_feature_buffer(buffer, transport, ctx=None, consume=True):
    """S5 on a full 64-byte feature buffer `01 <op> <n> <payload>` (USB or puck), with the live
    Context. Feature report 2 is never sent to the puck (S4) and never built for the board as a
    command. Opcode 0x7E is the test channel: check_test_request() (S7 for an injection, only
    reading ops through the puck, the HOST_SET allowlist); every other opcode check_vendor()."""
    if len(buffer) < 3:
        raise SafetyError("short feature buffer")
    if len(buffer) != C.FEATURE_LEN:
        raise SafetyError("a feature buffer is 64 bytes (%d)" % len(buffer))
    if buffer[0] != C.FEATURE_ID:
        raise SafetyError("feature report ID %d is never sent as a command (only report 1)"
                          % buffer[0])
    opcode = buffer[1]
    n = buffer[2]
    payload = bytes(buffer[3:3 + n])
    if opcode == C.TEST_OPCODE:
        check_test_request(payload, transport, ctx)
        return
    key_u32 = int.from_bytes(bytes(buffer[3:7]), "little")
    check_vendor(opcode, payload, transport, ctx=ctx, key_u32=key_u32, consume=consume)


def check_output_report(report):
    """An output report to the board (W: WriteFile on interrupt OUT): the IDs a host sends,
    0x80..0x89, and 0x8A (outputs.ignored), at most 64 bytes."""
    report = bytes(report)
    if not report or not 0x80 <= report[0] <= 0x8A or len(report) > C.FEATURE_LEN:
        raise SafetyError("output reports to the board are 0x80..0x8A, at most 64 bytes")


# ---- the test channel at the send (S7 again, the puck's reading ops, the HOST_SET list) -------

PUCK_READ_OPS = {C.OP_STATUS, C.OP_RING_READ, C.OP_CNT_READ, C.OP_SETTINGS_READ}


def _injection_op(op, args):
    """INJECT, SEQ_RUN, or APP_SEND of a controller-state report (0x45): what S7 guards."""
    if op in (C.OP_INJECT, C.OP_SEQ_RUN):
        return True
    return op == C.OP_APP_SEND and len(args) >= 2 and args[0] == 0 and args[1] == C.RPT_BT_STATE


def check_test_request(payload, transport, ctx=None):
    """A test-channel request (the payload of a 0x7E SET, or a console `raw` line) about to go
    out. `transport`: "usb" (the board's feature report 1), "puck" (through the puck) or
    "console". On USB a payload that is no request is one of the probes the firmware must refuse
    (check_test_probe); through the puck only the reading ops; HOST_SET only with an opcode of
    its allowlist; an injection only when S7 holds on the live Context (a missing Context refuses
    it: fail closed)."""
    payload = bytes(payload)
    if not _tp.is_request(payload):
        if transport != "usb":
            raise SafetyError("only test-channel requests (the key, version 1) go %s"
                              % ("through the puck" if transport == "puck" else
                                 "to the console"))
        check_test_probe(payload)
        return
    op, args = payload[6], payload[7:]
    if transport == "puck" and op not in PUCK_READ_OPS:
        raise SafetyError("only reading test-channel ops go through the puck")
    if op == C.OP_HOST_SET and len(args) >= 2:
        check_host_set(args[0], bytes(args[2:2 + args[1]]))
    if _injection_op(op, args):
        ctx = ctx if ctx is not None else Context()
        lizard_flag = (op == C.OP_INJECT and len(args) >= 1 and args[0] & C.INJECT_FLAG_LIZARD) \
            or (op == C.OP_SEQ_RUN and len(args) >= 2 and args[1] & C.SEQ_FLAG_LIZARD)
        if lizard_flag and not ctx.lizard_group:
            raise SafetyError("the lizard-test flag outside the lizard group")
        check_injection(ctx)


# ---- the HOST_SET allowlist mirror (the runner never builds a forbidden one) ----------------

HOST_SET_ALLOW = {0x81, 0x83, 0x85, 0x87, 0x89, 0x8E, 0x9F, C.VC_RESET_INFO, 0xAE, 0xBE, 0xC5,
                  0xDB, 0xE9, 0xED, 0xF2}


def check_host_set(opcode, payload, ctx=None):
    """What the runner may put on the test link (HOST_SET). The firmware enforces the same
    allowlist (R9); the runner must never build anything outside it."""
    if opcode == C.VC_POWER_REBOOT:
        k = int.from_bytes((bytes(payload) + bytes(4))[:4], "little")
        if k != C.KEY_TURN_OFF:
            raise SafetyError("0x95 on the test link only with the turn-off key")
        return
    if opcode not in HOST_SET_ALLOW:
        raise SafetyError("opcode 0x%02X is not on the HOST_SET allowlist" % opcode)
    if opcode == C.VC_DEVICE_INFO and bytes(payload)[:1] == b"\x01":
        raise SafetyError("0xF2 selector 1 is not on the HOST_SET allowlist (its GET marks a "
                          "crash record seen, a flash write)")
    # reads and the harmless setters above never write flash; still run the shared denylist for
    # the few that could carry a forbidden shape (none here), to keep one rule.
    if opcode in _HARD_REFUSE or opcode in _FLASH_OPCODES:
        raise SafetyError("opcode 0x%02X refused on the test link" % opcode)


# ---- the probes that must be refused (hooks.invisible, cmd.unknown, cmd.framing) ------------

def check_test_probe(payload):
    """A SET of opcode 0x7E that the firmware must not claim: a wrong key or version, shorter than
    the header, or (while unarmed) a keyed STATUS. A probe never carries an op that does
    something: a keyed one must be STATUS."""
    payload = bytes(payload)
    if len(payload) > C.ANSWER_MAX:
        raise SafetyError("a 0x7E probe longer than a vendor payload")
    if _tp.is_request(payload) and payload[6] != C.OP_STATUS:
        raise SafetyError("a keyed 0x7E probe must be a STATUS")


_FRAMING_PROBES = {
    "set2": bytes([2, C.VC_ATTRIBUTES, 0]) + bytes(C.FEATURE_LEN - 3),   # feature report 2
    "get2": bytes([2]) + bytes(C.FEATURE_LEN - 1),
    "set_output": bytes([0x80]) + bytes(C.FEATURE_LEN - 1),     # an all-zero 0x80: a rumble stop
    "get_input": bytes([C.RPT_STATE]) + bytes(C.FEATURE_LEN - 1),
}


def check_framing_probe(kind):
    """cmd.framing's four requests the board stalls before it looks at the data, to the board
    only (never the puck): exactly these buffers. Returns the buffer."""
    if kind not in _FRAMING_PROBES:
        raise SafetyError("no framing probe %r" % kind)
    return _FRAMING_PROBES[kind]


def check_framing_buffer(kind, buffer):
    """The same rule where the probe is sent (win/hid.py): exactly one of the four buffers."""
    if kind not in _FRAMING_PROBES or bytes(buffer) != _FRAMING_PROBES[kind]:
        raise SafetyError("only the four framing probes of cmd.framing go out that way")


# ---- the shell allowlist (S6) ---------------------------------------------------------------

from .proto import console as _console  # noqa: E402

_CONTROL_CHARS = re.compile(r"[\x00-\x1f\x7f]")
# `opensc2 test <id> <verb> [arguments]`: the machine verbs, and the ones that only read or
# stop (never `arm`, which arms without the restore flag)
_TEST_LINE = re.compile(r"opensc2 test ([0-9]{1,5}) (.+)$")
_TEST_VERBS = {"sync", "status", "disarm", "cnt", "settings", "inject off", "battery off",
               "seq stop"}
_TEST_RING = re.compile(r"ring(?: [0-9]{1,10})?$")
_TEST_RAW = re.compile(r"raw ([0-9a-fA-F]{14,122})$")


def _check_test_line(req_id, rest, ctx):
    if not 1 <= req_id <= 65535:
        raise SafetyError("a console test line with id %d (1..65535)" % req_id)
    if rest in _TEST_VERBS or _TEST_RING.match(rest):
        return
    m = _TEST_RAW.match(rest)
    if not m or len(m.group(1)) % 2:
        raise SafetyError("console test line not on the allowlist: %r" % rest[:40])
    check_test_request(bytes.fromhex(m.group(1)), "console", ctx)


def check_shell(command, flash_allowed=False, policy_token=None, policy_in_use=None, ctx=None,
                consume=True):
    """S6, on the exact line that goes out (ConsoleSession.send_line) and as the Rig's pre-check:
    only the allowed commands and `opensc2 test <id> sync|status|raw <hex>` (and the test verbs
    that only read or stop), each exactly as the runner builds it: no control character (an
    embedded CR or LF would start a second command), no stray white space, an id of 1..65535, a
    `raw` request that is a test request (S7 and the HOST_SET allowlist on its op, with the live
    Context `ctx`). One exception with --flash and a token: `opensc2 mode auto|wireless`,
    whichever `opensc2 mode` showed to be in use; `consume` spends the token (False: a pre-check)."""
    if not isinstance(command, str):
        raise SafetyError("a console line must be text")
    if _CONTROL_CHARS.search(command):
        raise SafetyError("a console line with a control character is never sent: %r"
                          % command[:40])
    if command != " ".join(command.split()):
        raise SafetyError("a console line with stray white space is never sent: %r"
                          % command[:40])
    cmd = command
    if cmd in _console.EXISTING_COMMANDS:
        return
    m = _TEST_LINE.match(cmd)
    if m:
        _check_test_line(int(m.group(1)), m.group(2), ctx)
        return
    m = re.match(r"opensc2 mode (auto|wireless)$", cmd)
    if m and flash_allowed and policy_token and not policy_token.used:
        want = m.group(1)
        if policy_in_use is not None and want != policy_in_use:
            raise SafetyError("opensc2 mode may only restate the policy in use")
        if consume:
            policy_token.used = True
        return
    raise SafetyError("shell command not on the allowlist: %r" % cmd[:60])


# ---- the puck writer gate (S3) --------------------------------------------------------------

def puck_writer(make_writer, *, through_puck, steam_running, slot_proven_s):
    """S3: a PuckSlotWriter can be made only with --through-puck, Steam not running, and the slot
    proven to be the board's within the last 10 s. `make_writer` is the win/ factory; this gate is
    the only way to it."""
    if not through_puck:
        raise SafetyError("writing to the puck needs --through-puck")
    if steam_running:
        raise SafetyError("Steam is running: no writes to the puck")
    if slot_proven_s is None or slot_proven_s > 10.0:
        raise SafetyError("the board's slot is not proven fresh (< 10 s)")
    return make_writer()


# ---- injection preconditions (S7) -----------------------------------------------------------

def check_injection(ctx):
    """S7: an injection (INJECT, SEQ_RUN, or APP_SEND of a 0x45 state report) needs: armed and
    guard set, the canary passed this session, Steam not running, the runner the sole reader, and,
    outside the lizard group, lizard mode off. The one exception to the last: inputs.lizard_lock's
    probe, an INJECT the firmware must answer DENIED because lizard mode is on (ctx.lizard_probe,
    set only around that one request)."""
    if not (ctx.armed and ctx.guard):
        raise SafetyError("injection before arming / guard")
    if not ctx.canary_passed:
        raise SafetyError("injection before the guard canary passed")
    if ctx.steam_running:
        raise SafetyError("injection while Steam is running")
    if not ctx.sole_reader:
        raise SafetyError("injection without holding the collection as the sole reader")
    if not ctx.lizard_group and not ctx.lizard_off and not ctx.lizard_probe:
        raise SafetyError("injection while lizard mode is on (outside the lizard group)")
