# Porting to other boards

The library itself has no pins and no board code; anything with a Nordic nRF52 radio that nRF
Connect SDK's Enhanced ShockBurst supports can run it. It has been built for the nRF52840 only (the
SuperMini / Pro Micro). Other nRF52 chips, and the nRF53 and nRF54L families that ESB also supports,
are untested.

## What a board needs

| Need | Why |
| --- | --- |
| An nRF52 radio (nRF52840, nRF52833, nRF52832, nRF52820, nRF52811, nRF52810) | ESB on the BLE 2M PHY at +6 dBm, as the real controller. The nRF52832, 52810 and 52811 stop at +4 dBm (set `CONFIG_OPENSC2_TX_POWER_DBM=4`); the 52810 and 52811 also have little RAM. |
| The 32 MHz crystal | ESB needs the high-frequency crystal; the library requests it while the radio is on. Every nRF52 module has it. |
| A 32.768 kHz source | A crystal, or the internal RC oscillator with calibration (`CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y`, `..._RC_CALIBRATION=y`), as the SuperMini samples do. |
| About 30 KB of RAM and 70 KB of flash | for the library, ESB, settings and a small application (the minimal sample, with USB logging, is 96 KB / 27 KB) |
| A flash partition for Zephyr settings | the bonds. Without one (or without `CONFIG_SETTINGS`), pairings are lost at every reboot. |

## Steps

1. **Add the module.** Point `ZEPHYR_EXTRA_MODULES` at this repository (as the samples' `CMakeLists.txt`
   do), or add it to your west manifest:

   ```yaml
   manifest:
     projects:
       - name: opensc2
         url: https://github.com/<you>/OpenSC2-Controller
         revision: main
         path: modules/lib/opensc2
   ```

2. **Kconfig.** In `prj.conf`:

   ```
   CONFIG_OPENSC2=y
   CONFIG_ESB_MAX_PAYLOAD_LENGTH=128
   CONFIG_ESB_TX_FIFO_SIZE=2
   CONFIG_FLASH=y
   CONFIG_FLASH_MAP=y
   CONFIG_NVS=y
   CONFIG_SETTINGS=y
   CONFIG_SETTINGS_NVS=y
   ```

   On the nRF54L family use ZMS instead of NVS (`CONFIG_ZMS=y`, `CONFIG_SETTINGS_ZMS=y`).

3. **Partitions.** The settings need a `storage_partition` that nothing else uses. With a
   bootloader of your own, keep it out of the bootloader's area. nRF Connect SDK's Partition
   Manager ignores devicetree partitions; with a fixed partition table (as on UF2 boards) turn it off
   in `sysbuild.conf` (`SB_CONFIG_PARTITION_MANAGER=n`), and check the build does not produce a
   `partitions.yml`.

4. **Inputs.** The blank canvas's overlay and `app/src/inputs.c` work on any nRF52 board: copy the
   overlay, rename it for your board (`boards/<board>.overlay`), and change the pins. Analog inputs are
   SAADC channels; the AIN number picks the pin (nRF52840: AIN0 P0.02, AIN1 P0.03, AIN2 P0.04, AIN3
   P0.05, AIN4 P0.28, AIN5 P0.29, AIN6 P0.30, AIN7 P0.31). The unwired-pin bias in `inputs.c` uses the
   nRF SAADC's internal resistors; other chips' ADCs would need another way (an external resistor
   to mid-supply does the same).

5. **Turn off what shares pins.** Board files often enable UART, I2C, SPI or PWM on header pins.
   Disable the ones whose pins you use for buttons (`&uart0 { status = "disabled"; };`).

6. **Logging and the shell.** Any console works. On boards with USB, the CDC-ACM serial port is the
   easiest; the SuperMini's board files set it up.

7. **Radio coexistence.** The library owns the radio while it is on. Bluetooth at the same time is
   not supported (it would need ESB on MPSL timeslots, `CONFIG_ESB_MPSL_TIMESLOT`, untested).

## Boards with more analog pins

The SuperMini brings out three analog pins, enough for one stick. Boards that expose more (the
Seeed XIAO nRF52840: A0..A5 on P0.02, P0.03, P0.28, P0.29, P0.04, P0.05; the nRF52840 DK: all eight)
can carry both sticks and analog triggers: add `channel@N` nodes and the names `right-stick-x`,
`right-stick-y`, `left-trigger`, `right-trigger` in the overlay. An external ADC through Zephyr's ADC
API works the same way (`io-channels = <&my_adc 0>`), minus the internal bias.
