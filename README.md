# OpenSC2-Controller

Make your own nRF52840 hardware act as a **Steam Controller 2** towards the official, unmodified
Steam Controller 2 puck, over the puck's 2.4 GHz link.

It is two things in one repository:

- **The library (`opensc2`)**: a Zephyr module for nRF Connect SDK and Zephyr applications on Nordic
  nRF52 chips. It does the radio protocol (pairing, connecting, reconnecting, keep-alives, channel
  changes, the protocol version, the identity packet) and the vendor commands Steam needs; your
  application supplies the inputs (buttons, sticks, triggers, trackpads, IMU, battery) and gets the
  outputs (rumble, haptics, LED colour, settings, turn-off) as callbacks. No pins inside.
- **The firmware (`app/`)**: a ready-to-flash "blank canvas" for the SuperMini / Pro Micro
  nRF52840 (nice!nano pinout, Adafruit UF2 bootloader). Every input is mapped to a pin in one
  devicetree file; wire what you have.

> **Status: untested on hardware.** Everything builds against a stock nRF Connect SDK v3.4.0
> without warnings and the wire-format tests pass, but no part of it has yet talked to a real puck.
> Treat the first pairing as an experiment. See [What works](#what-works-and-what-is-untested).

This is an unofficial personal research and interoperability project. It is not affiliated with,
endorsed by or supported by Valve. It contains no Valve code, firmware or data. Steam and Steam
Controller are trademarks of Valve Corporation, used here only to say what this works with.

## How it was made

A specification-driven rewrite, the same way as [Counterpuck](https://github.com/ded811/Counterpuck)
(the open firmware for the other end of the link, which makes an nRF52840 board act as the puck).
It was written from:

- the published protocol documentation of the
  [steam-controller-2-reverse-engineered](https://github.com/ded811/steam-controller-2-reverse-engineered)
  project (the controller's and the puck's radio link, report `0x42`, the vendor commands, the
  haptics reports), which came from studying the firmware Steam ships;
- Counterpuck's specification and source, which were themselves written from that documentation and
  checked in black-box tests against a real controller;
- the public names of reports, buttons and settings in SDL's open-source Steam Controller 2 driver.

It was written by the same people who documented the protocol, so it is not a strict clean-room
implementation; but none of its code comes from Valve's firmware or from a reconstruction of it.
Where the documentation leaves a gap, the code does what should work with the puck, and
[docs/protocol.md](docs/protocol.md#where-this-library-had-to-fill-gaps) lists each one.

## What works, and what is untested

| | |
| --- | --- |
| Builds | `app/` and the six samples build for `promicro_nrf52840/nrf52840/uf2` against stock nRF Connect SDK v3.4.0 with no compiler warnings |
| Tests | the wire formats, lizard mode, settings defaults and the fusion helper pass 20 host tests (242 checks); the same cases build as a Zephyr ztest suite |
| **Untested** | **everything on hardware**: pairing with a real puck, connecting, reports reaching Steam, rumble and haptics, LED commands, the turn-off and wake-up path, the analog input calibration, the optional touch, IMU, motor and LED parts |
| Not implemented | Bluetooth and USB as transports, the dock (pogo pads), lizard-mode momentum, PCM haptics playback in the samples |

## Quick start: the blank-canvas firmware

**1. Wire it.** Buttons go between their pin and GND (internal pull-ups). A thumbstick module's two
potentiometers go between VCC and GND with the wipers on the analog pins. Anything you leave out
reads neutral.

| Pin | Input | Pin | Input | Pin | Input |
| --- | --- | --- | --- | --- | --- |
| P0.06 | D-pad up | P0.31 | **pairing button** | P1.01 | RT (digital) |
| P0.08 | D-pad down | P0.29 | left stick Y (analog) | P1.02 | R3 |
| P0.17 | D-pad left | P0.02 | left stick X (analog) | P1.07 | VIEW |
| P0.20 | D-pad right | P1.15 | A | | |
| P0.22 | L (bumper) | P1.13 | B | | |
| P0.24 | LT (digital) | P1.11 | X | | |
| P1.00 | L3 | P0.10 | Y | | |
| P0.11 | MENU | P0.09 | R (bumper) | | |
| P1.04 | STEAM | | | | |
| P1.06 | QAM ("...") | | | on-board LED | status |

The SuperMini has only three analog-capable pins, so the right stick, analog triggers and the back
buttons L4/L5/R4/R5 are listed in the overlay but unassigned; see [app/README.md](app/README.md).

**2. Build** (Windows, nRF Connect SDK v3.4.0 installed in `C:\ncs` with Toolchain Manager):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Project app -OutDir C:\b\osc2
```

Use a short `-OutDir`: Windows' 260-character path limit breaks deep build folders. On Linux or
macOS, in an nRF Connect SDK environment: `west build -b promicro_nrf52840/nrf52840/uf2 app`.

**3. Flash.** Short RST to GND twice quickly: the board appears as a USB drive. Copy
`zephyr.uf2` (the build prints its path) onto it.

**4. Pair through Steam.** In Steam, add a controller as usual, which opens the puck's 60-second
pairing window, and press the board's pairing button. The LED double-flashes while pairing, blinks
slowly while it waits for the puck, and stays on when connected. The pairing is stored and survives
reflashing. (With [Counterpuck](https://github.com/ded811/Counterpuck) as the puck, its shell
command `pair 0` opens a window without Steam.)

The board also shows up on the PC as a USB serial port with logs and a shell: `opensc2 status`,
`opensc2 pair`, `opensc2 id set unit ABC0123456789`, `uf2` (back to the bootloader). More in
[app/README.md](app/README.md).

## Quick start: the library

Add the repository as a Zephyr module, in your application's `CMakeLists.txt` before
`find_package(Zephyr)`:

```cmake
list(APPEND ZEPHYR_EXTRA_MODULES /path/to/OpenSC2-Controller)
```

or in your west manifest. Then in `prj.conf`:

```
CONFIG_OPENSC2=y
CONFIG_ESB_MAX_PAYLOAD_LENGTH=128
CONFIG_ESB_TX_FIFO_SIZE=2
CONFIG_SETTINGS=y
CONFIG_SETTINGS_NVS=y
CONFIG_NVS=y
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
```

and in code:

```c
#include <opensc2/opensc2.h>

static void on_rumble(const struct opensc2_rumble *r, void *user)
{
	motor_set(MAX(r->left_speed, r->right_speed));   /* your hardware */
}

static const struct opensc2_callbacks cb = { .rumble = on_rumble };
static const struct opensc2_config cfg = { .callbacks = &cb, .start = true };

int main(void)
{
	opensc2_init(&cfg);
	opensc2_pair_start(0);                              /* while Steam's pairing window is open */
	opensc2_set_buttons(OPENSC2_BTN_A, OPENSC2_BTN_A);  /* A pressed */
	opensc2_set_stick(OPENSC2_LEFT, 0, 32767);          /* left stick fully up */
	return 0;
}
```

Every input and output, with its range and where Steam sees it: [docs/api.md](docs/api.md).

## The samples

All for the SuperMini; each builds on its own and is documented in [samples/README.md](samples/README.md).

| Sample | Shows |
| --- | --- |
| [minimal](samples/minimal) | the smallest application: init, pair on a button, send one button |
| [buttons_sticks](samples/buttons_sticks) | buttons, a stick and a trigger from devicetree (gpio-keys and ADC) |
| [touchpad](samples/touchpad) | a Cirque Pinnacle I2C trackpad module as the right trackpad (any input-subsystem touch device works) |
| [imu](samples/imu) | an MPU-6050 through Zephyr's sensor API, with the fusion helper for the quaternion |
| [rumble](samples/rumble) | rumble and haptics on a PWM-driven vibration motor |
| [rgb_led](samples/rgb_led) | an RGB LED that follows Steam's LED colour and shows the link |

The parts for the touchpad, IMU, rumble and RGB samples were not at hand: those samples build but
are untested.

## Repository layout

```text
zephyr/module.yml, CMakeLists.txt, Kconfig   the Zephyr module
include/opensc2/        public headers: opensc2.h (API), types.h, codec.h (wire formats),
                        settings.h, fusion.h
include/dt-bindings/opensc2/inputs.h   button bit numbers, for C and devicetree
lib/opensc2/            the library's sources
app/                    the blank-canvas firmware for the SuperMini
samples/                minimal, buttons_sticks, touchpad, imu, rumble, rgb_led
tests/codec/            ztest suite (native_sim or QEMU); tests/host/ the same cases on a PC
docs/                   api.md, porting.md, protocol.md
build.ps1, ncs_env.ps1  Windows build helpers
```

Host tests: `tests/host/run.sh` (or `run.ps1`) with any gcc or clang. Zephyr tests, on Linux:
`west twister -T tests -p native_sim`.

## Licence

The code and documentation here are MIT licensed ([LICENSE](LICENSE)). A built firmware also
contains Zephyr, nrfx and Nordic's Enhanced ShockBurst library under their own licences; the ESB
part may only be used with Nordic chips. See [THIRD_PARTY.md](THIRD_PARTY.md).
