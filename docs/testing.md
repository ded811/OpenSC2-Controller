# Testing on hardware: the automated test suite

The automated hardware test suite, for people who change the library, port it, or want to check a
board: what it covers, the rules it keeps, how to run it and the results so far. You don't need
this to use the firmware.

OpenSC2 emulates a Steam Controller 2: buttons, sticks, triggers, trackpads, an IMU, a battery,
rumble and haptics, an LED, the controller settings, lizard mode, the vendor commands Steam uses,
two modes (wired on USB, wireless through Valve's puck) and the switches between them. The test
suite checks all of it on a real board, automatically, **with nothing wired to the board and no
physical interaction**: you start it, it runs for about five to ten minutes, and it tells you, test
by test, what the firmware did right or wrong.

It has two halves that were written together and are checked against each other:

- **the firmware's test hooks** (`CONFIG_OPENSC2_TEST_HOOKS`, built into the blank-canvas app): a
  test channel that lets the PC inject input states, record every output the library produces,
  read counters, and emulate a host on any link ([api.md](api.md#hardware-test-hooks); the bytes
  are in [test-protocol.md](test-protocol.md));
- **the runner** in `tools/hwtest` (Python, standard library only): it finds the board, switches
  it between its modes, drives the hooks, reads what the PC and the puck see, compares everything
  with a model of the library's rules, restores the board and writes a report.

An unarmed board behaves exactly as one built without the hooks (the host tests compare the two
builds' answers to 2,000 commands byte for byte). Nothing happens until the runner arms the hooks,
and arming ends by itself 15 seconds after the PC goes quiet.

## What it covers

Each test runs on one or more **paths**: **W** wired (the PC talks to the board's own USB HID
device), **R** wireless (the board's reports are read through Valve's puck, the test channel goes
over the board's USB console), **P** through the puck (opt-in, below) and **S** with Steam open
(opt-in). `python -I tools\hwtest\run.py --list` prints every test with its paths. In the console
and the report, a test carries the mode it ran in after an `@`: `@W` or `@R`, also for the P and S
tests (for example `puck.output@R`, `steam.wired_passive@W`).

| Group | Paths | What it checks |
| --- | --- | --- |
| `hooks` | W R | the hooks themselves: present (a warning for the charger test build, whose wired boots go wireless without a host, unlike the real controller); invisible while unarmed (a wrong key, a wrong version, a short request and a keyed request all STALL and leave the stored request alone); arming and its first ring entries; the **guard canary**; the settings restore at the end of arming; the lease; the DISARM answer; the ring (wrapping, eviction count, paging); the paging of counters and settings |
| `desc` | W | the USB identity as Windows sees it: VID:PID and version, the three strings (the product string is the board's configured name, "Steam Controller" unless one is stored: checked against the CRC-32 of the name the hooks' caps carry, and against `opensc2 id` once the run has read the console), the collections, the HID capabilities against the real controller's descriptors |
| `cmd` | W (R) | the vendor commands Steam sends: `0x83` attributes, `0xAE` strings, `0xF2` device info, `0xA1` reset info, `0xDB` the chime level, `0xED` reads by name, the refusal forms of the writes (refused before anything is written), the `0xE2` forms Steam sends and one that must be refused, unknown opcodes STALL, the framing rules (length, report IDs other than 1, the control pipe's output and input reports), the stored request, turn-off ignored while wired, the first GET after a boot; on R the lizard commands refused while the radio link is down |
| `inputs` | W R | every button bit, both sticks, both triggers (analog and the digital bits with setting 68's hysteresis), both trackpads (position, touch, pressure), every IMU field, random full states, the latency from injection to report, the **lizard lock** (an injection is refused while lizard mode is on and stopped in the frame lizard mode comes back), an injected STEAM button that changes nothing |
| `battery` | W R | the library's default `0x43` byte for byte, the 3.5 s cadence, injected values in `0x43` and `0xBE`, the level clamp, back to the default |
| `stream` | W R | the report IDs, the state report cadence (about 248 a second wired), the sequence numbers and every hole explained by the counters; `--soak S` for a long run |
| `reports` | W R | the haptic stream status `0x44` and raw reports an application sends; on R the puck's exact-length filter |
| `outputs` | W R | rumble (`0x80`) decoded field by field, the 500 ms rumble timeout, rumble stopped when the link goes down, every haptic report the library decodes (`0x81`..`0x89`, PCM data included), `0x8A` ignored, short reports refused, padded reports equal to exact ones |
| `haptics` | W R | only on a board built with the haptics synthesiser (`app/haptics.conf`; SKIP on the default image): the CRCs of the blocks it renders for every record of the shared vectors (`tests/vectors/hwtest/haptics.txt`: tones, an LFO, a sweep in antiphase, pulse trains, both clicks, the rumble emulation with its seeded random bursts, settings 76 and 27), `0x81`'s special cases, how long a tone lasts and its blocks' first and last flags, the rumble emulation's generators and gains, a script's steps and the one-script rule, cues at each chime level, a PCM stream's flags (`0x10`, `0x20`, `0x08`, `0x04`, `0x02`, `0x01`, from the ring and as `0x44` reports where the board's reports can be read), a stop emptying its ring, the drop sounds' 250 ms and 5 s, and settings 75 and 76 |
| `settings` | W R | the LED colour and LED settings, `0x8E` defaults, the clamps of the ranged settings, changes and invalid numbers, the Steam watchdog (lizard mode and the defaults back 10 s after the last `0x81`), its re-arm, no watchdog while setting 71 is 0 |
| `modes` | W R | a plain reboot, the reboot into wireless (`0x95` with Steam's key), the USB session log, a warm restart, back to wired, the identity equal on both paths (the product name too), back to the mode the board was found in |
| `wireless` | R | which of the puck's slots carries the board (an injected signature), the puck's link status and link quality reports, the identity sent on connect, reconnecting after `opensc2 stop`/`start`, a lost link (the radio made deaf for 800 ms), the turn-off requests |
| `restore` | all | always last and always whole (whatever `--test` selects), also after a failure or Ctrl-C: outputs and injection off, settings and LED colour written back, disarmed, every link's stored request as after a boot, and the board **as found** (mode, bonds, identity, slot, chime level, mode policy, the settings and the LED colour) |
| `flash` | W (R) | opt-in `--flash`: the flash writes Steam makes when it pairs a controller, rehearsed with the values the board already holds (its bond, the chime level, the transport, `0xA2`, deleting an empty bond slot, restating the mode policy); see [the note below](#what---flash-writes) |
| `lizard` | W R | opt-in `--lizard`: lizard mode on injected inputs (keyboard keys, pointer, scroll, mouse buttons, thumb on the stick, lizard mode off and back), with only the guard keeping its mouse and keyboard reports on the board. It runs last on each path, and only when everything before it passed, `inputs.lizard_lock` passed on that path, and the leak detector watches that path (otherwise `--lizard` is refused there, exit code 2) |
| `puck` | P | opt-in `--through-puck`: rumble, haptics, vendor commands and the test channel through the puck ([below](#the---through-puck-opt-in)) |
| `steam` | S | opt-in `--with-steam`: what Steam sends to the board through the puck, the lines it logs on a reconnect (masked), the wired stream with Steam open; passive |

Because nothing is wired, the board's own inputs read neutral; the inputs come from the test
channel, and the outputs (rumble, haptics, LED) show up in the hooks' ring, entry by entry, with the
fields the library decoded; with the haptics synthesiser, what it renders, as CRCs of its blocks.
Not covered: the physical inputs and outputs of a board that has them (the samples' drivers,
the synthesiser's I2S, PWM and DRV2605 outputs), the USB cable rules (unplugging, plugging in),
Bluetooth and the dock.

## What it needs

- **The board** flashed with the blank-canvas app (`app/`, hooks on by default), on the PC's USB.
- **Valve's puck** plugged in, for the wireless phase (without it that phase runs only what needs
  no link and skips the rest, with the reason). **Only one puck**: with a second `28DE:1304` device
  connected (another puck, or a puck-like development board) the runner cannot tell their slots
  apart and refuses the wireless slot proof (exit code 2, the pucks listed by their USB port).
- **The real controller switched off and unplugged.** The runner never opens a `28DE:1302` whose
  serial does not start with `OSC`, but a second controller on the puck makes the slots harder to
  tell apart.
- **Steam closed** (or `--steam-control`, which lets the runner close and start it), and nothing
  else that reads controllers: games, controller-mapping tools, a browser tab using WebHID, a
  terminal connected to the board's console. The runner refuses to inject while another program
  holds the collection it reads (it says which one).
- **Windows 10 or 11** (tested on Windows 11) and **Python 3** (tested with 3.11; standard library
  and `ctypes` only, no packages), in a **normal, not elevated** terminal in the repository folder.
  `--selftest` and `--dry-run` also run on Linux and macOS.

## The safety rules it enforces

They are rules in code, each with its own self-test, not conventions:

- **It writes only to the board.** HID devices `28DE:1302` whose USB serial is 13 characters
  starting with `OSC`, and serial ports with the USB ID `2FE3:0004` and the product string
  "OpenSC2 console" that answer the OpenSC2 handshake. Before the first byte goes out, the HID serial string must
  equal the serial in the device's instance ID. Everything else is refused; a `28DE:1302` with
  another serial (a real controller) is never opened at all. The rules are checked again in the
  functions that actually send, on the exact bytes: every feature report, output report and
  console line, so no caller can go around them.
- **Valve's puck is read-only.** Its four slot collections are opened with read access only, and
  the reader's module binds no function that can send anything (a self-test parses its source and
  fails on any other name). Writing to the puck needs `--through-puck`, and then goes only to the
  exact slot that carried the board's signature (proved within the last 10 s); even then
  **feature report 2, the puck's own commands, is never built**, and of the test channel only
  reading requests go through the puck.
- **Injected inputs never reach the PC as keys or mouse moves.** Before every injection: the hooks
  armed with the guard on, this arming's **guard canary** passed (an all-zero mouse report and an
  empty keyboard report sent through the library must come back diverted into the ring), Steam
  closed, the runner the only reader of the collection the reports appear on, and lizard mode off
  as the board itself reports it, refreshed with a `0x81` every 5 s while an injection runs (not in
  the `lizard` group, whose sequences need lizard mode on). An injection that meets an ended lease
  is not sent again until all of this was checked for the new arming. The firmware adds its own
  layer: it refuses an injection while lizard mode is on and stops one in the frame lizard mode
  comes back. A **leak detector** watches which device raw-input events come from (never the keys
  or the motion): an event from the board's own mouse or keyboard collection (wired) or from the
  proven puck slot's (wireless) while armed stops all injection at once. Events while the runner is
  not armed are lizard mode's own and do not count, and neither do the empty keyboard and mouse
  reports the puck sends when the runner itself takes the link down (they are counted as
  `leak_ignored`). If the detector cannot see both collections of a path listed as raw-input
  devices, it is off there, and `--lizard` is refused on that path. If the canary fails, the
  firmware takes an injection it should refuse or does not stop one when lizard mode comes on, or
  the leak detector fires, nothing is injected for the rest of the run (exit code 2).
- **It never touches what it cannot put back.** Never the bootloader (`0x90`, `0x95` with the
  bootloader key, the shell's `uf2`), never a factory reset (`0x86`), never a calibration erase,
  never a write to the identity, the bonds or the mode policy, except the value-preserving rewrites
  of `--flash`, each allowed by a one-shot token for exactly the value just read back. The console
  accepts only an allowlist of commands, each exactly as the runner builds it (a line with a
  control character, which could start a second command, is never sent).
- **It leaves the board as it found it.** A snapshot at the start (the 86 settings, the LED colour,
  a CRC-32 of each bond, a hash of the identity, the slot, the chime level, the policy), the
  restore at the end (also after a failure or Ctrl-C), and a comparison: any difference is exit
  code 6. The runner arms the hooks so that the end of arming writes the settings back; if the
  runner dies, the lease runs out within 15 s and the firmware does it by itself. The restore ends
  arming first (so that the firmware's own write-back is done), then arms without that write-back,
  writes the start snapshot and checks it, and the final comparison reads the settings once more
  after the last disarm. After Steam was open the runner also arms without the write-back, which
  would otherwise bring back Steam's settings.
- **Nothing identifying is printed or saved.** Serials, bond IDs (in every form a log may print
  them, once the run has read the bond), the chip ID, the console's USB serial, device instance
  paths and user folders (also inside error messages) are masked in every line and every file, and
  no raw hex of an answer is ever written (any run of 16 or more hex digits that slipped through is
  replaced, and counted as a warning). Of Steam's log only lines with known phrases about the board
  are kept, and its bond lines lose their bond (`<bond>`, or `empty`). A product name stored on the
  board (`opensc2 id set name`) is compared in memory; the results show its length only (and
  CRC-32s when it does not match).

## How to run it

From the repository folder, in PowerShell:

```powershell
python -I tools\hwtest\run.py --selftest            # the runner's own tests, no device
python -I tools\hwtest\run.py --dry-run             # the whole suite against a simulator, no device
python -I tools\hwtest\run.py --list                # every test, its paths and what it needs
python -I tools\hwtest\run.py                       # the full run on the board
```

`powershell -NoProfile -ExecutionPolicy Bypass -File tests\host\run.ps1` on Windows, or
`sh tests/host/run.sh` on Linux and macOS, builds the C host tests and a **firmware emulator**, the
library's own command channel and hooks on the PC, and then runs the self-tests and a dry run of
the suite against that emulator. Today that is 80 host tests (6,409 checks); 109 (8,871 checks) with
the hardware test hooks built in, and the two builds answer 2,000 unarmed commands byte for byte
alike; the runner's 215 self-tests; its dry run against the library's own code in a firmware
emulator, which has the haptics synthesiser built in (as `app/haptics.conf`), so the `haptics`
group runs there too; 9 self-tests of `tools/match_steam_version.py`. To run the emulator by hand
after `run.ps1` built it:

```powershell
python -I tools\hwtest\run.py --dry-run --fw-emu $env:TEMP\opensc2_fwemu.exe --lizard
```

The host tests need a C compiler for the PC itself: the compiler of the nRF Connect SDK's toolchain
builds for the nRF chips, not for the PC. They are tested with gcc; clang is not tested.

| PC | The compiler |
| --- | --- |
| Windows | a gcc on PATH, for example MSYS2's ucrt64 gcc (`pacman -S mingw-w64-ucrt-x86_64-gcc` in MSYS2; tested with gcc 13.1), or name it: `powershell -NoProfile -ExecutionPolicy Bypass -File tests\host\run.ps1 -Cc C:\msys64\ucrt64\bin\gcc.exe` |
| Linux | the distribution's gcc (`run.sh` calls `gcc`; `CC=...` names another) |
| macOS | `gcc` there is Apple's clang (untested): install GCC (`brew install gcc` names it `gcc-NN`, after its version) and run `CC=gcc-NN sh tests/host/run.sh`, or accept clang |

Optional, on Linux: the same pure test cases as a ztest suite, `west twister -T tests -p native_sim`
in the SDK's shell (native_sim needs a Linux PC; not part of `run.ps1` or `run.sh`).

**The first time on hardware**, go step by step and stop at the first surprise. Steam closed, the
board wired (just plugged in), a harmless window focused (an empty Explorer window, not an editor):

```powershell
python -I tools\hwtest\run.py --wired-only --test desc.ids,desc.strings,desc.caps
python -I tools\hwtest\run.py --wired-only --test hooks.guard          # the canary, nothing injected
python -I tools\hwtest\run.py --wired-only --test inputs.lizard_lock
python -I tools\hwtest\run.py --wired-only --test inputs.buttons
python -I tools\hwtest\run.py --wired-only --group inputs
python -I tools\hwtest\run.py --wireless-only --group wireless
python -I tools\hwtest\run.py                                          # the full run
```

Then the opt-ins, one at a time:

```powershell
python -I tools\hwtest\run.py --flash --through-puck
python -I tools\hwtest\run.py --lizard
python -I tools\hwtest\run.py --with-steam --steam-control
```

| Option | |
| --- | --- |
| `--group G[,G...]`, `--test ID[,ID...]` | only these (the start snapshot, the switches they need and the whole restore always run) |
| `--wired-only`, `--wireless-only` | one path; the board is switched there first if needed, and back at the end |
| `--through-puck` | allow writes through the puck to the board's slot ([below](#the---through-puck-opt-in)) |
| `--flash` | allow the value-preserving flash rewrites of group `flash` ([what they write](#what---flash-writes)) |
| `--lizard` | run group `lizard`: lizard mode on injected inputs, only the guard holding its reports back (needs the canary, the leak detector watching that path, and `inputs.lizard_lock` passed on it in the same run, for example `--group hooks,inputs,lizard`) |
| `--with-steam`, `--steam-control` | the passive `steam` group at the end; with `--steam-control` the runner starts and closes Steam itself, without it you start Steam when asked |
| `--soak S` | `stream.soak` for S seconds |
| `--board N` | the board to use when more than one is connected (the runner lists them) |
| `--timeout-scale F` | multiply every timeout (a slow PC, a busy USB hub) |
| `--max-minutes N`, `--fail-fast` | stop after N minutes, or at the first failure; the restore still runs |
| `--yes` | never wait for you: with Steam open the run stops (exit 2) instead of asking you to close it |
| `--results DIR` | where the results folder goes (default `tools\hwtest\results`, git-ignored). `report.json` names a folder of your own only as `(--results)`, not by its path |
| `--dry-run`, `--sim-fault NAME`, `--fw-emu EXE` | offline: against the simulator (with one injected fault), or against the firmware emulator |
| `-v` | also print the board's console transcript at the end (masked, decoded) |

What it does to the board: it switches it between wired and wireless and back (about ten seconds
each time), arms the hooks in RAM, injects inputs, writes settings in RAM and restores them. It
needs no attention while it runs, unless Steam is open (it asks you to close it) or another
program holds the board's console.

### Results so far

Tested on a SuperMini nRF52840 with nothing wired to it, with Valve's puck on its October 2026
firmware and Steam on a Windows PC.

On the v0.2.0 release image every test passed (0 failed, 0 errors, 0 warnings) with every opt-in
on, `--through-puck --flash --lizard --with-steam --steam-control`: 165 of 165 in a run that
started with the board wireless, and 31 of 31 for `--group modes,cmd` in a run that started wired,
which adds the four tests that only run from a wired start (`modes.plain_reboot`,
`modes.to_wireless`, `modes.session_log`, `cmd.first_get`). Before the release, a development
build passed 169 of 169 in one run started wired. Only the optional soak test (`--soak S`), which
needs a duration, was not run. The suite counts one test per path (wired,
wireless, through the puck, with Steam); each test makes its own checks. In the report, the tests
of the through-puck and Steam paths carry the mode they ran in (`@R` or `@W`).

| | |
| --- | --- |
| Date | 2026-10-19 |
| PC | Windows 11, Python 3.11 |
| Firmware | the v0.2.0 release image `opensc2-supermini-nrf52840-v0.2.0.uf2` (revision string `osc2-0.2.0`, build time `0x6AD6E287`, test hooks on), with the identity cleared to the built-in one |
| Report | started wireless: `PASS 165 FAIL 0 WARN 0 ERROR 0 SKIP 2`; started wired, `--group modes,cmd`: `PASS 31 FAIL 0 WARN 0 ERROR 0 SKIP 0`; exit code 0 both times, the board left as found |

What it covered: inputs injected and checked wired and through the puck, battery, rumble and
haptics, LEDs, settings, all vendor commands Steam sends, lizard mode with nothing reaching the PC,
reboots, mode switches, reconnects, and a run with Steam open. Not covered: real buttons, sticks,
trackpads, IMU, rumble motor, RGB LED and battery hardware, which no test has run on
([What works](../README.md#what-works)). A release image is checked the same way before it is
tagged ([releasing.md](releasing.md)).

### What `--flash` writes

The `flash` rewrites store the values the board reports, and the board reports a stored value and
the build's default alike. So where nothing was stored yet, `flash.chime`, `flash.transport` and
`flash.policy` store the default in effect (the chime level, the selected bond slot, the mode
policy), and the board keeps that value from then on, also under a later build with another
default. Nothing in effect changes, and Steam stores the chime level and the bond itself when it
pairs the controller, but the stored state can differ afterwards; `restore.as_found` compares the
values in effect and does not see it. Each of these rows says so in its note, and every write
counts in `report.json` (`safety.flash_writes`).

## How to read the report

The runner prints one line per test and a summary:

```text
PASS  inputs.buttons@W                 1.84s  32 single bits, all 30, none: 34 reports as the model
FAIL  battery.values@R                 2.31s  charge state 0x00: a 0x43 with these values within 500 ms
SKIP  puck.output@R                    0.00s  opt-in (--through-puck)
```

That is the status, the test with the path it ran on, how long it took and a short reason. Then a
line per group, the totals and the exit code; the path of the results folder comes last.

| Status | Meaning |
| --- | --- |
| PASS | the firmware did what the library's rules say |
| FAIL | it did something else: the reason says what was expected and what came. "no link": the board's radio link through the puck did not connect, also after one `opensc2 stop` and `start` (is it paired with this puck?); the rest of the wireless phase is then skipped |
| ERROR | the test could not finish: the board reset ("board reset", with the reset cause), an answer that did not belong to the request, a refusal where none was expected, or a safety precondition. The run goes on with the next group where it safely can |
| WARN | passed, with a remark (a timing at the edge, the shell dropping console input) |
| SKIP | not run, and why: an opt-in, a path that does not apply, no puck, a firmware without hooks, or no injection after a safety layer failed |

| Exit code | Meaning |
| --- | --- |
| 0 | every test that ran passed (warnings and skips allowed) |
| 1 | at least one test failed or could not finish |
| 2 | a precondition or a safety refusal: no board, Steam open with `--yes`, another program reading the board's or a slot's collection, Windows not enforcing the sharing, two pucks connected, the guard canary failed, the firmware took an injection while lizard mode was on or did not stop one when lizard mode came on, the leak detector fired, `--lizard` on a path the leak detector cannot watch, the console held by another program |
| 3 | an environment or device error: not Windows, the board gone and not found again (unplug and replug it), the UF2 bootloader appeared |
| 4 | a usage error (bad options) |
| 5 | interrupted with Ctrl-C (the restore was attempted) |
| 6 | the board was not left as found, or the restore failed: the report says what differs. A setting: a reboot of the board restores it. A bond: add the controller in Steam again |

The results folder `tools\hwtest\results\<date-time>\` holds:

- `report.md`: the same tables, failures first, then every group, the safety counters and what was
  left as found;
- `report.json`: everything, for a script: the options, the host, the board (masked) and its caps,
  the puck's slot, every test with what it expected and observed and its ring excerpt, the safety
  counters (refusals, `puck_writes`, `flash_writes`, `hex_dropped`, `leak_events`, `leak_ignored`,
  `canary`, `sole_reader`, `lease_lost`), `left_as_found` (mode, bonds, identity, slot, chime,
  policy, settings, LED colour) and the exit code;
- `console.log`: the lines above, every `[through-puck]` write, and the board's console transcript
  with the test channel's hex decoded;
- `ring.jsonl`: the decoded ring entries the tests kept as evidence.

Everything in them is masked; they are safe to attach to an issue.

## The `--through-puck` opt-in

By default nothing is ever written to Valve's puck: its slots are only read. With `--through-puck`
the `puck` group also checks the path a game or Steam uses through the puck: the puck forwards
output reports and feature report 1 to the board over the radio. The runner then writes, **only to
the slot proven to be the board's within the last 10 seconds** (it injects a signature and finds
it), **only with Steam closed**, and **never feature report 2** (the puck's own commands):

| Test | What goes to the puck (for the board) |
| --- | --- |
| `puck.output` | three output reports: a rumble (`0x80`), a haptic pulse (`0x81`), a rumble stop (an all-zero `0x80`) |
| `puck.feature` | feature report 1 with vendor commands `0xF2 02` (device info) and `0x89 2D` (read setting 45), each followed by the puck's GET of the answer |
| `puck.cache` | `0x83` (attributes), `0xAE 01` and `0xAE 03` (the unit serial, compared in memory and masked, and the git revision) |
| `puck.refused` | `0xC1` with Steam's 16 bytes, which the board must refuse |
| `puck.test_channel` | one test-channel `STATUS` (`0x7E` with the key); only reading test ops may go through the puck |
| the restore | when the run ends wireless, one more `0x83`, so the slot's stored request is as after a boot (sent before the restore disarms the hooks: renewing the slot's proof needs them) |

Every one of these writes is printed as a `[through-puck]` line, kept in `console.log`, and counted
in `report.json` (`safety.puck_writes`).

## Tests of the test suite

- **Self-tests** (`--selftest`, 207 tests): the wire formats, the expected-report and lizard models,
  the test channel's codec, the console grammar, the masking, every safety rule (including the
  source scan of the puck reader, and that every function that sends checks the exact bytes), the
  link lock against a racing keep-alive, the leak detector's filter and watch list, the Windows
  backend's slot handling on synthetic device lists (nothing opened), the writers, and the whole
  runner against the simulator.
- **The dry run** (`--dry-run`): every group against a simulator with a virtual clock (seconds,
  not minutes), which models the board in both modes, the puck, the console with log noise, Steam
  and the reboots. `--sim-fault NAME` makes it misbehave in exactly one way, and each fault must
  give its own failing test and exit code: `guard_leak`, `seq_gap`, `trigger_bit`,
  `stall_missing`, `no_reconnect`, `stale_battery`, `lease_ignored`, `not_as_found`,
  `steam_appears`, `lizard_ignored`, `second_reader`, `share_not_enforced`, `leak_event`,
  `board_reset`, `wrong_mode`, `puck_reset`, `com_renumbered`, `console_busy`, `keepalive_race`,
  `answer_dropped`.
- **The firmware emulator** (`--fw-emu`): the same groups against the library's own code
  (commands, test channel, ring, injection, lizard mode, the battery and state reports), so the
  runner and the firmware cannot drift apart unnoticed.
- **The shared vectors** in `tests/vectors/hwtest/`: the C host tests and the runner's self-tests
  read the same files (the state report rules, lizard mode, ring entries, the test channel, the
  settings), so both sides agree on every byte.

Back to the [documentation map](README.md).
