# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# model.py: the expected-report model (the input rules), the lizard model, the target-channel
# table and the settings defaults and ranges. The firmware's own code (codec.c, input_rules.c,
# report.c, lizard.c, settings_defaults.c) is the reference; these mirror it so the runner can
# predict every report and the FW's host tests can check the same against the library. Integer
# division truncates toward zero, as C does (Python's // is wrong for negatives), so a c_div()
# helper is used throughout.
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


# ---- the input rules (input_rules.c), applied to every state report after injection -----------

RULE_TRIGGER_BITS = 0x001        # bits 23 and 27 from setting 68
RULE_RAW_TRIGGERS = 0x002        # no trigger bit while setting 62 is not 0
RULE_STICK_FALLBACK = 0x004      # a stick beyond 12000 counts as touched
RULE_PAD_BLANK = 0x008           # setting 82 blanks a pad in 0x42
RULE_PAD_CLICK = 0x010           # pad clicks from pressure (52, 53)
RULE_LIZARD_STABILISER = 0x100
RULE_LIZARD_SMOOTHING = 0x200
RULE_LIZARD_GLIDE = 0x400
RULE_LIZARD_TICKS = 0x800
RULES_KNOWN = 0xF1F
RULES_LIZARD = 0xF00
# every option's default (the blank canvas), and OpenSC2 0.2.0's rules (a board without RULES)
RULES_DEFAULT = (RULE_TRIGGER_BITS | RULE_RAW_TRIGGERS | RULE_STICK_FALLBACK | RULE_PAD_BLANK |
                 RULES_LIZARD)
RULES_V0_2 = RULE_TRIGGER_BITS
RULE_NAMES = {RULE_TRIGGER_BITS: "trigger_bits", RULE_RAW_TRIGGERS: "raw_triggers",
              RULE_STICK_FALLBACK: "stick_fallback", RULE_PAD_BLANK: "pad_blank",
              RULE_PAD_CLICK: "pad_click", RULE_LIZARD_STABILISER: "stabiliser",
              RULE_LIZARD_SMOOTHING: "smoothing", RULE_LIZARD_GLIDE: "glide",
              RULE_LIZARD_TICKS: "ticks"}

STICK_TOUCH_RADIUS = 12000
STABILISER_FINE = 4369
STABILISER_COARSE = 8738

_STICK_TOUCH = (1 << R.BIT["LSTICK_TOUCH"], 1 << R.BIT["RSTICK_TOUCH"])
_PAD_TOUCH = (1 << R.LPAD_TOUCH_BIT, 1 << R.RPAD_TOUCH_BIT)
_PAD_CLICK = (1 << R.BIT["LPAD_CLICK"], 1 << R.BIT["RPAD_CLICK"])
_TRIG_BIT = (1 << R.BIT["LTRIGGER_DIGITAL"], 1 << R.BIT["RTRIGGER_DIGITAL"])


def rule_names(rules):
    return [name for bit, name in sorted(RULE_NAMES.items()) if rules & bit]


def approx_len(dx, dy):
    """The controller's quick distance: with M the larger and m the smaller of |dx| and |dy|,
    (1007 M + 441 m, less 40 M when M < 16 m) / 1024, rounded to the nearest whole number."""
    a, b = abs(dx), abs(dy)
    big, small = max(a, b), min(a, b)
    s = 1007 * big + 441 * small
    if big < 16 * small:
        s -= 40 * big
    return (s + 512) // 1024


def pad_click(pressure, setting, previous):
    """A pad's click by pressure: on above (s + 8) %, off below (s - 8) % of 32767, where s is
    setting 52 or 53 clamped to -1..100 and -1 counts as 100."""
    s = -1 if setting < -1 else 100 if setting > 100 else setting
    if s == -1:
        s = 100
    p = min(pressure, TRIGGER_MAX)
    if p * 100 > (s + 8) * TRIGGER_MAX:
        return True
    if p * 100 < (s - 8) * TRIGGER_MAX:
        return False
    return previous


def settings_list(settings=None):
    """86 settings: the defaults, a full list, or the defaults with a {number: value} dict."""
    if settings is None:
        return default_settings()
    if isinstance(settings, dict):
        out = default_settings()
        for n, v in settings.items():
            out[n] = v
        return out
    out = list(settings)
    if len(out) != SETTINGS_COUNT:
        raise ValueError("86 settings expected")
    return out


class StateRules:
    """The rules' memory from one report to the next (struct osc2_input_rules) and apply() for
    each report: the expected 52 bytes after the sequence byte for an injected state52."""

    def __init__(self, rules=RULES_V0_2):
        self.rules = rules
        self.trigger_bit = [False, False]
        self.pad_click = [False, False]
        self.click_setting = [None, None]

    def set_clicks(self, left, right, settings=None):
        """The pads' clicks by pressure as worked out with the current thresholds."""
        s = settings_list(settings)
        self.pad_click = [bool(left), bool(right)]
        self.click_setting = [s[52], s[53]]

    def apply(self, state52, settings=None):
        s = settings_list(settings)
        rules = self.rules
        f = R.decode_state52(state52)
        b = f["buttons"]
        sticks = ((f["lstick_x"], f["lstick_y"]), (f["rstick_x"], f["rstick_y"]))
        pads = [[f["lpad_x"], f["lpad_y"], f["lpad_pressure"]],
                [f["rpad_x"], f["rpad_y"], f["rpad_pressure"]]]
        for side in (0, 1):
            if rules & RULE_STICK_FALLBACK and s[46] == 0 and \
                    approx_len(*sticks[side]) > STICK_TOUCH_RADIUS:
                b |= _STICK_TOUCH[side]
            if rules & RULE_PAD_CLICK:
                setting = s[53 if side else 52]
                if setting != self.click_setting[side]:
                    self.pad_click[side] = False
                    self.click_setting[side] = setting
                self.pad_click[side] = pad_click(pads[side][2], setting, self.pad_click[side])
                clicked = self.pad_click[side] and bool(b & _PAD_TOUCH[side])
                b = (b | _PAD_CLICK[side]) if clicked else (b & ~_PAD_CLICK[side])
            else:
                self.pad_click[side] = False
                self.click_setting[side] = None
            if rules & RULE_PAD_BLANK and s[82] & (2 if side else 1) and b & _STICK_TOUCH[side]:
                pads[side][0] = pads[side][1] = 0
                if s[60] != 1:
                    pads[side][2] = 0
                b &= ~(_PAD_TOUCH[side] | _PAD_CLICK[side])
        if rules & RULE_RAW_TRIGGERS and s[62] != 0:
            self.trigger_bit = [False, False]
            b &= ~(_TRIG_BIT[0] | _TRIG_BIT[1])
        elif rules & RULE_TRIGGER_BITS:
            for side, name in ((0, "left_trigger"), (1, "right_trigger")):
                self.trigger_bit[side] = trigger_click(f[name], s[68], self.trigger_bit[side])
                b = (b | _TRIG_BIT[side]) if self.trigger_bit[side] else (b & ~_TRIG_BIT[side])
        lt, rt = bool(b & _PAD_TOUCH[0]), bool(b & _PAD_TOUCH[1])
        out = {
            "buttons": b & 0xFFFFFFFF,
            "left_trigger": clamp_trigger(f["left_trigger"]),
            "right_trigger": clamp_trigger(f["right_trigger"]),
            "lstick_x": clamp_axis(f["lstick_x"]), "lstick_y": clamp_axis(f["lstick_y"]),
            "rstick_x": clamp_axis(f["rstick_x"]), "rstick_y": clamp_axis(f["rstick_y"]),
            "lpad_x": clamp_axis(pads[0][0]) if lt else 0,
            "lpad_y": clamp_axis(pads[0][1]) if lt else 0,
            "lpad_pressure": clamp_trigger(pads[0][2]),
            "rpad_x": clamp_axis(pads[1][0]) if rt else 0,
            "rpad_y": clamp_axis(pads[1][1]) if rt else 0,
            "rpad_pressure": clamp_trigger(pads[1][2]),
            "imu_timestamp": f["imu_timestamp"],
            "accel_x": f["accel_x"], "accel_y": f["accel_y"], "accel_z": f["accel_z"],
            "gyro_x": f["gyro_x"], "gyro_y": f["gyro_y"], "gyro_z": f["gyro_z"],
            "quat_w": f["quat_w"], "quat_x": f["quat_x"], "quat_y": f["quat_y"],
            "quat_z": f["quat_z"],
        }
        return R.make_state52(**out)


def expected_report(state52, prev_left_bit, prev_right_bit, setting68=90, trigger_click_auto=True):
    """The expected 0x42 payload (bytes 2..53, 52 bytes) for an injected `state52` under the rules
    of OpenSC2 0.2.0 (the trigger bits only), given the previous frame's trigger bits. Returns
    (expected_bytes, new_left_bit, new_right_bit). The sequence byte is not predicted (checked for
    continuity only, 3.7). StateRules has every rule."""
    r = StateRules(RULES_V0_2 if trigger_click_auto else 0)
    r.trigger_bit = [prev_left_bit, prev_right_bit]
    out = r.apply(state52, {68: setting68})
    if trigger_click_auto:
        return out, r.trigger_bit[0], r.trigger_bit[1]
    b = R.decode_state52(out)["buttons"]
    return out, bool(b & _TRIG_BIT[0]), bool(b & _TRIG_BIT[1])


# ---- the lizard model (1.3, 4.2 lizard.txt) -------------------------------------------------

KEY_ENTER, KEY_ESCAPE, KEY_TAB = 0x28, 0x29, 0x2B
KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP = 0x4F, 0x50, 0x51, 0x52
KEY_ROLLOVER = 0x01
POINTER_DIVISOR = 128
SCROLL_DIVISOR = 1024
HISTORY = 20             # movements kept per pad
AVERAGE = 8              # the motion: the mean of the newest 8
FLICK_MOVES = 15         # the flick speed: the mean of the oldest 15 of the last 20
FLICK_TRAVEL = 700       # a glide needs more travel than this since the touch-down
TICK_STEP = 1500         # pad units per step; a tick once the steps add up to more than 14
TICK_GAIN_DB = 8         # click A
CLICK_GAIN_DB = 6        # click B

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
_TOUCH = _PAD_TOUCH


def _keyboard_report(keys):
    return bytes([C.RPT_KEYBOARD, 0, 0] + list(keys) + [0] * (6 - len(keys)))


def _mouse_report(buttons, x, y, wheel, pan):
    return bytes([C.RPT_MOUSE, buttons & 0xFF, x & 0xFF, y & 0xFF, wheel & 0xFF, pan & 0xFF])


def _isqrt(v):
    """The integer square root, rounded down."""
    if v <= 0:
        return 0
    r = int(v ** 0.5)
    while r * r > v:
        r -= 1
    while (r + 1) * (r + 1) <= v:
        r += 1
    return r


def _close(gap):
    """An eighth of the gap, rounded down, plus one unit toward the thumb (the stabiliser)."""
    if gap > 0:
        return gap // 8 + 1
    if gap < 0:
        return gap // 8 - 1
    return 0


class Stabiliser:
    """The touch stabiliser (input_rules.c, osc2_stabiliser_step)."""

    def __init__(self):
        self.reset()

    def reset(self):
        self.on = False
        self.holding = False
        self.ax = self.ay = self.ox = self.oy = self.px = self.py = 0

    def step(self, radius, touched, x, y):
        if not touched:
            self.reset()
            return x, y
        if not self.on:
            self.on = self.holding = True
            self.ax = self.ox = self.px = x
            self.ay = self.oy = self.py = y
            return x, y
        if self.holding:
            if approx_len(x - self.ax, y - self.ay) <= radius:
                self.px, self.py = x, y
                return self.ox, self.oy
            self.holding = False
        dx, dy = x - self.px, y - self.py
        self.ox += dx
        self.oy += dy
        if dx or dy:
            self.ox += _close(x - self.ox)
            self.oy += _close(y - self.oy)
        self.px, self.py = x, y
        return self.ox, self.oy


STABILISER_FINE = 4369       # the pad rules' hold radius (setting 80 = 2), report units


class PadRules:
    """The pad rules of a build with CONFIG_OPENSC2_PAD_RULES (cal_store.c, osc2_cal_rules()), as
    far as the suite drives them: setting 80 = 2's touch stabiliser on both pads' reported
    positions, which run after the injection and before the input rules, so 0x42 and lizard mode
    both see the held position. The library halves a position back to the pad scale (turning Y
    up), doubles it again and stabilises that with the fine radius. The rotation (settings 2 and
    24 both non-zero) is not modelled: the suite leaves setting 2 at 0."""

    def __init__(self):
        self.stab = [Stabiliser(), Stabiliser()]

    def apply(self, state52, settings=None):
        s = settings_list(settings)
        f = R.decode_state52(state52)
        stabilise = s[80] == 2
        out = state52
        for side, (kx, ky) in enumerate((("lpad_x", "lpad_y"), ("rpad_x", "rpad_y"))):
            touched = bool(f["buttons"] & _PAD_TOUCH[side])
            if not touched or not stabilise:
                self.stab[side].reset()
                continue
            x = clamp_axis(2 * c_div(f[kx], 2))
            y = clamp_axis(2 * c_div(-f[ky], 2))
            x, y = self.stab[side].step(STABILISER_FINE, True, x, y)
            out = R.set_field(R.set_field(out, kx, clamp_axis(x)), ky, clamp_axis(-y))
        return out


class _Pad:
    """One pad's tracker (struct opensc2_lizard_pad)."""

    def __init__(self):
        self.touched = False
        self.x = self.y = 0
        self.stab = Stabiliser()
        self.hist = []               # the last 20 movements, oldest first
        self.moves = 0
        self.travel = 0
        self.gliding = False
        self.vx = self.vy = 0
        self.glide_frame = self.glide_frames = 0
        self.ticking = False
        self.step_x = self.step_y = 0
        self.steps = 0
        self.click = False


class LizardModel:
    """One instance of lizard mode's logic (lizard.c). step()/enter()/leave()/switches()/resync()
    each return a list of (kind, report-bytes) in the exact send order of the firmware; after a
    step, `haptics` lists its haptic events in order, as ("tick" or "click", side 0 left / 1
    right). `rules` is the rule word: its lizard bits switch the stabilisers, the average, the
    glide and the ticks (0: lizard mode as in OpenSC2 0.2.0)."""

    def __init__(self, rules=0):
        self.rules = rules
        self.reset()

    def reset(self):
        self.active = True
        self.keyboard_on = True
        self.buttons_on = True
        self.keyboard_sync = True
        self.mapped = 0
        self.haptics = []
        self._clear_tracker()

    def _clear_tracker(self):
        self.pad = [_Pad(), _Pad()]
        self.rem_x = self.rem_y = self.rem_pan = self.rem_wheel = 0
        self.mouse_owed = False
        self.click_sync = True

    def gliding(self):
        return any(p.gliding for p in self.pad)

    def enter(self):
        self._clear_tracker()
        self.active = True
        self.keyboard_on = self.buttons_on = True
        self.keyboard_sync = True
        # osc2_lizard_set(on): keyboard first, then mouse (the same order as off)
        return [("keyboard", _keyboard_report([])), ("mouse", _mouse_report(0, 0, 0, 0, 0))]

    def leave(self):
        was_active = self.active
        self.active = False
        self.keyboard_on = self.buttons_on = False
        self.mouse_owed = False
        for p in self.pad:
            p.gliding = False
            p.ticking = False
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

    def release(self):
        """opensc2_lizard_release(): the release pair when something is held, else nothing."""
        if not self.active or (self.mapped == 0 and not self.mouse_owed):
            return []
        self.mapped = 0
        self.mouse_owed = False
        return [("keyboard", _keyboard_report([])), ("mouse", _mouse_report(0, 0, 0, 0, 0))]

    def haptic_words(self):
        """The step's haptic events as lizard.txt writes them (TICK_L, CLICK_R, ...)."""
        return [("TICK_" if kind == "tick" else "CLICK_") + ("R" if side else "L")
                for kind, side in self.haptics]

    def _pad_haptics(self, side, touched, clicked, x, y, s):
        p = self.pad[side]
        on = bool(self.rules & RULE_LIZARD_TICKS) and s[70] == 1
        if touched and not p.touched:
            p.ticking = on
            p.step_x, p.step_y = x, y
            p.steps = 0
        elif touched and p.ticking and on and approx_len(x - p.step_x, y - p.step_y) > TICK_STEP:
            p.step_x, p.step_y = x, y
            p.steps += 1
            if p.steps * TICK_STEP > min(max(s[14], 0), 32767):
                p.steps = 0
                self.haptics.append(("tick", side))
        if not on:
            p.ticking = False
        if self.click_sync:
            p.click = clicked
        elif clicked != p.click:
            p.click = clicked
            if on and s[53 if side else 52] > -1:
                self.haptics.append(("click", side))

    def _pad_motion(self, p, touched, x, y, s):
        rules = self.rules
        if touched:
            if not p.touched:
                # touch-down: no motion; the history, the travel and any glide start over
                p.hist = []
                p.moves = 0
                p.travel = 0
                p.gliding = False
                m = (0, 0)
            else:
                dx, dy = x - p.x, y - p.y
                p.hist = (p.hist + [(dx, dy)])[-HISTORY:]
                p.moves += 1
                p.travel = min(p.travel + approx_len(dx, dy), 0x7FFFFFFF)
                if rules & RULE_LIZARD_SMOOTHING:
                    newest = p.hist[-min(p.moves, AVERAGE):]
                    n = len(newest)
                    m = (c_div(sum(a for a, _b in newest), n), c_div(sum(b for _a, b in newest), n))
                else:
                    m = (dx, dy)
            p.touched = True
            p.x, p.y = x, y
            return m
        if p.touched:
            p.touched = False
            if rules & RULE_LIZARD_GLIDE and p.moves >= HISTORY and p.travel > FLICK_TRAVEL:
                oldest = p.hist[:FLICK_MOVES]
                p.vx = c_div(sum(a for a, _b in oldest), FLICK_MOVES)
                p.vy = c_div(sum(b for _a, b in oldest), FLICK_MOVES)
                if _isqrt(p.vx * p.vx + p.vy * p.vy) > min(max(s[11], 0), 1000):
                    p.gliding = True
                    p.glide_frame = 0
                    p.glide_frames = min(max(s[12], 1), 1000)
        if p.gliding and not rules & RULE_LIZARD_GLIDE:
            p.gliding = False
        if p.gliding:
            left = p.glide_frames - p.glide_frame
            m = (c_div(p.vx * left, p.glide_frames), c_div(p.vy * left, p.glide_frames))
            p.glide_frame += 1
            if p.glide_frame >= p.glide_frames:
                p.gliding = False
            return m
        return (0, 0)

    def step(self, buttons=0, lpad=(0, 0), rpad=(0, 0), s82=None, lpad_touched=None,
             rpad_touched=None, rpad_clicked=False, lpad_clicked=False, settings=None):
        s = settings_list(settings)
        if s82 is not None:
            s[82] = s82
        self.haptics = []
        if not self.active:
            return []
        made = []
        pads = (lpad, rpad)
        touched_in = [
            lpad_touched if lpad_touched is not None else bool(buttons & _TOUCH[0]),
            rpad_touched if rpad_touched is not None else bool(buttons & _TOUCH[1]),
        ]
        clicked_in = [lpad_clicked, rpad_clicked]

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

        # mouse: each pad's stabiliser, tracker and haptics
        motion = [(0, 0), (0, 0)]
        blanked = [False, False]
        for side in range(2):
            p = self.pad[side]
            blanked[side] = bool(s[82] & (2 if side else 1)) and bool(buttons & _STICK_TOUCH[side])
            touched = (not blanked[side]) and (touched_in[side] or bool(buttons & _TOUCH[side]))
            clicked = (not blanked[side]) and (clicked_in[side] or bool(buttons & _PAD_CLICK[side]))
            x, y = pads[side] if touched else (0, 0)
            self._pad_haptics(side, touched, clicked, x, y, s)
            if self.rules & RULE_LIZARD_STABILISER and (side == 0 or s[80] == 1):
                x, y = p.stab.step(STABILISER_COARSE if side == 0 else STABILISER_FINE, touched,
                                   x, y)
            else:
                p.stab.reset()
            motion[side] = self._pad_motion(p, touched, x, y, s)
            if blanked[side]:
                motion[side] = (0, 0)   # held back; a glide's factor keeps running down
        self.click_sync = False

        x = self._divide("rem_x", motion[1][0], POINTER_DIVISOR)
        y = -self._divide("rem_y", motion[1][1], POINTER_DIVISOR)
        pan = self._divide("rem_pan", motion[0][0], SCROLL_DIVISOR)
        wheel = -self._divide("rem_wheel", motion[0][1], SCROLL_DIVISOR)

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

    def step_state52(self, state52, settings=None):
        """One step on a report's 52 bytes (after the input rules), as report.c hands it over."""
        f = R.decode_state52(state52)
        return self.step(buttons=f["buttons"], lpad=(f["lpad_x"], f["lpad_y"]),
                         rpad=(f["rpad_x"], f["rpad_y"]), settings=settings)

    def _divide(self, rem_name, delta, divisor):
        total = getattr(self, rem_name) + delta
        q = c_div(total, divisor)
        setattr(self, rem_name, total - q * divisor)
        return q


def lizard_haptic_fields(kind, side):
    """A lizard haptic event as the ring's HAPTIC entry decodes it (proto/testproto.py): kind
    COMMAND (1), report ID 0 (made by the library), the pad as target and channel, click A (+8 dB)
    for a tick, click B (+6 dB) for the pad click."""
    tick = kind == "tick"
    return {"haptic_kind": 1, "report_id": 0, "target": side,
            "channels": CH_RPAD if side else CH_LPAD, "command": 1 if tick else 2,
            "gain": TICK_GAIN_DB if tick else CLICK_GAIN_DB}


def lizard_haptic_of_entry(e):
    """("tick" or "click", side) for a decoded HAPTIC entry the library made (report ID 0; output
    reports have IDs from 0x80), else None. An entry of the library's that is not exactly what
    lizard_haptic_fields() gives (another kind, command, target, channel or level) comes back as a
    longer tuple carrying those fields, so that a comparison with the model shows it."""
    if e.get("kind") != C.RING_HAPTIC or e.get("report_id") != 0:
        return None
    kind = "tick" if e.get("command") == 1 else "click" if e.get("command") == 2 else None
    side = e.get("target")
    if e.get("haptic_kind") != 1 or kind is None or side not in (0, 1):
        return ("unexpected", e.get("haptic_kind"), e.get("command"), side)
    want = lizard_haptic_fields(kind, side)
    if (e.get("channels"), e.get("gain")) != (want["channels"], want["gain"]):
        return (kind, side, e.get("channels"), e.get("gain"))
    return kind, side


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
    1: 2, 3: 1200, 9: 1, 10: 7000, 11: 200, 12: 100, 13: 50, 14: 5500, 15: 923, 16: 382, 17: 2,
    18: 8000, 19: 1770, 20: 1630, 21: 5, 22: 2, 23: 2, 24: 20, 25: 40, 27: -10, 28: 16500,
    29: 15000, 30: 500, 31: 400, 32: 800, 34: 100, 35: 80, 38: 1, 40: 20, 41: 3900, 42: 1, 44: 50,
    45: 50, 49: 2, 50: 900, 51: 250, 52: 40, 53: 40, 54: 100, 55: 100, 56: 10, 57: 10, 63: 150,
    64: 4, 65: 1, 66: 1, 68: 90, 69: 1, 70: 1, 71: 1, 72: 1300, 73: 1000, 74: 3, 76: -3, 78: 1,
    79: 2, 80: 1, 82: 3, 83: 1, 84: 1, 85: 100,
}
_RANGES = {
    0: (0, 10), 1: (0, 10), 2: (0, 360), 3: (0, 25000), 4: (0, 1), 5: (0, 1), 6: (0, 255),
    7: (0, 255), 8: (0, 1), 9: (0, 1), 10: (0, 16384), 11: (0, 1000), 12: (1, 1000), 13: (1, 500),
    14: (0, 32767), 15: (0, 2000), 16: (0, 2000), 17: (1, 10), 18: (0, 20000), 19: (0, 4096),
    20: (0, 4096), 21: (0, 500), 22: (0, 8), 23: (0, 8), 24: (0, 30), 25: (3, 99), 26: (0, 1),
    27: (-24, 12), 28: (0, 4096), 29: (0, 4096), 30: (100, 1000), 31: (0, 800), 32: (100, 1200),
    33: (0, 1), 34: (25, 400), 35: (25, 100), 36: (0, 100), 37: (0, 1), 38: (0, 1), 39: (0, 1),
    40: (1, 180), 41: (0, 25000), 42: (0, 1), 43: (0, 1), 44: (0, 100), 45: (0, 100), 46: (0, 2),
    47: (0, 16), 49: (1, 2), 50: (0, 32767), 51: (0, 32767), 52: (-1, 100), 53: (-1, 100),
    54: (0, 100), 55: (0, 100), 56: (0, 100), 57: (0, 100), 58: (0, 100), 59: (0, 100), 60: (0, 1),
    61: (0, 15), 62: (0, 2), 63: (0, 300), 64: (1, 16), 65: (0, 1), 66: (0, 1), 67: (0, 7),
    68: (40, 99), 69: (0, 1), 70: (0, 2), 71: (0, 1), 72: (0, 16000), 73: (0, 16000), 74: (0, 3),
    75: (0, 1), 76: (-24, 6), 77: (0, 1), 78: (0, 1), 79: (1, 4), 80: (0, 2), 81: (0, 1),
    82: (0, 3), 83: (0, 1),
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


def reset_flags(causes):
    """0xA1's byte of reset-cause flags from Zephyr's RESET_* bits (proto_rules.c): power-on
    0x01, brown-out 0x02, any other cause 0x08, pin 0x10, watchdog 0x20, software 0x40."""
    named = 0x1F
    return ((0x01 if causes & 0x08 else 0) | (0x02 if causes & 0x04 else 0) |
            (0x08 if causes & ~named else 0) | (0x10 if causes & 0x01 else 0) |
            (0x20 if causes & 0x10 else 0) | (0x40 if causes & 0x02 else 0))


def led_readback(percent):
    """One channel of 0xE9 as the real controller reads it back (proto_rules.c): cut to whole
    hundreds, so 0 below 100, 100 from 100 to 199, 200 from 200 (the library keeps at most 100)."""
    return percent // 100 * 100


def charge_window_valid(value):
    """Does 0xED answer this mte/charge_level value (key_rules.c)? Two u32 mV values, the lower
    then the upper: both 0 (no window), or a lower one of at least 3400 with an upper one of at
    most 4120, both multiples of 10."""
    if len(value) != 8:
        return False
    lower, upper = int.from_bytes(value[:4], "little"), int.from_bytes(value[4:], "little")
    if lower == 0 and upper == 0:
        return True
    return lower >= 3400 and upper <= 4120 and lower % 10 == 0 and upper % 10 == 0


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
