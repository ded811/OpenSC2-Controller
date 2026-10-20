# Examples

Seven small applications, each showing one part of the OpenSC2 library on the SuperMini nRF52840.
For learners who want to see one feature at a time, and for anyone who wants code to copy into
their own controller.

> [!NOTE]
> The samples build for the SuperMini; none has run on hardware. What has been proven on
> hardware, with `app/`: [What works](../README.md#what-works).

## Where to start

| You want to | Start from |
| --- | --- |
| try OpenSC2 with nothing wired, or wire buttons and a stick to a finished firmware | [app/](../app/README.md), the tested firmware (the blank canvas) |
| write your own controller firmware | [template/](../template/README.md), a small project to copy; [docs/integration.md](../docs/integration.md) shows the ways to bring in the library |
| learn one part at a time | a sample below |

## The samples

| Sample | Shows | Parts | Pins | Status |
| --- | --- | --- | --- | --- |
| [minimal](minimal/README.md) | the least a project needs: one button, wired and wireless | two push buttons | P1.15 A, P0.31 pairing | builds; not run on hardware |
| [buttons_sticks](buttons_sticks/README.md) | buttons from `gpio-keys`, a stick and a trigger from the ADC | six push buttons, a thumbstick, a potentiometer or Hall trigger | P1.15 A, P1.13 B, P1.11 X, P0.10 Y, P1.04 STEAM, P0.09 pairing; P0.02 stick X, P0.29 stick Y, P0.31 trigger | builds; not run on hardware |
| [touchpad](touchpad/README.md) | a touch device of Zephyr's input subsystem as the right trackpad | a Cirque Pinnacle module on I2C, two push buttons | P0.17 SDA, P0.20 SCL, P0.22 data ready, P1.15 pad click, P0.31 pairing | builds; not run on hardware |
| [imu](imu/README.md) | an IMU through Zephyr's sensor API, with the fusion helper | an MPU-6050 module (GY-521) on I2C, a push button | P0.17 SDA, P0.20 SCL, P0.31 pairing | builds; not run on hardware |
| [rumble](rumble/README.md) | rumble and haptics on one motor | a vibration motor with a MOSFET and a flyback diode, a push button | P0.22 PWM, P0.31 pairing | builds; not run on hardware |
| [rgb_led](rgb_led/README.md) | a status LED that follows Steam's colour | a common-anode RGB LED, three resistors, a push button | P0.22 red, P0.24 green, P1.00 blue, P0.31 pairing | builds; not run on hardware |
| [haptics](haptics/README.md) | the library's haptics synthesiser on real outputs: the grips on I2S, the pads on a PWM DAC, or one actuator on a DRV2605 (`drv2605.conf`) | I2S amplifiers and exciters, H-bridges and RC filters, or a DRV2605L breakout; a push button | P0.17, P0.20, P0.22, P0.24 I2S; P0.06, P0.08, P1.04 the pads; P1.00, P0.11, P1.06 the DRV2605; P0.31 pairing | builds; not run on hardware |

Each sample's README has its parts, wiring, what to expect and what to change. The pins are in each
sample's `boards/promicro_nrf52840_nrf52840_uf2.overlay`. The SuperMini's silkscreen names pins by
three digits: "006" is P0.06, "111" is P1.11.

## What every sample has

Every sample is built with wired mode (for Steam to pair it over the USB-C cable, as with `app/`),
the same pairing-button rules and the shell in wireless mode. All samples build for the SuperMini
without warnings; none has run on hardware, so all of this is untested with the samples.

- **Wired mode.** Plugged into a PC, the board is built to come up as a wired Steam Controller 2
  (`28DE:1302`) that Steam can pair with its puck over the cable (untested with the samples). What
  that needs is the same in every sample: the HID node, the retained RAM area and the smaller
  `sram0` in the overlay, and the USB and log lines in `prj.conf`
  ([API reference: wired mode](../docs/api.md#wired-mode)).
- **The pairing button**, on P0.31 in every sample and the template except buttons_sticks, where
  it is P0.09 because the trigger uses P0.31's analog input:

  | When | What it does |
  | --- | --- |
  | held while plugging in | this boot is wireless |
  | pressed and released while wired | the board reboots into wireless mode |
  | pressed while wireless | it answers an open pairing window (untested with Steam) |

  `app/` has richer rules of its own ([app/README.md](../app/README.md#wired-and-wireless-modes)).
- **The shell in wireless mode.** The board has a USB serial console with a shell only in wireless
  mode. On a bare board, hold the pairing button's pin against GND with a wire while plugging it
  in: it starts wireless and the console appears. Right after Steam's pairing the board is wireless
  too. How to open it: [Reach the shell](../docs/getting-started.md#reach-the-shell).
- **Bonds in flash.** The pairing is kept in Zephyr settings on NVS, in the board's storage
  partition, which a `.uf2` never writes: flashing a new build keeps it.
- **No `power_request` callback.** The library then does the reboots Steam asks for itself, and it
  ignores Steam's request to enter a bootloader for a firmware update, so such an update cannot
  start and the firmware stays as it is.
- **The board's own files.** The SuperMini's Kconfig lines (its RC oscillator) are in
  `boards/promicro_nrf52840_nrf52840_uf2.conf`, its pins in the overlay next to it. For another
  board, add the two files named after its board target ([docs/porting.md](../docs/porting.md)).
- **No hardware test hooks.** The samples build without `CONFIG_OPENSC2_TEST_HOOKS`; the hardware
  test suite was written for and run against `app/` ([docs/testing.md](../docs/testing.md)).
- **Lizard mode.** Until Steam takes over, a Steam Controller 2 acts as a plain mouse and keyboard
  (the right trackpad moves the pointer, A is Enter, B is Escape, the D-pad gives the arrow keys).
  Steam switches it off while it runs; about 10 seconds after Steam stops, it comes back. OpenSC2
  does the same, in every sample.
- **One limit.** When the PC goes to sleep, the puck tells the controller to turn off, and the
  radio stays off: the samples have no code that turns it back on (`app/` turns it on at any button
  press). Reset the board, holding the pairing button if it is on USB power, so that it starts
  wireless again.

### Useful shell commands

| Command | Shows |
| --- | --- |
| `opensc2 status` | the mode and why, the link, the identity, a summary of the last wired session |
| `opensc2 state` | what the inputs read right now, as the board sends them |
| `opensc2 usb log` | what happened while the board was wired |
| `opensc2 bonds` | the pairings; `opensc2 slot 1` or `2` picks one |
| `opensc2 reboot wired` | reboot into wired mode once |

The shell numbers the two pairing slots 1 and 2; the API numbers them 0 and 1. `opensc2 bonds`
shows your puck's serial, and `opensc2 status` and `opensc2 id` show the board's serials: mask them
before you post them. `opensc2 usb log` and the test suite's reports are masked and safe to share.
Every command: [API reference: shell commands](../docs/api.md#shell-commands).

### Build and flash one

Windows, in PowerShell in the repository folder (here for minimal):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Project samples\minimal -OutDir C:\b\osc2-minimal
```

The image is `<OutDir>\<sample>\zephyr\zephyr.uf2`, here `C:\b\osc2-minimal\minimal\zephyr\zephyr.uf2`
(sysbuild names the image after the sample's folder). Without `-OutDir` it is
`build\samples_minimal\minimal\zephyr\zephyr.uf2` in the repository. Keep build folders short on
Windows: its 260-character path limit breaks deep ones.

Linux and macOS, in the nRF Connect SDK's shell with `ZEPHYR_BASE` set
([Build](../docs/getting-started.md#build); not tried for this project yet):

```sh
west build -b promicro_nrf52840/nrf52840/uf2 -d build/minimal samples/minimal
```

The image is `build/minimal/minimal/zephyr/zephyr.uf2`.

To flash, short RST to GND twice quickly: a USB drive appears. Copy `zephyr.uf2` onto it; the board
restarts with the new firmware ([Flash](../docs/getting-started.md#flash)).

### Add one in Steam

Plug the puck into the PC, start Steam, switch a real Steam Controller 2 off, start adding a
controller in Steam, and plug the board in with a USB-C data cable. Steam pairs it with the puck
over the cable and reboots it into wireless mode; it then connects through the puck. The steps in
full: [Add it in Steam](../docs/getting-started.md#add-it-in-steam).

## What each one teaches

- **minimal**: `opensc2_init()` with its error logged, `opensc2_set_buttons()`, and the mode: the
  pairing button held at boot sets `config.boot_mode = OPENSC2_BOOT_WIRELESS`; pressed and released
  while wired it reboots into wireless mode (`opensc2_reboot_later()`); pressed while wireless it
  answers an open pairing window (`opensc2_pair_start()`). One loop polls both buttons, with a
  two-read debounce.
- **buttons_sticks**: a `gpio-keys` node whose `zephyr,code` values are the controller's button bits
  (`<dt-bindings/opensc2/inputs.h>`), and ADC channels named in `zephyr,user`. The full version,
  with every input, ranges that grow with use and unwired pins held neutral, is
  [app/src/inputs.c](../app/src/inputs.c).
- **touchpad**: a touch device of Zephyr's input subsystem as a trackpad. Absolute X and Y become
  -32767..32767 with Y flipped (the controller counts up as positive); the touch key or the touch
  strength (Z) becomes touch and pressure; a button is the click. Zephyr's touch-panel drivers
  (FT5336, GT911, the CST8xx family with the CST816S) report the same events: change the
  devicetree node and the size. It also powers the module before its driver starts.
- **imu**: `sensor_sample_fetch()` and `sensor_channel_get()` at about 250 Hz, the conversion to the
  report's scale, and the fusion helper (`CONFIG_OPENSC2_FUSION`) for the orientation quaternion.
  It streams only while Steam has the IMU on (controller setting 48). Other IMUs: replace the node.
- **rumble**: the `rumble` and `haptic` callbacks on one motor: rumble as a duty cycle, the haptic
  effects as short buzzes. The library stops a rumble Steam stopped repeating.
- **rgb_led**: the status LED as `app/` shows it: the system state machine's mode
  (`opensc2_led_status()`) through the library's pattern player (`<opensc2/led.h>`), with the
  unit's LED trims, Steam's colour while its LED override is on and Steam's brightness.
- **haptics**: the library's haptics synthesiser (`CONFIG_OPENSC2_HAPTICS_SYNTH`) with its built-in
  outputs, each set up from a devicetree node (`opensc2,haptics-i2s`, `opensc2,haptics-pwm-dac`, or
  a `ti,drv2605` chosen as `opensc2,haptics-drv2605`), the start-up cue the system state machine
  plays itself (the sample plays it with `opensc2_haptics_play_cue()` only without the machine),
  and a shell command that plays the sounds by hand.

Back to the [documentation map](../docs/README.md)
