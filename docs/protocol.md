# The protocol, in brief

This library implements the controller's end of the Steam Controller 2 puck's 2.4 GHz link. The
full description, from which it was written, is the published protocol documentation of the
[steam-controller-2-reverse-engineered](https://github.com/ded811/steam-controller-2-reverse-engineered)
project:

| Topic | Page |
| --- | --- |
| The controller's end of the radio link: set-up, listening, connecting, keep-alives, records, link loss, channels, bonds, pairing, identity, protocol version, turn-off | [docs/controller/04-radio.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/04-radio.md) |
| The puck's end: slots, pairing window, connecting, channel choice, suspend | [docs/puck/04-radio.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/puck/04-radio.md) |
| Report `0x42`, the button bits, the vendor commands, the controller settings, lizard mode | [docs/controller/03-usb-and-hid.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md) |
| Output reports `0x80`..`0x8A` | [docs/controller/08-haptics.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/08-haptics.md) |
| Identity record, dock | [docs/controller/06-dock.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/06-dock.md) |
| What the puck shows Steam (feature report pass-through, attribute cache) | [docs/puck/03-usb-and-hid.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/puck/03-usb-and-hid.md) |
| The puck side as an open firmware | [Counterpuck](https://github.com/ded811/Counterpuck), [wire-formats.md](https://github.com/ded811/Counterpuck/blob/main/docs/wire-formats.md) |

## Overview

- **Radio**: Nordic Enhanced ShockBurst, dynamic payloads up to 128 bytes, 2 Mbit/s on the BLE 2M
  PHY, 16-bit CRC, +6 dBm, selective auto-acknowledgement, fast ramp-up. The puck transmits; the
  controller is the receiver and says everything in **acknowledgement payloads**, so each reply
  rides with the acknowledgement of the puck's next packet.
- **Pairing** (channel 2): the puck's pairing window (opened by Steam) beacons a hello to address
  `91 A2 A7 93`. The controller answers `0xF0 {T, serial}`; the puck sends `0xE0 {puck ID, puck
  serial}` to address T; the controller confirms with `0xF4 {C, serial}`; both store
  `{puck ID, C}`.
- **Connecting** (channel 2, address `ibex`, prefix `0x10`): the puck broadcasts `0xE1 {puck ID,
  controller ID, channel, its address, a pipe prefix}` without acknowledgement. The controller with
  that bond moves to the private pipe and acknowledges the hello that follows 1 ms later.
- **Connected**: the puck opens with `0xE7 {0, 1}` (protocol version 1); the controller answers
  `0xF3 {1}` and once per connection its identity `0xF5` (57 bytes). Then the puck sends `0xE3`
  keep-alives about every 3 ms, carrying records `{length, type, data}`: SET feature (1), GET feature
  (3), output report (5). The controller answers `0xF1`: one SET result (2) or GET answer (4), the
  state report `0x42` as an input record (6), and any queued input reports, at most 127 bytes.
- **Channels**: `0xE4` gives a new channel list (for version 1: the new channel plus 2 and 80). The
  controller waits 10 ms per turn on its current channel, then 13 ms on each alternate; 500 ms
  without a packet is a lost link, and it listens on channel 2 again.
- **The PC sleeps**: the puck sends `0xE5` (turn off); the controller answers `0xF2` to every packet
  until the puck stops. `0xE6` says the PC is asleep while the link stays up.

## Where this library had to fill gaps

The documentation describes one real controller firmware. Where it leaves something open, the
library does what should work with the puck, and says so here:

1. **Over-the-air pairing.** The documented controller firmware (July and October 2026) contains a
   pairing routine but never runs it: real controllers get their bonds from Steam through vendor
   commands over a cable. The puck's pairing window is still there, so the library implements the
   documented exchange from the controller's side, with the puck's documented timings (and keeps its
   confirmation queued if the puck repeats its offer). Whether a given Steam version opens the
   puck's window for a third-party controller is untested.
2. **Rumble report `0x80`.** The research page lists its fields one byte earlier than SDL's
   `MsgHapticRumble` (type, intensity, left speed and gain, right speed and gain), which fills the
   report's 9 bytes. The library decodes SDL's layout, since that is what a host sends.
3. **Report `0x43` charge state.** The firmware sends its charger's status byte (plus `0x80` when
   full); SDL reads the byte as `EChargeState`. The library sends SDL's values.
4. **Answer layouts not given byte for byte**: `0xF2` selector 2 (uptime u32, then the reason byte),
   `0xA1` (reset cause u32, device ID, zeros; the reset-cause flags are Zephyr's, not the original's),
   and `0xAE` (the tag, then the string, 20 bytes in all, as Counterpuck answers for the puck).
5. **Which commands Steam sends on connect** is not documented. The library answers the identity,
   settings, LED, user-store and settings-by-name commands and passes the rest to the application.
6. **Controller settings** have documented defaults but no ranges; values are stored as sent.
7. **The quaternion's W** (offset 45): one page calls it always 0, another the quaternion's W (since
   October). The library sends whatever the application provides; the fusion helper fills W.
8. **Payload limit**: the controller's payloads are "within 127 bytes", the puck accepts 128; the
   library stays at 127.
9. **The October firmware's change to the ESB driver** (the processor, not the radio, starts each
   reception and acknowledgement) is not reproduced: stock nRF Connect SDK ESB is used.
10. **Board revision.** What Steam does with the hardware ID is not documented; the default is
    `0x4B`, one of the values real controllers accept.
11. **Lizard mode** is simplified (no pointer momentum, no haptic ticks).
12. **A bond written with `0xA2`** takes effect at once here; the documented controller only reads
    it at the next boot.
