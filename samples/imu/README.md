# imu: an accelerometer and gyroscope as the controller's IMU

An MPU-6050 module, read through Zephyr's sensor API, as the Steam Controller 2's IMU, with the
library's fusion helper for the orientation. For anyone who wants gyro controls in Steam.

> [!NOTE]
> Builds for the SuperMini (`promicro_nrf52840/nrf52840/uf2`); not run on hardware.

## What it shows

- `sensor_sample_fetch()` and `sensor_channel_get()` for `SENSOR_CHAN_ACCEL_XYZ` and
  `SENSOR_CHAN_GYRO_XYZ` about 250 times a second.
- The library's IMU ([`<opensc2/imu.h>`](../../docs/api.md#the-imu)): each sample in m/s² and rad/s
  to `opensc2_imu_sample()`, which scales it as the real controller does (32767 = 2 g and
  2000 deg/s); like the real controller it streams from boot until a host writes controller
  setting 48 = 0 (`opensc2_imu_streaming()`), and the reports keep the last sample while it is off.
- The fusion helper (`CONFIG_OPENSC2_FUSION`): `opensc2_fusion_init()` and
  `opensc2_fusion_update()` for the orientation quaternion (`fusion.q`, W first).
- A part powered from the board's switched VCC before its driver starts: VCC goes on in a
  `SYS_INIT` hook, since Zephyr starts the sensor driver before `main()`.
- The pairing button with the same rules as every sample.

## Parts

- The SuperMini nRF52840, a Pro Micro-sized nRF52840 board with the nice!nano pinout and the
  Adafruit nRF52 UF2 bootloader.
- An MPU-6050 module, for example the GY-521 breakout, run by Zephyr's `invensense,mpu6050` driver
  (its devicetree binding in nRF Connect SDK v3.4.0:
  [invensense,mpu6050.yaml](https://github.com/nrfconnect/sdk-zephyr/blob/ncs-v3.4.0/dts/bindings/sensor/invensense,mpu6050.yaml)).
- A push button for pairing.

## Wiring

| SuperMini pin | Connects to |
| --- | --- |
| P0.17 | the module's SDA |
| P0.20 | the module's SCL |
| VCC | the module's VCC |
| GND | the module's GND, and the button |
| P0.31 | pairing button, other side to GND |

Leave the module's AD0 pin low (I2C address `0x68`); with AD0 high the address is `0x69` (change
`reg` and the node's unit address in the overlay). Breakout boards such as the GY-521 usually
have pull-up resistors on SDA and SCL; a bare sensor needs them (about 4.7 kOhm to VCC). The board
switches VCC with P0.13; the sample turns it on before the sensor driver starts.

Mount the sensor the controller's way round: X points right, Y away from the player, Z up when the
controller lies flat. The silkscreen names pins by three digits: "006" is P0.06, "111" is P1.11.
Pins are set in [boards/promicro_nrf52840_nrf52840_uf2.overlay](boards/promicro_nrf52840_nrf52840_uf2.overlay).

## Build and flash

Windows, in PowerShell in the repository folder:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Project samples\imu -OutDir C:\b\osc2-imu
```

The image is `C:\b\osc2-imu\imu\zephyr\zephyr.uf2`. `C:\b\osc2-imu` is any short folder: Windows'
260-character path limit breaks deep build folders.

Linux and macOS, in the nRF Connect SDK's shell with `ZEPHYR_BASE` set
([Build](../../docs/getting-started.md#build); not tried for this project yet):

```sh
west build -b promicro_nrf52840/nrf52840/uf2 -d build/imu samples/imu
```

The image is `build/imu/imu/zephyr/zephyr.uf2`.

To flash, short RST to GND twice quickly: a USB drive appears. Copy `zephyr.uf2` onto it; the board
restarts with the new firmware ([Flash](../../docs/getting-started.md#flash)).

## Add it in Steam

These steps are how the tested firmware in `app/` behaves; this sample follows the same rules
but has not run on hardware.

1. Plug the puck into the PC and start Steam. Switch a real Steam Controller 2 off and keep it
   unplugged while you add the board.
2. In Steam, start adding a controller. Steam asks you to connect it with a USB-C cable.
3. Plug the board in with a USB-C data cable (not a charge-only cable). It comes up wired. This
   sample does not drive the board's LED.
4. Steam pairs the board with the puck over the cable and reboots it into wireless mode. The board
   then connects through the puck.
5. From then on the board connects through the puck by itself whenever it starts wireless. Plugged
   into a PC again it comes up wired; hold the pairing button while plugging in to keep it
   wireless.

If Steam later forgets the controller, add it again the same way. The same steps for `app/`, with
what its LED shows: [Add it in Steam](../../docs/getting-started.md#add-it-in-steam).

The pairing button (P0.31):

| When | What it does |
| --- | --- |
| held while plugging in | this boot is wireless |
| pressed and released while wired | the board reboots into wireless mode |
| pressed while wireless | it answers an open pairing window (untested with Steam) |

## What to expect

The sample is built to do the following; none of it has run on hardware yet.

- The acceleration, angular rate and orientation follow the sensor from boot, wired and through
  the puck, until a host writes controller setting 48 = 0 (the last sample stays) and again after
  any other value.
- In wireless mode the board's USB serial port has a shell
  ([Reach the shell](../../docs/getting-started.md#reach-the-shell)). `opensc2 state` shows the
  line `imu       t <us>  a <x> <y> <z>  g <x> <y> <z>  q <w> <x> <y> <z>`; lying flat and still,
  `a` reads about 16383 (1 g) on Z and `g` about 0. `opensc2 status` shows the IMU mode
  Steam set, and the board's serials: mask them before you post the output.
- If the sensor did not start, the log says "The IMU did not start" and the controller goes on
  without IMU data.
- When the PC goes to sleep, the puck tells the controller to turn off: the library's system state
  machine switches wireless off, as the real controller does on USB power, and nothing in this
  sample turns it on again. Type `opensc2 start` on its console, or reset the board (short RST to
  GND once), holding the pairing button if it is on USB power, so that it starts wireless again.

## Change it

- **Another IMU**: any accelerometer and gyroscope with a Zephyr sensor driver that gives
  `SENSOR_CHAN_ACCEL_XYZ` and `SENSOR_CHAN_GYRO_XYZ`, for example the LSM6DSO (`st,lsm6dso`), the
  ICM-42688 (`invensense,icm42688`) or the BMI270 (`bosch,bmi270`). Replace the node in the overlay,
  keep the `imu` label, and set its ranges to the report's ±2 g and ±2000 deg/s where the driver
  allows it.
- **Another mounting**: swap or negate the axes in the `for` loop of `main()` before
  `opensc2_fusion_update()`; `app/src/imu.c` hands its overlay's `imu-mounting-matrix` to the
  library's IMU (`opensc2_imu_set_mounting()`) instead.
- **Keep streaming while a host has it off** (for a test): read and show the sensor yourself;
  the library's IMU refuses samples while setting 48 is 0, as the real controller streams
  nothing then.
- **An interrupt instead of polling**: the MPU-6050 driver has a data-ready trigger
  (`int-gpios` and `CONFIG_MPU6050_TRIGGER_*`); this sample polls to stay short.
- **Your own project**: start from [template/](../../template)
  ([docs/integration.md](../../docs/integration.md)).

Back to the [samples](../README.md) · the [documentation map](../../docs/README.md)
