# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# model.py: the expected-report model, the lizard model, the target-channel table and
# the settings defaults and ranges. The firmware's own code (codec.c, report.c, lizard.c,
# settings_defaults.c) is the reference; these mirror it so the runner can predict every report and
# the FW's host tests can check the same against the library. Integer division truncates toward
# zero, as C does (Python's // is wrong for negatives), so a c_div() helper is used throughout.
from . import reports as R
from .. import config as C

AXIS_MAX = 32767
TRIGGER_MAX = 32767


def c_div(a, b):
    """a / b truncated toward zero, as C integer division does (b > 0)."""
    q = abs(a) // b
    return q if a >= 0 else -q


def clamp_axis(v):
    """Signed axis (stick, pad position): only -32768 becomes -32767 (codec clamp_axis)."""
    return -AXIS_MAX if v < -AXIS_MAX else v


def clamp_trigger(v):
    """Trigger and pad pressure: above 32767 becomes 32767 (codec clamp_trigger). v is unsigned."""
    return TRIGGER_MAX if v > TRIGGER_MAX else v


def trigger_click(value, setting68, previous):
    """The trigger's digital bit by hysteresis (report_rules.c, osc2_trigger_click):
    p = clamp(setting68, 40, 99); on = min(32767*(p+5)//100, 32766); off = 32767*(p-5)//100.
    value > on sets, value < off clears, else keeps `previous`."""
    p = 40 if setting68 < 40 else 99 if setting68 > 99 else setting68
    on = min(TRIGGER_MAX * (p + 5) // 100, TRIGGER_MAX - 1)
    off = TRIGGER_MAX * (p - 5) // 100
    if value > on:
        return True
    if value < off:
        return False
    return previous


def expected_report(state52, prev_left_bit, prev_right_bit, setting68=90, trigger_click_auto=True):
    """The expected 0x42 payload (bytes 2..53, 52 bytes) for an injected `state52`, given the
    previous frame's trigger bits. Returns (expected_bytes, new_left_bit, new_right_bit). The
    sequence byte is not predicted (checked for continuity only, 3.7)."""
    f = R.decode_state52(state52)
    buttons = f["buttons"]
    lt = clamp_trigger(f["left_trigger"])
    rt = clamp_trigger(f["right_trigger"])

    if trigger_click_auto:
        left_bit = trigger_click(lt, setting68, prev_left_bit)
        right_bit = trigger_click(rt, setting68, prev_right_bit)
        buttons = (buttons | (1 << R.BIT["LTRIGGER_DIGITAL"])) if left_bit else \
                  (buttons & ~(1 << R.BIT["LTRIGGER_DIGITAL"]))
        buttons = (buttons | (1 << R.BIT["RTRIGGER_DIGITAL"])) if right_bit else \
                  (buttons & ~(1 << R.BIT["RTRIGGER_DIGITAL"]))
    else:
        left_bit = bool(buttons & (1 << R.BIT["LTRIGGER_DIGITAL"]))
        right_bit = bool(buttons & (1 << R.BIT["RTRIGGER_DIGITAL"]))

    lpad_touched = bool(buttons & (1 << R.LPAD_TOUCH_BIT))
    rpad_touched = bool(buttons & (1 << R.RPAD_TOUCH_BIT))

    out = {
        "buttons": buttons & 0xFFFFFFFF,
        "left_trigger": lt, "right_trigger": rt,
        "lstick_x": clamp_axis(f["lstick_x"]), "lstick_y": clamp_axis(f["lstick_y"]),
        "rstick_x": clamp_axis(f["rstick_x"]), "rstick_y": clamp_axis(f["rstick_y"]),
        "lpad_x": clamp_axis(f["lpad_x"]) if lpad_touched else 0,
        "lpad_y": clamp_axis(f["lpad_y"]) if lpad_touched else 0,
        "lpad_pressure": clamp_trigger(f["lpad_pressure"]),
        "rpad_x": clamp_axis(f["rpad_x"]) if rpad_touched else 0,
        "rpad_y": clamp_axis(f["rpad_y"]) if rpad_touched else 0,
        "rpad_pressure": clamp_trigger(f["rpad_pressure"]),
        "imu_timestamp": f["imu_timestamp"],
        "accel_x": f["accel_x"], "accel_y": f["accel_y"], "accel_z": f["accel_z"],
        "gyro_x": f["gyro_x"], "gyro_y": f["gyro_y"], "gyro_z": f["gyro_z"],
        "quat_w": f["quat_w"], "quat_x": f["quat_x"], "quat_y": f["quat_y"], "quat_z": f["quat_z"],
    }
    return R.make_state52(**out), left_bit, right_bit


# ---- the lizard model (1.3, 4.2 lizard.txt) -------------------------------------------------

KEY_ENTER, KEY_ESCAPE, KEY_TAB = 0x28, 0x29, 0x2B
KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP = 0x4F, 0x50, 0x51, 0x52
KEY_ROLLOVER = 0x01
POINTER_DIVISOR = 128
SCROLL_DIVISOR = 1024

_KEY_MAP = {
    R.BIT["A"]: KEY_ENTER, R.BIT["B"]: KEY_ESCAPE, R.BIT["VIEW"]: KEY_ESCAPE,
    R.BIT["DPAD_DOWN"]: KEY_DOWN, R.BIT["DPAD_RIGHT"]: KEY_RIGHT, R.BIT["DPAD_LEFT"]: KEY_LEFT,
    R.BIT["DPAD_UP"]: KEY_UP, R.BIT["MENU"]: KEY_TAB,
}
_MAPPED = 0
for _b in _KEY_MAP:
    _MAPPED |= (1 << _b)

LIZ_KEYBOARD = 1
LIZ_MOUSE = 2
_CLICK_RPAD = 1 << R.BIT["RPAD_CLICK"]
_CLICK_RTRIG = 1 << R.BIT["RTRIGGER_DIGITAL"]
_CLICK_LTRIG = 1 << R.BIT["LTRIGGER_DIGITAL"]
_TOUCH = (1 << R.LPAD_TOUCH_BIT, 1 << R.RPAD_TOUCH_BIT)   # left, right
_STICK_TOUCH = (1 << R.BIT["LSTICK_TOUCH"], 1 << R.BIT["RSTICK_TOUCH"])


def _keyboard_report(keys):
    return bytes([C.RPT_KEYBOARD, 0, 0] + list(keys) + [0] * (6 - len(keys)))


def _mouse_report(buttons, x, y, wheel, pan):
    return bytes([C.RPT_MOUSE, buttons & 0xFF, x & 0xFF, y & 0xFF, wheel & 0xFF, pan & 0xFF])


class LizardModel:
    """One instance of lizard mode's logic (lizard.c). step()/enter()/leave()/switches()/resync()
    each return a list of (kind, report-bytes) in the exact send order of the firmware."""

    def __init__(self):
        self.reset()

    def reset(self):
        self.active = True
        self.keyboard_on = True
        self.buttons_on = True
        self.keyboard_sync = True
        self.mapped = 0
        self.pad_touched = [False, False]
        self.pad_x = [0, 0]
        self.pad_y = [0, 0]
        self.rem_x = self.rem_y = self.rem_pan = self.rem_wheel = 0
        self.mouse_owed = False

    def _clear_tracker(self):
        self.pad_touched = [False, False]
        self.pad_x = [0, 0]
        self.pad_y = [0, 0]
        self.rem_x = self.rem_y = self.rem_pan = self.rem_wheel = 0
        self.mouse_owed = False

    def enter(self):
        self._clear_tracker()
        self.active = True
        self.keyboard_on = self.buttons_on = True
        self.keyboard_sync = True
        # osc2_lizard_set(on): mouse first, then keyboard
        return [("mouse", _mouse_report(0, 0, 0, 0, 0)), ("keyboard", _keyboard_report([]))]

    def leave(self):
        was_active = self.active
        self.active = False
        self.keyboard_on = self.buttons_on = False
        self.mouse_owed = False
        if not was_active:
            return []
        # osc2_lizard_set(off): keyboard first, then mouse
        return [("keyboard", _keyboard_report([])), ("mouse", _mouse_report(0, 0, 0, 0, 0))]

    def switches(self, up):
        if up and not self.keyboard_on:
            self.keyboard_sync = True
        self.keyboard_on = self.buttons_on = up
        return []

    def resync(self):
        self._clear_tracker()
        self.keyboard_sync = True
        return []

    def step(self, buttons=0, lpad=(0, 0), rpad=(0, 0), s82=3,
             lpad_touched=None, rpad_touched=None, rpad_clicked=False):
        if not self.active:
            return []
        made = []
        pads = (lpad, rpad)
        touched_in = [
            lpad_touched if lpad_touched is not None else bool(buttons & _TOUCH[0]),
            rpad_touched if rpad_touched is not None else bool(buttons & _TOUCH[1]),
        ]

        # keyboard: only when a mapped button changed
        if self.keyboard_on:
            mapped = buttons & _MAPPED
            if self.keyboard_sync:
                self.mapped = mapped
                self.keyboard_sync = False
            elif mapped != self.mapped:
                self.mapped = mapped
                keys = []
                count = 0
                for bit in range(20):
                    if not (buttons & (1 << bit)):
                        continue
                    if count < 6:
                        keys.append(_KEY_MAP.get(bit, 0))
                    count += 1
                if count > 6:
                    keys = [KEY_ROLLOVER] * 6
                made.append(("keyboard", _keyboard_report(keys)))

        # mouse: motion of each pad since the last frame
        dx = [0, 0]
        dy = [0, 0]
        blanked = [False, False]
        for side in range(2):
            blanked[side] = bool(s82 & (1 if side == 0 else 2)) and bool(buttons & _STICK_TOUCH[side])
            touched = (not blanked[side]) and (touched_in[side] or bool(buttons & _TOUCH[side]))
            if touched and self.pad_touched[side]:
                dx[side] = pads[side][0] - self.pad_x[side]
                dy[side] = pads[side][1] - self.pad_y[side]
            self.pad_touched[side] = touched
            self.pad_x[side] = pads[side][0] if touched else 0
            self.pad_y[side] = pads[side][1] if touched else 0

        x = self._divide("rem_x", dx[1], POINTER_DIVISOR)
        y = -self._divide("rem_y", dy[1], POINTER_DIVISOR)
        pan = self._divide("rem_pan", dx[0], SCROLL_DIVISOR)
        wheel = -self._divide("rem_wheel", dy[0], SCROLL_DIVISOR)

        mbuttons = 0
        if self.buttons_on:
            right_click = (not blanked[1]) and (bool(buttons & _CLICK_RPAD) or rpad_clicked)
            if (buttons & _CLICK_RTRIG) or right_click:
                mbuttons |= 0x01
            if buttons & _CLICK_LTRIG:
                mbuttons |= 0x02

        if x or y or wheel or pan or mbuttons:
            made.append(("mouse", _mouse_report(mbuttons, x, y, wheel, pan)))
            self.mouse_owed = True
        elif self.mouse_owed:
            made.append(("mouse", _mouse_report(0, 0, 0, 0, 0)))
            self.mouse_owed = False
        return made

    def _divide(self, rem_name, delta, divisor):
        total = getattr(self, rem_name) + delta
        q = c_div(total, divisor)
        setattr(self, rem_name, total - q * divisor)
        return q


# ---- target channels ------------------------------------------------------------------------

CH_LPAD = 1
CH_RPAD = 2
CH_LGRIP = 4
CH_RGRIP = 8


def target_channels(report_id, target):
    """Channels (bit mask) a target byte selects for an output report (codec target_channels)."""
    lp, rp, lg, rg = CH_LPAD, CH_RPAD, CH_LGRIP, CH_RGRIP
    if report_id == 0x80:
        return lg | rg
    if report_id == 0x86:
        return {0: lg, 1: rg, 2: lg | rg, 3: lp, 4: rp, 5: lp | rp,
                0x40: rg, 0x80: lg | rg}.get(target, 0)
    if report_id in (0x87, 0x89):
        return {0: lg, 2: lg | rg, 3: lp, 4: rg, 5: lp | rp, 0x80: lg | rg}.get(target, 0)
    if report_id == 0x88:
        return lg | rg   # it becomes two entries; the caller splits them
    t = target & 0x7F
    if t == 0:
        return rp if report_id == 0x81 else lp
    if t == 1:
        return lp if report_id == 0x81 else rp
    if t in (2, 6):
        return lp | rp
    if t == 3:
        return lg
    if t == 4:
        return rg
    if t in (5, 7):
        return lg | rg
    return 0


# ---- settings defaults and ranges (4.2 settings.txt, settings_defaults.c) -------------------

SETTINGS_COUNT = 86
SETTINGS_ALL = 0xFF

_DEFAULTS = {
    2: 0, 9: 1, 11: 200, 12: 100, 14: 5500, 24: 20, 27: -10, 30: 500, 31: 400, 34: 100, 35: 80,
    38: 1, 45: 50, 50: 900, 52: 40, 53: 40, 65: 1, 66: 1, 68: 90, 70: 1, 71: 1, 72: 1300,
    73: 1000, 76: -3, 80: 1, 82: 3, 84: 1, 85: 100,
}
_RANGES = {
    9: (0, 1), 11: (0, 1000), 12: (1, 1000), 14: (0, 32767), 36: (0, 100), 45: (0, 100),
    68: (40, 99), 70: (0, 2), 71: (0, 1),
}


def setting_default(number):
    return _DEFAULTS.get(number, 0)


def setting_clamp(number, value):
    if number in _RANGES:
        lo, hi = _RANGES[number]
        return lo if value < lo else hi if value > hi else value
    return value


def default_settings():
    return [setting_default(n) for n in range(SETTINGS_COUNT)]


def setting_range(number):
    return _RANGES.get(number)


# settings the real controller hands to its IMU: 0x89 answers their defaults (proto_rules.c)
IMU_SETTINGS = (48, 84, 85)


def setting_answer(number, value):
    """What 0x89 answers for a setting that holds `value` (the library keeps the written value)."""
    return setting_default(number) if number in IMU_SETTINGS else value


def led_readback(percent):
    """One channel of 0xE9 as the real controller reads it back: 100 stays, anything else is 0."""
    return 100 if percent == 100 else 0


# 0x44 channel IDs by channel bit, left first (proto_rules.c)
_STATUS_IDS = ((CH_LPAD, 3), (CH_RPAD, 4), (CH_LGRIP, 0), (CH_RGRIP, 1))


def stop_status(kind, fields):
    """The 0x44 channel IDs a decoded output brings back with flag 0x02 (proto_rules.c): every
    stop-all (0x81 on time 0, 0x82 command 0, a zero speed in 0x80 for that grip) and 0x86's
    stream stop, in the order the real controller queues them: left first for 0x81 and 0x82,
    right first for 0x86, the right grip before the left for 0x80. `kind` and `fields` as the
    simulator's decoder gives them ("rumble" or "haptic")."""
    if kind == "rumble":
        return ([1] if fields["right"] == 0 else []) + ([0] if fields["left"] == 0 else [])
    hk = fields.get("haptic_kind")
    stop = (hk == 0 and fields.get("on_us") == 0) or (hk == 1 and fields.get("command") == 0)
    if stop:
        return [sid for bit, sid in _STATUS_IDS if fields["channels"] & bit]
    if hk == 5 and fields.get("operation") == 1:
        order = ((CH_RPAD, 4), (CH_LPAD, 3), (CH_RGRIP, 1), (CH_LGRIP, 0))
        return [sid for bit, sid in order if fields["channels"] & bit]
    return []
