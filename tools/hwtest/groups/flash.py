# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# flash: the flash writes Steam makes when it pairs a controller over the cable, rehearsed with the
# values the board already holds (docs/testing.md, group "flash"), W, opt-in (--flash). Every
# write gets a one-shot token right after its own read-back: exactly that opcode, key and value;
# safety.py refuses any write without one. Bond bytes stay in memory; only CRC-32s are reported.
#
# What the board reads back is the value in effect, stored or the build's default: the firmware
# does not say which. So flash.chime, flash.transport and flash.policy store the value in effect
# even where nothing was stored (opensc2/chime, opensc2/slot, opensc2/mode); a later build with
# another default would then still find this value. They count in safety.flash_writes like every
# write here, and say so in their notes (docs/testing.md).
import time

PINNED = "if nothing was stored, the value in effect is stored now (a default becomes a stored " \
         "value)"

from . import base
from .. import config as C
from ..proto import console as cproto

GROUP = "flash"
TESTS = [("flash.pairing_rehearsal", "W", ["flash"]), ("flash.chime", "W", ["flash"]),
         ("flash.transport", "W", ["flash"]), ("flash.bond_a2", "W", ["flash"]),
         ("flash.delete_empty", "W", ["flash"]), ("flash.kept_keys", "W", ["flash"]),
         ("flash.policy", "R", ["flash"])]

TRANSPORT = b"user/wireless_transport\0"
BOND = b"esb/bond\0"
BOND2 = b"esb/bond_2\0"


def _skip_all(ctx, why):
    for tid, _p, _n in TESTS:
        if ctx.want(tid):
            ctx.record(GROUP, tid, base.SKIP, why)


def _token(rig, opcode, name, value):
    rig.ctx.add_token(opcode, name, value)


def run(ctx):
    rig = ctx.rig
    if not ctx.opt.get("flash"):
        _skip_all(ctx, "opt-in (--flash)")
        return
    writes = ctx.results.safety

    if ctx.want("flash.pairing_rehearsal") and rig.path == "W":
        with ctx.check(GROUP, "flash.pairing_rehearsal") as c:
            t = rig.read_named(TRANSPORT)
            c.eq("0xED user/wireless_transport: n", t.n, 1)
            b = rig.read_named(BOND)
            if b.n == 0:
                c.skip("esb/bond is empty: nothing to rewrite")
            c.eq("0xED esb/bond: n", b.n, 24)
            bond = b.data[:24]
            c.observe(bond_crc32="%08x" % b.crc)
            rig.vendor_set(C.VC_BY_NAME_SET, BOND + bond)
            again = rig.read_named(BOND)
            if not c.true("0xEE esb/bond with its own bytes, read back equal",
                          again.data[:24] == bond):
                _token(rig, C.VC_BY_NAME_SET, "esb/bond", bond)
                rig.vendor_set(C.VC_BY_NAME_SET, BOND + bond)
                return
            _token(rig, C.VC_BY_NAME_SAVE, "esb/bond", b"")
            t0 = time.monotonic()
            rig.vendor_set(C.VC_BY_NAME_SAVE, BOND)
            hold = (time.monotonic() - t0) * 1000.0
            writes["flash_writes"] += 1
            c.observe(save_hold_ms=round(hold, 1))
            c.true("0xED esb/bond after the save equals the bond", rig.read_named(BOND).data[:24]
                   == bond)
            c.note("0xEF held the request for %.1f ms; the next switch to wireless reconnects "
                   "with this bond" % hold)

    if ctx.want("flash.chime") and rig.path == "W":
        with ctx.check(GROUP, "flash.chime") as c:
            a = rig.vendor(C.VC_USER_STORE_GET)
            c.eq("0xDB", (a.n, a.data[:1]), (2, b"\x01"))
            level = a.data[1]
            value = bytes([1, level])
            _token(rig, C.VC_USER_STORE_SET, None, value)
            rig.vendor_set(C.VC_USER_STORE_SET, value)
            writes["flash_writes"] += 1
            c.eq("the chime level unchanged", rig.vendor(C.VC_USER_STORE_GET).data[:2], value)
            c.note("chime level rewritten; %s" % PINNED)

    if ctx.want("flash.transport") and rig.path == "W":
        with ctx.check(GROUP, "flash.transport") as c:
            before = rig.read_named(TRANSPORT).data
            _token(rig, C.VC_BY_NAME_SAVE, "user/wireless_transport", b"")
            rig.vendor_set(C.VC_BY_NAME_SAVE, TRANSPORT)
            writes["flash_writes"] += 1
            c.eq("0xED unchanged", rig.read_named(TRANSPORT).data, before)
            c.note("the selected slot saved; %s" % PINNED)

    if ctx.want("flash.bond_a2") and rig.path == "W":
        with ctx.check(GROUP, "flash.bond_a2") as c:
            b = rig.read_named(BOND)
            if b.n == 0:
                c.skip("esb/bond is empty: nothing to rewrite")
            bond = b.data[:24]
            _token(rig, C.VC_BOND_WRITE, None, bond)
            rig.vendor_set(C.VC_BOND_WRITE, bond)
            writes["flash_writes"] += 1
            if not c.true("0xED esb/bond = the bond after 0xA2", rig.read_named(BOND).data[:24]
                          == bond):
                _token(rig, C.VC_BY_NAME_SET, "esb/bond", bond)
                rig.vendor_set(C.VC_BY_NAME_SET, BOND + bond)
                _token(rig, C.VC_BY_NAME_SAVE, "esb/bond", b"")
                rig.vendor_set(C.VC_BY_NAME_SAVE, BOND)

    if ctx.want("flash.delete_empty") and rig.path == "W":
        with ctx.check(GROUP, "flash.delete_empty") as c:
            b2 = rig.read_named(BOND2)
            if b2.n != 0:
                c.skip("esb/bond_2 holds a bond: it is not deleted")
            _token(rig, C.VC_BY_NAME_DELETE, "esb/bond_2", b"")
            rig.vendor_set(C.VC_BY_NAME_DELETE, BOND2)
            writes["flash_writes"] += 1
            c.eq("esb/bond_2 still empty", rig.read_named(BOND2).n, 0)

    if ctx.want("flash.kept_keys") and rig.path == "W":
        with ctx.check(GROUP, "flash.kept_keys") as c:
            # the keys the library keeps and 0xED reads (keylock, charge window, crash record):
            # each value saved as it reads now, then read back
            for name, (size, readable) in sorted(C.KEPT_KEYS.items()):
                if not readable:
                    continue
                key = name.encode() + b"\0"
                a = rig.read_named(key)
                if not c.eq("0xED %s: n" % name, a.n, size):
                    continue
                value = a.data[:size]
                rig.vendor_set(C.VC_BY_NAME_SET, key + value)   # the value read back: RAM only
                _token(rig, C.VC_BY_NAME_SAVE, name, b"")
                rig.vendor_set(C.VC_BY_NAME_SAVE, key)
                writes["flash_writes"] += 1
                c.eq("0xED %s after the save" % name, rig.read_named(key).data[:size], value)
            c.note("user/keylock, mte/charge_level and debug/resetreason saved as they read; %s"
                   % PINNED)

    if ctx.want("flash.policy") and rig.path == "R":
        with ctx.check(GROUP, "flash.policy") as c:
            m = cproto.parse_mode(rig.console_run("opensc2 mode"))
            policy = m.get("policy")
            c.true("opensc2 mode shows the policy", policy in ("auto", "wireless"))
            token = rig.ctx.add_token("shell", "policy", policy.encode())
            lines = rig.console_run_flash("opensc2 mode %s" % policy, token, policy)
            c.true("policy %s stored, no reboot" % policy,
                   any(ln.strip() == "policy %s stored" % policy for ln in lines))
            writes["flash_writes"] += 1
            c.eq("opensc2 mode again", cproto.parse_mode(rig.console_run("opensc2 mode")).get(
                "policy"), policy)
            if rig.ctx.armed and rig.caps:
                caps = rig.arm()
                c.eq("caps byte 18", caps["policy"], 1 if policy == "wireless" else 0)
            c.note("policy %s restated; %s" % (policy, PINNED))
