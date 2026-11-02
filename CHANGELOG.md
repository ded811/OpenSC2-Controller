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
- The real controller's hardware watchdog (`CONFIG_OPENSC2_WATCHDOG`, on by default as there,
  except in a build for a UF2 bootloader, such as the SuperMini's): 5 s, fed every 2.5 s from the
  lowest priority, and a 4.5 s warning that crashes on purpose so a hang leaves a crash record. Not
  yet tried with the UF2 bootloader (`app/watchdog.conf` turns it on there).

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
- `opensc2_lizard_status()`: whether lizard mode's keyboard and mouse run, their switches, a held
  key, an owed zero mouse report and a glide on either pad.

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
- Trackpads frame by frame (`<opensc2/trackpad.h>`): `opensc2_pad_frame(side, &frame)` takes each
  frame of a pad sensor (touch, position, and the raw pressure, stick-touch and grip words it has)
  and processes it at the sensor's own rate as the real controller processes its pads: the pad
  scale, the setting-65 position filter (on by default), the rotation, the stabiliser, the stick
  touch with the deflection rule, the grip, the pressure and its click from `cal/prs_*`, setting 82,
  setting 60 and the start-overs; each frame's pressure word feeds the `0xC3` recording. A board
  without one of the words has its own bits stand in. `opensc2_pad_frame_release()` and
  `opensc2_pad_result()` go with it. The blank canvas and samples/touchpad feed their touch
  devices this way.
- The IMU as the real controller runs it (`<opensc2/imu.h>`): `opensc2_imu_sample()` takes a
  sensor's samples in SI units and the library scales them (kept to 16 bits as there), subtracts
  the sensor's bias while setting 84 is on, turns them by the mounting matrix
  (`opensc2_imu_set_mounting()`) and applies the dead zone; the stream runs from boot and is
  switched by every write of setting 48; the keys `settings/sensors/imu/mode`, `mounting_matrix`,
  `gyro_dz_threshold`, `use_bias` and `gyro_threshold` by name (`0xEE`, RAM only); setting 85 and
  the stream go to the application's sensor through `opensc2_imu_set_handler()`;
  `opensc2_imu_free_fall()` arms the drop sounds as the real controller does (again 5 s after a
  fall; never again until the next boot when the stream goes off within those 5 s). The blank
  canvas's IMU options and samples/imu use it.
- `opensc2_cal_set_dock_offsets()`: millivolts the board adds to its stored stick calibration while
  docked (revision `0x48` and later), as the real controller does for its dock's magnets; 0 by
  default. Docking and leaving the dock set the sticks and triggers up again.
- `opensc2_norm_setup()` / `opensc2_norm_step()` (the real controller's normalising stage),
  `opensc2_trigger_setup_level()` / `opensc2_trigger_step()` (with the digital bit),
  `opensc2_pad_stretch_by()`, `opensc2_imu_quat_from_unit()`, `opensc2_cal_rec_pressure_sample()`.
- `CONFIG_APP_ANALOG_SELF_CENTRE` in the blank canvas: an input without a stored calibration
  centred at boot (the earlier default behaviour) instead of reading 0.
- The real controller's button debounce (`opensc2_button_hold_*()` in `<opensc2/sensor_helpers.h>`):
  a change counts at once, then the button is held 16 ms. The blank canvas debounces its buttons
  this way (gpio-keys now waits 1 ms instead of 5), so a press shows up to 4 ms sooner and a press
  shorter than 16 ms is still reported for 16 ms, as on the real controller.

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
- The system machine's remaining rules of the real controller: the wired start attaches the USB
  device once STEAM is up and turns a STEAM press before a host (one held through the power-on
  included) into a reboot back to wireless; USB power in the battery wait turns a battery build
  wired in place; the warm-restart mark is set the moment Steam's request arrives, kept by later
  plain reboots of that boot and used up by a transport chord held at boot; the keylock chord held
  through a battery power-on unlocks and starts; the chords are two combinations each (with STEAM
  and without) in the real controller's order; dropping the link stops only a link that is up; the
  60 s update window with its green blink (a bootloader request on battery or the dock; USB power
  inside it asks for the bootloader); host requests queue like the machine's other events; the
  factory charge window's announcement at power-on (cue 8, seven magenta blinks).
- The battery level kept over a warm reset (`opensc2_battery_kept_level()`, in the retained RAM
  area), the level the power-on decision sees (as the real controller's first report: kept, else
  255); `opensc2_charge_target_mv()` for a charger that can stop at a stored factory charge window;
  the Zephyr statistics group "battery" (`CONFIG_OPENSC2_BATTERY_STATS`, with `CONFIG_STATS`).
- `<opensc2/gauge.h>`: a step in parts (`opensc2_gauge_begin()`, `_add()`, `_finish()`) for the
  real controller's 40 fresh readings after a start or a charging change and none while the charge
  is done, and `opensc2_gauge_voltage()`, the averaged voltage report `0x43` carries (held while
  done). The blank canvas's battery option uses both, and starts from the kept level.
- `<opensc2/charger.h>`: the real controller's charging rules for a charger the application drives
  (`opensc2_charger_init()`, `_step()`, `_window()`, `_gauge_state()`, `_state()`,
  `_ship_mode_ok()`): the charge state report `0x43` carries (0 before the first step, 1 on
  battery, 3 while a new supply is checked, 2 charging, 4 done, `0x80` after giving up), the charge
  voltage (the target, raised by the hysteresis while a supply is checked and while charging), the
  input-voltage limit lowered every 5 s while the charger does not charge (4600, 4500, 4400 mV;
  giving up at 4300 and trying again 60 s later), the factory window's cap and the ship-mode rule.
  The blank canvas's charge state follows these rules (charger status pin or USB power).
- `opensc2_gauge_set_curves()`: a board's own voltage curves and charge targets for the gauge.
- `opensc2_gauge_begin_charger()`: the gauge's history starts over whenever the charger itself
  starts or stops charging, also while the charge is done (a top-up), as on the real controller;
  the blank canvas uses it and keeps its battery step at once a second from start to start.
- `opensc2_docked_sticks()`: whether the sticks read as on the dock, as the real controller counts
  it (from the first docked state until the board is lifted off), for a board that applies its own
  docked stick offsets; `opensc2 power` shows it.
- The LED player: the update window's blink (`update_window`, `update_requests` in the status),
  Steam's colour between a breathe's two fades, the searching pattern's fade-out skipped when the
  link is up by then and its dark part looked at every 50 ms.

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
- `opensc2_ble_status()` (`include/opensc2/ble.h`): Bluetooth's state for the application (which
  identities hold a bond, security, the ATT MTU, the interval, the window left, the name and
  address), so a custom board can show what the shell's `opensc2 ble status` shows.
- `CONFIG_OPENSC2_BLE_VENDOR_STATE_QUIRK` (on by default, as the real controller): the vendor
  `0x45` flag quirk below can be turned off by a board that switches from the puck to Bluetooth at
  run time and serves Windows.
- The log has the connection's details, the PHYs at connection and at every change
  (`CONFIG_BT_USER_PHY_UPDATE` in the app's configuration), a warning when a host asks for an
  interval longer than 7.5 ms, and the bonds of each identity at every start, as the real
  controller's log has them (in OpenSC2's words).

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
- What a custom board supplies or changes itself on the command channel, with the side effects the
  host's commands have: `opensc2_setting_set()` (one setting, as one triplet of `0x87`),
  `opensc2_led_set_color()` (as `0xC5`), `opensc2_chime_level_set()` (as `0xDC` type 1), and
  `opensc2_identity_set()`, the configured identity changed in RAM at any time after
  `opensc2_init()` (a hardware revision read later, its own bootloader's build time), answered at
  once by `0x83`, `0xAE` and `0xF2`.

### Changed

- The calibration's and sensors' arithmetic is now the real controller's single-precision floating
  point in its order of operations, with its fused multiply-adds and its float-to-integer
  conversions (the library's arithmetic files build with `-ffp-contract=off`), instead of exact
  fractions: values that differed by a count now match, and so do the real controller's quirks. A
  turned-over stick half (an extreme on the wrong side of the centre) reads 0 instead of inverting
  the axis; a range end of 0 counts as "not set"; a pressure range of 0 is usable but reads 0; the
  pad rotation's table has 4095 at 30° and 8191 at 90° (worked out with π as 3.141592); the grip
  level is one lower for 15 values of setting 34; the stick-touch and grip hysteresis toggles every
  other word when on is below off; the lift-off damping uses the average updated with the frame;
  the position filter takes its speed from the previous input; the IMU scaling, the mounting matrix
  and the ADC average keep the low 16 bits (wrapping past full scale) instead of saturating.
- The trigger bits are the real controller's trigger stage: never on from setting 68 = 95 up (a full
  pull used to count), off below 68 % − 5 % in single precision (27851 is now off at the default
  90), cleared by a change of 68 and by a set-up of the triggers (a stick or trigger key, the dock);
  a raw trigger's bit is its calibrated stage's, direction included.
- The recordings: each trigger keeps the candidate with the larger |A − B| (a tie goes to direction
  0); a step 0 unhooks at the next frame, so a 0 and a 3 before it still save; a lone `0xC3` step 3
  saves the records as they are (format 0 after a boot); records are written whatever their format
  and reach RAM from the write; a stick or trigger recording loads every stored `cal/` key again
  (a RAM-only `0xEE` change of another key goes back); a side is saved when the board has it.
- The IMU's stream going off (setting 48 = 0, or the key `mode`) now also drops a drop sound still
  waiting its 250 ms, as on the real controller (`opensc2_haptics_free_fall_cancel()`, called by the
  library's IMU for a board that set its IMU up). Such a board's reports carry all IMU fields 0, the
  quaternion's W included, until its first sample, as the real controller's do (they started with
  the identity); a board without an IMU keeps the identity.
- `app/imu-stream.conf` programs the gyro bias at boot also when none is stored (zeros), and leaves
  the stream off when the driver cannot program it, as the real controller does (it started the
  stream without the bias).
- Settings 48, 84 and 85 reach the IMU at every write (not only on a change); a sensor error for 85
  fails `0x87`, and the settings before it in the same command (the LED's included) have taken
  effect, as on the real controller.
- Trackpad frames from a raw sensor: the centroid pushed and stretched goes into the lift-off
  damping as it is, also past the pad's edge, and is limited to the pad only afterwards, as the real
  controller's trackpad driver does (it was rounded and limited first).
- Settings 46 and 62 give a raw reading above 4095 as it is (an average below 0 V wraps round to 16
  bits on the real controller and in the blank canvas), and its millivolts unlimited up to 65535,
  for the recordings too (both were limited to 4095 counts).
- Docking and leaving the dock set the sticks and triggers up again (the triggers' bits clear) on a
  board of any revision; the dock offsets still apply from revision `0x48` on.
- The blank canvas: the sticks and triggers are read right before each state report and processed
  by the library with the stored calibration by default (`CONFIG_APP_ANALOG_SAMPLE` and
  `CONFIG_APP_ANALOG_RAW` on), so an uncalibrated input reads 0, as on the real controller
  (`CONFIG_APP_ANALOG_SELF_CENTRE=y` brings back the boot-time centring); a trackpad's pressure is
  0 without a pressure calibration (it was the touch strength, or half scale); its IMU streams from
  boot and keeps the last sample while setting 48 is 0 (it sent nothing until 48 was written, and
  zeros while 48 was 0); `CONFIG_APP_IMU_ALWAYS` is gone; `app/imu-stream.overlay` runs the
  LSM6DSV16X at the real controller's 250 Hz high-accuracy rate.
- Vendor command `0x89` answers the defaults of settings 48, 84 and 85, as the real controller does
  (it hands them to its IMU); the application still sees the values Steam wrote. A number above 85
  reads 0, also as on the real controller (it was a STALL).
- Every controller setting has the real controller's default and range: `0x87` clamps each value to
  its range (0.2.0 knew nine ranges and stored the rest as sent; 48, 84 and 85 still go unclamped,
  as on the real controller), and the settings 0.2.0 had no default for start from theirs (setting
  3 reads 1200 after a boot, 28 and 29 start above their range, as there).
- `0xE9` reads the LED colour back the way the real controller does: 0 below 100, 100 from 100 to
  199, 200 from 200. The colour itself is kept as sent, now also above 100 (it was clamped to 100):
  such a channel is brighter, up to full duty, as on the real controller.
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
- Lizard mode's keyboard and mouse turn on and off by events taken up in the real controller's
  order, its quirks included: `0x85` while the keyboard runs leaves an "on" pending, so the next
  switch-off sends the empty `0x41` twice and the keyboard keeps typing with setting 9 at 0 until
  the switch-off after that; lowering the switches (`0x81` with lizard mode already off) stops a
  running keyboard with its empty report. The keyboard compares the buttons with the frame before,
  also while it is stopped, so a button held when lizard mode comes back on is reported at its
  next change (it used to be adopted silently). Every accepted `0x81` logs an INFO line.
- Lizard mode's arithmetic is the real controller's: the travel a glide needs adds exact lengths,
  the flick speed and the glide's factor are single precision with setting 12 read in every frame;
  setting 82 drops a pad's motion after the division, so the remainders keep their part; a mouse
  report goes out while one of its bytes is not zero; the right pad's stabiliser is left as it is
  while setting 80 is not 1; a pad click counts only while its pad is touched; the ticks and clicks
  run while setting 9 is on and 70 is exactly 1, forget the touch but keep the click state while
  stopped, and compare the steps with setting 14 in every touched frame.
- Lizard mode's keyboard hears the buttons the moment the application changes them, as the real
  controller's keyboard listens to its button driver (`CONFIG_OPENSC2_LIZARD_KEYS_AT_ONCE`, on by
  default, a thread of its own): the key report goes out then instead of with the next state
  report, and a change made while no link runs goes nowhere instead of being sent by the first
  frame after the gap. While the hardware test hooks are armed the frames bring the buttons, as
  before.
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
- The hardware watchdog (`CONFIG_OPENSC2_WATCHDOG`) is on by default, as on the real controller
  (not yet tried on hardware with this firmware), except in a build for a UF2 bootloader
  (`CONFIG_BUILD_OUTPUT_UF2`): whether that bootloader feeds a watchdog left running has not been
  tried, so the SuperMini builds still need `app/watchdog.conf` for it.
- Reports `0x43` and `0xBE` send the level as given (it was capped at 100), so 255, the real
  controller's "no level yet", goes out as such. A battery build reports what the real controller's
  first report carries until the application gives a battery (the kept level or 255, charge state
  0, zeros); a build without a battery keeps reporting a full one.
- With the system state machine the power source counts at every 50 ms poll whichever way (power
  going away waited 0.2 s), and `opensc2_reboot()` leaves Bluetooth with an all-zero report and
  100 ms first, as the real controller does before every reboot.
- The blank canvas's IR LED goes off when a shutdown or reboot starts, and a later change of setting
  36 applies again, as on the real controller (it stayed off until the end).
- `<opensc2/gauge.h>`'s history averages the readings since its last reset (the first reading no
  longer fills all 40 slots); `struct opensc2_gauge_history` has `count` in place of `filled`.
- The retained RAM area's layout is version 3 (the crash record's slot, the test hooks' system words
  and the kept battery level), so the first boot of this firmware starts with an empty USB session
  log.
- Vendor commands run one at a time across links (a mutex in the command channel), now that the dock
  and Bluetooth can run them beside USB or the radio.
- Bluetooth behaves as the real controller where it differed on purpose before (docs/protocol.md's
  gap 23 lists what is left): a host that encrypts the link on an identity that had no bond is the
  slot's new host whether it bonds or not; the pairing cue plays at every start of pairing-mode
  advertising, the restart after a failed connection too; the last ATT MTU is kept across links and
  a link past the gate stays active until it ends; feature report 1 reads 64 bytes, its GET runs at
  every read (a read blob too) and answers over the start of the stored value, so left-overs follow
  the answer and a read blob gets the start again; the stored request stays from one connection to
  the next; the state is sampled ahead of the report slot by the worst frame time of the last 65
  frames (2.5 ms until the first run); a report's value is kept before it is sent and a report that
  cannot be sent is logged as an error; the vendor service's `0x45` flag is bit 0 of the puck link's
  last disconnect reason (the real controller's quirk: after an odd reason in the same boot, state
  reports go to the vendor characteristic and a host that only subscribed on the HID service gets
  none); the system state machine's drop reaches Bluetooth only past its gate (advertising, or a
  host not yet past it, is left running until the window ends), the dock's turn-off ends the link
  without the zero report, and the status LED and the plug-in rule count a host as connected before
  the gate; the device name is set once per boot, so a name a host writes over GAP lasts until the
  next boot (a unit serial stored at run time applies at Bluetooth's next start).
- With the synthesiser, `app/`'s optional vibration motor follows the rendered blocks' loudness.
- The test hooks' `HOST_SET` no longer takes `0xF2` with selector 1 (its GET can write flash now);
  `retained.c` is always built.
- The hardware test suite's lizard group runs its mapping tests with lizard mode's processing
  switched off, and follows every sequence until lizard mode goes quiet (a glide keeps the reports
  going); `outputs.stop_status` expects the replies by the build's caps.
- The USB device, HID and the command channel behave as the real controller's (October 2026):
  - The October device form is the default (USB 2.01 with its BOS descriptor, class `00/00/00`,
    release 4.04, 63-byte GET replies); July's stays as `CONFIG_OPENSC2_USB_PROFILE_JULY`. The
    test hooks' own opcode keeps 64-byte replies.
  - The report descriptor's mouse and keyboard items are in the real controller's order: all 372
    bytes are its own (also as the Bluetooth report map).
  - Reports are built while the bus is suspended and wait in the eight IN buffers for the resume;
    a report that finds no buffer is lost (a state report with its sequence number), lizard
    mode's and the application's too: nothing waits for a buffer on USB any more. The all-zero
    report before leaving the bus is tried once.
  - A wired boot with STEAM held goes on the bus once STEAM is released.
  - A GET's reply carries the request's bytes after the answer, then the rest of the last
    `SET_REPORT` (0.2.0: zeros); commands read their payload whatever the length says (a puck
    record shorter than its length reads zeros: it was refused).
  - The result codes are the real controller's: `-22` for an opcode nobody knows (through the
    puck it was `-134`; the application's `vendor_command` returning `-ENOTSUP` now means "not
    mine"), `-95` for a GET of a command that only acts.
  - `0x87` takes length / 3 triplets and ignores the bytes after the last one (a length that was
    not a multiple of 3 was refused). `0xDC` acknowledges every type and a failed save, and uses
    a new chime level only once it is saved. `0xC5` keeps channels above 100 (they were clamped).
  - `0xA2` stores the bond for the next boot after the answer, and the bond in use stays (it was
    in use at once). `0x86` deletes from flash 10 ms after the answer and leaves the values in
    RAM until its reboot 100 ms later (they were cleared at once). A host's power requests keep
    the first request's 20 ms when a second one follows within them.
  - Settings by name follow the real controller's handlers: an empty `0xED` name is refused, a name
    nobody has is `-22` for `0xEE` and `0xEF` and acknowledged by `0xF0`, `0xEF` of a key without a
    read function is `-134` and of one with nothing valid to read `-19`, and names no library key
    has reach any other Zephyr settings handler of the build (Bluetooth's, the application's;
    `CONFIG_OPENSC2_STORAGE` implies `CONFIG_SETTINGS_RUNTIME`), never the library's `opensc2/`
    subtree. `user/wireless_transport` takes any byte into RAM and changes nothing at once (it
    switched the puck slot at once and refused values above 3); `0xF0` of it no longer resets the
    slot.
  - A host that deconfigures the USB device no longer cuts a running rumble: it runs out by
    itself (500 ms), as on the real controller.
  - `0xE2` with a side above 1 answers -1 (`-EPERM`; it was `-22`), the real controller's code.
  - Through the puck, output reports `0x87` and `0x89` are told the record's length, one more than
    the report's payload, as the real controller's handlers are.
  - Over Bluetooth a feature read's answer is written over the value the host last wrote, so the
    bytes after the request and the answer are that value's (they were zeros).
  - `0xA1`'s "other" reset-cause flag counts Zephyr's causes from debug to temperature only; the
    bootloader and flash-error causes set no flag, as on the real controller.
  - Without `CONFIG_SETTINGS_RUNTIME`, `0xF0` no longer deletes another settings handler's key
    from flash (Bluetooth's bonds included): no other handler is reached by name then.
  - `opensc2_identity_set()`, `_store()`, `_clear()` and the by-name identity keys make one change
    at a time, so a board's call racing a host's write cannot mix the two.
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
