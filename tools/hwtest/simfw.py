# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# simfw.py: the simulator's model of one boot of the firmware (the library with the test hooks),
# for the offline dry run. It follows the library's own rules as docs/api.md and
# docs/test-protocol.md state them: one stored request per link, a STALL on USB for a refused SET
# or a GET without an answer, the test channel's claiming rule and held answers, the lease and the
# end of arming, the ring as bytes (2048 of them, oldest entries evicted), the counters, the
# settings and their side effects, input injection and sequences, the input rules (with the rule
# word of RULES and RULES_SET), lizard mode with its haptic events, the battery report, output
# reports, the rumble timeout and the Steam watchdog. The firmware emulator
# (fwemu.py) runs the real C code for the same purpose; the self-tests run the groups against
# both.
import struct

from . import config as C
from .proto import model as M
from .proto import reports as R
from .proto import testproto as T

ENOTSUP, EINVAL, ENOENT, EMSGSIZE, EBADMSG, E2BIG = -134, -22, -2, -122, -74, -7
KEY_FACTORY_RESET, KEY_BOOTLOADER = 0xEFC1A917, 0xECAABAC0
KEY_KEEP_LINK, KEY_TURN_OFF = 0xA427AF52, 0xABDF9710
LINKS = {"radio": 0, "usb": 1, "test": 2}
LINK_NONE = 3
NAMED = ["esb/bond", "esb/bond_2", "user/wireless_transport", "user/haptic_boot_level"]
HOST_SET_ALLOW = {0x81, 0x83, 0x85, 0x87, 0x89, 0x8E, 0x9F, 0xA1, 0xAE, 0xBE, 0xC5, 0xDB, 0xE9,
                  0xED, 0xF2}
OPS = {   # op: (min args, max args, runs unarmed)
    C.OP_ARM: (7, 7, True), C.OP_DISARM: (0, 0, False), C.OP_STATUS: (0, 0, True),
    C.OP_INJECT: (53, 53, False), C.OP_INJECT_OFF: (0, 0, False),
    C.OP_BATTERY: (16, 16, False), C.OP_BATTERY_OFF: (0, 0, False),
    C.OP_SEQ_LOAD: (54, 54, False), C.OP_SEQ_RUN: (2, 2, False), C.OP_SEQ_STOP: (0, 0, False),
    C.OP_RULES: (0, 0, False), C.OP_RULES_SET: (4, 4, False),
    C.OP_RING_READ: (5, 5, False), C.OP_RING_CLEAR: (0, 0, False),
    C.OP_CNT_READ: (2, 2, False), C.OP_CNT_RESET: (0, 0, False), C.OP_MARK: (4, 4, False),
    C.OP_SETTINGS_READ: (2, 2, False), C.OP_HOST_OUT: (1, 54, False),
    C.OP_HOST_SET: (2, 54, False), C.OP_HOST_GET: (0, 0, False),
    C.OP_APP_SEND: (1, 54, False), C.OP_RADIO_DEAF: (2, 2, False),
}


class Identity:
    def __init__(self, world):
        self.unit = world.unit_serial
        self.board = world.board_serial
        self.git = world.git
        self.pid = 0x1302
        self.rev = 0x48                       # the library's default hardware ID
        self.build = 0x6A628359
        self.blbuild = 0x68D2F9F2
        self.name = C.DEFAULT_PRODUCT_NAME    # the USB product name (`opensc2 id set name`)

    def as_dict(self):
        return {"unit": self.unit, "board": self.board, "git": self.git, "pid": self.pid,
                "rev": self.rev, "build": self.build, "blbuild": self.blbuild,
                "name": self.name}


class SimBoard:
    """One boot. `world` holds what survives a reboot (the bonds, the identity, the chime, the
    policy, the session log) and the clock."""

    def __init__(self, world, mode, reason):
        self.world = world
        self.fault = world.fault
        self.mode = mode                      # "W" or "R"
        self.reason = reason                  # enum opensc2_mode_reason
        self.boot_ms = world.now_ms()
        self.settings = M.default_settings()
        if self.fault == "not_as_found" and world.boots >= 1:
            world.chime = 3                   # stored state that changed: nothing restores it
        self.led = {"red": 0, "green": 0, "blue": 0, "white": 0, "override": 0, "brightness": 50}
        self.battery = dict(R.BATTERY_DEFAULT_FIELDS)
        self.stored = {k: (0x83, b"") for k in LINKS}
        self.sequence = 0
        self.rules = M.RULES_DEFAULT          # the blank canvas's rule word
        self.state_rules = M.StateRules(self.rules)
        self.lizard = M.LizardModel(self.rules)   # on at boot (setting 9 = 1)
        self.set_rules(self.rules)
        self.slot = world.slot
        # wireless: connected through the puck at once, or listening when there is no puck
        self.link_state = (4 if world.puck_present else 3) if mode == "R" else 0
        self.last_reason = 0
        self.usb = {"requests": 0, "stalls": 0, "outputs": 0, "reports": 0, "dropped": 0}
        self.log = []                          # the wired session's log lines (text only)
        self.queue = []                        # reports the application sent, for the stream
        self.timers = {}                       # name -> due ms
        self.rumble_active = False
        self.power_pending = None
        self.deaf_until = 0
        self.dead = False
        self.reboot_to = None                  # set when a power request reboots this boot
        self.hooks = Hooks(self)
        self.timers["battery"] = self.now() + C.BATTERY_INTERVAL_MS
        self.timers["watchdog"] = self.now() + C.WATCHDOG_MS
        if mode == "W":
            self.log.append("configured: state reports every 132 ticks (4.03 ms)")

    # ---- time ----
    def now(self):
        return self.world.now_ms()

    def uptime(self):
        return self.now() - self.boot_ms

    def run_timers(self):
        """Everything due at the world's time, in order, each run at its own due time (the
        system work queue)."""
        target = self.world.t_ms
        try:
            for _ in range(256):
                if self.dead:
                    return
                due = [(t, n) for n, t in self.timers.items() if t is not None and t <= target]
                if not due:
                    return
                t, name = min(due)
                del self.timers[name]
                self.world.t_ms = float(t)
                getattr(self, "_timer_" + name)(t)
        finally:
            self.world.t_ms = target

    def _timer_battery(self, t):
        self.timers["battery"] = t + C.BATTERY_INTERVAL_MS
        self.send_battery()

    def _timer_watchdog(self, t):
        if self.settings[9] != 0:
            return
        self.lizard.switches(True)
        self.hooks.note_lizard(C.LIZARD_SWITCHES_UP, LINK_NONE)
        keep = {n: self.settings[n] for n in (48, 84, 85)}
        self.settings = M.default_settings()
        for n, v in keep.items():
            self.settings[n] = v
        self.led["override"] = 0
        self.led["brightness"] = self.settings[45]
        self.lizard_set(True, LINK_NONE)
        self.hooks.note_setting(0xFF, 0, LINK_NONE)
        self.notify_led(LINK_NONE)

    def _timer_rumble(self, t):
        if self.rumble_active:
            self.rumble_off()

    def _timer_power(self, t):
        request, why = self.power_pending
        self.power_pending = None
        self.hooks.note(C.RING_POWER, LINK_NONE,
                        bytes([request]) + why.encode("ascii")[:32])
        if request == 1:      # turn off: the blank canvas stops the radio
            if self.mode == "R" and self.link_state == 4:
                self.world.radio_stop(self)
        elif request in (2, 5):
            self.leave(why, "plain")
        elif request == 3:
            self.leave(why, "wireless")

    def leave(self, why, how):
        """opensc2_reboot(): a wired board sends one all-zero state report (sequence 0 too),
        then leaves the bus."""
        if self.mode == "W":
            self.queue.append((self.world.clock.perf_ns(), bytes([C.RPT_STATE]) + bytes(53)))
        self.end_session(why)
        self.reboot_to = how
        self.dead = True
        self.world.reboots_requested += 1

    def _timer_lease(self, t):
        self.hooks.poll()

    def _timer_completion(self, t):
        self.hooks.poll()

    def watchdog_arm(self):
        if self.settings[71] != 0:
            self.timers["watchdog"] = self.now() + C.WATCHDOG_MS

    def schedule_power(self, request, why, delay_ms=20):
        self.power_pending = (request, why)
        self.timers["power"] = self.now() + delay_ms

    def end_session(self, why):
        if self.mode == "W":
            self.world.last_session = (self.world.boots, self.usb["requests"], self.usb["stalls"],
                                       self.usb["outputs"], self.usb["reports"],
                                       self.usb["dropped"], why)
            self.world.session_log = list(self.log)

    # ---- the links ----
    def link_active(self, link):
        if link == "test":
            link = "usb" if self.mode == "W" else "radio"
        if link == "usb":
            return self.mode == "W"
        return self.mode == "R" and self.link_state == 4

    def input_send(self, report):
        """osc2_input_send(): the guard first, then the link of this mode."""
        if self.hooks.divert(report):
            return 0
        if self.mode == "R" and self.link_state != 4:
            return -107
        self.queue.append((self.world.clock.perf_ns(), bytes(report)))
        return 0

    def send_battery(self):
        b = dict(self.battery)
        self.hooks.battery(b)
        if self.fault == "stale_battery" and self.hooks.battery_on:
            b = dict(self.battery)
        rc = self.input_send(R.encode_battery(b))
        self.hooks.count("battery_sent" if rc == 0 else "battery_dropped")

    # ---- lizard mode ----
    def lizard_on(self):
        return self.settings[9] != 0

    def lizard_set(self, on, link):
        made = self.lizard.enter() if on else self.lizard.leave()
        if made:
            self.hooks.note_lizard(C.LIZARD_ENTER if on else C.LIZARD_LEAVE, link)
            self.hooks.count("lizard_made", 2)
        for _kind, rep in made:
            self.input_send(rep)

    def notify_led(self, link):
        self.hooks.note(C.RING_LED, link, bytes([self.led["red"], self.led["green"],
                                                 self.led["blue"], self.led["white"],
                                                 1 if self.led["override"] else 0,
                                                 self.led["brightness"]]))

    # ---- settings (commands.c) ----
    def write_setting(self, number, value, link):
        """One 0x87 triplet: (led touched)."""
        old = self.settings[number]
        v = M.setting_clamp(number, value)
        self.settings[number] = v
        led = False
        if number == 9:
            if v == 0:
                self.watchdog_arm()
                if old != 0:
                    self.lizard_set(False, link)
            elif old == 0:
                self.lizard_set(True, link)
        elif number == 71:
            if old != v:
                if v:
                    self.watchdog_arm()
                else:
                    self.timers.pop("watchdog", None)
        elif number in (37, 45):
            self.led["override"] = 1 if self.settings[37] != 0 else 0
            self.led["brightness"] = max(0, min(100, self.settings[45]))
            led = True
        if old != v:
            self.hooks.note_setting(number, v, link)
        return led

    def settings_write(self, number, value, link):
        if 0 <= number < 86 and self.write_setting(number, value, link):
            self.notify_led(link)

    def led_color_write(self, rgbw, link):
        for k, v in zip(("red", "green", "blue", "white"), rgbw):
            self.led[k] = min(v, 100)
        self.notify_led(link)

    def load_defaults(self, link):
        old9, old71 = self.settings[9], self.settings[71]
        keep = {n: self.settings[n] for n in (48, 84, 85)}
        self.settings = M.default_settings()
        for n, v in keep.items():
            self.settings[n] = v
        self.led["override"] = 0
        self.led["brightness"] = self.settings[45]
        if old71 == 0 and self.settings[71] != 0:
            self.watchdog_arm()
        if old9 == 0 and self.settings[9] != 0:
            self.lizard_set(True, link)
        self.hooks.note_setting(0xFF, 0, link)
        self.notify_led(link)

    def lizard_off(self, link):
        if not self.link_active(link):
            return EINVAL
        old = self.settings[9]
        self.settings[9] = 0
        if old != 0:
            self.lizard_set(False, link)
        else:
            self.lizard.switches(False)
            self.hooks.note_lizard(C.LIZARD_SWITCHES_DOWN, link)
        self.watchdog_arm()
        if old != 0:
            self.hooks.note_setting(9, 0, link)
        return 0

    # ---- the command channel (commands.c) ----
    def _named(self, payload):
        nul = payload.find(b"\0")
        if nul < 0:
            return None, None
        name = payload[:nul].decode("ascii", "replace")
        return (NAMED.index(name) if name in NAMED else -1), payload[nul + 1:]

    def run_set(self, link, opcode, payload, avail):
        if opcode in (0x83, 0xAE, 0xF2, 0xA1, 0xBE, 0xE9, 0xDB, 0x89, 0xED):
            return 0
        if opcode == 0x87:
            if len(payload) % 3:
                return EINVAL
            led = False
            for i in range(0, len(payload), 3):
                num = payload[i]
                if num >= 86:
                    continue
                led |= self.write_setting(num, struct.unpack_from("<h", payload, i + 1)[0], link)
            if led:
                self.notify_led(link)
            return 0
        if opcode == 0x8E:
            self.load_defaults(link)
            return 0
        if opcode == 0x81:
            return self.lizard_off(link)
        if opcode == 0x85:
            if not self.link_active(link):
                return EINVAL
            self.lizard.switches(True)
            self.hooks.note_lizard(C.LIZARD_SWITCHES_UP, link)
            return 0
        if opcode == 0xC5:
            self.led_color_write((list(payload) + [0, 0, 0, 0])[:4], link)
            return 0
        if opcode == 0xDC:
            if len(payload) >= 2 and payload[0] == 1:
                self.world.chime = payload[1]
                return 0
            return EINVAL
        if opcode == 0xE2:
            if payload and payload[0] > 1:
                return EINVAL
            return 0
        if opcode in (0xEE, 0xEF, 0xF0):
            key, value = self._named(payload)
            if key is None or key < 0:
                return ENOENT   # the blank canvas's vendor_command knows no other key
            if opcode == 0xEE:
                return self._key_set(key, value)
            if len(value) != 0:
                return EINVAL
            if opcode == 0xEF:
                if key in (0, 1):
                    return 0 if self.world.bonds[key] else ENOENT
                return 0
            if key in (0, 1):
                self.world.bonds[key] = b""
            return 0
        if opcode == 0xA2:
            if len(payload) != 24:
                return EINVAL
            self.world.bonds[0] = bytes(payload) if any(payload[:8]) else b""
            return 0
        if opcode == 0x86:
            return 0          # (the runner never sends it)
        if opcode == 0x90:
            return 0
        if opcode == 0x95:
            k = int.from_bytes((bytes(avail[:4]) + bytes(4))[:4], "little")
            if k == KEY_BOOTLOADER:
                pass
            elif k == KEY_KEEP_LINK:
                self.schedule_power(3, "host: reboot into wireless")
            elif k == KEY_TURN_OFF:
                if self.mode == "R":
                    self.schedule_power(1, "host: turn off")
            else:
                self.schedule_power(2, "host: reboot")
            return 0
        if opcode == 0x9F:
            if self.mode == "R":
                self.schedule_power(1, "host: turn off")
            return 0
        if opcode == C.TEST_OPCODE and self.world.hooks_built and self.hooks.claim(link, payload):
            return 0
        if opcode == 0xC1 and self.fault == "stall_missing":
            return 0
        return ENOTSUP          # the blank canvas's vendor_command refuses the rest

    def _key_set(self, key, value):
        if key in (0, 1):
            if len(value) != 24:
                return EINVAL
            self.world.bonds[key] = bytes(value) if any(value[:8]) else b""
            return 0
        if len(value) != 1:
            return EINVAL
        if key == 2:
            if value[0] in (2, 3):
                self.slot = value[0] - 2
            return 0
        self.world.chime = value[0]
        return 0

    def run_get(self, link, opcode, payload):
        """(rc, answer bytes)."""
        sel = payload[0] if payload else 0
        ident = self.world.identity
        if opcode == 0x83:
            out = b""
            for tag, val in ((1, ident.pid), (2, 0), (10, ident.blbuild), (4, ident.build),
                             (9, ident.rev), (11, 4000)):
                out += bytes([tag]) + struct.pack("<I", val)
            return 0, out
        if opcode == 0xAE:
            text = {0: ident.board, 4: ident.board, 1: ident.unit, 3: ident.git}.get(sel, "NA")
            out = bytearray(20)
            out[0] = sel
            t = text.encode("ascii")[:19]
            out[1:1 + len(t)] = t
            return 0, bytes(out)
        if opcode == 0xF2:
            if sel == 0:
                out = bytearray(41)
                struct.pack_into("<BII", out, 0, 0, ident.build, ident.rev)
                g = ident.git.encode("ascii")
                out[9:9 + len(g)] = g
                out[25:25 + 13] = ident.unit.encode("ascii")
                return 0, bytes(out)
            if sel == 1:
                return 0, bytes([1]) + bytes(33)
            if sel == 2:
                return 0, struct.pack("<BII", 2, self.uptime() & 0xFFFFFFFF, self.last_reason)
            return 0, b""
        if opcode == 0xA1:
            if sel == 1:
                return 0, struct.pack("<BI", 1, self.world.reset_cause) + self.world.chip_id + \
                    bytes(8)
            return 0, b""
        if opcode == 0xBE:
            b = dict(self.battery)
            self.hooks.battery(b)
            return 0, R.encode_be(b)
        if opcode == 0xE9:
            return 0, bytes([self.led["red"], self.led["green"], self.led["blue"],
                             self.led["white"]])
        if opcode == 0xDB:
            return 0, bytes([1, self.world.chime])
        if opcode == 0x89:
            if sel >= 86:
                return EINVAL, b""
            return 0, bytes([sel]) + struct.pack("<h", self.settings[sel])
        if opcode == 0xED:
            key, _value = self._named(payload)
            if key is not None and key in (0, 1):
                return 0, self.world.bonds[key]
            if key == 2:
                return 0, bytes([2 + self.slot])
            if key == 3:
                return 0, bytes([self.world.chime])
            return 0, b""
        if opcode in (0x81, 0x85, 0x86, 0x87, 0x8E, 0x90, 0x95, 0x9F, 0xA2, 0xC5, 0xDC, 0xE2,
                      0xEE, 0xEF, 0xF0):
            return ENOTSUP, b""
        if opcode == C.TEST_OPCODE:
            held = self.hooks.answer(link)
            if held is not None:
                return 0, held
        return ENOTSUP, b""

    def command_set(self, link, rec, avail=None):
        """osc2_command_set(): {opcode, n, payload}; `avail` the bytes after n that exist."""
        if len(rec) < 2:
            return EINVAL
        opcode, n = rec[0], rec[1]
        payload = bytes(rec[2:2 + n])
        avail = bytes(rec[2:]) if avail is None else avail
        if n > len(rec) - 2 or n > 61:
            self.hooks.note_vendor(link, False, opcode, n, EINVAL, payload, avail)
            return EINVAL
        self.hooks.ctx = link
        rc = self.run_set(link, opcode, payload, avail)
        if rc == 0:
            self.stored[link] = (opcode, payload)
            self.hooks.stored(link)
        if opcode != C.TEST_OPCODE:
            self.hooks.count("vendor_set_ok" if rc == 0 else "vendor_set_err")
            self.hooks.note_vendor(link, False, opcode, n, rc, payload, avail)
        self.hooks.ctx = None
        return rc

    def command_get(self, link):
        """osc2_command_get(): (rc, opcode, data)."""
        opcode, payload = self.stored[link]
        self.hooks.ctx = link
        rc, data = self.run_get(link, opcode, payload)
        if opcode != C.TEST_OPCODE:
            self.hooks.count("vendor_get_ok" if rc == 0 else "vendor_get_err")
            self.hooks.note_vendor(link, True, opcode, 0 if rc else len(data), rc, payload,
                                   payload)
        self.hooks.ctx = None
        return rc, opcode, data

    # ---- USB: feature report 1 ----
    def usb_feature_set(self, buf):
        self.usb["requests"] += 1
        if len(buf) != 64:
            rc = EMSGSIZE
        elif buf[0] not in (1, 2):
            rc = EBADMSG
        elif buf[2] > 61:
            rc = E2BIG
        else:
            rc = None
        if rc is not None:
            self.hooks.count("usb_frame_err")
        else:
            rc = self.command_set("usb", bytes(buf[1:64]))
        quiet = self.hooks.take_quiet("usb")
        if rc:
            self.usb["stalls"] += 1
        if not quiet:
            self.log.append("SET %02X n=%d %s" % (buf[1], buf[2], "ok" if rc == 0 else "STALL"))
        return rc

    def usb_feature_get(self):
        self.usb["requests"] += 1
        rc, opcode, data = self.command_get("usb")
        quiet = self.hooks.take_quiet("usb")
        if rc:
            self.usb["stalls"] += 1
            self.log.append("GET %02X STALL" % opcode)
            return rc, None
        if not quiet:
            self.log.append("GET %02X n=%d ok" % (opcode, len(data)))
        buf = bytearray(64)
        buf[0], buf[1], buf[2] = 1, opcode, min(len(data), 61)
        buf[3:3 + min(len(data), 61)] = data[:61]
        return 0, bytes(buf)

    # ---- output reports (report.c) ----
    def output(self, link, report):
        report = bytes(report)
        if self.mode == "W" and link == "usb":
            self.usb["outputs"] += 1
        n = decode_output_count(report)
        lk = LINKS[link]
        self.hooks.note(C.RING_OUTPUT, lk, bytes([report[0], min(len(report), 255), n & 0xFF]) +
                        report[1:17])
        self.hooks.count("out_reports")
        if n < 0:
            self.hooks.count("out_short")
            return n
        for kind, fields in decode_outputs(report):
            if kind == "rumble":
                self.rumble_active = bool(fields["left"] or fields["right"])
                if self.rumble_active:
                    self.timers["rumble"] = self.now() + C.RUMBLE_TIMEOUT_MS
                else:
                    self.timers.pop("rumble", None)
                self.hooks.note(C.RING_RUMBLE, lk, T._encode_entry_data(
                    dict(fields, kind=C.RING_RUMBLE)))
            else:
                self.hooks.note(C.RING_HAPTIC, lk, T._encode_entry_data(
                    dict(fields, kind=C.RING_HAPTIC)))
        return n

    def rumble_off(self):
        self.rumble_active = False
        self.hooks.note(C.RING_RUMBLE, LINK_NONE, struct.pack("<HHHbbBB", 0, 0, 0, 0, 0, 0, 1))

    def link_down(self):
        self.timers.pop("rumble", None)
        if self.rumble_active:
            self.rumble_off()

    def set_rules(self, rules):
        self.rules = rules
        self.state_rules.rules = rules
        self.lizard.rules = rules
        if self.fault == "glide_ignored":
            self.lizard.rules &= ~M.RULE_LIZARD_GLIDE   # lizard mode never glides

    def app_state(self):
        """The blank canvas on a bare board: all zero, but for the left stick's raw output
        (setting 46), which reads its biased pins at half of full scale."""
        mode = self.settings[46]
        if mode in (1, 2):
            v = 2048 if mode == 2 else 1650
            return R.make_state52(lstick_x=v, lstick_y=v)
        return R.make_state52()

    # ---- the frame path (report.c) ----
    def frame(self):
        """One state report built (and the lizard frame after it)."""
        s = self.app_state()
        injected, lizard_test = self.hooks.input(self.lizard_on())
        if injected is not None:
            s = injected
        # the pad_blank_ignored fault: setting 82 does not blank an injected pad in 0x42
        blank_off = self.fault == "pad_blank_ignored" and injected is not None
        self.state_rules.rules = self.rules & ~M.RULE_PAD_BLANK if blank_off else self.rules
        exp = self.state_rules.apply(s, self.settings)
        if self.fault == "trigger_bit" and injected is not None:
            exp = _trigger_off_by_one(s, exp, self.settings[68])
        report = bytes([C.RPT_STATE, self.sequence]) + exp
        self.sequence = (self.sequence + 1) & 0xFF
        self.hooks.frame(report)
        rc = self.input_send(report)
        if self.mode == "W":
            if rc == 0:
                self.usb["reports"] += 1
                self.hooks.count("usb_sent")
        elif rc == 0:
            self.hooks.count("radio_sent")
        # lizard mode: never on a frame injected without the lizard test
        if self.lizard_on() and not (injected is not None and not lizard_test):
            made = self.lizard.step_state52(exp, self.settings)
            for _kind, rep in made:
                self.hooks.count("lizard_made")
                self.input_send(rep)
            for kind, side in self.lizard.haptics:
                f = M.lizard_haptic_fields(kind, side)
                self.hooks.note(C.RING_HAPTIC, self.hooks._mode_link(),
                                bytes([f["haptic_kind"], 0, side, f["channels"], f["command"],
                                       f["gain"] & 0xFF]))
        return report


class Hooks:
    """test_core.c: arming, the lease, the guard, the held answers, the ring, the counters,
    injection and sequences, the ops."""

    def __init__(self, board):
        self.b = board
        self.armed = False
        self.guard = False
        self.completing = False
        self.nonce = 0
        self.lease_s = C.LEASE_DEFAULT_S
        self.arm_ms = 0
        self.frame_index = 0
        self.last_seq = 0
        self.static = None
        self.static_lizard = False
        self.battery_on = False
        self.battery_fields = None
        self.steps = {}
        self.seq = None               # running: {count, flags, step, left, started}
        self.snapshot = None
        self.held = {}
        self.just_claimed = set()
        self.quiet = set()
        self.ring = []                # [(seq, entry bytes)]
        self.ring_first = 0
        self.ring_next = 0
        self.ring_size = 2048
        self.counters = {n: 0 for n in C.COUNTER_NAMES}
        self.deaf_ms = 0
        self.ctx = None               # the link of the command being run (a name), or None
        self.rules_changed = False    # RULES_SET: the build's rule word comes back at the end
        self.skip_lizard = False
        self.frame_seq = None
        self.gen = 0
        self.lease_ms = 0
        self.completion_ms = 0

    # ---- counters and the ring ----
    def count(self, name, n=1):
        if self.armed:
            self.counters[name] += n

    def note(self, kind, link, data):
        """`link`: a number, a link name, or None for the link of the command being run (none
        outside one: the work queue)."""
        if not self.armed:
            return
        if link is None:
            link = LINKS.get(self.ctx, LINK_NONE) if self.ctx else LINK_NONE
        elif isinstance(link, str):
            link = LINKS[link]
        t = ((self.b.now() - self.arm_ms) * 1000) & 0xFFFFFFFF
        data = bytes(data)[:41]
        entry = struct.pack("<IBBB", t, kind, link, len(data)) + data
        while sum(len(e) for _s, e in self.ring) + len(entry) > self.ring_size:
            self.ring.pop(0)
            self.ring_first += 1
            self.counters["ring_evicted"] += 1
        self.ring.append((self.ring_next, entry))
        self.ring_next += 1
        self.counters["ring_entries"] += 1

    def note_setting(self, number, value, link=None):
        self.note(C.RING_SETTING, link, bytes([number]) + struct.pack("<h", value))

    def note_lizard(self, what, link=None):
        self.note(C.RING_LIZARD, link, bytes([what]))

    def note_vendor(self, link, get, opcode, n, rc, payload, avail):
        if opcode == C.TEST_OPCODE:
            return
        self.note(C.RING_VENDOR, LINKS[link], bytes([1 if get else 0, opcode, n & 0xFF]) +
                  struct.pack("<i", rc) + safe_payload(opcode, payload, avail))

    def _ring_reset(self, first):
        self.ring = []
        self.ring_first = self.ring_next = first

    # ---- the transports ----
    def claim(self, link, payload):
        if not T.claims(payload, self.armed):
            return False
        answer = self.run(LINKS[link], payload, console=False)
        self.held[link] = answer
        self.just_claimed.add(link)
        self.quiet.add(link)
        self._refresh()
        if self.armed:
            self.counters["test_requests"] += 1
        return True

    def answer(self, link):
        if link in self.held:
            self.quiet.add(link)
            return self.held[link]
        return None

    def stored(self, link):
        if link in self.just_claimed:
            self.just_claimed.discard(link)
        else:
            self.held.pop(link, None)

    def take_quiet(self, link):
        q = link in self.quiet
        self.quiet.discard(link)
        return q

    def raw(self, payload):
        """The console's `raw`: None for a request that fails the key or version check."""
        if not T.is_request(payload):
            return None
        self.ctx = "test"             # everything the console causes is the test link's
        try:
            answer = self.run(2, payload, console=True)
        finally:
            self.ctx = None
        if self.armed:
            self.counters["test_requests"] += 1
        self._refresh()               # a console command that ends with END 0, while armed
        return answer

    def _refresh(self):
        if self.armed:
            self.lease_ms = self.b.now() + self.lease_s * 1000
            self.b.timers["lease"] = self.lease_ms

    # ---- the lease and the end of arming ----
    def poll(self):
        now = self.b.now()
        if self.armed and self.b.world.fault != "lease_ignored" and self.lease_ms <= now:
            self.end_arming()
        if self.completing and not self.armed and self.completion_ms <= now:
            self.b.timers.pop("completion", None)
            self.b.lizard.resync()
            self.note_lizard(C.LIZARD_RESYNC, LINK_NONE)
            self.completing = False
            self.guard = False

    def end_arming(self):
        if not self.armed:
            return
        self.armed = False
        self.static = None
        self.battery_on = False
        self.seq = None
        self.steps = {}
        self.deaf_ms = 0
        self.held = {}
        self.just_claimed = set()
        snap, self.snapshot = self.snapshot, None
        self._ring_reset(0)
        self.counters = {n: 0 for n in C.COUNTER_NAMES}
        self.completing = True
        self.b.timers.pop("lease", None)
        if self.rules_changed:
            self.rules_changed = False
            self.b.set_rules(M.RULES_DEFAULT)
        if snap:
            settings, led = snap
            for n in range(86):
                if n in (9, 71):
                    continue
                if self.b.settings[n] != settings[n]:
                    self.b.settings_write(n, settings[n], None)
            for n in (71, 9):
                if self.b.settings[n] != settings[n]:
                    self.b.settings_write(n, settings[n], None)
            now_led = [self.b.led[k] for k in ("red", "green", "blue", "white")]
            if now_led != led:
                self.b.led_color_write(led, None)
        self.completion_ms = self.b.now() + 50
        self.b.timers["completion"] = self.completion_ms

    # ---- frames ----
    def input(self, lizard_on):
        """(the injected state52 or None, the lizard-test flag)."""
        self.skip_lizard = False
        if not self.armed:
            return None, False
        if lizard_on and self.b.world.fault != "lizard_ignored":
            what = 0
            if self.seq and not (self.seq["flags"] & C.SEQ_FLAG_LIZARD):
                self._seq_end(3)
                what |= 2
            if self.static is not None and not self.static_lizard:
                self.static = None
                what |= 1
            if what:
                self.note(C.RING_INJECT_STOP, self._mode_link(),
                          bytes([1]) + struct.pack("<I", self.frame_index) + bytes([what]))
        self.frame_seq = None
        if self.seq:
            st = self.steps[self.seq["step"]][1]
            self.frame_seq = (self.seq["gen"], self.seq["step"])
            lizard_test = bool(self.seq["flags"] & C.SEQ_FLAG_LIZARD)
            self.skip_lizard = not lizard_test
            return st, lizard_test
        if self.static is not None:
            self.skip_lizard = not self.static_lizard
            return self.static, self.static_lizard
        return None, False

    def frame(self, report):
        if not self.armed:
            return
        idx = self.frame_index
        self.frame_index += 1
        self.last_seq = report[1]
        self.counters["frames"] += 1
        s = self.seq
        if s and getattr(self, "frame_seq", None) == (s["gen"], s["step"]):
            if not s["started"]:
                s["started"] = True
                self.note(C.RING_SEQ_STEP, self._mode_link(),
                          bytes([s["step"]]) + struct.pack("<I", idx) + bytes([report[1]]))
                self.counters["seq_steps"] += 1
            s["left"] -= 1
            if s["left"] <= 0:
                nxt = s["step"] + 1
                if nxt < s["count"] or s["flags"] & C.SEQ_FLAG_LOOP:
                    s["step"] = nxt if nxt < s["count"] else 0
                    s["left"] = self.steps[s["step"]][0]
                    s["started"] = False
                elif s["flags"] & C.SEQ_FLAG_KEEP_LAST:
                    self.static = self.steps[s["step"]][1]
                    self.static_lizard = bool(s["flags"] & C.SEQ_FLAG_LIZARD)
                    self._seq_end(1)
                else:
                    self._seq_end(0)
        self.frame_seq = None

    def _seq_end(self, how):
        self.seq = None
        self.note(C.RING_SEQ_END, self._mode_link(), struct.pack("<I", self.frame_index) +
                  bytes([how]))

    def _mode_link(self):
        return 1 if self.b.mode == "W" else 0

    def battery(self, b):
        if self.armed and self.battery_on:
            b.update(self.battery_fields)

    def divert(self, report):
        if not self.guard or not report or report[0] not in (C.RPT_MOUSE, C.RPT_KEYBOARD):
            return False
        if self.b.world.fault == "guard_leak":
            return False
        if self.armed:
            frame = self.frame_index - 1 if self.frame_index else 0xFFFFFFFF
            self.note(C.RING_LIZARD_RPT, self._mode_link(),
                      bytes([1]) + struct.pack("<I", frame) + bytes(report)[:36])
            self.counters["lizard_diverted"] += 1
        return True

    # ---- the ops ----
    def run(self, link, req, console):
        op, args = req[6], req[7:]
        status, result = self._dispatch(link, op, args, console)
        if status != C.ST_OK:
            result = b""
        return bytes([C.TEST_VERSION, req[5], op, status]) + result

    def _dispatch(self, link, op, args, console):
        spec = OPS.get(op)
        if spec is None:
            return C.ST_BAD_OP, b""
        lo, hi, unarmed = spec
        if not lo <= len(args) <= hi or not _length_ok(op, args):
            return C.ST_BAD_LEN, b""
        if console and not unarmed and not self.armed:
            return C.ST_NOT_ARMED, b""
        return getattr(self, "_op_%02x" % op)(link, args)

    def _op_01(self, link, a):            # ARM
        nonce, lease, flags = struct.unpack_from("<IHB", a, 0)
        if lease > 600 or flags & ~C.ARM_FLAG_RESTORE:
            return C.ST_BAD_ARG, b""
        lease = lease or C.LEASE_DEFAULT_S
        fresh = not self.armed and not self.guard
        want = bool(flags & C.ARM_FLAG_RESTORE) and self.snapshot is None
        if fresh:
            made = []
            if self.b.lizard.active and (self.b.lizard.mapped or self.b.lizard.mouse_owed):
                made = [M._keyboard_report([]), M._mouse_report(0, 0, 0, 0, 0)]
                self.b.lizard.mapped = 0
                self.b.lizard.mouse_owed = False
            for rep in made:
                self.b.input_send(rep)
        if want:
            self.snapshot = (list(self.b.settings),
                             [self.b.led[k] for k in ("red", "green", "blue", "white")])
        if self.rules_changed:
            self.rules_changed = False
            self.b.set_rules(M.RULES_DEFAULT)
        self.completing = False
        self.b.timers.pop("completion", None)
        self._ring_reset(0)
        self.static = None
        self.battery_on = False
        self.seq = None
        self.steps = {}
        self.nonce = nonce
        self.lease_s = lease
        self.arm_ms = self.b.now()
        self.frame_index = 0
        self.guard = True
        self.armed = True
        self._refresh()
        self.counters = {n: 0 for n in C.COUNTER_NAMES}
        self.note(C.RING_ARMED, link, struct.pack("<I", nonce))
        self.b.lizard.resync()
        self.note_lizard(C.LIZARD_RESYNC, link)
        return C.ST_OK, T.encode_caps({
            "version": 1, "mode": 1 if self.b.mode == "W" else 0,
            "build_flags": (1 << C.CAP_TRIGGER_CLICK_AUTO) | (1 << C.CAP_LIZARD_MODE) |
            (1 << C.CAP_STEAM_WATCHDOG) | (1 << C.CAP_USB) | (1 << C.CAP_USB_CONSOLE) |
            (1 << C.CAP_STORAGE) | (1 << C.CAP_USB_LOG) | (1 << C.CAP_USB_PLUG_IN_REBOOT) |
            (1 << C.CAP_SHELL) | (1 << C.CAP_RULES) | self.b.world.extra_build_flags,
            "battery_interval_ms": C.BATTERY_INTERVAL_MS, "rumble_timeout_ms": C.RUMBLE_TIMEOUT_MS,
            "ring_size": self.ring_size, "sequence_depth": 8, "counter_count": C.COUNTER_COUNT,
            "radio_report_delay_us": 2000, "lease_s": lease, "policy": self.b.world.policy,
            "git_revision": self.b.world.git, "product_name": self.b.world.identity.name})

    def _op_02(self, link, a):            # DISARM
        self.end_arming()
        return C.ST_OK, b""

    def _op_03(self, link, a):            # STATUS
        flags = ((1 if self.armed else 0) | (2 if self.guard else 0) |
                 (4 if self.static is not None else 0) | (8 if self.battery_on else 0) |
                 (16 if self.seq else 0) | (32 if self.b.lizard_on() else 0) |
                 (64 if self.completing else 0) | (128 if self.snapshot else 0))
        return C.ST_OK, T.encode_status({
            "flags": flags, "mode": 1 if self.b.mode == "W" else 0, "mode_reason": self.b.reason,
            "link_state": self.b.link_state, "usb_state": 3 if self.b.mode == "W" else 0,
            "slot": self.b.slot, "last_disconnect_reason": self.b.last_reason,
            "last_seq": self.last_seq, "nonce": self.nonce,
            "uptime_ms": self.b.uptime() & 0xFFFFFFFF, "ring_first": self.ring_first,
            "ring_next": self.ring_next, "frames": self.frame_index,
            "seq_step": self.seq["step"] if self.seq else 0xFF, "seq_loaded": len(self.steps),
            "seq_frames_left": self.seq["left"] if self.seq else 0})

    def _op_10(self, link, a):            # INJECT
        flags = a[0]
        if not flags & C.INJECT_FLAG_LIZARD and self.b.lizard_on() and \
                self.b.world.fault != "lizard_ignored":
            return C.ST_DENIED, b""
        self.static = bytes(a[1:53])
        self.static_lizard = bool(flags & C.INJECT_FLAG_LIZARD)
        self.b.world.injects += 1
        if flags & C.INJECT_FLAG_STOP_SEQ and self.seq:
            self._seq_end(2)
        return C.ST_OK, struct.pack("<I", self.frame_index)

    def _op_11(self, link, a):            # INJECT_OFF
        self.static = None
        if self.seq:
            self._seq_end(2)
        return C.ST_OK, struct.pack("<I", self.frame_index)

    def _op_12(self, link, a):            # BATTERY
        self.battery_fields = R.decode_battery_args(a)
        self.battery_on = True
        if a[15] & 1:
            self.b.send_battery()
            self.b.timers["battery"] = self.b.now() + C.BATTERY_INTERVAL_MS
        return C.ST_OK, b""

    def _op_13(self, link, a):            # BATTERY_OFF
        self.battery_on = False
        return C.ST_OK, b""

    def _op_14(self, link, a):            # SEQ_LOAD
        step, hold = a[0], a[1]
        if step >= 8 or hold == 0:
            return C.ST_BAD_ARG, b""
        if self.seq:
            return C.ST_BUSY, b""
        self.steps[step] = (hold, bytes(a[2:54]))
        return C.ST_OK, b""

    def _op_15(self, link, a):            # SEQ_RUN
        count, flags = a[0], a[1]
        if count == 0 or count > 8 or any(i not in self.steps for i in range(count)):
            return C.ST_BAD_ARG, b""
        if self.seq:
            return C.ST_BUSY, b""
        if not flags & C.SEQ_FLAG_LIZARD and self.b.lizard_on():
            return C.ST_DENIED, b""
        self.gen = getattr(self, "gen", 0) + 1
        self.seq = {"count": count, "flags": flags, "step": 0, "left": self.steps[0][0],
                    "started": False, "gen": self.gen}
        return C.ST_OK, struct.pack("<I", self.frame_index)

    def _op_16(self, link, a):            # SEQ_STOP
        if self.seq:
            self._seq_end(2)
        return C.ST_OK, b""

    def _op_20(self, link, a):            # RING_READ
        frm, mx = struct.unpack_from("<IB", a, 0)
        if frm > self.ring_next:
            return C.ST_BAD_ARG, b""
        start = max(frm, self.ring_first)
        out, n = b"", 0
        for seq, e in self.ring:
            if seq < start:
                continue
            if (mx and n >= mx) or len(out) + len(e) > 48:
                break
            out += e
            n += 1
        return C.ST_OK, struct.pack("<IIB", self.ring_first, self.ring_next, n) + out

    def _op_21(self, link, a):            # RING_CLEAR
        self._ring_reset(self.ring_next)
        return C.ST_OK, struct.pack("<I", self.ring_next)

    def _op_22(self, link, a):            # CNT_READ
        first, count = a[0], a[1]
        if count == 0 or count > 13:
            return C.ST_BAD_ARG, b""
        n = 0 if first >= C.COUNTER_COUNT else min(count, C.COUNTER_COUNT - first)
        vals = [self.counters[C.COUNTER_NAMES[first + i]] & 0xFFFFFFFF for i in range(n)]
        return C.ST_OK, bytes([first, n]) + b"".join(struct.pack("<I", v) for v in vals)

    def _op_23(self, link, a):            # CNT_RESET
        self.counters = {n: 0 for n in C.COUNTER_NAMES}
        return C.ST_OK, b""

    def _op_24(self, link, a):            # MARK
        seq = self.ring_next
        self.note(C.RING_MARK, link, bytes(a[:4]))
        return C.ST_OK, struct.pack("<I", seq)

    def _op_30(self, link, a):            # SETTINGS_READ
        first, count = a[0], a[1]
        if count == 0 or count > 27:
            return C.ST_BAD_ARG, b""
        n = 0 if first >= 86 else min(count, 86 - first)
        return C.ST_OK, bytes([first, n]) + b"".join(
            struct.pack("<h", self.b.settings[first + i]) for i in range(n))

    def _op_40(self, link, a):            # HOST_OUT
        return C.ST_OK, struct.pack("<b", self.b.output("test", a))

    def _op_41(self, link, a):            # HOST_SET
        opcode, n = a[0], a[1]
        payload = bytes(a[2:2 + n])
        if not host_set_allowed(opcode, payload):
            return C.ST_DENIED, b""
        rc = self.b.command_set("test", bytes(a[:2 + n]))
        return C.ST_OK, struct.pack("<i", rc)

    def _op_42(self, link, a):            # HOST_GET
        rc, opcode, data = self.b.command_get("test")
        n = 0 if rc else min(len(data), 61)
        sop, spayload = self.b.stored["test"]
        if n and _host_get_masked(sop, spayload):
            return C.ST_OK, bytes([opcode, n, 1]) + struct.pack("<I", T.crc32(data[:n]))
        return C.ST_OK, bytes([opcode, n, 2 if n > 54 else 0]) + data[:min(n, 54)]

    def _op_43(self, link, a):            # APP_SEND
        kind = a[0]
        if kind > 1:
            return C.ST_BAD_ARG, b""
        if kind == 0:
            if a[1] == C.RPT_STATE:
                return C.ST_DENIED, b""
            rc = self.b.input_send(a[1:])
        else:
            rc = self.b.input_send(bytes([C.RPT_HAPTIC_STATUS, a[1]]) + bytes(a[2:6]))
        if rc == 0:
            self.counters["app_reports_sent"] += 1
        return C.ST_OK, struct.pack("<i", rc)

    def _op_17(self, link, a):            # RULES
        return C.ST_OK, struct.pack("<III", self.b.rules, M.RULES_DEFAULT, M.RULES_KNOWN)

    def _op_18(self, link, a):            # RULES_SET
        rules = struct.unpack_from("<I", a, 0)[0]
        if rules & ~M.RULES_KNOWN:
            return C.ST_BAD_ARG, b""
        self.rules_changed = True
        self.b.set_rules(rules)
        self.b.lizard.resync()
        self.note_lizard(C.LIZARD_RESYNC, link)
        return C.ST_OK, struct.pack("<I", self.b.rules)

    def _op_44(self, link, a):            # RADIO_DEAF
        ms = struct.unpack_from("<H", a, 0)[0]
        if ms < 100 or ms > 5000:
            return C.ST_BAD_ARG, b""
        if self.deaf_ms:
            return C.ST_BUSY, b""
        if not (self.b.mode == "R" and self.b.link_state == 4):
            return C.ST_NO_LINK, b""
        self.deaf_ms = ms
        self.b.world.radio_deaf(self.b, ms)
        return C.ST_OK, b""


# ---- helpers -------------------------------------------------------------------------------------

def _length_ok(op, args):
    if op == C.OP_HOST_SET:
        return args[1] == len(args) - 2
    if op == C.OP_APP_SEND:
        if args[0] == 0:
            return len(args) >= 2
        if args[0] == 1:
            return len(args) == 6
    return True


def host_set_allowed(opcode, payload):
    if opcode in HOST_SET_ALLOW:
        return True
    if opcode == 0x95:
        return int.from_bytes((bytes(payload[:4]) + bytes(4))[:4], "little") == KEY_TURN_OFF
    return False


def _key_id(payload):
    nul = payload.find(b"\0")
    if nul < 0:
        return 0, 0
    name = payload[:nul].decode("ascii", "replace")
    names = list(C.KEY_NAMES.values())
    return (names.index(name) + 1 if name in names else 0), len(payload) - nul - 1


def _host_get_masked(opcode, payload):
    sel = payload[0] if payload else 0
    if opcode == 0xED:
        return _key_id(payload)[0] in (1, 2)
    if opcode == 0xAE:
        return sel in (0, 1, 4)
    if opcode == 0xF2:
        return sel == 0
    if opcode == 0xA1:
        return sel == 1
    return False


def safe_payload(opcode, payload, avail):
    if opcode == 0x87:
        k = min(len(payload), 12) // 3 * 3
        return bytes(payload[:k])
    if opcode in (0x89, 0xAE, 0xF2, 0xA1):
        return bytes(payload[:1])
    if opcode == 0xC5:
        return (bytes(payload) + bytes(4))[:4]
    if opcode in (0xDC, 0xE2):
        return (bytes(payload) + bytes(2))[:2]
    if opcode in (0x86, 0x95):
        return (bytes(avail) + bytes(4))[:4]
    if opcode in (0xED, 0xEE, 0xEF, 0xF0):
        kid, vlen = _key_id(bytes(payload))
        return bytes([kid, vlen & 0xFF])
    return b""


def decode_output_count(report):
    """opensc2_decode_output()'s result: outputs made, 0 for an ID that does nothing, -1 short."""
    rid = report[0]
    n = len(report) - 1
    mins = {0x80: 9, 0x81: 7, 0x82: 3, 0x83: 9, 0x84: 8, 0x85: 3, 0x86: 2, 0x87: 1}
    if rid in mins:
        return 1 if n >= mins[rid] else -1
    if rid == 0x88:
        if n < 1:
            return -1
        count = min(report[1], 31)
        return 2 if n >= 32 + count else -1
    if rid == 0x89:
        return 1 if n >= 2 and n >= 2 + report[1] else -1
    return 0


def _u16(d, o):
    return d[o] | (d[o + 1] << 8)


def _s8(v):
    return v - 256 if v & 0x80 else v


def decode_outputs(report):
    """[(kind, fields)] of an output report long enough for its ID (codec.c's decoder)."""
    rid, d = report[0], report[1:]
    if rid == 0x80:
        return [("rumble", {"left": _u16(d, 3), "right": _u16(d, 6), "intensity": _u16(d, 1),
                            "left_gain": _s8(d[5]), "right_gain": _s8(d[8]), "type": d[0],
                            "timeout": 0})]
    if rid in (0x81, 0x82, 0x83, 0x84, 0x85):
        kind = {0x81: 0, 0x82: 1, 0x83: 2, 0x84: 3, 0x85: 4}[rid]
        target = d[0]
        f = {"haptic_kind": kind, "report_id": rid, "target": target,
             "channels": M.target_channels(rid, target)}
        if kind == 0:
            f.update(on_us=_u16(d, 1), off_us=_u16(d, 3), count=_u16(d, 5))
        elif kind == 1:
            f.update(command=d[1], gain=_s8(d[2]))
        elif kind == 2:
            f.update(gain=_s8(d[1]), freq=_u16(d, 2), duration=_u16(d, 4), lfo=_u16(d, 6),
                     depth=d[8])
        elif kind == 3:
            f.update(gain=_s8(d[1]), duration=_u16(d, 2), start=_u16(d, 4), end=_u16(d, 6),
                     antiphase=1 if (target & 0x7F) in (6, 7) else 0)
        else:
            f.update(id=d[1], gain=_s8(d[2]))
        return [("haptic", f)]
    if rid == 0x86:
        fmt = d[2] if len(d) >= 3 else 0
        enc = fmt // 4 if fmt // 4 <= 2 else 3
        return [("haptic", {"haptic_kind": 5, "report_id": rid, "target": d[1],
                            "channels": M.target_channels(rid, d[1]), "operation": d[0],
                            "format": fmt, "encoding": enc, "rate": 8000 >> (fmt % 4)})]
    if rid == 0x87:
        data = bytes(d[1:])
        return [("haptic", {"haptic_kind": 6, "report_id": rid, "target": d[0],
                            "channels": M.target_channels(rid, d[0]), "len": len(data) & 0xFF,
                            "crc32": T.crc32(data)})]
    if rid == 0x89:
        n = d[0]
        data = bytes(d[2:2 + n])
        return [("haptic", {"haptic_kind": 6, "report_id": rid, "target": d[1],
                            "channels": M.target_channels(rid, d[1]), "len": n,
                            "crc32": T.crc32(data)})]
    if rid == 0x88:
        n = min(d[0], 31)
        return [("haptic", {"haptic_kind": 6, "report_id": rid, "target": 0,
                            "channels": M.CH_LGRIP, "len": n, "crc32": T.crc32(bytes(d[1:1 + n]))}),
                ("haptic", {"haptic_kind": 6, "report_id": rid, "target": 0,
                            "channels": M.CH_RGRIP, "len": n,
                            "crc32": T.crc32(bytes(d[32:32 + n]))})]
    return []


def _trigger_off_by_one(state52, exp, setting68):
    """The trigger_bit fault: the hysteresis' "on" threshold two counts too high."""
    f = R.decode_state52(state52)
    p = max(40, min(99, setting68))
    on = min(M.TRIGGER_MAX * (p + 5) // 100, M.TRIGGER_MAX - 1)
    b = R.decode_state52(exp)["buttons"]
    bit = 1 << R.BIT["LTRIGGER_DIGITAL"]
    v = min(f["left_trigger"], M.TRIGGER_MAX)
    if on < v <= on + 2:
        b &= ~bit
    return R.set_field(exp, "buttons", b)
