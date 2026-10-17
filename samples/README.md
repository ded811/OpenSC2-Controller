# Samples

Small applications, each showing one part of the library on the SuperMini / Pro Micro nRF52840.
Build any of them from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Project samples\minimal -OutDir C:\b\osc2-minimal
```

(on Linux or macOS: `west build -b promicro_nrf52840/nrf52840/uf2 samples/minimal`), then flash the
`.uf2` as for the [blank canvas](../app/README.md). Each sample has a pairing button: start pairing
in Steam (add a controller), then press it. Pins are in each sample's
`boards/promicro_nrf52840_nrf52840_uf2.overlay`. None of the samples has run on hardware yet.

| Sample | Parts | Pins | Status |
| --- | --- | --- | --- |
| [minimal](minimal) | two push buttons | P1.15 A, P0.31 pairing | builds; untested |
| [buttons_sticks](buttons_sticks) | five buttons, a pairing button, a thumbstick module, a potentiometer or Hall trigger | P1.15 A, P1.13 B, P1.11 X, P0.10 Y, P1.04 STEAM, P0.09 pairing; P0.02 stick X, P0.29 stick Y, P0.31 trigger | builds; untested |
| [touchpad](touchpad) | a Cirque Pinnacle trackpad module on I2C (TM040040 and similar) | P0.17 SDA, P0.20 SCL, P0.22 data ready, P1.15 pad click, P0.31 pairing | builds; untested, part not at hand |
| [imu](imu) | an MPU-6050 module (GY-521) on I2C | P0.17 SDA, P0.20 SCL, P0.31 pairing | builds; untested, part not at hand |
| [rumble](rumble) | a vibration motor with a logic-level MOSFET and flyback diode, or an LRA driver in PWM mode | P0.22 PWM, P0.31 pairing | builds; untested, part not at hand |
| [rgb_led](rgb_led) | a common-anode RGB LED and three resistors | P0.22 red, P0.24 green, P1.00 blue, P0.31 pairing | builds; untested, part not at hand |

## What each one teaches

- **minimal**: `opensc2_init()` with the defaults, `opensc2_pair_start()`, `opensc2_set_buttons()`.
  About 40 lines.
- **buttons_sticks**: a `gpio-keys` node whose `zephyr,code` values are the controller's button bits
  (`<dt-bindings/opensc2/inputs.h>`), and ADC channels named in `zephyr,user`. The full version, with
  every input, auto-ranging and the unwired-pin bias, is [app/src/inputs.c](../app/src/inputs.c).
- **touchpad**: a touch device of Zephyr's input subsystem as a trackpad. Absolute X/Y become
  -32767..32767 with Y flipped (the controller counts up as positive); the touch key or the touch
  strength (Z) becomes touch and pressure; a button is the click. Other parts (FT5336, GT911,
  CST816S touch panels) report the same events: change the devicetree node and the size.
- **imu**: `sensor_sample_fetch()` and `sensor_channel_get()` at 250 Hz, the unit conversion to the
  report's scale, and the fusion helper (`CONFIG_OPENSC2_FUSION`) for the orientation quaternion.
  Streams only while Steam has the IMU on (controller setting 48). Other IMUs: replace the node.
- **rumble**: the `rumble` and `haptic` callbacks on one motor: rumble as a duty cycle, the haptic
  effects as short buzzes. The library stops a rumble Steam stopped repeating.
- **rgb_led**: `opensc2_led_get()` / the `led` callback: Steam's colour and brightness once it turns
  its LED override on, and the link status pattern otherwise.
