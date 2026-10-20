# Security

What counts as a security problem in OpenSC2, what the firmware deliberately allows, and how to
report a problem privately: for users and researchers.

## Reporting

Use GitHub's private vulnerability reporting: the **Security** tab of
[the repository](https://github.com/ded811/OpenSC2-Controller/security), then "Report a
vulnerability". Please do not open a public issue for a security problem. Leave serials, bond IDs
and Steam IDs out of the report, or mask them.

## In scope

- The firmware and the library: anything that lets something other than the PC it is plugged into
  or the puck it is paired with control the board, change what it stores (bonds, identity, the
  mode policy), or read what it should not give out.
- Leaks of identifying data: a serial, a bond ID or a puck serial in the USB session log, in the
  test suite's reports, or in the hooks' ring.
- The PC tools (`tools/hwtest`, `tools/match_steam_version.py`): anything that makes them write to
  a device other than the board they checked, or write to Valve's puck without `--through-puck`.

Bugs in Steam, in Valve's puck or in the nRF Connect SDK belong to their makers.

## What the firmware allows on purpose

- **The hardware test hooks** (`CONFIG_OPENSC2_TEST_HOOKS`, on in the default `app/` build and the
  release `.uf2`, so the test suite checks the very image you run). The key that arms them,
  `0x7E57AB1E`, is published with the code: it prevents accidents, it is not a lock. Anything that
  can send feature reports to the board, or to the puck it is paired with, can arm the hooks and
  inject controller input, which a local program could do anyway by creating input itself. Arming
  lives in RAM and ends at every reboot, or 15 s after the PC goes quiet. One thing outlives a
  reboot: a boot-time injection of the system state machine (`SYS_BOOT`: a power source, a battery
  level, buttons) that the armed hooks stored for the next boot. If the board resets before arming
  ends, that boot makes its power-on decision on the injected source and runs on it unarmed
  (shutdowns then reboot) until a power cycle or an `ARM` and `SYS_INJECT_OFF`; the end of arming
  (`DISARM`, the lease) drops one that no reboot has used yet. While armed, the lizard
  guard keeps the board's mouse and keyboard reports on the board, and inputs are injected only
  with lizard mode off, so injected input does not reach the PC as keys or mouse moves
  ([api.md](docs/api.md#hardware-test-hooks)). For a board you give to someone else, build with
  `CONFIG_OPENSC2_TEST_HOOKS=n`.
- **Identity writes over vendor commands** (`0xEE` with the `opensc2/` keys) are off by default
  (`CONFIG_OPENSC2_IDENTITY_VENDOR_WRITE`): with them on, anyone who can send feature reports to
  the puck could change the board's identity.
- **Steam's own commands** work as on a real controller: Steam, or any program that can send
  feature reports to the board or the puck, can write the bond, the settings and the LED, and can
  reboot the board. The firmware in `app/` ignores Steam's request to enter a bootloader, and so
  does the library when an application has no `power_request` callback.

Back to the [documentation map](docs/README.md).
