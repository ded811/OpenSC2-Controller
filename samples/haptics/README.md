# haptics: the haptics synthesiser with real outputs

Everything Steam plays on the controller's four haptic actuators, rendered by the library's
haptics synthesiser and played on real outputs: the grips on an I2S amplifier, the trackpads on a
PWM "DAC", or one actuator on a DRV2605. For anyone who wants the real controller's haptics on a
board of their own.

> [!NOTE]
> Builds for the SuperMini (`promicro_nrf52840/nrf52840/uf2`); not run on hardware. The
> synthesiser's rendering is checked by the host tests and, on a board with nothing wired, by the
> hardware test suite's `haptics` group ([Testing](../../docs/testing.md)); these outputs have
> never run on parts.

## What it shows

- `CONFIG_OPENSC2_HAPTICS_SYNTH`: the library renders rumble (`0x80`), pulse trains, clicks,
  tones, sweeps and scripts (`0x81` to `0x85`) and PCM streams (`0x86` to `0x89`) into sample
  blocks, as the real controller's two sequencers do: the pads at 4 kHz, the grips at 8 kHz, one
  8 ms block at a time ([API reference: the haptics synthesiser](../../docs/api.md#the-haptics-synthesiser)).
- Two built-in outputs, set up from the overlay's devicetree nodes:
  `CONFIG_OPENSC2_HAPTICS_OUT_I2S` (`opensc2,haptics-i2s`: Zephyr's I2S driver as master, 16-bit
  stereo at 8 kHz) and `CONFIG_OPENSC2_HAPTICS_OUT_PWM_DAC` (`opensc2,haptics-pwm-dac`: nrfx's PWM
  sequence playback, each sample a duty cycle, exactly 4 kHz).
- With `drv2605.conf` on top, `CONFIG_OPENSC2_HAPTICS_OUT_DRV2605` instead: one ERM or LRA on a
  DRV2605(L) in real-time playback through Zephyr's `ti,drv2605` driver; each 8 ms block's loudness
  becomes its amplitude (the chip has no drive-frequency control: tones and sweeps lose their pitch).
- The synthesiser's own sounds (OpenSC2's, not Valve's): the start-up cue, which the library's
  system state machine plays itself, and a `hap` shell command that plays the cues, the 17 built-in
  scripts, the clicks and the drop sounds by hand.
- The pairing button with the same rules as every sample.

## Parts

- The SuperMini nRF52840.
- For the grips: an I2S class-D amplifier per grip (for example a MAX98357A board, one set to the
  left word and one to the right) or a stereo one, and a small wideband voice-coil exciter or
  speaker-like actuator per amplifier.
- For the pads: a small H-bridge or audio amplifier per pad, each fed through an RC low-pass from
  its PWM pin (for example 1 kOhm and 100 nF, about 1.6 kHz), and an actuator per pad.
- Or, with `drv2605.conf`: a DRV2605L breakout and an ERM motor or an LRA.
- A push button for pairing.

## Wiring

| SuperMini pin | Connects to |
| --- | --- |
| P0.17 | I2S BCLK (bit clock) |
| P0.20 | I2S LRCLK (word clock) |
| P0.22 | I2S DIN (data) |
| P0.24 | the amplifiers' SD (shutdown) pin: high while something plays |
| P0.06 | the left pad's PWM, through its RC low-pass |
| P0.08 | the right pad's PWM, through its RC low-pass |
| P1.04 | the pads' driver enable: high while something plays |
| P1.00, P0.11 | `drv2605.conf`: the DRV2605's SDA and SCL (with pull-ups) |
| P1.06 | `drv2605.conf`: the DRV2605's EN |
| P0.31 | pairing button, other side to GND |

Controller setting 75 = 1 (Steam's "amplifier always on") keeps the enables high between sounds.
Never drive an actuator straight from a pin. Pins are set in
[boards/promicro_nrf52840_nrf52840_uf2.overlay](boards/promicro_nrf52840_nrf52840_uf2.overlay); the
nRF52840 routes I2S, PWM and I2C to any GPIO. The PWM DAC uses PWM3, which Zephyr's PWM driver must
leave alone (its node stays disabled).

## Build and flash

Windows, in PowerShell in the repository folder:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Project samples\haptics -OutDir C:\b\osc2-hap
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Project samples\haptics\drv2605.conf -OutDir C:\b\osc2-drv
```

The images are `C:\b\osc2-hap\haptics\zephyr\zephyr.uf2` and `C:\b\osc2-drv\haptics\zephyr\zephyr.uf2`.

Linux and macOS, in the nRF Connect SDK's shell with `ZEPHYR_BASE` set (not tried for this project
yet):

```sh
west build -b promicro_nrf52840/nrf52840/uf2 -d build/haptics samples/haptics
```

To flash, short RST to GND twice quickly: a USB drive appears. Copy `zephyr.uf2` onto it
([Flash](../../docs/getting-started.md#flash)). Adding it in Steam works as for every sample
([samples](../README.md#what-every-sample-has)).

## What to expect

The sample is built to do the following; none of it has run on hardware yet.

- At start-up a short cue of OpenSC2's own on the grips (wired: cue 0; wireless: the stored
  transport's cue), at the chime level Steam stores (`0xDC`; 1 quiet, 2 normal, anything else
  silent). The library's system state machine plays it, as the real controller does; the sample
  plays it itself only in a build without the machine.
- Games' rumble through Steam: a 50 Hz tone with a slow wobble on the left grip and 50 Hz bursts
  on the right one, each lasting 500 ms from Steam's latest rumble report (Steam repeats them while
  a game rumbles), as the real controller plays it.
- Steam's haptic effects: pulse trains, clicks on the pads, tones, sweeps and scripts, each with
  its gain and Steam's master gain (setting 76). PCM streams play once Steam has set their format.
- `hap cue 5`, `hap script 13 3`, `hap click a`, `hap fall`, `hap info` in the shell (wireless
  mode, [Reach the shell](../../docs/getting-started.md#reach-the-shell)).

## Change it

- **One output only**: delete the node you do not use from the overlay and turn its option off
  in `prj.conf`.
- **An output of your own**: implement `struct opensc2_haptics_output_api` (`write` is the only
  required call) and register it with `opensc2_haptics_set_output()`, or take only each block's
  loudness with `opensc2_haptics_set_level_output()` ([API reference](../../docs/api.md#the-haptics-synthesiser)).
- **Your own project**: start from [template/](../../template)
  ([docs/integration.md](../../docs/integration.md)).

Back to the [samples](../README.md) · the [documentation map](../../docs/README.md)
