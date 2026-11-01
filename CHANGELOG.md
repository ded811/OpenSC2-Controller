# Changelog

Every change to OpenSC2 that matters to people who use it, release by release. The form follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html): while the version is 0.x, the API may
still change between minor versions.

## [Unreleased]

Work towards feature parity with the real controller's firmware, on top of 0.2.0: everything here
is built and checked on the host (the host tests, the firmware emulator and the hardware test
suite's simulator); none of it has been tested on hardware yet. What still differs from the real
controller is listed in docs/protocol.md's gaps.

### Added

**Protocol completeness**

- `opensc2_radio_set_tx_power()` and `opensc2_radio_tx_power()`: the puck link's transmit power at
  run time, for boards with an amplifier or a weaker antenna (from the next time the link listens);
  `opensc2_transport_key()`: `user/wireless_transport` as a host reads it;
  `opensc2_radio_set_give_ups()` and `opensc2_radio_give_ups()`: the 60 s and 10 s give-ups at run
  time.

- The real controller's haptic stream status replies: every stop-all (`0x81` with an on time of 0,
  `0x82` command 0, a zero speed in `0x80`) and `0x86`'s stream stop send `0x44 {channel, 0x02}`
  for each channel they reach (`CONFIG_OPENSC2_HAPTIC_STOP_STATUS`, on by default, and selected by
  the haptics synthesiser). Steam's pair of zero pulses now gets `44 04 02 ...` and `44 03 02 ...`,
  as from the real controller.
- Every settings key of the real controller in `0xED`..`0xF0`: the library keeps `user/keylock`,
  `user/fully_charged_led_intensity`, `mte/charge_level`, `debug/resetreason` and
  `settings/haptics/...` (stored under `opensc2/k/`, each exactly its size, read back and saved by
  `0xEF` where the real controller has a read function, so not the haptics keys, and
  `mte/charge_level` only while it is a valid window; deleted by the factory reset where it deletes
  them), and the `cal/` keys are the calibration store's (below): `0xEF` saves what `0xED` would
  read back, as on the real controller. Any other name is still the application's, so a host cannot
  fill the storage partition.
- Crash records, as on the real controller (`CONFIG_OPENSC2_CRASH_RECORD`, on by default): the
  library's fatal-error handler leaves a 33-byte record in the retained RAM region and resets; the
  next boot stores it as `debug/resetreason`, and `0xF2` selector 1 answers it, then marks it seen.
  A crash at every boot writes flash once.
- The settings partition's recovery (`CONFIG_OPENSC2_STORAGE_RECOVERY`, on with NVS settings): a
  partition with every sector occupied is erased at boot, as on the real controller.
- The real controller's hardware watchdog (`CONFIG_OPENSC2_WATCHDOG`, off by default; the test
  build `app/watchdog.conf`): 5 s, fed every 2.5 s from the lowest priority, and a 4.5 s warning
  that crashes on purpose so a hang leaves a crash record. Not yet tried with the UF2 bootloader.

**Inputs and lizard mode**

- The real controller's rules on every state report, after the test hooks' injection point: a stick
  deflected beyond 12000 counts as touched (`CONFIG_OPENSC2_STICK_TOUCH_FALLBACK`), setting 82
  blanks a pad in report `0x42` too (`CONFIG_OPENSC2_PAD_BLANK_ON_STICK`), no trigger bit while
  setting 62 asks for raw triggers, and, opt-in, the pads' clicks from their pressure with settings
  52 and 53 (`CONFIG_OPENSC2_PAD_CLICK_AUTO`).
- Lizard mode's processing as the real controller's, each part a Kconfig option, all on by default:
  the touch stabilisers (the right pad's while setting 80 is 1, the left pad's always), the 8-frame
  motion average, the glide after a flick (settings 11 and 12; a new touch, lizard mode turning off
  or the test hooks' resync end it), and the haptic ticks and pad clicks (settings 14, 52, 53 and
  70), which reach the application as haptic events with report ID 0 and which the haptics
  synthesiser plays itself (lizard mode on, setting 70 exactly 1, the current link not the dock).
- The blank canvas: setting 46's raw stick output (ADC counts, or millivolts the real controller's
  way).

**Calibration and sensors**

- Calibration as the real controller keeps it (`CONFIG_OPENSC2_CAL`, on by default): the `cal/` keys
  (`cal/joy_l`, `cal/trg_l`, `cal/prs_l`, the LED trims, the battery offset, the gyro bias and the
  rest) under `opensc2/cal/` in the board's storage, read with `0xED`, changed with `0xEE`, `0xEF`
  and `0xF0`, kept by the factory reset; the recordings `0xD8` (sticks), `0xC0` (triggers) and
  `0xC3` (pad pressure); `0xE2` saves one side's values and `0xFE` erases them, only with its key.
  The console's `opensc2 cal` does the same. The blank canvas takes its LED trims from
  `cal/rgbw_*` unless its overlay sets `led-trims`, and uses any stored trim as the real
  controller does (0 leaves a channel dark). `0xEF` and `0xF0` of a `cal/` key wait for an erase
  or a save asked for before them, so a host's "erase, then store" keeps the new keys. The pad
  pressure filter steps only while the pad is calibrated, and the stored gyro bias is taken as the
  real controller takes it (a NaN as 0; 2^31 or more refused, which keeps the IMU stream of
  `app/imu-stream.conf` off at boot).
- Raw inputs (`opensc2_set_stick_raw()`, `_trigger_raw()`, `_pressure_raw()`, `_touch_raw()`,
  `opensc2_set_raw()`): the library processes them at every state report as the real controller
  does, with the stored calibration and controller settings 30, 31, 34, 35, 46, 52, 53, 60 and 62
  (millivolts or counts, pressure and click by pressure, stick touch and grip).
- The pad rules (`CONFIG_OPENSC2_PAD_RULES`, on by default, inactive with the default settings): the
  pad rotation of settings 2 and 24 and the touch stabiliser of setting 80 = 2, on report `0x42` and
  lizard mode.
- Pure helpers for applications: `<opensc2/cal.h>` (the calibration's arithmetic),
  `<opensc2/sensor_helpers.h>` (capacitive touch, the trackpad's touch, filter, rotation and
  stabiliser, IMU scaling and mounting, a gyro-bias estimate, ADC bursts) and `<opensc2/gauge.h>`
  (battery voltage and state of charge), each with host tests.
- The blank canvas's options for real parts, all off by default: `app/sensors.conf` (with
  `app/sensors.overlay`) reads sticks and triggers right before each state report with four
  conversions averaged and a switched sensor supply, calibrated axes through the library, an
  LSM6DSV16X IMU with a mounting matrix (`imu-mounting-matrix`) and a rest-based gyro bias (settings
  84 and 85), the trackpad's processing (settings 65, 72, 73), stick touch and grips from a CAP1206
  capacitive sensor (any input device named by the chosen node `opensc2,touch-keys`), the battery
  level, and I2C bus recovery at boot. `app/imu-stream.conf` streams the LSM6DSV16X's FIFO with its
  own fusion instead, as the real controller reads it. `app/expander.conf` adds L4, L5, R4 and R5
  through an I2C port expander (a PCA9555) polled every 16 ms, with every button debounced 16 ms.

**Power, chords, the status LED and cues**

- The real controller's system state machine (`CONFIG_OPENSC2_SYSTEM`, on by default): the power-on
  decision on USB power, on the dock and on a battery; STEAM pressed, and held 7 s; the transport
  chords (the puck's two slots, and Bluetooth's two in a Bluetooth build) and their 4 s pairing
  hold; the keylock (`user/keylock`); the idle timeout (setting 50); low battery; the turn-off
  command, a link that gives up and the puck's turn-off; the cable pulled; setting 77 (reboot
  instead of power-off); what powers the board (`opensc2_set_power_source()`,
  `opensc2_power_source()`, `opensc2_keylock()`); the shell's `opensc2 power`. It reads the stored
  transport as `user/wireless_transport` 0 to 3 and starts the link itself.
- A battery build: `CONFIG_OPENSC2_BATTERY` and `app/battery.conf`. Its shutdowns end in
  `OPENSC2_POWER_OFF` (the blank canvas: System OFF, woken by STEAM or USB power), and the radio
  gives up as the real controller does (60 s of listening; `CONFIG_OPENSC2_NO_BOND_TIMEOUT_S`, 10 s
  with an empty slot, link reason `OPENSC2_REASON_NO_BOND_TIMEOUT`). Without the option a board
  never switches itself off: every shutdown reboots.
- The status LED: the library selects the real controller's modes (the `led_status` callback,
  `opensc2_led_status()`, also after a transport chosen at run time), and `<opensc2/led.h>` has its
  patterns as a pure player (the steady link colour chosen again every 2 s, the breathing patterns
  starting from dark, as on the real controller), the brightness
  curve (setting 38), the channel trims, the fully-charged display
  (`user/fully_charged_led_intensity`) and the IR LED (setting 36). The blank canvas shows them on
  its one-colour LED, an optional RGB or RGBW LED and an optional IR LED.
- Cue events (the `cue` callback, `enum opensc2_cue`): the power-on and power-off chimes, the
  transport cues, pairing and paired (over the air and over Bluetooth), the PC asleep, each with the
  built-in script it stands for and its gain from the chime level. The haptics synthesiser plays
  OpenSC2's own sound for each when it is built in; otherwise playing them is the application's.
- `OPENSC2_CHARGE_GAVE_UP` (the charge state's `0x80` flag) and `opensc2_charge_state_of()`; an
  optional charger status pin in the blank canvas (`charger-status-gpios`).

**The haptics synthesiser**

- `CONFIG_OPENSC2_HAPTICS_SYNTH` (off by default; `app/haptics.conf` builds the blank canvas with
  it): the library renders the haptic output reports into sample blocks as the real controller's
  two sequencers do (the pads at 4 kHz, the grips at 8 kHz, one thread each): `0x81`'s pulse trains
  and special cases, clicks, tones with an LFO, exponential sweeps, the October rumble emulation
  with settings 27, 75 and 76, scripts (one per channel), PCM streams with their rings, s16le, s8
  and mu-law, interpolation and the `0x44` flow control. Sounds of OpenSC2's own (clicks, scripts 1
  to 17, cues by number at the chime level, drop sounds after a free fall), never Valve's
  recordings or script data. Outputs the application provides (`<opensc2/haptics.h>`), a level
  callback for amplitude-only actuators, and three built-in outputs: an I2S amplifier for the
  grips, a PWM "DAC" on nrfx for the pads, a DRV2605 in real-time playback. Integer arithmetic:
  blocks are bit for bit the same on the board and on a PC.
- `samples/haptics`: the synthesiser on the I2S and PWM DAC outputs, or a DRV2605 (`drv2605.conf`),
  with a `hap` shell command to play the sounds by hand.
- `tools/gen_haptics_tables.py`, which writes the synthesiser's constant tables.

**Bluetooth LE**

- An opt-in build (`CONFIG_OPENSC2_BLE`; for the SuperMini the fragment `app/ble.conf`): the board
  as a HID-over-GATT Steam Controller 2 (PnP ID `28DE:1303`) with the real controller's name,
  advertising and pairing (Just Works, LE Secure Connections), the USB report map, the vendor
  commands on feature report 1 (refusals as ATT errors), output reports, the battery and device
  information services, October's gate (encryption and an ATT MTU of 64), a state report `0x45` per
  connection event and lizard mode.
- Wireless transports: the real controller's four `user/wireless_transport` values (Bluetooth slots
  1 and 2, the puck with bond 1 or 2), `opensc2_select_transport()`,
  `opensc2_select_transport_pairing()`, `opensc2_selected_transport()` and the shell's `opensc2
  transport`; the puck's ESB and Bluetooth hand the radio over to each other at run time. With
  Bluetooth, `opensc2 ble status` and `opensc2 ble pair`.
- Bluetooth as the real controller does it: five identities, two per slot, with the rule that picks
  one and a pairing request that resets the slot's other identity (so the old host keeps its bond
  until a new one has paired); the bond rules (a new host deletes the slot's other bond, a deleted
  bond resets its identity, a host bonded on another identity loses that bond); high-duty directed
  advertising to the bonded host, then allow-listed, else pairing mode; a 60 s advertising window
  from every start (`CONFIG_OPENSC2_BLE_ADV_WINDOW_S`); "wireless off" when the window runs out or
  the host ends the link (`0x13`, `0x15`), with the reasons `LISTEN_TIMEOUT` and
  `HOST_DISCONNECTED` and the power request `OPENSC2_POWER_WIRELESS_OFF`; advertising again 500 ms
  after any other disconnect; a factory reset deletes every Bluetooth bond and identity. With the
  system state machine the Bluetooth chords, its cues and its "wireless off" are the machine's.
- Valve's vendor GATT service (`CONFIG_OPENSC2_BLE_VENDOR_SERVICE`, on in a Bluetooth build): the
  command channel, inputs and outputs as plain characteristics for SDL's Android and Apple back
  ends, sharing their values with the HID service; reports go to the vendor characteristic first
  when the host subscribed there.

**The dock**

- The controller's end of Valve's puck's charging pads: the 73-byte frames with their CRC-16/KERMIT
  (`<opensc2/dock.h>`); the dock link (`CONFIG_OPENSC2_DOCK_LINK`, on with the test hooks): sessions
  that a failed frame ends, the identity record for the probe, Steam's vendor commands on a stored
  request of the dock's own (`0x81` and `0x85` acknowledged and ignored there), GET input from the
  copy of the latest state report or, with the dock alone, from a new one; the dock's rules (the
  pilot's window, dock or USB power, the docked states).
- `CONFIG_OPENSC2_DOCK`, the dock's UART and pilot sense from the devicetree alias `opensc2-dock`
  (`dts/bindings/opensc2,dock.yaml`), and `app/dock.conf` with `app/dock.overlay`, a SuperMini
  variant: built, never run on hardware. With a sense input for the dock's `+` (`plus-gpios`, none
  on the SuperMini) the supply rule gives the system state machine the power source "dock". Nothing
  may be connected to Valve's puck's pads without the owner's go-ahead.
- `tools/fake_puck.py`, the puck's end of the dock link through the firmware emulator, a board's
  test hooks or a USB-UART adapter (untested).

**Builds**

- `app/all.conf`: Bluetooth, the haptics synthesiser, the dock link and the watchdog in one test
  build, for a board with nothing wired. A fragment of `app/` brings the overlay of the same name,
  and one can build on others with an `# OPENSC2_FRAGMENTS:` line (`all.conf` does).

**The hardware test suite and the test hooks**

- One numbering for the test channel, documented in `docs/test-protocol.md` and checked by a
  self-test against the firmware's headers: links 0 radio, 1 USB, 2 test, 3 none, 4 Bluetooth, 5
  the dock; ops `0x17`/`0x18` (the rule word), `0x60`..`0x62` (Bluetooth), `0x70`..`0x74` (system
  injection), `0x80`..`0x83` (the synthesiser), `0x90`..`0x93` (the dock), `0xA0`..`0xA2` (raw
  inputs); ring kinds `0x30` (a stop's reply), `0x40` (Bluetooth), `0x50`..`0x56` (the system
  state machine), `0x60`..`0x63` (the synthesiser), `0x70`..`0x73` (the dock); caps build flags 15
  rules, 16 stop replies, 17 Bluetooth, 18 the system state machine, 19 the synthesiser, 20 the dock
  link, 21 calibration.
- New groups: `cal` (raw inputs and the pad rules), `system` (the system state machine through
  injected buttons, battery and power source, boot-time injections), `haptics` (CRCs of rendered
  blocks against shared vectors, generators, the rumble's gains, scripts, cues, the streams' flags,
  the drop sounds' timing; only with the synthesiser), `ble` (the Bluetooth link) and the opt-in
  `dock` (`--dock`: the dock link fed frames through the test hooks, never the puck's pads); new
  tests in `inputs`, `lizard`, `outputs`, `cmd`, `settings` and `flash`, and `steam.session`, which
  counts what Steam sends the outputs through the puck while it runs and checks the board's `0x44`
  reply to each stop.
- The Bluetooth path (`--bluetooth`, path B, alone): the board found through its console and the HID
  device Windows made of it, only that device ever opened; the groups hooks, desc, cmd, inputs,
  battery, stream, reports, outputs, settings, ble and lizard over Bluetooth, then a warm reboot
  that must come back on Bluetooth (`modes`). Run only against the simulator so far.
- Safety: the suite never writes, saves or deletes a `cal/` key (rule S5), and a dock frame that
  runs a command is held to the test link's allowlist. The firmware emulator runs the system state
  machine, the synthesiser, the calibration and the dock link, so the dry run checks those groups
  against the library's own code. The dock never carries the test channel (a `0x7E` in a dock frame
  answers -22); the end of arming drops a boot-time injection that no reboot has used yet, and the
  runner clears one after an unplanned reset; a failure line never prints serial or chip ID bytes
  or a bare Bluetooth address; a reboot the board makes by itself gets a window to leave that
  covers it.

### Changed

- Vendor command `0x89` answers the defaults of settings 48, 84 and 85, as the real controller does
  (it hands them to its IMU); the application still sees the values Steam wrote. A number above 85
  reads 0, also as on the real controller (it was a STALL).
- Every controller setting has the real controller's default and range: `0x87` clamps each value to
  its range (0.2.0 knew nine ranges and stored the rest as sent; 48, 84 and 85 still go unclamped,
  as on the real controller), and the settings 0.2.0 had no default for start from theirs (setting
  3 reads 1200 after a boot, 28 and 29 start above their range, as there).
- `0xE9` reads the LED colour back the way the real controller does: a channel reads 100 when it is
  exactly 100, otherwise 0. The colour itself is kept as sent.
- `0xBE`: byte 13 is 1 while the charge state is "charging" (else 0) and byte 14 is the charge state
  without its `0x80` flag, as on the real controller. The `gauge_flags` field of
  `struct opensc2_battery` is no longer sent.
- `0xA1` answers 18 bytes as the real controller does: selector 1 gives 1, one byte of reset-cause
  flags in its encoding (it was Zephyr's flags as a u32), the chip ID and 8 zero bytes; any other
  selector gives 0 and 17 zero bytes (it was an empty answer).
- `0xED` answers the `cal/` keys as the real controller does even on a board without calibration,
  where 0.2.0 answered them all with length 0: the LED trims with their defaults (90, 35, 120, 80)
  and `cal/volt_offset` (2 bytes); the stick, trigger, pressure and touch keys with length 0 until
  one with a non-zero format word is set. `0xD8`, `0xC0`, `0xC3` and `0xFE` are answered by the
  library instead of the application's `vendor_command`, and `0xE2` reads its side and mask
  whatever its length byte says.
- Attribute 11 of `0x83` is the current link's report interval, whichever link asks: the connection
  interval while Bluetooth is the transport, 64000 µs while the dock is the only link up, 4000 µs
  otherwise (0.2.0 always answered 4000).
- Output report `0x88` with more than 31 bytes a side does nothing, as on the real controller (it
  was clamped to 31).
- Lizard mode coming back on sends its empty keyboard report `0x41` before the all-zero mouse
  report `0x40`, the real controller's order (0.2.0 sent the mouse report first).
- Without an IMU (the application's quaternion all 0, "none") the state reports carry the identity
  quaternion, W = 32767, as the real controller's always do; 0.2.0 sent all four words 0.
- Report `0x81` swaps the two pads only for the exact targets 0 and 1, as the real controller does:
  targets `0x80` and `0x81` are the left and the right pad there too (0.2.0 swapped them as well).
- A transport or puck slot chosen at run time (`opensc2_select_slot()`, the shell's `opensc2 slot`)
  is stored once its link is up, as the October 2026 firmware does, instead of at once. It is
  stored as `opensc2/transport`, with `opensc2/slot` still written for a puck transport.
- The puck's radio link turn by turn as on the real controller: only a keep-alive gets a state
  report (a hello, a channel change or the PC-asleep packet no longer do, so the sequence number no
  longer moves on for them); its slot is 2075 µs after the keep-alive is handled and the report is
  built early by the longest build of the last 65 (`CONFIG_OPENSC2_REPORT_DELAY_US`, was 2000 µs
  from its arrival), while the radio thread already waits for the next packet, so the next turn's
  windows start right after the keep-alive, as on the real controller; "disconnecting" (every turn), the version answer and the identity are queued
  at once, each its own payload, and keep-alives still get state reports while disconnecting; one
  GET answer and one SET result wait, one each, the GET answer first (was one queue of 4); lizard
  mode's reports of a frame ride with the next state report; up to 5 other reports wait
  (`CONFIG_OPENSC2_REPORT_QUEUE_DEPTH`, was 6) and one that finds them full is dropped, lizard
  mode's included (`CONFIG_OPENSC2_REPORT_WAIT_DEPTH`, OpenSC2's option to let them wait, is now 0
  by default); a SET record is zero padded to a whole command; an output record is handed on with
  its record's length, one more than the report (so `0x87` and `0x89` through the puck behave as on
  the real controller); after 10 ms of silence the walk listens 13 ms on each entry of the channel
  list in turn, the first one included, and a channel change moves 0.5 ms after the packet.
- Listening and waiting for a bond as on the real controller: ESB is set up again every time the
  link listens; a connection request must be exactly 18 bytes; any bond write (either slot) starts
  listening again, with a fresh 60 s, also one that leaves the slot empty (it then listens for IDs
  0 and 0); the first disconnect reason of a connection stays, so a stop after the puck's turn-off
  keeps reason 3 and the link goes `OFF` (wireless off). A slot chosen at run time posts an event
  of its own.
- `CONFIG_OPENSC2_LISTEN_TIMEOUT_S` (60 s) and `CONFIG_OPENSC2_NO_BOND_TIMEOUT_S` (10 s) are on in
  every build, as on the real controller (they were 0, never, without `CONFIG_OPENSC2_BATTERY`):
  a board on USB power that finds no puck goes "wireless off" (amber), and STEAM reboots it wired.
- `CONFIG_OPENSC2_RX_QUEUE_DEPTH` is 5 (was 8), the real controller's queue of received packets.
- `user/wireless_transport` as on the real controller: `0xEE` takes any one byte and leaves the
  running link alone (a new puck slot no longer counts at once; 0 and 1 without Bluetooth and values
  above 3 are kept as written), `0xED` reads it back, `0xF0` sets 2 and leaves the link alone; the
  running transport is written back into the key at every connection; a choice made at run time
  shows in the key only once its link is up. At boot 3 runs the second puck, 0 and 1 Bluetooth, any
  other byte the main puck.
- The bond keys as on the real controller: `0xEE` keeps a 24-byte bond as written (one with an ID of
  0 reads back empty), `0xEF` of an empty bond answers `-ENODEV` (was `-ENOENT`), `0xF0` empties
  the RAM bond only once the stored one is deleted, and `0xA2` writes the stored bond only, from a
  work item: it counts from the next boot (it took effect at once).
- With the system state machine, `opensc2_init()` leaves starting the link to it (still only when
  `config.start` is set); the turn-off command and the cable pulled are its events, and the
  application gets `OPENSC2_POWER_OFF` or a reboot at the end of a shutdown; a host's reboot and
  bootloader requests reach `power_request` only in the states that act on them.
- Wireless on USB power, STEAM held 7 s, the turn-off command, a link that gave up and the puck's
  turn-off now switch wireless off (the amber LED), and a STEAM press then reboots the board, which
  comes back wired, as on the real controller; `opensc2 start` turns wireless on again. In 0.2.0 the
  blank canvas only stopped the radio, and any button started it again.
- The blank canvas's LED shows the real controller's patterns (on its one-colour LED: on once set up
  or connected, a slow blink while it looks for the puck, two quick flashes while it pairs over the
  air, a short dark blink every second for amber).
- The retained RAM area's layout is version 2 (the crash record's slot and the test hooks' system
  words), so the first boot of this firmware starts with an empty USB session log.
- Vendor commands run one at a time across links (a mutex in the command channel), now that the dock
  and Bluetooth can run them beside USB or the radio.
- With the synthesiser, `app/`'s optional vibration motor follows the rendered blocks' loudness.
- The test hooks' `HOST_SET` no longer takes `0xF2` with selector 1 (its GET can write flash now);
  `retained.c` is always built.
- The hardware test suite's lizard group runs its mapping tests with lizard mode's processing
  switched off, and follows every sequence until lizard mode goes quiet (a glide keeps the reports
  going); `outputs.stop_status` expects the replies by the build's caps.
- Host tests: 188 (25,251 checks), and 235 (28,483 checks) with the hardware test hooks built in;
  each test also checks that every mutex the library took was let go again. The runner has 281
  self-tests; its dry runs against the firmware emulator pass 233 tests and skip 26
  (`--lizard --dock`), and pass 241 and skip 18 with `--flash` (the flash group and
  `system.keylock`), both run by the host tests; `tools/fake_puck.py` has 6 self-tests.

### Not proven

- Everything above is built and checked on the host; none of it has run on hardware yet. The
  default image differs from 0.2.0's on the wire and on the board as the Changed section says, and
  has new ops in its test hooks.

## [0.2.0] - 2026-10-19

First public release.

### Added

- Wired mode: the board is a USB Steam Controller 2 (`28DE:1302`) with the real controller's
  descriptors, in the July 2026 form by default and the October 2026 form as an option. Steam's
  vendor commands on feature report 1; pairing with the puck over the USB-C cable, and Steam's
  "reboot into wireless mode".
- The boot-mode rule (wired or wireless, decided once at power-up) and a stored mode policy; the
  ways back to wireless; the plug-in rule.
- The USB session log, kept across a reboot and read in wireless mode.
- The debug console (a USB serial port with the shell) in wireless mode.
- Identity commands in the shell, and an optional product name.
- The hardware test hooks (`CONFIG_OPENSC2_TEST_HOOKS`) and the hardware test suite
  (`tools/hwtest`).
- `tools/match_steam_version.py`, which gives a board the build time of Steam's current controller
  firmware.
- `template/`, a project to copy as the start of your own, and `docs/integration.md`.
- Wired mode and the shell in every sample, and a README for each.
- The documentation map, getting started, troubleshooting, test protocol and release pages;
  `NOTICE.md`, `CONTRIBUTING.md`, `SECURITY.md`; `tools/check_links.py` and `tools/collect_licenses.py`.

### Changed

- The revision string Steam reads is `osc2-0.2.0`, both as the library's default and in `app/`
  (they were `opensc2-0.1` and `osc2-app-0.1`).
- The SuperMini's RC oscillator lines moved from `prj.conf` to
  `boards/promicro_nrf52840_nrf52840_uf2.conf`.
- The host tests are now 52 (2,209 checks), and 79 (4,658 checks) with the hardware test hooks
  built in.

### Proven on hardware

Tested with the v0.2.0 release image (`opensc2-supermini-nrf52840-v0.2.0.uf2`, 2026-10-19), on a
SuperMini nRF52840 with nothing wired to it, with Valve's puck on its October 2026 firmware and
Steam on a Windows PC.

- It enumerates over USB exactly like a real Steam Controller 2 (`28DE:1302`).
- Steam pairs it with Valve's puck over the USB-C cable, like a real controller.
- It then reboots into wireless mode and connects through the puck.
- Steam shows no "needs an update" prompt for a freshly flashed board: it reports a build time newer
  than Steam's current controller firmware (set to an older one, it gets the prompt).
- The automated hardware test suite passed every test on the release image with every opt-in on
  (`--through-puck --flash --lizard --with-steam --steam-control`): 165 of 165 in a run started
  wireless, and 31 of 31 for the `modes` and `cmd` groups in a run started wired, which adds the
  four tests that only run from a wired start (0 failed, 0 errors, 0 warnings). It covers inputs
  injected and checked wired and through the puck, battery, rumble and haptics, LEDs, settings,
  all vendor commands Steam sends, lizard mode with nothing reaching the PC, reboots, mode
  switches, reconnects, and a run with Steam open. Only the optional soak test (`--soak S`),
  which needs a duration, was not run. The suite counts one test per path (wired, wireless,
  through the puck, with Steam); each test makes its own checks. In the report, the tests of the
  through-puck and Steam paths carry the mode they ran in (`@R` or `@W`).
- The charger test aid (a separate test build): on a plain USB wall charger it switches to
  wireless and connects through the puck.

### Not proven

- Built, not yet tried with real parts: real buttons, sticks, trackpads, IMU, rumble motor, RGB LED
  and battery hardware (the drivers in `app/` and the samples have never run on such parts).
- Built, not run on hardware: the samples and the template.
- Built, untested with Steam: pairing over the air (the puck's pairing window).
- Not tested: other boards; Linux and macOS as the PC (the PC side was tested with Steam on
  Windows).
- Not implemented: Bluetooth; the dock (pogo pads); lizard mode's smoothing, glide and haptic
  ticks; PCM haptics playback in the samples.

## [0.1.0] - 2026-10-17 (not released)

The first commits: the library for the puck's 2.4 GHz link (wireless only: pairing over the air,
connecting, keep-alives, channel changes, the identity packet, the vendor commands through the puck,
lizard mode, the controller settings), the blank-canvas firmware for the SuperMini and six samples,
20 host tests (242 checks). Untested on hardware.

[Unreleased]: https://github.com/ded811/OpenSC2-Controller/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/ded811/OpenSC2-Controller/releases/tag/v0.2.0
