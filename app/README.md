# The blank canvas

A ready-to-flash Steam Controller 2 for the SuperMini / Pro Micro nRF52840 (nice!nano pinout,
Adafruit UF2 bootloader; Zephyr board `promicro_nrf52840/nrf52840/uf2`). Every input is set up in
one file, [boards/promicro_nrf52840_nrf52840_uf2.overlay](boards/promicro_nrf52840_nrf52840_uf2.overlay):
change a pin there, or delete an input you do not wire. Untested on hardware.

## Pins

| Pin | Input | Pin | Input |
| --- | --- | --- | --- |
| P0.06 | D-pad up | P0.31 | pairing button |
| P0.08 | D-pad down | P0.29 (AIN5) | left stick Y |
| P0.17 | D-pad left | P0.02 (AIN0) | left stick X |
| P0.20 | D-pad right | P1.15 | A |
| P0.22 | L, left bumper | P1.13 | B |
| P0.24 | LT, left trigger (digital) | P1.11 | X |
| P1.00 | L3, left stick click | P0.10 | Y |
| P0.11 | MENU (left-hand view button) | P0.09 | R, right bumper |
| P1.04 | STEAM | P1.01 (bottom pad) | RT, right trigger (digital) |
| P1.06 | QAM, quick access ("...") | P1.02 (bottom pad) | R3, right stick click |
| P0.15 (on-board LED) | link status | P1.07 (bottom pad) | VIEW (right-hand menu button) |

- **Buttons**: between the pin and GND; the internal pull-up keeps them released.
- **Sticks**: each potentiometer between VCC and GND, wiper to the pin. The ADC measures against
  VDD, so the reading does not depend on the exact supply voltage.
- **VCC**: nice!nano-style boards switch their VCC pin with P0.13; the firmware drives it to "on"
  (low) at boot. Check your board: if its VCC is always on or switched differently, delete the
  `vcc-enable-gpios` line.
- **Not assigned** (no pins left on the SuperMini): the right stick, analog triggers, L4, L5, R4,
  R5, the touch bits, trackpads and the IMU. They are listed in the overlay, ready to enable on a
  board with more pins, or in place of buttons you do not need. Only P0.02, P0.29 and P0.31 can be
  analog on the SuperMini's edge, so it can carry one full stick.

Unwired inputs read neutral. Analog pins that are listed but left open are held at mid-scale
(sticks) or at their released end (triggers) by the nRF52840's own bias resistors, so they do not
drift.

### Calibration

None to do. At power-on each stick's resting position is its centre (keep your thumbs off the
sticks while it starts). The usable range starts at 40 % of full scale either side of the centre
and grows the first time a stick goes further, so after one full circle each stick reaches its full
output at its own extremes. A 6 % centre dead zone (`stick-deadzone-percent` in the overlay) hides
potentiometer noise. Flip an axis with `invert-left-stick-y;` and friends.

## Build and flash

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ..\build.ps1 -Project app -OutDir C:\b\osc2
```

(or `.\build.ps1` in this folder). The result is `C:\b\osc2\app\zephyr\zephyr.uf2`. Short RST to GND
twice quickly; copy the `.uf2` onto the drive that appears. The Partition Manager is off
(`sysbuild.conf`) so the application lands at the UF2 bootloader's address and the settings stay in
the board's storage partition, clear of the bootloader.

## Pair and play

1. In Steam, add a controller as usual: Steam opens the puck's pairing window for 60 s.
2. Press the pairing button (or type `opensc2 pair` in the shell). The LED gives quick double
   flashes while pairing.
3. Paired: the LED blinks slowly while it waits for the puck, then stays on once connected.

| LED | Means |
| --- | --- |
| off | radio off (turned off by the puck when the PC sleeps; press any button to come back) |
| a short blink every 2 s | no pairing yet: pair first |
| two quick flashes, repeating | pairing |
| a slow blink | waiting for the puck |
| on | connected (or Steam's LED colour, when Steam turns its override on) |

The board keeps two pairings (`opensc2 slot 1` or `2`), like the real controller.

## The USB serial port

The board shows up as a USB serial port (any terminal, any baud rate) with the log and a shell:

| Command | Does |
| --- | --- |
| `opensc2 status` | link, channel, counters, identity |
| `opensc2 pair`, `opensc2 cancel` | pairing |
| `opensc2 bonds`, `opensc2 slot 1`, `opensc2 forget 1` | pairings |
| `opensc2 id`, `opensc2 id set unit ABC0123456789`, `opensc2 id clear` | the identity Steam sees |
| `opensc2 state` | what the inputs read right now |
| `uf2` | reboot into the UF2 bootloader to flash a new build |

## Identity

By default the board reports development serials made from its chip ID ("OSC..." and "OSB..."),
which no real controller has, product ID `0x1302`, board revision `0x4B` and its build time. Change
them in `prj.conf` (`CONFIG_OPENSC2_*`) or on the board with `opensc2 id set` (kept across reboots
and reflashing). See [docs/api.md](../docs/api.md#identity).

## Optional parts

Each needs pins, which the default map has used up: take them from buttons you do not need.

| Part | In the overlay | Example |
| --- | --- | --- |
| Vibration motor or LRA driver | a PWM channel named `rumble` in zephyr,user's `pwms` | [samples/rumble](../samples/rumble) |
| RGB LED following Steam's colour | PWM channels named `red`, `green`, `blue` | [samples/rgb_led](../samples/rgb_led) |
| Trackpads (any touch device of Zephyr's input subsystem) | `chosen { opensc2,right-trackpad = &touch; }`, `right-trackpad-size` | [samples/touchpad](../samples/touchpad) |
| IMU (any accelerometer + gyroscope sensor driver) | `aliases { imu0 = &imu; }` | [samples/imu](../samples/imu) |

The drivers they need (PWM, I2C, sensors, the fusion helper, the FPU) are switched on by the overlay
itself (see [Kconfig](Kconfig)). The IMU streams only while Steam has it on (controller setting 48),
as on the real controller.

## Steam's firmware update

Steam may ask a controller to reboot into its bootloader for an update. The UF2 bootloader does not
speak Steam's update protocol, so the request is ignored (logged). Set
`CONFIG_APP_OBEY_BOOTLOADER_REQUEST=y` to reboot into the UF2 drive instead.
