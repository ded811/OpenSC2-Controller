# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# simble.py: the simulator's Bluetooth link (the offline dry run of --bluetooth, path B). The
# board's side follows the library's Bluetooth rules as docs/api.md and docs/test-protocol.md state
# them: the slot's identities and their bonds, the three advertising forms, the window, what follows
# a disconnect, the gate, BLE_STATUS, BLE_DROP and BLE_WINDOW, the Bluetooth ring entries. The PC's
# side is a Windows host bonded to the board's identity 1: it connects whenever the board advertises
# for it (directed or allow-listed), and never answers pairing mode. Nothing here is Windows' real
# behaviour, which only a hardware run can show.
import struct

from . import config as C
from .proto import testproto as T

SIM_BLE_ADDRESS = "C34F12ABCDEF"     # the board's identity in use (masked in every output)
SIM_BLE_OTHER = "D2A1B2C3D4E5"       # a real controller paired with the same PC
INTERVAL_US = 7500
HOST_CONNECT_MS = 200                # Windows connects this long after advertising for it starts
READVERTISE_MS = 500
WINDOW_MS = 60000

LINK_OFF, LINK_PAIRING, LINK_LISTENING, LINK_CONNECTED = 0, 2, 3, 4
REASON_NONE, REASON_LINK_LOST, REASON_DISABLED = 0, 1, 2
REASON_PAIRED, REASON_LISTEN_TIMEOUT, REASON_HOST_DISCONNECTED = 16, 18, 20
FORM_PAIRING, FORM_ALLOW, FORM_DIRECTED, FORM_NONE = 0, 1, 2, 0xFF
POWER_WIRELESS_OFF = 8


class BleLink:
    """One boot's Bluetooth side, while Bluetooth is the running transport."""

    def __init__(self, board):
        self.b = board
        w = board.world
        self.slot = w.ble_slot
        self.identity = 1 + 2 * self.slot
        self.connected = self.gate = self.advertising = False
        self.form = FORM_NONE
        self.wireless_off = self.pair_request = False
        self.security, self.mtu = 0, 23
        self.last_reason = 0
        self.reports = self.dropped = 0
        self.window_once_ms = 0
        self.window_end = None
        self.drop_reason = 0
        self.want = True
        self.round = 0
        self.off_why = ""

    # ---- what the board shows ----
    @property
    def bonded(self):
        return self.b.world.ble_bonded

    def link_state(self):
        if self.connected:
            return LINK_CONNECTED
        if self.advertising:
            return LINK_PAIRING if self.form == FORM_PAIRING else LINK_LISTENING
        return LINK_LISTENING if self.want else LINK_OFF

    def status(self):
        left = max(0, int(self.window_end - self.b.now())) if self.window_end else 0
        return {"enabled": True, "advertising": self.advertising, "connected": self.connected,
                "active": self.gate, "trigger": True, "running": True,
                "pair_request": self.pair_request, "wireless_off": self.wireless_off,
                "form": self.form if self.advertising else FORM_NONE, "slot": self.slot,
                "identity": self.identity, "bonded": self.bonded, "identities": 5,
                "security": self.security, "last_reason": self.last_reason,
                "link_state": self.link_state(), "reason": self.b.reason_link,
                "mtu": self.mtu, "interval_us": INTERVAL_US if self.connected else 0,
                "window_left_ms": left, "reports": self.reports, "dropped": self.dropped}

    def shell_status(self):
        """For `opensc2 ble status` (shellfmt.ble_status_lines)."""
        st = self.status()
        st.update(running=True, address=":".join(SIM_BLE_ADDRESS[i:i + 2]
                                                  for i in range(0, 12, 2)) + " (random)",
                  name=C.BLE_NAME_PREFIX + self.b.world.unit_serial,
                  bonded=[bool(self.bonded & (1 << i)) for i in (1, 2, 3, 4)],
                  window_left_s=st["window_left_ms"] // 1000,
                  idle="idle" if not self.wireless_off else
                  ("window" if self.b.reason_link == REASON_LISTEN_TIMEOUT else "host"))
        return st

    # ---- ring entries ----
    def note(self, event, a=0, b=0, c=0):
        self.b.hooks.note(C.RING_BLE, 4, bytes([event, a & 0xFF]) + struct.pack("<HI", b & 0xFFFF,
                                                                                c & 0xFFFFFFFF))

    def set_state(self, reason):
        self.b.reason_link = reason
        self.b.link_state = self.link_state()
        self.note(C.BLE_EV_STATE, self.b.link_state, reason,
                  self.slot | (self.identity << 8) | ((self.form & 0xFF) << 16))

    # ---- advertising ----
    def start(self, pair=False):
        self.want = True
        self.wireless_off = False
        if pair:
            self.pair_request = True
        if self.connected:
            if not pair:
                return
            self.end_link()
        self.adv_start(0)

    def adv_start(self, how):
        """how: 0 a start (the identity is picked), 1 again after a lost link, 2 after a
        failed connection."""
        reset = False
        self.round += 1
        if how == 0:
            first = 1 + 2 * self.slot
            first_bonded = bool(self.bonded & (1 << first))
            second_bonded = bool(self.bonded & (1 << (first + 1)))
            usual = first if first_bonded or not second_bonded else first + 1
            if self.pair_request:
                self.identity = first + 1 if usual == first else first
                reset = True
                self.pair_request = False
                self.b.world.ble_bonded &= ~(1 << self.identity)
                self.note(C.BLE_EV_IDENTITY, 1, 0, self.identity)
            else:
                self.identity = usual
        bonded = bool(self.bonded & (1 << self.identity))
        self.form = FORM_PAIRING if not bonded else (FORM_ALLOW if how == 2 else FORM_DIRECTED)
        self.advertising = True
        self.note(C.BLE_EV_ADV, self.form, 0,
                  self.slot | (self.identity << 8) | (how << 16) | (int(reset) << 24))
        ms = self.window_once_ms or WINDOW_MS
        self.window_once_ms = 0
        self.window_end = self.b.now() + ms
        self.b.timers["ble_window"] = self.window_end
        self.set_state(REASON_NONE)
        if bonded and self.b.world.ble_host_on and self.identity == 1:
            # the Windows host bonded to identity 1 connects (directed or allow-listed)
            self.b.timers["ble_connect"] = self.b.now() + HOST_CONNECT_MS
        else:
            self.b.timers.pop("ble_connect", None)

    def adv_stop(self, why=3):
        if self.advertising:
            self.advertising = False
            self.note(C.BLE_EV_ADV_END, why, 0, self.identity)
        self.window_end = None
        self.b.timers.pop("ble_window", None)
        self.b.timers.pop("ble_connect", None)

    # ---- the link ----
    def connect(self):
        self.adv_stop(0)
        self.connected = True
        self.security, self.mtu = 2, 67
        self.reports = self.dropped = 0
        self.set_state(REASON_NONE)
        self.note(C.BLE_EV_SECURITY, 2, 0, 0)
        self.note(C.BLE_EV_MTU, 0, self.mtu, 0)
        self.gate = True
        self.note(C.BLE_EV_GATE, 1, self.mtu, INTERVAL_US)
        self.b.world.ble_seen_connected = True

    def link_lost(self, reason):
        """The link ended with `reason` (the host's, or BLE_DROP's)."""
        self.connected = self.gate = False
        self.last_reason = reason
        self.b.link_down()
        off = reason in (0x13, 0x15)
        self.note(C.BLE_EV_DISCONNECT, reason, 1 if off else 0, reason if not self.drop_reason
                  else 0x16)
        self.drop_reason = 0
        if off:
            self.go_off(1, reason)
            return
        self.set_state(REASON_LINK_LOST)
        self.b.timers["ble_readvertise"] = self.b.now() + READVERTISE_MS

    def end_link(self):
        """The board ends the link itself (stop, a slot change, a pairing request)."""
        if self.connected:
            self.connected = self.gate = False
            self.note(C.BLE_EV_DISCONNECT, 0x16, 2, 0x16)
            self.b.link_down()

    def go_off(self, cause, reason):
        self.want = False
        self.pair_request = False
        self.wireless_off = True
        self.b.timers.pop("ble_readvertise", None)
        self.adv_stop(2 if cause == 0 else 3)
        self.note(C.BLE_EV_OFF, cause, reason, 0)
        self.set_state(REASON_LISTEN_TIMEOUT if cause == 0 else REASON_HOST_DISCONNECTED)
        # The power request through a work item of its own (ble.c off_power_work): a host's
        # request still pending (a reboot, a factory reset) is not replaced.
        self.off_why = ("bluetooth: no host within the window" if cause == 0 else
                        "bluetooth: the host left")
        self.b.timers["ble_off_power"] = self.b.now()
        # The system state machine of the Bluetooth build answers the give-up with its drop: a
        # stop that keeps the stack, which finds Bluetooth off already and changes nothing, so the
        # flag and the reason stay (ble.c do_stop()).
        self.stop(release=False)

    def stop(self, release=True):
        already_off = (self.wireless_off and not self.connected and not self.advertising and
                       self.b.link_state == LINK_OFF)
        self.b.timers.pop("ble_readvertise", None)
        if already_off and not release:
            self.want = False
            self.pair_request = False
            return
        self.want = False
        self.pair_request = False
        self.wireless_off = False
        self.adv_stop(3)
        self.end_link()
        self.set_state(REASON_DISABLED)

    # ---- timers (SimBoard forwards them) ----
    def timer(self, name):
        if name == "ble_connect" and self.advertising and self.b.world.ble_host_on:
            self.connect()
        elif name == "ble_window" and self.advertising and not self.connected and self.want:
            self.advertising = False
            self.note(C.BLE_EV_ADV_END, 2, 0, self.identity)
            self.go_off(0, 0)
        elif name == "ble_readvertise" and self.want and not self.connected:
            self.adv_start(1)
        elif name == "ble_drop" and self.connected:
            self.link_lost(self.drop_reason)

    # ---- the test channel's ops ----
    def op(self, op, args):
        if op == C.OP_BLE_STATUS:
            return C.ST_OK, T.encode_ble_status(self.status())
        if op == C.OP_BLE_DROP:
            if args[0] == 0:
                return C.ST_BAD_ARG, b""
            if not self.connected:
                return C.ST_NO_LINK, b""
            self.drop_reason = args[0]
            self.b.timers["ble_drop"] = self.b.now() + 100
            return C.ST_OK, b""
        if op == C.OP_BLE_WINDOW:
            ms = struct.unpack_from("<I", args, 0)[0]
            if ms and not C.BLE_WINDOW_MIN_MS <= ms <= C.BLE_WINDOW_MAX_MS:
                return C.ST_BAD_ARG, b""
            self.window_once_ms = ms
            return C.ST_OK, b""
        return C.ST_BAD_OP, b""


def hid_nodes(world):
    """The Bluetooth HID collections Windows lists: the board's (while it is connected) and a real
    controller's (never the board's: another address and name)."""
    out = []
    for address, serial, present in ((SIM_BLE_ADDRESS, world.unit_serial,
                                      world.board_ble_connected()),
                                     (SIM_BLE_OTHER, "NOTOSC000SIM1", True)):
        if not present:
            continue
        svc = "{00001812-0000-1000-8000-00805F9B34FB}_DEV_VID&0228DE_PID&1303_REV&0100_" + address
        for col in ("COL01", "COL02", "COL03"):
            out.append({"kind": "hid",
                        "own_instance_id": "HID\\%s&%s\\9&SIM&0&0000" % (svc, col),
                        "instance_id": "BTHLEDEVICE\\%s\\8&SIM" % svc,
                        "interface_id": "\\\\?\\hid#%s&%s#9&sim#{guid}" % (svc.lower(),
                                                                           col.lower()),
                        "serial": None, "usb_product": None, "location": None,
                        "ble_address": address, "ble_names": [C.BLE_NAME_PREFIX + serial]})
    return out
