# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# restore: the snapshot at the start of a run and the restore at its end (docs/testing.md, group
# "restore"), always last, also after a failure or Ctrl-C. The snapshot records how the board was
# found: its mode, the 86 settings, the LED colour, a CRC-32 per bond slot, a hash of its identity,
# the selected slot, the chime level and the stored mode policy. The restore stops every output
# and injection (with the system state machine its injection too, and a boot-time injection left
# for the next boot is cleared), writes the settings and the colour back, disarms, leaves every
# link's stored request at 0x83 as after a boot, and compares with the snapshot: any difference
# is exit code 6.
#
# [R7] The settings: a DISARM first, so that the firmware writes back the snapshot of the armed
# period that is ending (it does that at every end of arming), then an ARM WITHOUT the restore
# flag, the start snapshot written and checked, and the final DISARM, which writes nothing back.
# Writing the start snapshot while armed with the flag would be undone by that DISARM whenever the
# period's own snapshot differs (after Steam, it holds Steam's settings). restore.as_found reads
# the settings once more after it, armed without the flag as well.
import struct

from . import base
from .. import config as C
from ..board import BoardGone, DeviceError
from ..proto import console as cproto
from ..proto import model as M
from ..proto import testproto as T
from ..proto import vendor as V

GROUP = "restore"
TESTS = [("restore.outputs", "", []), ("restore.inputs", "", []),
         ("restore.settings", "", []), ("restore.disarm", "", []),
         ("restore.as_found", "", [])]

BONDS = (b"esb/bond\0", b"esb/bond_2\0")
TRANSPORT = b"user/wireless_transport\0"


def _identity(rig):
    """The identity's seven fields, read the way this path shows them (W: 0x83 and 0xAE 0, 1, 3;
    R: `opensc2 id`), so that a snapshot taken on one path compares with a restore on the other.
    In memory only."""
    if rig.path == "W":
        attrs = dict(V.decode_attributes(rig.vendor(C.VC_ATTRIBUTES).data))
        text = {}
        for tag, key in ((0, "board"), (1, "unit"), (3, "git")):
            a = rig.vendor(C.VC_STRING_ATTR, bytes([tag]))
            text[key] = a.data[1:20].split(b"\0", 1)[0].decode("ascii", "replace")
        return {"unit": text["unit"], "board": text["board"], "git": text["git"],
                "pid": attrs.get(1), "rev": attrs.get(9), "build": attrs.get(4),
                "blbuild": attrs.get(10)}
    return cproto.parse_id(rig.console_run("opensc2 id"))


def _identity_hash(rig):
    """A CRC-32 of the identity (never the identity itself), the same on both paths."""
    ident = _identity(rig)
    text = "|".join(str(ident.get(k)) for k in ("unit", "board", "git", "pid", "rev", "build",
                                                "blbuild"))
    return "%08x" % T.crc32(text.encode())


def state(rig):
    """What restore.as_found compares (and the start snapshot holds), on the current path."""
    s = {"mode": rig.path}
    s["bonds"] = ["%08x" % rig.read_named(name).crc for name in BONDS]
    s["identity"] = _identity_hash(rig)
    t = rig.read_named(TRANSPORT)
    s["slot"] = t.data[0] - 2 if t.n == 1 else None
    s["chime"] = rig.vendor(C.VC_USER_STORE_GET).data[1:2].hex()
    s["policy"] = (rig.caps or {}).get("policy")
    if rig.wireless:
        s["policy_shell"] = cproto.parse_mode(rig.console_run("opensc2 mode")).get("policy")
    return s


def wait_watchdog(rig, notes):
    """R24: a Steam watchdog still pending would change the settings during the run: wait it out
    (up to 11 s, without a 0x81) before the snapshot."""
    s = rig.settings_read()
    if s[9] == 0 and s[71] != 0:
        notes.append("waited for the Steam watchdog (lizard mode was off)")
        for _ in range(23):
            rig.sleep(0.5)
            if rig.settings_read()[9] != 0:
                break
    elif s[71] == 0:
        notes.append("setting 71 is 0: the board is in Steam's settings, taken as found")


def take_snapshot(ctx):
    """Step 2 of a run: how the board was found (and the baseline of the application's state)."""
    rig = ctx.rig
    notes = []
    wait_watchdog(rig, notes)
    snap = state(rig)
    # the mode the run started in (the first phase may come after a switch: --wired-only on a
    # board found wireless, say); everything else in state() reads the same on both paths
    snap["mode"] = getattr(ctx.runner, "start_mode", None) or rig.path
    snap["settings"] = rig.settings_read()
    snap["led"] = rig.led_color()
    rig.start_snapshot = snap
    ctx.results.snapshot_notes = notes
    return snap


def _puck_stored_request(rig):
    """R: written through the puck since the last boot (a boot resets every link's stored
    request), so the slot's stored request goes back to 0x83 too. Sent while still armed: a
    proof older than 10 s is renewed first, and that injects (S7 needs the hooks armed). Returns
    (ok, note); ok None when it could not be sent."""
    try:
        rig.puck_vendor(C.VC_ATTRIBUTES)
        return True, "the puck slot's stored request is 0x83 again"
    except (BoardGone, DeviceError):
        raise
    except base.Refused:
        return False, "the 0x83 through the puck failed"
    except Exception as e:      # a refused re-proof, no link, the slot gone: the rest goes on
        return None, "the puck slot's stored request was not reset to 0x83 (%s)" % \
            base.describe(e)


def run(ctx, start_mode):
    rig = ctx.rig
    snap = rig.start_snapshot or {}

    if rig.hooks and not rig.ctx.armed:
        try:
            rig.arm(restore=False)     # the restore writes the start snapshot itself
        except Exception:
            pass

    if ctx.want("restore.outputs"):
        with ctx.check(GROUP, "restore.outputs") as c:
            if not rig.ctx.armed:
                c.skip("not armed")
            start = rig.ring_next()
            for report in (bytes([0x80]) + bytes(9), bytes([0x82, 2, 0, 0]),
                           bytes([0x82, 5, 0, 0])):
                rig.output(report)
            _f, _n, entries = rig.ring_wait(start, lambda es: len(base.of_kind(
                es, C.RING_OUTPUT)) >= 3, 0.5)
            rums = base.of_kind(entries, C.RING_RUMBLE)
            haps = base.of_kind(entries, C.RING_HAPTIC)
            c.true("RUMBLE zero", rums and rums[0]["left"] == rums[0]["right"] == 0)
            c.eq("HAPTIC command 0 on targets 2 and 5",
                 [(h["target"], h.get("command")) for h in haps], [(2, 0), (5, 0)])

    if ctx.want("restore.inputs"):
        with ctx.check(GROUP, "restore.inputs") as c:
            if not rig.ctx.armed:
                c.skip("not armed")
            rig.seq_stop()
            rig.inject_off()
            rig.battery_off()
            st = rig.status()
            c.true("STATUS: no injection, no battery, no sequence",
                   st and not st["inject"] and not st["battery"] and not st["sequence"])
            if rig.caps and T.cap_flag(rig.caps, C.CAP_SYSTEM):
                # The system injection off, and no boot-time injection left behind: a system
                # test that stopped before its reboot would leave one for the board's next boot.
                rig.sys_inject_off()
                rig.sys_boot_clear()
                sst = rig.sys_status()
                c.true("SYS_STATUS: nothing injected into the system rules",
                       sst is not None and not sst["flags"] & 0x1E)

    # [R31, I8] the through-puck 0x83 before the DISARM below (restore.disarm reports it)
    puck_reset = None
    if ctx.want("restore.disarm") and rig.path == "R" and getattr(rig, "puck_writer_used", False):
        puck_reset = _puck_stored_request(rig)

    if ctx.want("restore.settings"):
        with ctx.check(GROUP, "restore.settings") as c:
            if not rig.ctx.armed or not snap.get("settings"):
                c.skip("no snapshot to restore")
            # the end of this armed period first: the firmware writes back its own snapshot;
            # then armed without the restore flag, so that the final DISARM keeps what is written
            rig.disarm()
            rig.sleep(0.06)                  # the guard's 50 ms after the end of arming
            rig.arm(restore=False)
            want = snap["settings"]
            now = rig.settings_read()
            pairs = [(n, want[n]) for n in range(86) if n not in (9, 71) and now[n] != want[n]]
            pairs += [(n, want[n]) for n in (71, 9) if now[n] != want[n]]
            for i in range(0, len(pairs), 4):
                rig.write_settings(pairs[i:i + 4])
            if snap.get("led") is not None and rig.led_color() != snap["led"]:
                rig.vendor_set(C.VC_LED_SET, bytes(snap["led"]))
            again = rig.settings_read()
            if not c.eq("settings that differ from the snapshot",
                        [n for n in range(86) if again[n] != want[n]], []):
                ctx.not_as_found = True
            c.eq("the LED colour", rig.led_color(), snap.get("led"))
            c.note("%d settings written back (armed without the restore flag)" % len(pairs))

    if ctx.want("restore.disarm"):
        with ctx.check(GROUP, "restore.disarm") as c:
            if rig.hooks:
                rig.disarm()
                rig.sleep(0.06)
            if rig.own_hid:
                rig.vendor_set(C.VC_ATTRIBUTES)
                a = rig.vendor_get()
                c.eq("the stored request is 0x83 again", (a.opcode, a.n), (C.VC_ATTRIBUTES, 30))
                c.true("a keyed STATUS STALLs", rig.test(C.OP_STATUS, stall_ok=True) is None)
                a = rig.vendor_get()
                c.eq("a further GET answers 0x83", (a.opcode, a.n), (C.VC_ATTRIBUTES, 30))
            elif rig.hooks:
                st = rig.status()
                c.true("armed = 0", st is not None and not st["armed"])
            else:
                c.true("no test hooks in this firmware: nothing was armed", True)
            if puck_reset is not None:
                ok, note = puck_reset
                if ok:
                    c.true(note, True)
                elif ok is None:
                    c.warn(note)
                else:
                    c.fail(note)
            if rig.console is not None:
                rig.console.restore_shell()
                c.note("the console's echo, colours and prompt back on")

    if ctx.want("restore.as_found"):
        with ctx.check(GROUP, "restore.as_found") as c:
            if not snap:
                c.skip("no start snapshot")
            if rig.path != snap.get("mode"):
                c.fail("the board is %s, it was found %s" % (rig.path, snap.get("mode")))
                ctx.not_as_found = True
            else:
                # armed without the restore flag: the DISARM after it writes nothing back, so
                # what is read here is what the board keeps
                armed = False
                if rig.hooks is not False:
                    try:
                        rig.arm(restore=False)
                        armed = True
                    except base.Stall:
                        armed = False
                now = state(rig)
                settings = rig.settings_read() if armed and snap.get("settings") else None
                led = rig.led_color() if settings is not None else None
                if armed:
                    rig.disarm()
                    rig.sleep(0.06)
                if rig.own_hid:
                    rig.vendor_set(C.VC_ATTRIBUTES)
                    rig.vendor_get()
                diffs = [k for k in ("bonds", "identity", "slot", "chime", "policy")
                         if now.get(k) != snap.get(k)]
                if now.get("policy_shell") and snap.get("policy_shell") and \
                        now["policy_shell"] != snap["policy_shell"]:
                    diffs.append("policy_shell")
                keys = ["mode", "bonds", "identity", "slot", "chime", "policy"]
                if settings is not None:
                    keys += ["settings", "led"]
                    differ = [n for n in range(86) if settings[n] != snap["settings"][n]]
                    if differ:
                        diffs.append("settings")
                        c.observe(settings_differ=differ)
                    if snap.get("led") is not None and led != snap["led"]:
                        diffs.append("led")
                ctx.results.left_as_found = {k: ("differs" if k in diffs else "equal")
                                             for k in keys}
                if diffs:
                    ctx.not_as_found = True
                    c.fail("not as found: %s (exit 6)" % ", ".join(diffs))
                else:
                    c.true("as found", True)
                    c.note("%s as found" % ", ".join(keys))
