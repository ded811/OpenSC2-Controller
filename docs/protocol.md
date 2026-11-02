# The protocol, in brief

What OpenSC2 implements, in short: for the curious, and for anyone who wants to check the library
against the protocol documentation once it is public (until then, against this page and the API
reference). The essentials are on this page; the [API reference](api.md) has every field, command
and setting the library handles.

The library implements the controller's end of the 2.4 GHz link to Valve's puck, and the USB device
a Steam Controller 2 is on its cable. It was written from the protocol documentation the same
authors wrote (to be published in
[steam-controller-2-reverse-engineered](https://github.com/ded811/steam-controller-2-reverse-engineered);
not public yet), plus black-box tests and USB captures of Steam with a real controller
([How it was made](../README.md#how-it-was-made)). A few facts first came directly from the same
authors' study of the controller's firmware rather than from that documentation; the gaps below
say which, and which of them the documentation has now.

> [!NOTE]
> The research repository is not public yet, so the links to it on this page do not open until it
> is. They name where each topic is described in full; what OpenSC2 relies on is stated in the
> sections the "In brief" column points to.

| Topic | In brief | Full description (research repository) |
| --- | --- | --- |
| The controller's end of the radio link: set-up, listening, connecting, keep-alives, records, link loss, channels, bonds, the identity packet, protocol version, turn-off | [The radio link](#overview-the-radio-link) | [docs/controller/04-radio.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/04-radio.md) |
| The puck's end: slots, pairing window, connecting, channel choice, suspend | [The puck's end](#overview-the-pucks-end) | [docs/puck/04-radio.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/puck/04-radio.md) |
| The wired USB device: descriptors, strings, report timing | [The cable](#overview-the-cable), [api.md, Wired mode](api.md#wired-mode) | [docs/controller/03-usb-and-hid.md, The device](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#the-device) |
| The report descriptor and its collections | [The cable](#overview-the-cable), [gap 13](#where-this-library-had-to-fill-gaps) | [The report descriptor](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#the-report-descriptor) |
| Report `0x42`, the button bits, the vendor commands | [Reports and outputs](#overview-reports-and-outputs), [The vendor commands](#overview-the-vendor-commands), [api.md, Inputs](api.md#inputs) | [docs/controller/03-usb-and-hid.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md) |
| The inputs from sensor to report: sticks, triggers, trackpads, stick touch and grips, the IMU; the calibration keys and commands | [gaps 29, 31 to 33](#where-this-library-had-to-fill-gaps), [api.md, Calibration and raw inputs](api.md#calibration-and-raw-inputs) | [docs/controller/07-input-and-sensors.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/07-input-and-sensors.md) |
| The controller settings | [The vendor commands](#overview-the-vendor-commands), [api.md, Controller settings](api.md#controller-settings) | [Controller settings](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#controller-settings) |
| Lizard mode | [Lizard mode](#overview-lizard-mode) | [Lizard mode](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#lizard-mode) |
| Steam's wired session | [The cable](#overview-the-cable), [The vendor commands](#overview-the-vendor-commands) | [The wired session as Steam runs it](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#the-wired-session-as-steam-runs-it) |
| Pairing over the cable, and the reboot into wireless mode | [The cable](#overview-the-cable) | [Pairing a controller with a puck over the cable](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#pairing-a-controller-with-a-puck-over-the-cable), [Reboot into wireless mode](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#reboot-into-wireless-mode) |
| The July and October 2026 forms of the USB device | [gap 14](#where-this-library-had-to-fill-gaps) | [What changed since July](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#what-changed-since-july) |
| Wired or wireless at boot, the warm restart, the LEDs | [The cable](#overview-the-cable), [Reports and outputs](#overview-reports-and-outputs), [api.md, Modes](api.md#modes) | [docs/controller/09-system.md, The state machine](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/09-system.md#the-state-machine) |
| The battery voltage and charge level, what erases the stored settings | [api.md, Battery helpers](api.md#battery-helpers), [gap 31](#where-this-library-had-to-fill-gaps) | [Power and charging](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/09-system.md#power-and-charging), [What erases what](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/09-system.md#what-erases-what) |
| Power, the chords, the keylock, the idle timeout, low battery, shutdown, the status LED's modes and patterns, the cues | [Power, chords and the status LED](#overview-power-chords-and-the-status-led), [api.md, Power and the system state machine](api.md#power-and-the-system-state-machine), [api.md, Status LED](api.md#status-led), [gap 34](#where-this-library-had-to-fill-gaps) | [docs/controller/09-system.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/09-system.md) |
| Output reports `0x80`..`0x8A` | [Reports and outputs](#overview-reports-and-outputs), [api.md, Outputs](api.md#outputs) | [docs/controller/08-haptics.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/08-haptics.md) |
| Bluetooth LE: advertising, pairing, the HID service, the gate, reports, the transport choice (an opt-in build here) | [Bluetooth LE](#overview-bluetooth-le), [api.md, Wireless transports](api.md#wireless-transports-and-bluetooth-le) | [docs/controller/05-bluetooth.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/05-bluetooth.md) |
| The dock: the frames over the puck's charging pads, the pilot, dock power, the docked states (the protocol here; its UART build has never run on hardware) | [The dock](#overview-the-dock), [api.md, The dock](api.md#the-dock) | [docs/controller/06-dock.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/06-dock.md), [docs/puck/05-dock.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/puck/05-dock.md) |
| What the puck shows Steam (feature report pass-through, attribute cache) | [The puck's end](#overview-the-pucks-end) | [docs/puck/03-usb-and-hid.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/puck/03-usb-and-hid.md) |

The puck's side as an open firmware, public: [Counterpuck](https://github.com/ded811/Counterpuck)
and its [wire-formats.md](https://github.com/ded811/Counterpuck/blob/main/docs/wire-formats.md).

## Overview: the radio link

- **Radio**: Nordic Enhanced ShockBurst, dynamic payloads up to 128 bytes, 2 Mbit/s on the BLE 2M
  PHY, 16-bit CRC, +6 dBm, selective auto-acknowledgement, fast ramp-up. The puck transmits; the
  controller is the receiver and says everything in **acknowledgement payloads**, so each reply
  rides with the acknowledgement of the puck's next packet.
- **Bonds**: a bond, one pairing, is 24 bytes: the puck ID, the controller ID and the puck's
  serial. It is valid when both IDs are non-zero. The controller keeps two, for two pucks; the puck
  keeps one record per slot, with the controller's serial in place of the puck's.
- **Pairing** (channel 2): the puck's pairing window (which Steam can open with the puck's vendor
  command `0xAD`) beacons a hello to address `91 A2 A7 93`. The controller answers `0xF0 {T,
  serial}`; the puck sends `0xE0 {puck ID, puck serial}` to address T; the controller confirms with
  `0xF4 {C, serial}`; both store `{puck ID, C}`.
- **Connecting** (channel 2, address `ibex`, prefix `0x10`): the puck broadcasts `0xE1 {puck ID,
  controller ID, channel, its address, a pipe prefix}` without acknowledgement. The controller with
  that bond (exactly 18 bytes, both IDs equal) moves to the private pipe and acknowledges the
  hello that follows 1 ms later. The controller sets ESB up afresh each time it starts listening,
  listens for 60 s, then turns its radio off ("wireless off"); with the selected slot empty it
  waits 10 s for a bond first. Any bond write (either slot) starts the 60 s again, also one that
  leaves the slot empty; a bond written while connected counts from the next time it listens.
  The library does all of this (`CONFIG_OPENSC2_LISTEN_TIMEOUT_S` and
  `CONFIG_OPENSC2_NO_BOND_TIMEOUT_S` can turn the give-ups off).
- **Connected**: the puck opens with `0xE7 {0, 1}` (protocol version 1); the controller answers
  `0xF3 {1}` and once per connection its identity `0xF5`, 57 bytes: the product ID, the application
  and bootloader build times and the board revision (four bytes each), then the unit serial, the
  board serial and the git revision. Then the puck sends `0xE3` keep-alives about every 3 ms,
  carrying records `{length, type, data}`: SET feature (1), GET feature (3), output report (5). The
  controller answers `0xF1`: one SET result (2) or GET answer (4), the state report `0x42` as an
  input record (6), and any queued input reports, at most 127 bytes. Only a keep-alive gets a state
  report, due about 2.08 ms after it; one GET answer and one SET result wait, one each (a newer one
  is dropped while one waits; the GET answer goes first); up to 5 other input reports wait and ride
  behind the next state report (lizard mode's of a frame with the next one), and one that finds
  them full is dropped. "Disconnecting", the version answer and the identity are queued as soon as
  a packet is handled, each its own payload; the first disconnect reason of a connection stays.
  A GET record is answered with the last SET's command, as on USB: its one data byte is not the
  opcode (Valve's puck leaves it unset), and the puck drops an answer whose opcode is not its SET's.
  Seen on hardware: with that byte taken as the opcode, Steam's bond read through Valve's puck
  failed ("Failed to read triton info").
- **Channels**: `0xE4` gives a new channel list (for version 1: the new channel plus 2 and 80). The
  controller waits 10 ms per turn on its current channel, then 13 ms on each alternate; 500 ms
  without a packet is a lost link, and it listens on channel 2 again.
- **The PC sleeps**: the puck sends `0xE5` (turn off); the controller answers `0xF2` to every packet
  until the puck stops. `0xE6` says the PC is asleep while the link stays up.

## Overview: the puck's end

What OpenSC2 counts on from Valve's puck (`28DE:1304`), which it does not implement itself:

- **Slots**: the puck serves up to four controllers, one slot each. Every slot has its own USB HID
  interface, with the same 372-byte report descriptor as the controller, and its own pairing
  record. The puck's USB device also has a serial port with a shell, and on boards with a dock a
  dock interface.
- **The pairing window**: opened with the puck's vendor command `0xAD` (on its feature report 2),
  60 s by default. The slot first forgets its old pairing, then beacons every 10 ms (the exchange:
  [above](#overview-the-radio-link)). Steam pairs a Steam Controller 2 over the cable instead, and
  its log shows it writing the slot's record itself (the command it uses for that was not
  captured).
- **Connecting**: each new connection gets a new pipe prefix and a data channel. The puck sends
  `0xE1` every 4 ms (after 250 tries every 100 ms), each followed 1 ms later by a hello on the data
  channel, until the controller acknowledges one.
- **Connected**: after `0xE7` the puck exchanges packets for up to 1 s until the identity `0xF5`
  arrives (a controller that sends none is served anyway), then makes one exchange about every
  3 ms. It retries on its channel for 10 ms, then on the alternates too; 500 ms without an
  acknowledgement ends the link, and the slot starts connecting again.
- **Channel choice**: a data channel is one of the even channels 4 to 78 (2404 to 2478 MHz), scored
  by its background noise, the slot's packet error rate on it and its distance from the current
  channel. A link that keeps needing retries, or that answers only on an alternate, is moved to a
  new channel with `0xE4`.
- **Suspend**: 2 s after the PC goes to sleep the puck tells its controller to turn off (`0xE5`). A
  controller that connects while the PC sleeps makes the puck wake the PC; if it stays asleep, the
  puck says so with `0xE6` and keeps the link.
- **What Steam sees**: a slot passes the controller's input reports `0x40` to `0x45` only at exactly
  their size, and keyboard reports only with Enter, Escape, Tab or the arrows and no modifiers.
  Output reports and feature report 1 go to the controller as records. A GET of feature report 1
  works in two steps: the first one sends the request over the radio and fails ("try again"), and
  a later one returns the answer once it has arrived; an answer to another command than the SET's,
  or none within 1 s, ends the exchange. Since October 2026 the puck answers a GET after a SET of
  `0x83` or `0xAE` itself, from the identity packet (the SET still reaches the controller). Input
  report `0x79` gives the link state when it changes (1 no link, 2 connected, 3 just paired), and
  `0x7B` the link quality: every 500 ms while the link is healthy, and with a reason when it
  degrades, changes channel or drops. When a link drops, the puck sends an empty keyboard and mouse
  report, so nothing stays held.
- **The dock**: the puck is also a charging dock. A controller resting on it gets power through
  three pogo pads, and the puck talks to it over a slow serial link on the same contacts. OpenSC2
  has the controller's end of that protocol ([The dock](#overview-the-dock)); its UART build has
  never run on hardware.

## Overview: the cable

From the research documentation's USB page (its sections on Steam's wired session, pairing over
the cable and the reboot into wireless mode) and this project's own tests with Steam and Valve's
puck:

- **When**: only at a boot with USB power and without the warm-restart mark. The real controller
  then plays its chime, waits for STEAM to be released and attaches; every way out of wired mode is
  a reboot or a power-off, so the device attaches at most once per boot. A controller whose radio
  link is up stays wireless on the cable (it only charges), unless STEAM is held while plugging in.
- **The device**: `28DE:1302`, strings "Valve Software", "Steam Controller" and the unit serial (the
  same serial as vendor command `0xAE` tag 1 and the radio identity record); one configuration, bus
  powered, remote wakeup, 500 mA; one HID interface without boot protocol, interrupt IN `0x81` and OUT
  `0x01`, 64 bytes every 1 ms; a 372-byte report descriptor with a mouse (`0x40`), a keyboard
  (`0x41`) and the vendor collection (everything else), which Windows shows as `Col01` to `Col03`.
  Steam opens `Col03`.
- **Reports**: `0x42` (the same 53 bytes as through the puck, one sequence counter for every link)
  every 132 ticks of the 32,768 Hz clock (4.03 ms) from the moment the host sets the configuration,
  whether or not an application has the device open, through eight buffers (in the library a state
  report that finds none free is dropped, so nothing waits for the host). `0x43` every 3.5 s. The
  mouse and keyboard reports of lizard mode. Output reports arrive on interrupt OUT.
- **Commands**: feature report 1. A 64-byte `SET_REPORT` carries `01 <opcode> <n> <payload>` and runs
  the command; if it succeeds it becomes the stored request, which `GET_REPORT` answers (64 bytes in
  the July form, 63 in the library's October form). Each link keeps its own stored request. A
  refused command, an unknown opcode, a SET of the wrong length and a GET of a command without an
  answer stall the control request.
- **Steam's session**: identity reads (`0x83`, `0xAE`), lizard mode off (`0x81`, repeated), its
  settings (`0x87`), the LED, and two zero pulses that stop the trackpad haptics.
- **Keeping lizard mode off through the puck** (seen with this board and the test suite): once
  logged in, Steam writes setting 9 = 0 with `0x87` every 3.0 s (the period SDL uses too) and sends
  no `0x81` meanwhile. Before its logon it wrote 9 = 0 once and then nothing, so the 10 s watchdog
  turned lizard mode back on, as on the real controller.
- **Pairing over the cable**, with a puck on the same PC: Steam reads the stored transport and the
  current bond (`0xED`), writes the puck's slot on the puck, then writes the controller's new 24-byte
  bond (`0xEE esb/bond`) and saves it (`0xEF esb/bond`), reads it back, reads the identity, and sends
  `0x95` with `0xA427AF52`: "reboot into wireless mode". Both IDs are new at every pairing.
- **Reboot into wireless mode**: the controller acknowledges; 20 ms later it sends one all-zero
  `0x42`, waits 10 ms, leaves the bus and reboots with the warm-restart mark set. At the next boot
  the mark (read and cleared at once) makes it start its radio with the stored bond although USB
  power is there, and the puck, already calling the new controller ID, connects within a second.

## Overview: the vendor commands

Steam's command channel is feature report 1, with the same commands on every link. A command is
`{opcode, length, payload}`: a SET runs it, and a GET reads the answer to the last SET on the same
link (each link keeps its own; before the first SET a GET answers `0x83`). How the cable and the
puck's records carry them is above. What Steam was seen sending:

| Opcode | Does | When Steam sends it |
| --- | --- | --- |
| `0x83` | reads six attributes: the product ID `0x1302`, capabilities, the bootloader and application build times, the hardware ID, the report interval | to identify the controller: on the cable, and through the puck (which since October 2026 answers it itself) |
| `0xAE` | reads a string: tag 1 the unit serial (0 and 4 the board serial, 3 the git revision) | the same |
| `0x81` | turns lizard mode off | on the cable, at the start of a session and again later |
| `0x87` | writes controller settings, `{number, value}` each | on the cable in a few groups (the IMU off, both pad click thresholds to 100 %, calibrated sticks, the LED brightness to 100, ...); through the puck setting 9 = 0 every 3 s |
| `0xDC` | stores the power-on chime level | on the cable |
| `0xE2` | persists calibration; Steam's form (side 1, mask `0x20`) saves nothing and is acknowledged | on the cable |
| `0xC1` | nothing: the controller does not know it and stalls it, and Steam carries on | on the cable |
| `0xED` | reads a setting by name: `user/wireless_transport`, `esb/bond` | when it pairs the controller over the cable |
| `0xEE`, `0xEF` | sets, then saves, `esb/bond`: the new 24-byte bond | the same |
| `0x95` with `0xA427AF52` | reboots into wireless mode | at the end of that pairing |

**Controller settings** are 86 numbered 16-bit values, in RAM only and back to their defaults at
every boot. `0x87` writes any number of them (each clamped to its range, but for the IMU's 48, 84
and 85), `0x89` reads one, `0x8E` restores the defaults. Setting 9 is lizard mode, 37 the LED colour
override, 45 the LED brightness, 48 the IMU stream and 71 the Steam watchdog.

The controller also takes `0x90`, and `0x95` with `0xECAABAC0`, as a request to reboot into its
bootloader, for Steam's firmware update; the library hands those to the application. What the
library answers to each command, and every setting it knows: [api.md](api.md#vendor-commands).

## Overview: lizard mode

Until Steam takes over, a Steam Controller 2 also works as a plain mouse and keyboard, so that it is
usable without Steam. OpenSC2 does the same, on every link:

- **Mouse** (`0x40`): the right trackpad moves the pointer and the left one scrolls (up and down as
  the wheel, left and right as horizontal pan); the right trigger or a right-pad click is the left
  button, the left trigger the right one.
- **Keyboard** (`0x41`): A is Enter, B and VIEW are Escape, the D-pad gives the arrow keys and MENU
  is Tab; with more than six keys held the report carries the roll-over code. These are the only
  keys the puck lets through.
- **On** at every boot (setting 9 = 1). **Off** with `0x81` or with setting 9 = 0; the first `0x81`
  sends an empty keyboard report and then an all-zero mouse report, so nothing stays held.
- **The Steam watchdog**: unless setting 71 is 0, the host has to repeat one of those within 10 s,
  or lizard mode comes back and the settings return to their defaults. How Steam keeps it off
  through the puck: [the cable](#overview-the-cable).
- **The motion** as the real controller's: a new touch is held still until the thumb really
  moves, the motion is averaged over 8 frames, a flick keeps gliding (settings 11 and 12), and
  moving across a pad makes haptic ticks (setting 14), which the library hands to the application
  as haptic events. What stays different, and why: [gap 11](#where-this-library-had-to-fill-gaps).
  Every rule, with its scales: [api.md](api.md#lizard-mode).

## Overview: reports and outputs

- **Input reports**, the same on every link: `0x42`, the state (53 bytes after the ID: a sequence
  number, the button word, both triggers, both sticks, both trackpads with their pressure, and the
  IMU's timestamp, acceleration, angular rate and orientation; [api.md](api.md#inputs) has every
  offset and button bit); `0x43`, the battery, every 3.5 s; `0x44`, the haptic stream status;
  `0x40` and `0x41` in lizard mode. The library applies the real controller's rules to every `0x42`
  ([api.md](api.md#the-rules-on-every-report)): a stick deflected beyond 12000 counts as touched, a
  pad is blank while the thumb rests on its stick (setting 82), no trigger bit with raw trigger
  output (setting 62), the trigger bits from setting 68, and optionally the pads' clicks from their
  pressure (settings 52 and 53).
- **Output reports**: `0x80` rumble (a level and a speed for each side; it lasts 500 ms, so the
  host repeats it), `0x81` a pulse train, `0x82` a stop or one of two clicks, `0x83` a tone, `0x84`
  a frequency sweep, `0x85` one of the built-in haptic scripts, `0x86` to `0x89` PCM streamed to
  the actuators (`0x86` starts or stops it and sets the format, the others carry the samples);
  `0x8A` does nothing. Most of them start with a target byte that picks the pads or the grips. The
  library decodes each one for the application ([api.md](api.md#outputs)) and, with its haptics
  synthesiser (in the app), also plays them as the real controller's two sequencers do: sample
  blocks for the four channels, outputs the application provides, the streams' flow control and
  the replies to stops, `0x44` ([api.md](api.md#the-haptics-synthesiser); gap 35).
- **The LED**: Steam sets its brightness (setting 45) and can override its colour (`0xC5`, shown
  while setting 37 is on); the library hands both to the application. The real controller's
  status LED shows its mode by colour: green while wired, white with a little blue on the puck's
  link, amber on the dock. The library selects that mode too ([below](#overview-power-chords-and-the-status-led)).

## Overview: power, chords and the status LED

From the research documentation's page on the controller's system state machine. OpenSC2 runs the
same machine (`CONFIG_OPENSC2_SYSTEM`, on by default; built, not yet tested on hardware); every
rule, state and timing is in [api.md](api.md#power-and-the-system-state-machine):

- **Power-on**: a wired boot plays the power-on chime and waits for a host; a wireless boot on USB
  power (after the warm restart) plays the stored transport's cue and starts its link; on the dock
  the radio stays off (after a warm restart the stored link starts); on battery the board first
  checks its level (0 %: off; 2 % or less: off with the low-battery signal) and the keylock (off),
  then plays the stored transport's cue and waits 1 s for USB power or the dock before it starts
  the link.
- **STEAM**: pressed 20 ms it is a press, held 7 s it turns the board off on battery, or switches
  wireless off on USB power or the dock (the amber LED); on USB power a press then reboots the
  board, which comes back wired (on the dock the chords choose a link again instead).
- **The transport chords**: A + right bumper the main puck, A + left bumper the second, B + right
  or left bumper Bluetooth slot 1 or 2, STEAM alone the stored transport. Held at a battery power-on
  (or pressed on the dock with the radio off) a chord chooses its transport; held 4 s it also asks
  for pairing. **The keylock** (STEAM + L4 + L5 + R4 + R5 held 3 s) makes the next battery
  power-on turn straight off.
- **Turning off**: the idle timeout (setting 50, 15 minutes), 0 % battery, the turn-off command
  (`0x9F`), a link that gives up (60 s listening, 10 s without a bond) and the puck's turn-off end
  battery operation with the power-off chime. On USB power the turn-off command, a link that gives
  up and the puck's turn-off only switch wireless off, and the idle timeout and the level do
  nothing. Setting 77 makes the shutdown reboot instead.
- **The status LED**: blue for Bluetooth, white with a little blue for the puck, green while wired,
  amber on USB power without a host or with wireless off, red flashes for low battery, white
  flashes for the keylock, each with its pattern (searching, connected, pairing, breathing on
  external power, a dimmer display once charged).
- **Cues**: the power-on and power-off chimes, a cue per transport, pairing and paired, and one when
  the puck says the PC is asleep; the chime level Steam stores (`0xDC`) sets their gain or mutes
  them.

## Overview: Bluetooth LE

The controller can also connect straight to a PC, phone or console over Bluetooth LE, without the
puck. OpenSC2 has it as an opt-in build (`CONFIG_OPENSC2_BLE`), built and **not yet tested on
hardware**; [api.md](api.md#the-bluetooth-build) has every detail and what is not built yet.

- **One transport at a time**: the stored `user/wireless_transport` decides (0 and 1 Bluetooth slots
  1 and 2, 2 and 3 the puck with bond 1 or 2); October saves a new choice only once its link is up.
- **Advertising**: "Steam Ctrl (BT) " and the unit serial; connectable legacy advertising every 100
  to 150 ms; flags, the gamepad appearance (`0x03C4`), the HID and battery service UUIDs, the TX
  power and manufacturer data of company ID `0x055D` with no payload; the name in the scan response.
  Without a bond the slot is open to any host (pairing mode); with one, advertising is directed to
  its host (1.28 s), then allow-listed for it. Every start of advertising arms a 60 s window, after
  which wireless goes off.
- **Identities and pairing**: Just Works, LE Secure Connections only, bonding; every HID value needs
  encryption. Each slot has two identities (1 and 2, 3 and 4) with addresses of their own; a
  pairing request resets the slot's other identity and advertises on it, so the old host keeps its
  bond until a new one has paired, and each pairing appears to hosts as a new device.
- **Disconnects**: the host ending the link (`0x13`) or powering off (`0x15`) turns wireless off;
  any other loss advertises again 500 ms later on the same identity.
- **Services**: HID over GATT with the USB report map byte for byte (inputs `0x40` to `0x45`,
  outputs `0x80` to `0x89`, feature report 1); Device Information with PnP ID `28DE:1303`, model
  "Steam Controller", an empty manufacturer name, the unit serial and the build time; Battery; and
  Valve's vendor service (`100F6Cxx-1735-4313-B402-38567131E5F3`), the same reports as plain
  characteristics, which SDL's Android and Apple back ends use. Vendor command `0x83` still reports
  product ID `0x1302`.
- **The gate**: no report until the link is encrypted and the last ATT MTU heard of (kept across
  links) is 64 or more; then Bluetooth is the active transport and attribute 11 of `0x83` is the
  connection interval in µs.
- **Reports**: one state report per connection event, as `0x45` (the first 45 bytes of `0x42`, the
  quaternion cut); lizard mode, battery and haptic status on their own characteristics.
- **Commands and outputs**: feature report 1 is the command channel (refusals are ATT errors);
  output reports go to the same dispatcher as on every other link.

## Overview: the dock

From the research documentation's dock pages (the controller's and the puck's); nothing of it has
been seen on a wire. The puck's three pogo pads are `+` (a rail at about USB voltage the puck
switches on itself), `−` (ground) and `P`, the **pilot**: an analog level that tells the puck a
controller is there, and (inferred) the line of a half-duplex serial link too.

- **The puck's side**: idle, it keeps `+` off and reads the pilot every 100 ms, every 1 ms once it
  lies inside its window; after 200 readings in a row inside it, it switches `+` on, checks both
  voltages every 5 ms (not during its own exchanges), and after 1 s sends a probe. An answer means
  attached (the puck reports it to the PC as input report `0x79` = 2 on its fifth HID interface);
  no answer is a fault (`+` off for 10 s). While attached it relays the PC's feature report 1 and,
  with nothing pending for 64 ms, polls the controller's input; a failed poll that a probe 10 ms
  later does not forgive sends it back to idle. Each exchange: send; its own request must come
  back as an echo, whole and with a good CRC, within 100 ms (or the exchange fails), and is thrown
  away; then up to 64 ms until the whole answer is in, so the controller has about 57 ms after the
  request to start sending; then 10 ms; one retry. Of an answer the puck checks only the length and
  the CRC, and it reads the result only of a GET of a vendor command. The puck answers Steam's
  `0x83` and `0xAE` for a docked controller itself, from the probe's record (attribute 11 = 64000).
- **The link**: 115200 baud, 8N1, no flow control. Every message is a 73-byte frame: the start byte
  (`0x5A` from the puck, `0xAC` from the controller, checked by nobody), the request type, two unused
  bytes, the result (s32), 63 payload bytes and a CRC-16/KERMIT of bytes 0 to 70, low byte first.
  The receiver runs the CRC over all 73 bytes and accepts 0.
- **The requests**: 0 probe (the answer: the 57-byte identity record, as radio packet `0xF5`); 1 GET
  of a vendor command (`{opcode, length, data}` of the command last set on the dock, result 63 or a
  negative error); 2 SET (`{opcode, length, data}` through the same command channel as every link,
  result 0 or a negative error); 3 GET input (the latest state report `0x42`, its 53 bytes after the
  ID). The dock keeps its own stored request, `0x83` after a boot. `0x81` and `0x85` from the dock
  are acknowledged and do nothing.
- **The controller's session**: it checks the pilot (three readings 3 ms apart, present when two lie
  inside 2000 to 3300 mV at its pin), then serves one request at a time with no time limit for the
  first byte; a 64 ms silence inside a frame, a bad length or CRC, or a receive error ends the
  session and it checks the pilot again. It arms its receiver only after its answer has left.
- **Answers**: each is built over the previous one; a request writes only what it defines (a
  probe its record, not its type or result; a SET its type and result, not the payload; an unknown
  type nothing at all).
- **Input over the dock**: with another link up, the dock gets a copy of that link's latest state
  report; with the dock alone, each GET input takes one sample, which the next GET input answers.
  Nothing else goes to the dock (no lizard mode, no `0x43` or `0x44`), and nothing comes back but
  vendor commands: no output reports.
- **Dock power**: the controller tells the dock from USB power by a fresh pilot check at the moment
  its supply input (the dock's `+` and the USB-C port share it) changes; its state machine then has
  docked states: with the radio off (charging; a transport chord or STEAM starts the link), with
  the link up (turn-off goes back to radio-off with the power-off chime), and lifted off (5 s, then
  off). A warm reboot on the dock starts the stored link again. While it rests on the dock, its
  sticks' calibration is moved by fixed offsets (the dock's magnets).

What OpenSC2 does with it, and what it leaves out: [api.md, The dock](api.md#the-dock) and gaps 36
to 41 below.

## Where this library had to fill gaps

The research documentation describes one real controller firmware. Where it leaves something
open, the library does what should work with Steam and the puck, and says so here:

1. **Over-the-air pairing.** Pairing over the air (the puck's pairing window) is implemented but
   untested with Steam. Steam pairs controllers over a wired connection, and pairing over the USB-C
   cable is the tested way (the sequence in brief: [the cable](#overview-the-cable); in full:
   [the documented sequence](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#pairing-a-controller-with-a-puck-over-the-cable)).
   The documented controller firmware contains an over-the-air pairing routine that nothing in it
   ever starts
   ([pairing is unreachable](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/04-radio.md#pairing-is-unreachable)),
   but the puck still has its pairing window, so the library implements the documented exchange
   from the controller's side, with the puck's documented timings (and keeps its confirmation queued
   if the puck repeats its offer). A wireless board answers an open window when its pairing button
   is pressed; with [Counterpuck](https://github.com/ded811/Counterpuck) as the puck, its shell
   command `pair 0` opens a window without Steam (also untested with OpenSC2).
2. **Rumble report `0x80`** (closed). The research documentation and SDL's `MsgHapticRumble` agree:
   `[0]` the type, `[1..2]` the level, `[3..4]` the left speed, `[6..7]` the right speed. The
   library also decodes the gain bytes `[5]` and `[8]`, which the July 2026 firmware used.
3. **Report `0x43` charge state.** The library sends the application's charge state as given, in
   SDL's `EChargeState` numbering (the type SDL gives this byte; SDL reads 1 as on battery, 2 as
   charging, 4 as charged). The real controller adds a flag bit `0x80` to it while its charger has
   given up on a supply that does not charge the battery (it tries again 60 s later). The library
   sends what the application gives, flag included (`OPENSC2_CHARGE_GAVE_UP`), and then, as on the
   real controller, the power rules and the LED are told 100 % while the level is above 0. The
   real controller's byte is the state of its charging rules; `<opensc2/charger.h>` gives a board
   the same rules for its own charger ([api.md](api.md#charging-helpers)), and the blank canvas uses
   them for an optional charger status pin. A board without a charger it can read reports what it
   knows. The level goes out as given, 255 included (the real
   controller's "no level yet"); before the application gives a battery, a battery build sends what
   the real controller's first report after start-up carries (the level kept over a warm reset, else
   255, charge state 0, zeros), and a build without a battery a full one.
4. **Answer layouts not given byte for byte**: `0xF2` selector 2 (uptime u32, then the reason as a
   u32) and `0xAE` (the tag, then the string, 20 bytes in all, as Counterpuck answers for the
   puck). `0xA1` follows the documentation byte for byte (18 bytes; the real controller's flag
   encoding of Zephyr's reset causes); whether the UF2 bootloader, which runs before the
   application at every reset, leaves the chip's reset-reason register alone has not been
   checked.
5. **Which commands Steam sends through the puck** is not fully documented. The library answers the
   identity, settings, LED, user-store and settings-by-name commands and passes the rest to the
   application. On the cable the commands Steam sends are known
   ([the vendor commands](#overview-the-vendor-commands);
   [Steam's wired session](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#the-wired-session-as-steam-runs-it)),
   and each gets the real answer or the real STALL (Steam's `0xC1` is stalled, as by the real
   controller).
6. **Controller settings** follow the documentation: every one of the 86 has its documented default
   and range, and `0x87` clamps each value to its range ([api.md](api.md#controller-settings)),
   also for the 49 settings nothing reads (0.2.0 knew nine ranges and stored the rest as sent, with
   0 as the default where it knew none; seven of those nine, the ranges of settings 9, 11, 12, 14,
   45, 70 and 71, first came from the same authors' study of the controller's firmware, and the
   documentation has them now). Settings 48, 84 and 85 are stored as sent, for the
   application (the real controller hands them to its IMU unclamped), and `0x89` answers their
   defaults, as on the real controller. Whether Steam reads them back is not known. `0x89` of a
   number above 85 reads 0, as on the real controller (0.2.0 stalled it).
7. **The quaternion** (offsets 45 to 52): the real controller never sends W = 0. July sent the
   identity (W 32767, X = Y = Z = 0); October sends its fused rotation and keeps the last sample
   while the stream is off. The library sends the application's quaternion, and its "none" (all
   four 0, the state of a board without an IMU or without an orientation) as the identity; the
   library's IMU (`<opensc2/imu.h>`) keeps the last sample while the stream is off, as October
   does, and the blank canvas feeds its IMU through it. October's state starts empty: until its
   IMU's first sample (only seen when its stream does not start at boot) it sends all 0, W
   included, and so does the library for a board that set its IMU up. (0.2.0 sent all four 0 without an IMU; the
   parity work's blank canvas sent "none" while setting 48 was 0.)
8. **Payload limit**: the controller's payloads are "within 127 bytes", the puck accepts 128; the
   library stays at 127.
9. **The radio link: what stays different.** The link follows the documented controller turn by
   turn (the states, timers, replies and their order, the answers, the queues, the reasons, the
   bonds and the transport key). What stays different, none of it seen by the puck, Steam or SDL:
   - the October firmware's change to the ESB driver (the processor, not the radio, starts each
     reception and acknowledgement) is not reproduced: stock nRF Connect SDK ESB is used (it would
     take a changed SDK);
   - one thread does the work of two: the real controller's input loop builds the state report
     at its time while its radio thread goes on with the next turn; OpenSC2's radio thread builds
     it while it waits for the next packet (on any channel of the walk), so packets, turns and
     timers keep the real controller's timing, but a report that falls due while a packet is
     being handled (a channel change's 0.5 ms wait, a command that writes flash) is built right
     after that packet, where the real input loop would cut in; the thread runs at priority 5
     (`CONFIG_OPENSC2_THREAD_PRIORITY`), the real one at 9 below its input loop's 8;
   - the 32 MHz crystal is waited for at most 500 ms (the real controller waits without a limit;
     either way the radio stays off), and MPSL is not shut down when the link stops (gap 23);
   - "wireless off" reaches the system state machine only for the give-ups, the puck's turn-off
     and a refused version, not after a stop of the board's own (gap 34): the states that stop
     the link themselves ignore it either way;
   - the identity packet `0xF5` carries the identity in use when it is sent (the real controller
     fills it once at start-up; only OpenSC2 can change an identity at run time);
   - the documented defects are not copied where copying them would corrupt memory or hang the
     link: records are read within the packet and a SET record's bytes within the command
     buffer, a channel list without a valid channel (or one above 100) is ignored, a connection
     request for a channel above 100 is not taken, a short `0xE7` counts its missing bytes as 0,
     a report sent in the moment the link drops leaks nothing, output `0x8A` does nothing, and a
     stop that arrives together with a bond write while listening stops the link (the documented
     controller listens again and loses the stop);
   - when the link drops, the application's `rumble` callback hears a stop at once (`timeout`
     set), so a board's motors never run on without a host; the real controller's rumble runs out
     on its own;
   - over-the-air pairing (gap 1), run-time transport and slot switching, the give-ups and the
     transmit power set at run time, the test hooks and the application's view (`link_changed`,
     `opensc2_link_info()`) are OpenSC2's own.
10. **Board revision.** What Steam does with the hardware ID is not documented; the default is
    `0x48` (72), what a real controller reports and the value Steam has been seen working with
    ("Controller Info: HWID: 72"); `0x45` and `0x49`..`0x4B`, the other values real controllers
    accept, stay selectable.
11. **Lizard mode** follows the documented key map and mouse mapping, the release reports when it
    turns off, the 10 s Steam watchdog, and the documented motion: the touch stabilisers, the
    8-frame average, the glide after a flick (settings 11 and 12) and the haptic ticks and clicks
    (14, 52, 53, 70)
    ([Lizard mode](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#lizard-mode),
    [the touch stabiliser](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/07-input-and-sensors.md#the-touch-stabiliser),
    [haptics in lizard mode](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/08-haptics.md#haptics-in-lizard-mode)).
    The library follows it to the detail, quirks included: the keyboard and the mouse turn on and
    off by events that they take up in the real controller's order once the command that posted
    them is done, so `0x85` while the keyboard runs leaves an "on" pending and the next switch-off
    sends the empty `0x41` twice and leaves the keyboard typing with setting 9 at 0 until the
    switch-off after that; the keyboard hears the buttons when the application changes them, also
    while it is stopped and while no link runs (its report then goes nowhere); the travel a glide
    needs adds exact lengths, and the flick speed and the glide's factor are single precision,
    with setting 12 read in every frame; setting 82 drops a pad's
    motion after the division (the remainders keep their part); a mouse report goes out while one
    of its bytes is not zero; the right pad's stabiliser is left as it is while setting 80 is not
    1; a pad click counts only while its pad is touched; the ticks and clicks forget the touch but
    not the click while setting 9 or 70 stops them, and compare the steps with setting 14 in every
    touched frame. What stays different, and why:

    | Detail | The real controller | The library |
    | --- | --- | --- |
    | When ticks and clicks are worked out | per trackpad sample | once per state report, on the pad as the report carries it: the library sees the trackpad only through the input state, and its pad processing (rotation, the setting-80 stabiliser, setting 82) runs once per report. The same on the cable and through the puck when the pad samples no faster than the reports; over Bluetooth a fast swipe across a long connection interval can count fewer steps |
    | When a key report goes out | as soon as its button driver reports the change, debounced (16 ms) and polled every 2 ms | as soon as the application reports the change (`CONFIG_OPENSC2_LIZARD_KEYS_AT_ONCE`, default on; also while no link runs, when it goes nowhere): the debouncing and polling are the board's, and a board that updates its buttons only in the `sample` callback gets them at the frames |
    | The haptic events' sound | its own click recordings on its pad actuators | the library hands over click A or B and the documented level ([api.md](api.md#outputs)); with the haptics synthesiser it plays them itself with its own click sounds, while lizard mode is on, setting 70 is exactly 1 and the current link is not the dock (gap 35). The recordings are Valve's data and stay out |
    | A state report skipped on the cable (bus suspended, no free buffer) | the frame still runs: the motion is worked out and a glide runs down, and its reports wait for a buffer | the frame is skipped with the state report: the library's frames are its state reports. The next frame takes up the motion (a difference of positions); a glide pauses for the skipped frames |
    | Reports waiting through the puck | the reports wait for a buffer, then queue for the radio and are dropped when its queue is full | gap 30 |

    The key-slot rule, the scales (128 pad units per pointer count, 1024 per scroll step, pad up
    scrolls with a negative wheel value) and the rule that a mouse report goes out every frame
    while anything is non-zero first came from the same authors' study of the controller's
    firmware; the documentation has them now too (the ranges of the settings: gap 6). All of this
    is built and tested on the host (the library against the runner's model, and in the firmware
    emulator); it is not yet tested on hardware, and its feel needs a trackpad, which no test has
    had.
12. **A bond written with `0xA2`** (closed): as on the documented controller, it is written to
    flash from a work item after the answer (an invalid one, an ID of 0, deletes the stored bond)
    and counts from the next boot; the bond in RAM and the radio are not touched.
13. **The report descriptor's bytes** (closed). All 372 bytes are the real controller's, the items of
    the mouse and keyboard collections in its order too (0.2.0 had 20 of their bytes in another
    order); the same bytes are the Bluetooth report map. The collections, every report's ID,
    direction and size and the vendor items' rule are in the research documentation
    ([The report descriptor](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#the-report-descriptor));
    the mouse and keyboard items' order and the vendor reports' order come from the same authors'
    study of the controller's firmware.
14. **Two forms of the device.** The library has both:
    `CONFIG_OPENSC2_USB_PROFILE_OCTOBER` (the default after 0.2.0), the October 2026 firmware's
    form: USB 2.01 with a USB 2.0 extension BOS descriptor, class `00/00/00`, release 4.04, 63-byte
    GET replies; and `CONFIG_OPENSC2_USB_PROFILE_JULY`, the July 2026 controller firmware's form:
    USB 2.00, class `EF/02/01`, release 3.07, each GET answered with 64 bytes (both described
    in the research documentation:
    [The device](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#the-device),
    [What changed since July](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#what-changed-since-july)).
    The July form is the one Steam was seen pairing over the cable with a real controller, and the
    one tested with OpenSC2 0.2.0; the October form has not been run on hardware with this library
    yet. A GET of the hardware test channel's own opcode (answered only while the test hooks are
    armed) is 64 bytes in both forms.
15. **Standard requests** are answered by Zephyr's USB stack and HID class (`SET_IDLE` stored, then
    stalled; `SET_PROTOCOL` and `GET_PROTOCOL` stalled), the stack the October 2026 firmware is built
    on too. `SET_REPORT` of an output report on the control endpoint and feature report 2 stall, as
    with the October firmware.
16. **Suspend and full buffers** (closed). As on the October firmware, nothing reacts to a suspended
    bus: reports go on being built and wait in the eight IN buffers, so a host first reads up to
    eight older ones after a resume. A report that finds all eight in use is lost, a state report
    with the sequence number it was built with (0.2.0 skipped it before building it, and let
    lizard mode's and the application's reports wait). July's STEAM-press remote wakeup is an
    option (`CONFIG_OPENSC2_USB_REMOTE_WAKEUP`).
17. **USB power that appears later.** The real controller reacts to its charger's power events. The
    library checks the chip's own USB power detection at boot and, in wireless mode, every 100 ms
    (in a battery build the system state machine does, every 50 ms, in its battery state); with the
    link down, or STEAM held, it asks the application to reboot, which brings the board back wired
    (only while the policy is auto). In the 1 s battery wait at power-on a battery build turns wired
    in place, as the real controller does.
18. **STEAM while waiting for a host** (closed). As on the real controller, a wired boot with
    STEAM held goes on the bus once STEAM is released, and a STEAM press of 20 ms or more before a
    host configures the device, one that began before the boot included, sets the warm-restart
    mark and reboots: the board comes back wireless on USB power. The system state machine's wired
    start runs this rule; without `CONFIG_OPENSC2_SYSTEM` the USB layer keeps the device off the
    bus while STEAM is held and counts a press only after it has seen STEAM released. The library
    sees STEAM held at the start when the application reported it before `opensc2_init()` (the
    blank canvas does). The power-on chime is the system state machine's (gap 34).
19. **Haptic stream status after a stop** (closed). The real controller answers each pair of
    Steam's zero pulses with two `0x44` reports (right pad, then left pad, flag `0x02`)
    ([Steam's wired session](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#the-wired-session-as-steam-runs-it));
    the documentation of its haptics explains them: every stop-all stops the channel's stream,
    which reports `0x02`, whether or not anything was streaming. The library sends the same
    replies for the same stops, in the documented order. With the haptics synthesiser (in the
    app) its sequencers send them as the real ones do, when each takes the stop: within about a
    millisecond while idle, up to one 8 ms block later while something plays, and not at all for a
    stop that a full queue lost. Without it, `CONFIG_OPENSC2_HAPTIC_STOP_STATUS` (on by default;
    [api.md](api.md#other-input-reports)) sends them as soon as the output is decoded. Steam does
    not appear to need them.
20. **Identity on the cable.** The USB strings are the real ones by default and the serial string is
    the identity's unit serial, kept equal to `0xAE` tag 1 and the radio identity record. A
    development board reports its own "OSC..." serial, which no real controller has. With the
    default identity (an "OSC..." serial, hardware ID `0x48`, no bootloader build time) Steam paired
    the board in the tested setup ([What works](../README.md#what-works)); other values are
    untested. A build time at or before `0x6A18D057` makes Steam's updater force an update, and one
    older than the controller firmware Steam ships makes Steam offer one
    ([troubleshooting](troubleshooting.md#steam-asks-to-update-the-controller)). The product string
    is the identity's product name, the real "Steam Controller" unless a name is stored on the
    board; a stored one makes the wired device differ from the real controller. The real
    controller reads its values from its own chip at every start; a board supplies its own in
    `opensc2_init()`, or later with `opensc2_identity_set()` (RAM only), or stores them.
21. **The cable pulled while wired.** The real controller shuts down. With the system state
    machine the library does the same at its next look at the power source (every 50 ms, as the
    real controller polls its supply): the power-off cue, 0.5 s, off (a power-off in a battery build,
    a reboot otherwise, [gap 34](#where-this-library-had-to-fill-gaps)). Without it
    (`CONFIG_OPENSC2_SYSTEM=n`) the library waits 0.2 s for USB power to come back, then asks the
    application to power off or reboot (`OPENSC2_POWER_USB_UNPLUGGED`), and without a callback it
    reboots, which comes back wireless on a battery. A board without a battery has lost its power
    anyway.
22. **The wireless transport** (`user/wireless_transport`, closed). As on the documented controller:
    `0xEE` writes any one byte into RAM and leaves the running link alone, `0xED` reads it back,
    `0xEF` stores it, `0xF0` deletes it (the key reads 2); at boot 0 and 1 run Bluetooth, 3 the
    second puck and every other byte the main puck; a choice made at run time leaves the key alone
    until its link comes up, and the running transport is then written back into the key, and
    stored, at every connection. The library's own additions: without Bluetooth, 0 and 1 run the
    main puck; a board that stored only `opensc2/slot` (older firmware) starts on that puck slot;
    the application can choose the puck's slot for one boot (`opensc2_config.bond_slot`). What
    Steam does when it reads 0 or 1 during a cable pairing is not known.
23. **Bluetooth LE.** The bond rule, the pairing cue's replay, the kept ATT MTU, feature report
    1's reads and its stored request, the sampling margin, a refused report's log, the vendor
    `0x45` flag, the system state machine's drop and the LED's "connected" follow the documented
    controller, quirks included. Where OpenSC2 still differs, or the documentation is silent:

    | Point | The documented controller | OpenSC2 |
    | --- | --- | --- |
    | Sharing the radio with the puck's ESB | MPSL is shut down when the puck link stops, and started again by Bluetooth; Bluetooth, then the puck, then Bluetooth within one boot breaks (inferred) | MPSL stays initialised (in this build its clock driver also serves the USB console's crystal); ESB takes the radio interrupt while it runs and the library hands it back to MPSL; switching between the two within one boot is supported both ways |
    | The advertising window | 60 s | 60 s by default, a Kconfig option (`CONFIG_OPENSC2_BLE_ADV_WINDOW_S`; 0: advertise until a host connects) |
    | "Wireless off" | on battery the controller turns off; on USB power wireless goes off | the same with the system state machine (the default); the library also keeps the wireless-off flag and its reason (`opensc2 ble status`, `opensc2_ble_status()`, the test hooks' `BLE_STATUS`); without the machine the application gets `OPENSC2_POWER_WIRELESS_OFF` and the blank canvas stays on until a button or `opensc2 start` |
    | A start while Bluetooth runs | a start is ignored unless Bluetooth is off; the state machine never sends one then | only the library's API and shell reach this: the same slot without pairing keeps the link; another slot, or a pairing request (`opensc2_pair_start()`, `opensc2_select_transport_pairing()`), ends the link (the zero report, `0x13`) and advertises for it; `opensc2_stop()` stops Bluetooth in any state |
    | Bluetooth's state after a host's `0x13` on USB power | stays "connected" until a disable reaches it, which never happens: that state only reboots or shuts down | the link is `OFF` at once; nothing a host or user can see differs |
    | A deleted bond of the identity in use | the identity is reset, which, on a connected link, ends it (no route to this is known) | only logged while that identity advertises or is connected |
    | LED patterns | blue: the pairing double flash, the searching fade, connected steady or fading on external power | the system state machine selects them; the blank canvas shows them in blue on an RGB LED, as brightness alone on its one-colour LED |
    | No connection-event trigger | no state reports (inferred) | a timer at the connection interval paces them |
    | The sampling margin | starts over when a link becomes active and when the stick or trigger calibration changes | starts over when a link passes the gate |
    | Buffers and threads | 5 L2CAP and 25 connection TX buffers; the report thread waits for a buffer; both threads cooperative (priority -1) | 8 ACL, 8 L2CAP and 10 ATT TX buffers; at most 8 notifications in flight, a state or battery report without room dropped (a battery report waits), the others queued; preemptible threads (`CONFIG_OPENSC2_BLE_THREAD_PRIORITY`, `_REPORT_THREAD_PRIORITY`). Seen only as latency or a drop under congestion |
    | A device name over 29 characters (with the default prefix only a host's GAP write makes one, up to 31) | pairing-mode advertising cannot start: the name does not fit the scan response (inferred from the stack) | the name is sent shortened, so a board's longer `CONFIG_OPENSC2_BLE_NAME_PREFIX` still advertises |
    | The log | Valve's lines | the same moments in OpenSC2's own words |
    | The report map's bytes | Valve's 372 bytes | the USB report descriptor (gap 13: the same reports, 20 bytes in another item order) |

    Valve's Bluetooth company ID and the PnP ID `28DE:1303` are advertised for interoperability, as
    the USB IDs are, and the vendor service uses Valve's UUIDs for the same reason
    ([NOTICE.md](../NOTICE.md)); the IDs are Kconfig options and the vendor service can be left
    out (`CONFIG_OPENSC2_BLE_VENDOR_SERVICE`). **The vendor `0x45` quirk, copied:** the real
    controller keeps the vendor service's subscription flag of the state report in bit 0 of the
    puck link's last disconnect reason (what `0xF2` selector 2 reports), so a host's subscription
    to the vendor `0x45` sets that bit and its end clears it, and after a puck link that ended with
    an odd reason (1, link lost; 3, turned off by the puck) in the same boot, state reports go to
    the vendor characteristic whatever the host subscribed to: a host that uses only the HID
    service gets none until the board resets or a vendor `0x45` subscription ends. The library does
    the same (read from the protocol documentation, not observed on a real controller;
    `CONFIG_OPENSC2_BLE_VENDOR_STATE_QUIRK`, on by default). The real controller reaches it only
    through the dock (a puck link, then Bluetooth chosen there); OpenSC2 also through a switch from
    the puck to Bluetooth at run time (`opensc2_select_transport()`, `opensc2 transport ble`), so a
    board that switches at run time and serves Windows may want it off.
24. **`0xC5` and `0xE9`, copied** (closed). The real controller keeps `0xC5`'s bytes as sent (a
    channel above 100 is brighter, up to full duty) and converts the stored colour in a way that
    truncates before it scales, so each channel of `0xE9` reads 0 below 100, 100 from 100 to 199
    and 200 from 200. The library does the same: the colour is kept as sent, 0..255, the
    application gets it unchanged (`led` callback, `opensc2_led_get()`), and the system state
    machine's LED rules render a channel above 100 at full duty at most (`opensc2_led_duty()`). (0.2.0 clamped it to 100.)
25. **`0xBE`'s bytes 13 and 14** (closed). The documentation gives byte 13 as "1 while charging" and
    byte 14 as `0x43`'s charge state without its `0x80` flag; byte 12, the level, goes out as given
    (255 included), as `0x43`'s does. On the real controller byte 13 is 1 exactly while its
    charging rules are in state 2, and the flag is never set in that state; the library derives
    byte 13 from the application's charge state the same way (1 for state 2), which is identical
    whenever the application reports the charging rules' state (`<opensc2/charger.h>`).
26. **Settings by name.** The library knows exactly the real controller's keys
    ([api.md](api.md#settings-by-name)); other names go to the application, then, as on the real
    controller, to any other settings handler of the build (Bluetooth's keys, the application's;
    never the library's own subtree), with the real controller's refusals and result codes
    (`-22` for a name no handler has, `-134` for a key without a read function, `-19` for one with
    nothing valid to read; `0xF0` of a name nothing stores is acknowledged; an empty name in `0xED`
    is refused). It keeps `user/keylock`,
    `user/fully_charged_led_intensity`, `mte/charge_level` and the `settings/haptics/` keys as plain
    values: set in RAM, saved and read back where the real controller has a read function, deleted
    by the factory reset where it deletes them. The system state machine acts on the keylock and the
    fully-charged LED level (gap 34), and on the factory charge window: a stored window other than
    0/0 brings the real controller's announcement at every power-on (gap 34), and
    `opensc2_charge_target_mv()` gives its upper value to a board whose charger can stop there (the
    real controller programs its charger chip with it; a board without such a charger cannot).
    With the haptics synthesiser, `0xEE` on
    `settings/haptics/haptic_master_gain_db` or `.../amplifier_mode` goes to the four channels at
    once, as on the real controller, past settings 76 and 75 (which `0x89` still reads as they
    were); `.../enabled` is only kept, as there. As
    on the real controller, `0xED` answers `mte/charge_level` only while it holds a
    valid window (both values 0, or 3400 to 4120 mV in 10 mV steps), and `0xEF` saves what `0xED`
    would read back, as the real controller saves what the key's read function gives: it fails for
    the `settings/haptics/` keys (no read function) and for an invalid charge window, and writes
    nothing then. A choice where the documentation says nothing: a readable key that was never set
    answers its whole size in zeros (the real controller's RAM value). The calibration keys are the
    calibration store's (gap 31); a build without `CONFIG_OPENSC2_CAL` gives them to the
    application, so without one `0xED` answers length 0 and the others are refused.
27. **Crash records.** The record and `0xF2`'s rules follow the documentation
    ([api.md](api.md#crash-records)). OpenSC2's own: where the record waits for the next boot (the
    retained RAM region, with a magic word and a CRC-32 of its own), the build time and revision it
    names (the firmware's own, not a build time stored on the board for Steam), that a crash
    repeated at every boot writes flash once until a host reads it (the documentation names no
    such guard), and that `0xF2` saves the record it marked seen from the system work queue rather
    than inside the request.
28. **The hardware watchdog** follows the documented timings (5 s, a feed every 2.5 s from the
    lowest priority, the 4.5 s warning that raises a kernel oops, only a log with a debugger
    attached) and is on by default, as on the real controller (`CONFIG_OPENSC2_WATCHDOG`, wherever
    the devicetree has a `watchdog0` alias), except in a build for a UF2 bootloader such as the
    SuperMini's, where it stays off until it is turned on on purpose (`app/watchdog.conf`). The feeder is a work queue of its own at the lowest
    application priority, where the real controller shares its background queue with the battery
    report and the idle monitor. Not yet tried on hardware with this firmware: the watchdog keeps
    running into a bootloader, which has to feed it, and whether the UF2 bootloader does has not
    been tried ([api.md](api.md#the-hardware-watchdog)).
29. **The rules on every state report** are the documented ones
    ([pad processing](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/07-input-and-sensors.md#pad-processing),
    [raw output](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/07-input-and-sensors.md#raw-output-settings-46-and-62)).
    Inputs the library processes itself (raw sticks and triggers, raw pressure and capacitive
    words, trackpad frames) get them as the real controller works them out. For the values an
    application gives finished, the library applies them to those values:

    | Rule | The library, on finished values |
    | --- | --- |
    | Stick-touch fallback | on the application's stick values, with the documented approximation and radius; not with raw sticks (46). The real controller reads its calibrated values, which are 0 in raw mode: the same result |
    | Setting 82 | the documented blanking; raw pressure (60 = 1) passes |
    | Raw triggers (62) | both bits 0 |
    | Trigger bits (68) | the real controller's trigger stage in single precision on value / 32767: on above 68 % + 5 %, off below 68 % − 5 %, never on from 95 up; a change of 68 or a set-up of the triggers clears them. The real stage works on the fraction before it is scaled to 32767, a hair above value / 32767, so a finished value within one count of a level can differ; a raw trigger's bit is the stage's own, its direction-1 quirk at 95 included |
    | Click by pressure (52, 53) | opt-in for finished values (`CONFIG_OPENSC2_PAD_CLICK_AUTO`), so boards with a click switch keep it; the pressure / 32767 taken as the normalised press (gain 1), in single precision: at −1 (100 %) a press would have to pass 108 %, which a value capped at 32767 never does, where the real controller's uncapped value can on a very firm press. Raw pressure and trackpad frames click from their calibrated pressure as on the real controller |
    | Raw sticks (46) | raw sticks give counts and millivolts the documented way; the blank canvas also gives them for an input it centres itself |

    The pressure of an untouched pad given finished is sent as the application reports it, where
    the real controller sends 0; raw pressure and trackpad frames send 0.
30. **Reports waiting through the puck** (closed). As on the documented controller, up to 5 input
    reports other than the state report wait for the next state report (lizard mode's of a frame
    ride with the next one), and a report that finds the queue full is dropped, lizard mode's and
    the application's included. `CONFIG_OPENSC2_REPORT_WAIT_DEPTH` (0 by default) is OpenSC2's
    option to let lizard mode's and the application's reports wait for room instead, while lizard
    mode waits with its next frame.
31. **Calibration** (`CONFIG_OPENSC2_CAL`, on by default). The keys, their sizes and formats, the
    commands `0xD8`, `0xC0`, `0xC3`, `0xE2` and `0xFE`, the recordings' steps and the arithmetic of
    sticks, triggers and pad pressure follow the protocol documentation
    ([input and sensors](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/07-input-and-sensors.md#calibration-keys)),
    in the real controller's single-precision floating point, its order of operations and its
    fused multiply-adds, so the values are the real ones to the last count, quirks included (a
    turned-over stick half reads 0, a range end of 0 counts as "not set", a pressure range of 0
    reads 0, the recordings' quirks: a step 0 and 3 before the next frame still save, a lone `0xC3`
    step 3 saves format 0, a stick or trigger recording loads every stored key again). Where it
    leaves something open, or a part is Valve's hardware, OpenSC2 chose:
    - a recording saves the sides the board gives samples for (or that got samples since its
      step 1): the real controller always has both; a side the board does not have keeps its
      stored key;
    - raw inputs given with the setters are processed once per state report: the pressure filter
      and the two-word means of the capacitive words step per report, where the real controller
      steps them per trackpad frame; a board that gives [trackpad frames](api.md#trackpads-frame-by-frame)
      gets them per frame, as on the real controller;
    - `0xED` answers `cal/volt_offset` always, the LED trims while they lie within 1..199, a
      stick, trigger, pressure or touch key (`cal/touch_l`, `cal/touch_r`: 10 bytes, used by
      nothing) while its format word is not 0, and never the gyro bias, as the documentation
      says; `0xEF` saves only what `0xED` would read back (the real controller saves what the
      key's read function gives), so it refuses an analog or touch key whose format word is 0,
      an LED trim outside 1..199 and the gyro bias; the LED itself uses any trim, 0 leaving the
      channel dark, as the real controller does;
    - the flash writes that a finished recording, `0xE2` and `0xFE` cause run from the system work
      queue, in the order they were asked for (`0xFE` drops the saves still waiting from before
      it; a recording still waiting goes into RAM at once, as the real controller had loaded it
      before the erase); `0xEF` and `0xF0` of a `cal/` key first wait for those, then write before
      they answer, as for the bonds. The real controller's `0xEF` does not wait: there an erase
      still running on its work queue can delete a key that a later `0xEF` saved, so a host's
      "erase, then store the new calibration" can lose the new keys at the next boot. OpenSC2
      keeps them (a deliberate difference, in timing only when nothing is waiting); a recording's
      keys reach RAM a moment after its frame, from the work queue, not inside the frame;
    - a settings partition with every sector occupied at boot: the real controller erases all of
      it, calibration included, and starts over; so does OpenSC2 with
      `CONFIG_OPENSC2_STORAGE_RECOVERY`, on by default with NVS settings
      ([api.md](api.md#the-settings-partition)); without it Zephyr's NVS does not start, and the
      board runs on its built-in values with nothing loaded;
    - `0xE2` and `0xFE` read their bytes whatever the length byte says, as the real controller
      does, so an empty request reads the report's zero padding;
    - a trigger direction byte other than 0 counts as 1 (the voltage falls when pulled);
    - the stick offsets while docked follow the docked states as on the real controller, with the
      board's own values (`opensc2_dock_set_stick_offsets()` or `opensc2_cal_set_dock_offsets()`,
      [api.md](api.md#the-docked-stick-offsets); none by default, Valve's being its hardware's), on
      a board revision of `0x48` or later; the sticks and triggers are set up again when the
      controller docks or leaves the dock (on a board of any revision), as the real controller
      does;
    - not reproduced, being Valve's hardware: the scaling of the capacitive words without
      external power and the trackpad chip's electrode processing (an application gives positions,
      a signal level or the words);
    - left as they are, being timing inside one frame or a failure path no host sees: the real
      controller's set-up of its sticks and triggers (a key load, setting 68, the dock) runs in its
      sampling loop and can cost that loop one state report when it coincides with a slot, where
      the library sets them up between two reports; its buttons, once one is down, are looked at
      every 2 ms, so a release is seen up to 2 ms later than with the button helper
      (`opensc2_button_hold_*()`), which takes each change as it comes; it stops (`k_panic`) when a
      button port's interrupts cannot be switched, which the library leaves to the board's
      driver; a pad set-up asked for by a setting that changes and changes back between two
      frames still happens there, while the library compares the settings at each frame;
    - not done: the real controller also sets its sticks and triggers up again whenever its last
      transport goes away, so after a disconnect its triggers start with their digital bits
      clear; the library keeps them, which shows only for a trigger held inside its 5-point
      hysteresis band across a disconnect.
    The hardware test suite never writes, saves or deletes a `cal/` key and never sends the
    calibration commands (its safety rule S5): calibration is tested by the host tests only.
32. **The pad rules.** A pad fed by trackpad frames (`<opensc2/trackpad.h>`) is processed as the real
    controller processes its pads, frame by frame: the position filter of setting 65 (on by default;
    the speed from the previous input, in the real controller's single-precision rounding), the
    rotation of settings 2 and 24 with the real controller's sine table (worked out with π as
    3.141592: 30° gives 4095, 90° 8191), the stabiliser of setting 80 = 2, the stick touch, grip,
    pressure, click, setting 82 and the start-overs. A board without a pressure, stick-touch or
    grip word for it has its own click, stick-touch and grip bits stand in for them. For pads given
    finished (`opensc2_set_trackpad()`), the pad rules (`CONFIG_OPENSC2_PAD_RULES`, on by default,
    inactive with the default settings) run the rotation and the stabiliser on the application's
    finished positions, after the hardware test hooks' injection: the library halves them back to
    the pad scale, turns Y back, applies the documented arithmetic and doubles them again, so an
    odd position loses its lowest bit while a rule is on; the position filter is then the
    application's (as a library rule it would change every board's pads).
33. **The IMU** (`<opensc2/imu.h>`). The stream from boot and its switching by setting 48 and the key
    `mode`, the scaling (kept to 16 bits, so past full scale it wraps as on the real controller),
    the bias of setting 84 and the key `use_bias`, the mounting matrix (the board's from
    `opensc2_imu_set_mounting()`, the key `mounting_matrix` until the next boot), the dead zone of
    the key `gyro_dz_threshold` and the free fall's arming (counting again 5 s after a fall, never
    again until the next boot when the stream goes off within them) are the real controller's.
    Setting 85 and the key `gyro_threshold` go to the application's sensor (its handler): the real
    controller hands it to its IMU's fusion block, where its role is not known; the blank canvas's
    LSM6DSV16X stream option takes it and does not apply it (the stock driver has no such
    attribute). The stream going off also drops a drop sound still waiting its 250 ms, as on the
    real controller. OpenSC2's rest-based gyro-bias estimate
    (`<opensc2/sensor_helpers.h>`, for IMUs without one of their own) takes setting 85 as its rest
    threshold in report units. The blank canvas's LSM6DSV16X stream option (`app/imu-stream.conf`)
    runs the chip at the real controller's 250 Hz high-accuracy rate and takes the stored gyro bias
    as the real controller does: a NaN goes in as 0, and a value of 2^31 or more in magnitude (an
    infinity included) is refused, so the stream does not start at boot and waits for a host to
    write setting 48; so does a bias the IMU's driver cannot program (with none stored it programs
    zeros, as the real controller programs its empty copy). When the stream fails, the real
    controller submits it again 500 ms later even if a host turned it off within those 500 ms; the
    blank canvas submits it again only while setting 48 wants it. Not done there: the fusion
    block's alpha-blend that the real controller writes on its early boards, and the chip's own
    free-fall detector in the stream (the stock driver has none; the library's software detector
    stands in).
34. **The system state machine** (power, chords, the status LED and cues; built, not yet tested on
    hardware). OpenSC2 follows the documented machine ([in brief](#overview-power-chords-and-the-status-led);
    [api.md](api.md#power-and-the-system-state-machine)): the power-on decision with the level the
    real controller's first battery report passes on (kept over a warm reset, else 255), the
    factory charge window's announcement before it, the wired start that attaches once STEAM is up
    (gap 18), USB power in the battery wait turning the boot wired in place, the warm-restart mark
    set the moment Steam's request arrives and used up by a transport chord held at boot, the
    keylock chord's tap deleting the lock before a battery power-on decides, the chords as two
    combinations each (with STEAM and without) in the real controller's order, dropping only a link
    that is up, the 60 s update window and its green blink, host requests queued like any event,
    the 60 s and 10 s give-ups of the radio. These differences and choices stay:
    - **No battery, no power-off.** Without `CONFIG_OPENSC2_BATTERY` (the default) a board that
      runs without USB power is taken to be on an external supply, which counts as USB power, and
      every shutdown that would turn it off reboots instead (what setting 77 does on the real
      controller), so a board that could not be woken again is never switched off. The radio's
      give-ups (60 s of listening, 10 s without a bond) are the real controller's in every build:
      on USB power, or an external supply, they end in "wireless off" (amber), where STEAM reboots
      the board wired.
      The battery wait (and so the wired start in place) is reached only in a battery build.
    - **The wired or wireless decision stays before the machine** (the boot rule in
      [api.md](api.md#modes)), with OpenSC2's own overrides (the stored policy, the application's
      choice, a wired mark). USB power in the battery wait turns the boot wired in place only under
      the plug-in rule's options (policy auto, `CONFIG_OPENSC2_USB_PLUG_IN_REBOOT`, a mode the
      application did not choose); otherwise the board reboots and comes back wired. The warm mark's
      chord rule needs the buttons before `opensc2_init()`; an application that gives them only
      later has its warm restarts go on whatever is held.
    - **Wireless on again**: a link started from outside the machine (`opensc2 start`, the
      application's `opensc2_start()`, pairing) while wireless is off on USB power counts as
      wireless on. A radio stopped on the board (`opensc2_stop()`) is not "wireless off": only a
      link that gave up, a protocol version the library does not speak, and the puck's turn-off are.
      A transport selected at run time (`opensc2_select_transport()`, the shell's `opensc2
      transport`) becomes the machine's choice, as a chord's would: the LED shows its colour, the
      battery wait's timer starts it, and `opensc2 power` reports it.
    - **The power source**: the dock is a power source an application names, or the one the dock's
      supply rule gives whenever the board has a pilot to read (gap 40).
    - **The bootloader**: requests that take the real controller into its own bootloader
      reach the application (`OPENSC2_POWER_BOOTLOADER`), whose bootloader cannot take Steam's
      update; after the update window the library reboots if the application did not.
    - **The Bluetooth chords** exist only in a build with Bluetooth; without one, B with a bumper
      is no chord, and a stored Bluetooth transport starts the main puck.
    - **Cues 6 and 7** (pairing, paired) are raised by the over-the-air pairing (gap 1); the real
      controller's pairing routine has them, but nothing there reaches it. Cue 0 is also raised at
      a remote wakeup (`CONFIG_OPENSC2_USB_REMOTE_WAKEUP`), as on the real controller's wake-up path
      (unreachable in its October firmware). OpenSC2 has none of the real scripts: the cue event
      names the real script it stands for, and the sound is OpenSC2's own, played by the haptics
      synthesiser when it is built in (gap 35), else by the application.
    - **The keylock chord** saves the lock for the next power-on and leaves the value in RAM as it
      is, so `0xED user/keylock` reads the old value until the next boot, as documented (a quirk
      Steam could see, copied).
    - **The LED**: the documentation gives the brightness curve for setting 38 = 1 (the default) and
      a linear brightness for 0; OpenSC2 uses the curve for every value that is not 0. The blank
      canvas scales its channels by the per-unit `cal/rgbw_*` keys, as the real controller does
      (`led-trims` in its overlay can fix them for a board instead). The over-the-air pairing (gap
      1), which the real controller never shows, flashes like Bluetooth's pairing mode. The
      application drives the LED from the library's status every 10 ms through the pattern player,
      where the real controller's LED thread drives its PWM itself. None of the patterns has been
      compared with a real controller's LED.
    - **Power-off** is the application's: the blank canvas arms STEAM (or every key) as a wake-up
      pin before `sys_poweroff()`, where the real controller sets no wake-up source and has its
      charger cut the battery off (ship mode) when no charger is connected; a board with a charger
      that has a ship mode can do the same on `OPENSC2_POWER_OFF`
      (`opensc2_charger_ship_mode_ok()`).
    - **The charger** is the board's. The real controller's charging rules (the charge state, the
      charge voltage, the input-voltage search and its give-up, the factory window's cap) are
      helpers the application runs with its own charger (`<opensc2/charger.h>`); the library
      cannot reach a charger chip itself. Its register values, and the gauge's curves, are Valve's
      data: OpenSC2 has curves of its own, and a board can give its own
      (`opensc2_gauge_set_curves()`).

35. **The haptics synthesiser** (`CONFIG_OPENSC2_HAPTICS_SYNTH`, on by default; built, not yet
    tested on hardware). Re-implemented from the protocol documentation's description of the real
    controller's sequencers, generators, scripts, cues, streams and rumble emulation, rule for
    rule, the accidents of its arithmetic included, in integers, so blocks are bit for bit the same
    on the board and on a PC:

    | Point | OpenSC2 |
    | --- | --- |
    | As the real controller plays it | the fifth-order polynomial sine (peak about 1.0045) with its one-cycle phase wrap (a tone above the channel's rate turns into clipped noise); values that are not a number (a sweep from 0 Hz, with equal ends, of duration 0 or with its 16-bit count wrapped to 0 while rising) playing +1, full scale, over the whole channel mix, an end of 0 Hz spoiling the first sample only, a wrapped falling one growing to full scale through the fade-out's arithmetic; the sweep's 16-bit count (longer sweeps run faster and higher), its fade-out to 0 and over the whole of a sweep under 5 ms; pulse periods in 16 bits (under one sample, a constant -1 for 65,536 samples a period); a generator that ends by itself lingering 1 (pads) or 4 (grips) silent samples, which can add a silent block; dB sums in 8 bits (click A from `0x82` +122 dB, script offsets, the rumble's left gain); the real scalings and roundings; the channels rendered one after the other, the negated one first (which orders two `0x44` of one block) |
    | Streams | the interpolated values computed ahead survive a stop and play first at the next start; a stream that runs dry with half a 16-bit sample left loses that byte, one that cannot interpolate plays its current sample once; no water mark before a format (no `0x08`; `0x01` for every write once the ring is full); a format byte of 12 and up sets the water mark and start point and keeps the last sample size and rate (s8 after mu-law; on a channel whose stream never ran, no bytes at all: a silent value without end); each write that finds enough audio queues a start until one is applied; the `0x02` replies to stops sent when the sequencer takes them (gap 19) |
    | Timing | a command is applied before the thread waits for the output to take a block, and the block rendered after the wait (an output's optional `ready()`; the built-in outputs have it): a command is heard about 4 ms after it on an idle path and 16 to 24 ms after it while something plays, and a stream's bytes are read and its flags sent when the real one does; commands arriving while a path drains wait for its next start; a path without an output is paced by a timer at the real block times, 8 ms on the grips and 8.192 ms on the pads |
    | Settings 75 and 76 | handed to the four channels as configure commands at the end of start-up and on every change, through the queues (a command a full queue loses keeps the old value; the hand-out stops at the first channel it does not reach); the channel gains are 0, silence, until then; amplifier mode 1 raises the enable at once and holds it, 0 drops it at once, even in the middle of a sound; the keys by name act on the channels directly (gap 26). Setting 70 does not silence the synthesiser (on the real controller only lizard mode's pad feedback reads it) |
    | The pads' PWM DAC | 256 steps of 16 us, each sample held 16 periods: about 3906 Hz, as the real one (`CONFIG_OPENSC2_HAPTICS_OUT_PWM_DAC_EXACT_4K` for exactly 4 kHz). One difference of the pins: the real controller hands its samples to the PWM as they are, without the polarity bit, so its pins are low for a sample's counts and high for the rest (256 - v); OpenSC2's are high for v, the same waveform inverted. Which is right depends only on what the pins drive, the board's wiring |
    | `0x87` and `0x89` through the puck | the record's length, one more than the report's, as the real controller's handlers get it: `0x87` takes one byte from beyond the report and `0x89` accepts an n one larger, the extra byte the next record's length byte or whatever the receive buffer holds there |
    | The drop sounds | reported by the application (the blank canvas: a software detector, below 0.35 g for 150 ms, where the real controller uses its IMU's own); the IMU stopping within 5 s of a fall cancels the sound and leaves the drop sounds off until a reboot (`opensc2_haptics_imu_stopped()`), as on the real controller |
    | Valve's data | not carried: the two clicks are damped sine bursts of OpenSC2's own at the real clicks' lengths; scripts 1 to 17 (the cues' and the drop sounds' too) are short sounds of OpenSC2's own; the 18th script, which nothing plays, is left out. A board can give its own clicks and scripts, recordings included ([api.md](api.md#sounds-the-application-supplies)) |
    | Still different | an output report shorter than its fields is refused (the real handlers read past its end into whatever the link's receive buffer holds, which cannot be reproduced); the pad frame's word order inside OpenSC2's output API is left first (the real device's first word is the right pad: only which pin carries which pad, the board's choice); integer arithmetic where the real one has floats: values within a count of the real ones at first; the real phases are floats that gather rounding as they run, so after seconds a tone is some tens of counts away (78 after 5 s of 1 kHz on a grip), a sweep within a few tens of counts (its phase summed sample by sample, each step taken half a sample on so the sum stays with the real closed form; over sweeps of several seconds it drifts by up to a few hundredths of a cycle), a tone far above the rate saturating at 65536 where the real values grow without end, the constant of a wrapped falling sweep exact where the real one is rounded away (for very long or very flat ones the real float phase has no correct digits left); the free-fall detector above |
    | `0x88` | with more than 31 bytes a side the report does nothing, as on the real controller (in every build: the decoder; v0.2.0 clamped it to 31) |

36. **The dock's answers** are built as the real controller builds them, over the previous answer
    (closed). What stays different: after a boot the bytes no request has written yet are zeros
    (the real controller's are leftover stack contents), and the bytes a GET's command handler
    leaves after its answer are OpenSC2's handler's (some of them clear the rest of their answer,
    where the real one may not).
37. **GET input with the dock alone** answers the report the request before it built, as on the
    real controller (closed). OpenSC2 builds the new report while the answer goes out, in the
    thread that serves the dock (the application's `sample` runs there); the real controller wakes
    its input thread for it. The zeroed state report sent while a link is dropped never reaches the
    dock's copy, on either firmware. Not copied: when the real controller drops its links (a
    shutdown, or USB power with the radio turned off) while a dock session is still open, its copy
    stops filling and attribute 11 reads 0 until that session ends; a controller in those states is
    off the dock or on USB, so a puck never sees it.
38. **Result codes on the dock** are the real controller's numbers whatever this platform's errno
    values are: `-22` for an opcode nobody knows or a length above 61, `-95` for a GET whose
    command has nothing to read. Through the puck the library's codes are the real controller's
    numbers too (the nRF Connect SDK's errno values are), after 0.2.0 also for an opcode nobody
    knows (0.2.0 gave `-134`) and for the settings by name (gap 26).
39. **One command at a time.** The real controller runs each link's commands in that link's own
    thread, with no lock between links. OpenSC2 runs one command at a time across links, so a command
    from the dock waits for one from USB or the radio and the other way round; a flash write on one
    link can delay another link's command by its length. A host sees no other difference. This
    stays: the commands share the settings, bonds and identity, and the library's own state has
    no per-field locks the real firmware's can do without.
40. **Dock power and the docked states.** The supply rule is the real controller's (closed): "a
    supply is present" is VBUS, or the dock's `+` on a sense input of the board's own for a board
    whose `+` must not reach VBUS; a fresh pilot check whenever that changes makes a supply that
    appears with the pilot the dock's power and any other USB power, so the dock and USB at once
    count as the dock and `+` without a valid pilot as USB power. The docked stick offsets follow
    the docked states as on the real controller, with the board's own values (none by default:
    they are a property of the hardware). What stays different: the dock's `+` alone (no VBUS)
    without a valid pilot at a power-on boots wireless on USB power, where the real controller
    enables USB and waits for a host (OpenSC2's wired-or-wireless decision needs VBUS); the dock
    thread serves only once `opensc2_init()` has run (the real one from boot; the puck probes a
    second after switching `+` on); a board without a pilot to read (the default image, which has
    the dock link only through the test hooks) never counts as docked. Without the machine
    (`CONFIG_OPENSC2_SYSTEM=n`) a cold boot on the dock starts the radio when the application asks,
    where the real controller charges with the radio off.
41. **The dock's wire is untried.** The pilot's window is at the controller's pin with no divider;
    how the pilot and the serial link share the `P` pad, the pad's idle level and its load are not
    known. The SuperMini dock build (`app/dock.conf`) is a guess at the controller's side (an
    open-drain TX, RX without a pull, the pilot on an analog pin), built and never run; nothing may
    be connected to Valve's puck's pads without the owner's go-ahead.

## What this library adds for development boards

These have no counterpart on the real controller, and are there because a development board has no
case, no battery gauge and no other way to tell what happened:

- **Escapes into wireless mode**: the pairing button held at power-up (this boot only), the pairing
  button pressed before a host configured the board, or held for 3 s while wired (acted on when it
  is released), an optional mode switch, and a stored policy (`opensc2 mode wireless`: never
  wired). `opensc2 reboot wired` comes back wired once. A wired boot whose USB device cannot be set
  up goes on wireless.
- **The USB session log**: a ring in RAM that a reboot keeps, with every control request, its result
  and its timing, so a wired session can be read afterwards in wireless mode. Bond values and serials
  are left out.
- **A debug serial port in wireless mode** (non-Valve IDs `2FE3:0004`), where the real controller
  shows nothing on USB; optionally also while wired, as a second interface (a composite device,
  unlike the real controller; for debugging only).
- **The status LED on a one-colour LED**: the real controller's patterns as their rhythm (on while
  a pattern is at least half as bright as its peak), and amber, which only a colour can tell from
  green, adds a 100 ms dark blink every second, so "no host" and "wired" differ. An RGB(W) LED shows
  the real colours ([api.md](api.md#status-led)). Without the system state machine the blank canvas
  shows the puck's patterns while wireless, and green or amber while wired.
- **A product name of the board's own** (`opensc2 id set name`), optional: the real controller's
  "Steam Controller" by default.
- **A charger test build** (`CONFIG_OPENSC2_NO_HOST_WIRELESS_MS`, off by default; the blank canvas's
  `test-charger.conf`): a testing aid for a board with no buttons, which goes wireless when no host
  sends a USB request within 5 s of a wired boot. The real controller waits for a host or for STEAM;
  this differs from it on purpose and is never part of a default build.
- **The hardware test hooks** (`CONFIG_OPENSC2_TEST_HOOKS`): input injection and output capture for
  the PC test suite, inert until it arms them ([api.md](api.md#hardware-test-hooks)).

Back to the [documentation map](README.md).
