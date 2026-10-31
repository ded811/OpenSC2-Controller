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
over the board's USB console), **P** through the puck (opt-in, below), **S** with Steam open
(opt-in) and **B** over Bluetooth (opt-in `--bluetooth`, a run of its own: [Bluetooth](#bluetooth)).
`python -I tools\hwtest\run.py --list` prints every test with its paths. In the console and the
report, a test carries the mode it ran in after an `@`: `@W`, `@R` or `@B`, also for the P and S
tests (for example `puck.output@R`, `steam.wired_passive@W`).

| Group | Paths | What it checks |
| --- | --- | --- |
| `hooks` | W R B | the hooks themselves: present (a warning for the charger test build, whose wired boots go wireless without a host, unlike the real controller); invisible while unarmed (a wrong key, a wrong version, a short request and a keyed request all STALL and leave the stored request alone); arming and its first ring entries; the **guard canary**; the settings restore at the end of arming; the lease; the DISARM answer; the ring (wrapping, eviction count, paging); the paging of counters and settings |
| `desc` | W B | the USB identity as Windows sees it: VID:PID and version, the three strings (the product string is the board's configured name, "Steam Controller" unless one is stored: checked against the CRC-32 of the name the hooks' caps carry, and against `opensc2 id` once the run has read the console), the collections, the HID capabilities against the real controller's descriptors; on B the Bluetooth HID device: `28DE:1303` version `0x0100` (the PnP ID), the same collections and capabilities, the strings reported |
| `cmd` | W B (R) | the vendor commands Steam sends: `0x83` attributes, `0xAE` strings, `0xF2` device info (selector 1, the crash record, only when it is not new: reading a new one writes flash), `0xA1` reset info (18 bytes, the real controller's flag encoding), `0xDB` the chime level, `0xED` reads by name (every key of the real controller: the kept keys at their size, zeros until set, or empty without a read function; the calibration keys empty), the refusal forms of the writes (refused before anything is written; another size for the kept keys), the kept keys rewritten with the value just read (RAM only), the `0xE2` forms Steam sends and one that must be refused, unknown opcodes STALL, the framing rules (length, report IDs other than 1, the control pipe's output and input reports), the stored request, turn-off ignored while wired, the first GET after a boot; on R the lizard commands refused while the radio link is down; on B the same commands over Bluetooth's feature report 1 (a refusal is an ATT error there), attribute 11 the connection interval, without the USB framing and never `0x9F` |
| `inputs` | W R B | every button bit, both sticks, both triggers (analog and the digital bits with setting 68's hysteresis), both trackpads (position, touch, pressure), every IMU field, random full states, the rules on every report (the stick-touch fallback at its radius, setting 82's blanking for each of its values, no trigger bit with raw triggers, the click by pressure with its thresholds, switched on for the test where the build has it off), the blank canvas's raw stick output (setting 46: counts and millivolts of the biased left-stick pins, the one test of the application's own state), the latency from injection to report, the **lizard lock** (an injection is refused while lizard mode is on and stopped in the frame lizard mode comes back), an injected STEAM button that changes nothing |
| `battery` | W R B | the library's default `0x43` byte for byte, the 3.5 s cadence, injected values in `0x43` and `0xBE` (`0xBE`'s byte 13 is 1 while charging and byte 14 the charge state without the `0x80` flag), the level clamp, back to the default |
| `stream` | W R B | the report IDs, the state report cadence (about 248 a second wired; on B one a connection event, the rate the connection interval gives, and only `0x45` and `0x43` arriving), the sequence numbers and every hole explained by the counters; `--soak S` for a long run |
| `reports` | W R B | the haptic stream status `0x44` and raw reports an application sends; on R the puck's exact-length filter |
| `outputs` | W R B | rumble (`0x80`) decoded field by field, the 500 ms rumble timeout, rumble stopped when the link goes down, every haptic report the library decodes (`0x81`..`0x89`, PCM data included), `0x8A` ignored, short reports refused, padded reports equal to exact ones, the real controller's `0x44 {channel, 0x02}` replies to every stop (Steam's zero pulses, `0x82` stop all, zero rumble speeds, `0x86`'s stream stop) in its order, in the ring and on the link (none in a build without `CONFIG_OPENSC2_HAPTIC_STOP_STATUS`, caps bit 16) |
| `haptics` | W R | only on a board built with the haptics synthesiser (`app/haptics.conf`; SKIP on the default image): the CRCs of the blocks it renders for every record of the shared vectors (`tests/vectors/hwtest/haptics.txt`: tones, an LFO, a sweep in antiphase, pulse trains, both clicks, the rumble emulation with its seeded random bursts, settings 76 and 27), `0x81`'s special cases, how long a tone lasts and its blocks' first and last flags, the rumble emulation's generators and gains, a script's steps and the one-script rule, cues at each chime level, a PCM stream's flags (`0x10`, `0x20`, `0x08`, `0x04`, `0x02`, `0x01`, from the ring and as `0x44` reports where the board's reports can be read), a stop emptying its ring, the drop sounds' 250 ms and 5 s, and settings 75 and 76 |
| `settings` | W R B | the LED colour (and `0xE9`'s read-back as the real controller's: a channel reads 0 unless it is 100) and LED settings, `0x89`'s answers for the IMU's settings 48, 84 and 85 (their defaults, while the application sees what was written), `0x8E` defaults, the clamps of the ranged settings, changes and invalid numbers, the Steam watchdog (lizard mode and the defaults back 10 s after the last `0x81`), its re-arm, no watchdog while setting 71 is 0 |
| `modes` | W R B | a plain reboot, the reboot into wireless (`0x95` with Steam's key), the USB session log, a warm restart, back to wired, the identity equal on both paths (the product name too), back to the mode the board was found in; on B a warm restart that comes back on Bluetooth (Windows connects the board again) and the identity read over Bluetooth |
| `wireless` | R | which of the puck's slots carries the board (an injected signature), the puck's link status and link quality reports, the identity sent on connect, reconnecting after `opensc2 stop`/`start`, a lost link (the radio made deaf for 800 ms), the turn-off requests |
| `ble` | B | the Bluetooth link: connected and past its gate (encrypted, ATT MTU 64 or more), the identity in use and its bond, the connection interval, the name and address the console shows; the state report cut to `0x45` (no quaternion); feature report 1 read as the real controller answers it (after a `0xAE` request longer than its answer, the request's own bytes follow the answer, then the SET's zeros; the stored request answers again); a lost link (the board drops it with reason `0x08`): the same identity advertises again 500 ms later, directed to its host, and Windows connects it again; the host ending the link (`0x13`): wireless off and the `WIRELESS_OFF` power request, and `opensc2 start` brings it back; with `--flash` the 60 s advertising window, shortened to 3 s, running out in pairing mode on the slot's other identity while the slot's host keeps its bond |
| `cal` | W R | builds with `CONFIG_OPENSC2_CAL` (caps bit 21): raw inputs given with the `RAW_INJECT` test op and processed by the library as an application's would be: uncalibrated sticks, triggers and pad pressure read 0, settings 46, 62 and 60 give millivolts or counts, the stick-touch and grip bits follow the means of their words with the hysteresis of settings 30, 31, 34 and 35, a change of setting 34 starts them over; on injected pads the rotation (settings 2 and 24) and the touch stabiliser (setting 80 = 2); `RAW_OFF`. It writes no calibration (safety rule S5): a board that holds a calibration skips the uncalibrated checks |
| `system` | W R | the [system state machine](api.md#power-and-the-system-state-machine), through the hooks' system injection (buttons, a battery level and a power source that reach the firmware's rules, never a report), Steam closed: the power-on state and its cue; battery operation with the link kept and back on USB power; the turn-off command, a link that gave up and STEAM held 7 s (its 20 ms and 7 s timings) switching wireless off on USB power, and `opensc2 start` back on; a STEAM press then rebooting into wired; the PC-asleep cue, once; the idle monitor on USB power (nothing acts on it); on an injected battery with setting 77 = 1, so that each shutdown reboots where the suite can see it: STEAM held 7 s, the idle timeout (its 16 to 17 periods), 0 % (the low-battery shutdown), the power-on chord with and without its 4 s pairing hold (a boot-time injection), the keylock (`--flash`); wired, the cable pulled (an injected battery). Each shutdown's end is read back after the reboot. Built, not yet run on hardware |
| `restore` | all | always last and always whole (whatever `--test` selects), also after a failure or Ctrl-C: outputs and injection off (the system injection too, and no boot-time injection left for the board's next boot), settings and LED colour written back, disarmed, every link's stored request as after a boot, and the board **as found** (mode, bonds, identity, slot, chime level, mode policy, the settings and the LED colour) |
| `flash` | W (R) | opt-in `--flash`: the flash writes Steam makes when it pairs a controller, rehearsed with the values the board already holds (its bond, the chime level, the transport, `0xA2`, deleting an empty bond slot, the kept keys `user/keylock`, `mte/charge_level` and `debug/resetreason` saved as they read, restating the mode policy); see [the note below](#what---flash-writes) |
| `lizard` | W R B | opt-in `--lizard`: lizard mode on injected inputs (keyboard keys, pointer, scroll, mouse buttons, also from the click by pressure switched on for the test, thumb on the stick by its sensor's bit or the stick-touch fallback, lizard mode off and back, with its processing switched off), then its processing part by part (the touch stabilisers with setting 80 at 0, 1 and 2, the 8-frame average, the glide after a flick and what ends it: a new touch, `0x81`, an `ARM`, and the haptic ticks and clicks) and all together as the build has it, each sequence followed until lizard mode goes quiet, with only the guard keeping its mouse and keyboard reports on the board. It runs last on each path (before the `system` group, whose reboots come at the very end), and only when everything before it passed, `inputs.lizard_lock` passed on that path, and the leak detector watches that path (otherwise `--lizard` is refused there, exit code 2) |
| `puck` | P | opt-in `--through-puck`: rumble, haptics, vendor commands and the test channel through the puck ([below](#the---through-puck-opt-in)) |
| `dock` | W R | opt-in `--dock`: the board's dock link, fed 73-byte frames through the test hooks as if the puck had sent them over its pads, nothing wired ([below](#the-dock)): the frame rules (the CRC, a short frame, nothing arriving, the start byte ignored, a type the puck never sends), the probe's identity record against the board's identity (its serials by CRC-32 only), commands on the dock's own stored request, `0x81` and `0x85` doing nothing there, GET input from the stream's latest report without taking a sequence number, every answer within 50 ms; on R the dock alone with the radio stopped (attribute 11 = 64000, one new report per request, no `0x43`, `0xBE` answered); the pilot's 2-of-3 rule; the docked states on injected supply and events, recorded and not acted on |
| `steam` | S | opt-in `--with-steam`: what Steam sends to the board through the puck (the vendor commands, and `steam.session` counts the output reports by ID, rumble, haptic commands by kind, PCM bytes and stops, and checks the `0x44` reply to each stop; the counts go into `report.json`), the lines it logs on a reconnect (masked), the wired stream with Steam open; passive |

Because nothing is wired, the board's own inputs read neutral; the inputs come from the test
channel, and the outputs (rumble, haptics, LED) show up in the hooks' ring, entry by entry, with the
fields the library decoded; with the haptics synthesiser, what it renders, as CRCs of its blocks.
Not covered: the physical inputs and outputs of a board that has them (the samples' drivers, the
synthesiser's I2S, PWM and DRV2605 outputs), the USB cable rules on a real cable (unplugging,
plugging in: the `system` group injects the power source instead), a real power-off (the board has
no battery, so every shutdown reboots) and the dock's wire (its UART and pilot: the `dock` group
reaches the dock link through the test hooks only). Bluetooth is a run of its own, `--bluetooth`
([Bluetooth](#bluetooth)); of it the suite does not cover Valve's vendor GATT service, other hosts
(phones, SDL) or Steam over Bluetooth, which are checked by hand.

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
- **For `--bluetooth`**: the board flashed with the Bluetooth build (`app/ble.conf`), wireless on
  its Bluetooth transport and paired with Windows, and the PC's Bluetooth on; no puck needed. A real
  controller paired with the same PC is never opened, but switched off is simpler
  ([Bluetooth](#bluetooth)).

## The safety rules it enforces

They are rules in code, each with its own self-test, not conventions:

- **It writes only to the board.** HID devices `28DE:1302` whose USB serial is 13 characters
  starting with `OSC`, and serial ports with the USB ID `2FE3:0004` and the product string
  "OpenSC2 console" that answer the OpenSC2 handshake. Before the first byte goes out, the HID serial string must
  equal the serial in the device's instance ID. Everything else is refused; a `28DE:1302` with
  another serial (a real controller) is never opened at all. Over Bluetooth (`--bluetooth`), where
  the board and a real controller both appear as `28DE:1303` without a USB serial, only the vendor
  collection of the Bluetooth device with the address the board's own console reports for the
  identity it advertises on, and with the name "Steam Ctrl (BT) " and the board's `OSC` serial, is
  the board's; every other Bluetooth device is never opened, and after opening the HID serial string
  must name nothing else. The rules are checked again in the functions that actually send, on the
  exact bytes: every feature report, output report and console line, so no caller can go around
  them.
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
  the leak detector fires, nothing is injected for the rest of the run (exit code 2). The system
  injection of the `system` group reaches only the firmware's rules, never a report, so the reader
  and lizard-mode conditions do not apply to it; it still needs the hooks armed with the guard on,
  this arming's canary passed and Steam closed (the shutdowns and reboots it causes would disturb a
  Steam session), and while the hooks are armed the firmware turns every power-off into a reboot.
- **It never touches what it cannot put back.** Never the bootloader (`0x90`, `0x95` with the
  bootloader key, the shell's `uf2`), never a factory reset (`0x86`), never a calibration recording
  or erase (`0xD8`, `0xC0`, `0xC3`, `0xFE`; `0xE2` only in forms that save nothing), never a write,
  save or delete of a `cal/` key (not even with a token), never a write to the identity, the bonds
  or the mode policy, except the value-preserving rewrites of `--flash`, each allowed by a one-shot
  token for exactly the value just read back. The console
  accepts only an allowlist of commands, each exactly as the runner builds it (a line with a
  control character, which could start a second command, is never sent). A dock frame that runs a
  vendor command (a SET) is held to the test link's allowlist, where the frame is built and again
  where it leaves the PC, and the firmware checks it a third time; dock frames never go through
  the puck, and nothing is ever connected to the puck's pads ([The dock](#the-dock)).
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
  them, once the run has read the bond), the chip ID, the console's USB serial, Bluetooth addresses
  (as text and inside Windows' device names), device instance paths and user folders (also inside
  error messages) are masked in every line and every file, and
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
library's own command channel and hooks on the PC, and then runs the self-tests and two dry runs of
the suite against that emulator. Today that is 188 host tests (25,251 checks); 235 (28,483 checks)
with the hardware test hooks built in, and the two builds answer 2,000 unarmed commands byte for
byte alike (after every test, each mutex the library took must have been let go again); a compile of
the library files whose calibration calls become stand-ins without `CONFIG_OPENSC2_CAL`; the
runner's 281 self-tests (a dry run of the Bluetooth path among them); its dry runs against the
library's own code in a firmware emulator (`--lizard --dock`: 233 tests pass, 26 are skipped; and
`--flash --lizard --dock`, with the `flash` group and the tests that store something, such as
`system.keylock`: 241 pass, 18 skipped), which has the system state machine, the calibration, the
dock link and the haptics synthesiser built in (as `app/haptics.conf`), so the `system`, `cal`,
`dock` and `haptics` groups run there too; 6 self-tests of `tools/fake_puck.py`; 9 self-tests of
`tools/match_steam_version.py`. With every opt-in that runs offline, the simulator passes 182
(`--lizard --flash`, 60 skipped: it models no calibration, system state machine, synthesiser or
dock) and the simulator's Bluetooth path 100 (`--bluetooth --lizard --flash`, the soak skipped). To
run the emulator by hand after `run.ps1` built it:

```powershell
python -I tools\hwtest\run.py --dry-run --fw-emu $env:TEMP\opensc2_fwemu.exe --lizard --dock
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
python -I tools\hwtest\run.py --dock                   # the dock link through the test hooks
python -I tools\hwtest\run.py --with-steam --steam-control
```

The Bluetooth build has a path of its own, `--bluetooth` ([Bluetooth](#bluetooth)).

| Option | |
| --- | --- |
| `--group G[,G...]`, `--test ID[,ID...]` | only these (the start snapshot, the switches they need and the whole restore always run) |
| `--wired-only`, `--wireless-only` | one path; the board is switched there first if needed, and back at the end |
| `--bluetooth` | the Bluetooth path alone (B): a board of the Bluetooth build, wireless on its Bluetooth transport and paired with Windows ([Bluetooth](#bluetooth)); not with `--wired-only`, `--wireless-only`, `--through-puck`, `--with-steam` or `--dock` |
| `--through-puck` | allow writes through the puck to the board's slot ([below](#the---through-puck-opt-in)) |
| `--flash` | allow the value-preserving flash rewrites of group `flash` ([what they write](#what---flash-writes)) |
| `--lizard` | run group `lizard`: lizard mode on injected inputs, only the guard holding its reports back (needs the canary, the leak detector watching that path, and `inputs.lizard_lock` passed on it in the same run, for example `--group hooks,inputs,lizard`) |
| `--dock` | run group `dock`: the board's dock link through the test hooks, nothing wired ([The dock](#the-dock)); on the wireless path it stops and restarts the radio once; not with `--bluetooth` |
| `--with-steam`, `--steam-control` | the passive `steam` group at the end; with `--steam-control` the runner starts and closes Steam itself, without it you start Steam when asked |
| `--soak S` | `stream.soak` for S seconds |
| `--board N` | the board to use when more than one is connected (the runner lists them) |
| `--timeout-scale F` | multiply every timeout (a slow PC, a busy USB hub) |
| `--max-minutes N`, `--fail-fast` | stop after N minutes, or at the first failure; the restore still runs |
| `--yes` | never wait for you: with Steam open the run stops (exit 2) instead of asking you to close it |
| `--results DIR` | where the results folder goes (default `tools\hwtest\results`, git-ignored). `report.json` names a folder of your own only as `(--results)`, not by its path |
| `--dry-run`, `--sim-fault NAME`, `--fw-emu EXE` | offline: against the simulator (with one injected fault), or against the firmware emulator; `--dry-run --bluetooth` runs the Bluetooth path against the simulator's Bluetooth link (the emulator has none) |
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
tagged ([releasing.md](releasing.md)). The Bluetooth path (`--bluetooth`) has run only against the
simulator so far ([Bluetooth](#bluetooth)).

The groups and tests added since v0.2.0 (the groups `cal`, `system`, `haptics`, `ble` and `dock`;
`outputs.stop_status`, `settings.imu_answers`, `flash.kept_keys`, `steam.session`'s traffic counts,
the input rules' and lizard mode's processing tests, and the new checks in `cmd`, `battery` and
`settings`) have run only against the simulator and the firmware emulator so far, not on a board.

### What `--flash` writes

The `flash` rewrites store the values the board reports, and the board reports a stored value and
the build's default alike. So where nothing was stored yet, `flash.chime`, `flash.transport` and
`flash.policy` store the default in effect (the chime level, the selected bond slot, the mode
policy), `flash.kept_keys` stores `user/keylock`, `mte/charge_level` and `debug/resetreason` as
they read (zeros until set: no lock, no charge window, no crash), and the board keeps each value
from then on, also under a later build with another default. Nothing in effect changes, and
Steam stores the chime level and the bond itself when it pairs the controller, but the stored
state can differ afterwards; `restore.as_found` compares the values in effect and does not see
it. Each of these rows says so in its note, and every write counts in `report.json`
(`safety.flash_writes`).

`system.keylock` also needs `--flash`: the keylock chord stores `user/keylock`, the next power-on
(on an injected battery) shows the keylock shutdown, and the following power-on on USB power
deletes the key again, as on the real controller (two writes; a board that had no keylock has none
afterwards).

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

## Bluetooth

The Bluetooth build (`app/ble.conf`) is built and **not yet tested on hardware**: this is the
procedure for its first run, and for any later one. It needs a PC with Bluetooth LE (Windows 10 or
11 with its own Bluetooth stack), the board on the PC's USB for power and its console, and Steam
closed.

1. **Build and flash** the Bluetooth image: `powershell -NoProfile -ExecutionPolicy Bypass -File
   build.ps1 -Project app\ble.conf -OutDir C:\b\osc2-ble -Pristine` (or `west build -p always -d
   build-ble -b promicro_nrf52840/nrf52840/uf2 app -- -DEXTRA_CONF_FILE=ble.conf`), then `uf2` on
   the board's console (a wireless boot), or RST shorted to GND twice quickly, and copy
   `zephyr.uf2` to the USB drive that appears (`NICENANO`).
2. **The puck first.** The image behaves like the default one while the puck is the transport:
   run the suite as usual (`python -I tools\hwtest\run.py`, wired and through the puck); a
   Bluetooth build reports caps bit 17. Then get the board wireless, with its console: plug it in
   with P0.31 held against GND
   ([I cannot find the console](troubleshooting.md#i-cannot-find-the-console)); on the console of a
   wireless boot, `opensc2 mode wireless` keeps every boot wireless (`opensc2 mode auto` goes back
   to the rule).
3. **Switch to Bluetooth** on the console: `opensc2 transport ble`. `opensc2 ble status` shows
   `bluetooth: on; slot 1, identity 1 <its address>` and `link: advertising open to any host
   (pairing mode); N s left of the window`, N counting down from 60: the window is 60 s, as on the
   real controller. When it runs out the board stops advertising (`link: wireless off: no host
   within the advertising window`) and the system state machine switches wireless off, as on the
   real controller on USB power: the LED turns amber, and STEAM then reboots the board, which comes
   back wired. `opensc2 start` advertises again.
4. **Pair it in Windows**: Settings, Bluetooth & devices, Add device, Bluetooth. "Steam Ctrl (BT)
   OSC..." appears; select it: it pairs without a PIN or a prompt (Just Works).
5. **Check it**: `opensc2 ble status` shows `connected and active, security 2` (or more), an ATT MTU
   of 64 or more, the interval Windows granted (7500 µs asked for) and `paced by the connection
   events`, and the state reports counting up. Device Manager (View, Devices by connection) shows
   under the Bluetooth device a HID device with three collections (a mouse, a keyboard and a vendor
   collection); its hardware IDs contain `VID&0228DE_PID&1303` (Windows may print the hex in lower
   case). The transport is now stored (`opensc2 transport`: `stored ble`): wireless boots start on
   Bluetooth, and Windows connects the board by itself when it advertises for it. With Steam closed
   the board is also a Bluetooth mouse and keyboard (lizard mode; with nothing wired it sends no
   motion).
6. **Run the Bluetooth path**, Steam closed, a harmless window focused, step by step the first time:

   ```powershell
   python -I tools\hwtest\run.py --bluetooth --test desc.ids,desc.collections,desc.caps,desc.strings
   python -I tools\hwtest\run.py --bluetooth --test hooks.present,hooks.arm,hooks.guard
   python -I tools\hwtest\run.py --bluetooth --test cmd.attributes,ble.link
   python -I tools\hwtest\run.py --bluetooth --group inputs
   python -I tools\hwtest\run.py --bluetooth                     # the whole Bluetooth path
   python -I tools\hwtest\run.py --bluetooth --lizard
   python -I tools\hwtest\run.py --bluetooth --flash             # ble.window (see below)
   ```

   The runner finds the board through its console (the address of the identity it advertises on,
   its unit serial) and then the HID device Windows made of it; it waits up to 20 s for Windows to
   connect it. Feature report 1, the output reports and the input reports go over Bluetooth, the
   test channel too; the console carries what must work while the link is down (the waits for the
   link, the lease, the ring during a drop). `ble.reconnect` and `ble.wireless_off` take the link
   down on purpose and wait for Windows to connect the board again (the leak detector ignores the
   board's mouse and keyboard while the link goes down and comes back). A run that does not start
   on Bluetooth stops at once and says so.
7. **Back to the puck**: `opensc2 transport puck` (stored once the puck connects). The board keeps
   its Bluetooth bond; `opensc2 transport ble` connects to Windows again.

**Removing the pairing.** In Windows: Settings, Bluetooth & devices, the "Steam Ctrl (BT) ..." device,
Remove device. The board keeps its own record of the bond and, as the real controller does, refuses
a new pairing on the same identity. To pair again, open the slot for a new host first: `opensc2 ble
pair` on the console (or the pairing button, while Bluetooth is the transport) resets the slot's
other identity, which gets a fresh address, and advertises it in pairing mode for 60 s; Windows
then lists it as a new device. The old bond goes once the new host has encrypted the link; until then
`opensc2 start` advertises for the old host again. A factory reset (vendor command `0x86` with its
key) deletes every Bluetooth bond and identity.

What `--bluetooth` writes: like a run without it, settings in RAM (restored), and nothing in flash,
except that the board stores Bluetooth as its transport when a link comes up while another one is
stored (October's rule; it did so when you paired it) and `ble.window` (`--flash` only) resets the
slot's other identity, which stores that identity's new address.

What the first hardware run will show, and the suite reports rather than assumes: the interval
Windows grants, the strings Windows gives a Bluetooth HID device, how Windows passes a refusal (an
ATT error) to a feature request, and how quickly it connects the board again.

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

## The dock

> [!WARNING]
> **Nothing may be connected to Valve's puck's pads without the owner's step-by-step go-ahead**: not
> a board, not a USB-UART adapter, not a meter's probe. The pads' voltages and loads have not been
> measured; the `+` pad carries about 5 V once the puck switches it on, and a load on `P` can make
> the puck switch it on. Each step towards the real puck (looking at its pads with nothing touching
> them, a high-impedance meter, a current-limited pilot, charging) needs that go-ahead first.

What can be tested without the puck, and how:

- **The protocol, on a bare board** (`--dock`, the `dock` group above): the test hooks' dock ops
  hand the board's dock link 73-byte frames as if the puck had sent them, and read the answers
  back ([the ops](test-protocol.md#the-test-channel-vendor-command-0x7e)). Nothing is wired; the
  frames never leave the PC except to the board's own test channel, and through the puck never.
  Built: the dock link is in the default `app/` image, with the test hooks. Not yet run on
  hardware; the dry run checks the group against the library's own code.
- **The wire, against a fake puck**: [tools/fake_puck.py](../tools/fake_puck.py) plays the puck's end
  (a probe, polls 64 ms apart, a vendor command, a short frame and a bad CRC) through the firmware
  emulator (`--fw-emu`), a board's test hooks (`--test-op`), or a 3.3 V USB-UART adapter wired to
  the dock UART of a board built with `app/dock.conf` (`--port COMn`, with `--pilot dtr` or `rts`
  to present the pilot through a divider). The adapter path is untested; the tool refuses Valve's
  devices, the board's console and a UF2 bootloader as its port, and only sends commands the test
  link's allowlist has.

```powershell
python -I tools\fake_puck.py --fw-emu $env:TEMP\opensc2_fwemu.exe --command 89:2d --errors
python -I tools\fake_puck.py --test-op --polls 20
```

## Tests of the test suite

- **Self-tests** (`--selftest`, 281 tests): the wire formats, the expected-report and lizard models,
  the test channel's codec, the console grammar, the masking (Bluetooth addresses too), every
  safety rule (including the source scan of the puck reader, that every function that sends checks
  the exact bytes, and which Bluetooth node is the board's), the link lock against a racing
  keep-alive, the leak detector's filter and watch list, the Windows backend's slot handling on
  synthetic device lists (nothing opened), the writers, the dock's frames, rules and safety, one
  numbering of the test channel's ops, ring kinds, caps bits and links (the firmware's headers, the
  runner and [test-protocol.md](test-protocol.md) against each other), and the whole runner against
  the simulator, the Bluetooth path included.
- **The dry run** (`--dry-run`): every group against a simulator with a virtual clock (seconds,
  not minutes), which models the board in both modes, the puck, the console with log noise, Steam
  and the reboots; with `--bluetooth`, the board's Bluetooth link and a Windows host bonded to it. `--sim-fault NAME` makes it misbehave in exactly one way, and each fault must
  give its own failing test and exit code: `guard_leak`, `seq_gap`, `trigger_bit`,
  `stall_missing`, `no_reconnect`, `stale_battery`, `lease_ignored`, `not_as_found`,
  `steam_appears`, `lizard_ignored`, `second_reader`, `share_not_enforced`, `leak_event`,
  `board_reset`, `wrong_mode`, `puck_reset`, `com_renumbered`, `console_busy`, `keepalive_race`,
  `answer_dropped`, `pad_blank_ignored` (setting 82 does not blank an injected pad),
  `glide_ignored` (lizard mode never glides), `ticks_ignored` (lizard mode makes no ticks and no
  pad clicks).
- **The firmware emulator** (`--fw-emu`): the same groups against the library's own code
  (commands, test channel, ring, injection, lizard mode, the battery and state reports, the
  calibration's raw inputs and pad rules, the system state machine's rules with their test ops and
  reboots, and the dock link), so the runner and the firmware cannot drift apart unnoticed. The
  simulator has no calibration (no caps bit 21), so group `cal` runs against the emulator only.
- **The shared vectors** in `tests/vectors/hwtest/`: the C host tests and the runner's self-tests
  read the same files (the state report rules with every rule word, lizard mode with its processing
  and haptic events, ring entries, the test channel, the settings, the dock's frames and rules), so
  both sides agree on every byte.

Back to the [documentation map](README.md).
