# Porting to other boards

What it takes to run OpenSC2 on a board other than the SuperMini nRF52840: for people porting the
library or the firmware to their own hardware.

The library itself has no pins and no board code; anything with a Nordic nRF52 radio that nRF
Connect SDK's Enhanced ShockBurst supports can run it. It has been built for the nRF52840 only, and
run on the SuperMini nRF52840. Other nRF52 chips, other boards, and the nRF53 and nRF54L families
that ESB also supports are untested.

## What a board needs

| Need | Why |
| --- | --- |
| An nRF52 radio (nRF52840, nRF52833, nRF52832, nRF52820, nRF52811, nRF52810) | ESB on the BLE 2M PHY at +6 dBm, as the real controller. The nRF52832, 52810 and 52811 stop at +4 dBm (set `CONFIG_OPENSC2_TX_POWER_DBM=4`); the 52810 and 52811 also have little RAM. |
| The 32 MHz crystal | ESB needs the high-frequency crystal; the library requests it while the radio is on. Every nRF52 module has it. |
| A 32.768 kHz source | A crystal, or the internal RC oscillator with calibration (`CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y`, `..._RC_CALIBRATION=y`), as the SuperMini's `boards/promicro_nrf52840_nrf52840_uf2.conf` files do. |
| About 180 KB of flash and 45 KB of RAM, plus a 16 KB retained RAM area for wired mode | measured with nRF Connect SDK v3.4.0 for the SuperMini, with wired mode, the shell and the log: the minimal sample uses 177,184 bytes of flash and 43,004 bytes of RAM, the template 177,452 and 44,860, `app/` (with the test hooks) 213,392 and 52,028. Bluetooth (`app/ble.conf`) takes about 150 KB of flash and 30 KB of RAM more, the haptics synthesiser about 15 KB and 10 KB ([every build of `app/`](../app/README.md#build-options)). A build without USB, the shell or the log is smaller (not measured). Each build prints its exact use. |
| A flash partition for Zephyr settings | the bonds. Without one (or without `CONFIG_SETTINGS`), pairings are lost at every reboot. |

## Steps

1. **Add the module.** The three ways (a plain folder, a git submodule, a west workspace) are in
   [integration.md](integration.md#way-1-a-plain-folder); for a west manifest of your own, see
   [Way 3: a west workspace](integration.md#way-3-a-west-workspace).

2. **Kconfig.** In `prj.conf`:

   ```text
   CONFIG_OPENSC2=y
   CONFIG_ESB_MAX_PAYLOAD_LENGTH=128
   CONFIG_ESB_TX_FIFO_SIZE=2
   CONFIG_FLASH=y
   CONFIG_FLASH_MAP=y
   CONFIG_NVS=y
   CONFIG_SETTINGS=y
   CONFIG_SETTINGS_NVS=y
   ```

   On the nRF54L family use ZMS instead of NVS (`CONFIG_ZMS=y`, `CONFIG_SETTINGS_ZMS=y`). Every
   option: [api.md](api.md#kconfig-options).

3. **Board-specific Kconfig goes in `boards/<board>.conf`.** Zephyr merges
   `boards/<board target with / turned into _>.conf` after `prj.conf`, so `prj.conf` stays the same
   for every board. The SuperMini's file, `boards/promicro_nrf52840_nrf52840_uf2.conf`, holds its
   two RC oscillator lines (it has no 32.768 kHz crystal):

   ```text
   CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y
   CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC_CALIBRATION=y
   ```

   A board with a crystal leaves them out.

4. **Partitions.** The settings need a `storage_partition` that nothing else uses. With a
   bootloader of your own, keep it out of the bootloader's area. nRF Connect SDK's Partition
   Manager ignores devicetree partitions; with a fixed partition table (as on UF2 boards) turn it off
   in `sysbuild.conf` (`SB_CONFIG_PARTITION_MANAGER=n`), and check the build does not produce a
   `partitions.yml`.

5. **Inputs.** The blank canvas's overlay and `app/src/inputs.c` work on any nRF52 board: copy the
   overlay, rename it for your board (`boards/<board>.overlay`), and change the pins. Analog inputs are
   SAADC channels; the AIN number picks the pin (nRF52840: AIN0 P0.02, AIN1 P0.03, AIN2 P0.04, AIN3
   P0.05, AIN4 P0.28, AIN5 P0.29, AIN6 P0.30, AIN7 P0.31). The unwired-pin bias in `inputs.c` uses the
   nRF SAADC's internal resistors; other chips' ADCs would need another way (an external resistor
   to mid-supply does the same).

6. **Many buttons: the GPIO sense mechanism.** The nRF52840 has only eight GPIOTE channels. By
   default each button of a `gpio-keys` node takes one for its edge interrupt, so the ninth button
   fails (`-ENOMEM`). The GPIO sense mechanism has no such limit: put the button pins in the
   ports' `sense-edge-mask`, as the app overlay does for every pin:

   ```dts
   &gpio0 { sense-edge-mask = <0xffffffff>; };
   &gpio1 { sense-edge-mask = <0xffff>; };
   ```

   Pins outside the mask keep using GPIOTE channels.

7. **Turn off what shares pins.** Board files often enable UART, I2C, SPI or PWM on header pins.
   Disable the ones whose pins you use for buttons (`&uart0 { status = "disabled"; };`).

8. **Logging and the shell.** Any console works. On boards with USB, the CDC-ACM serial port is the
   easiest; the SuperMini's board files set it up. With wired mode the library runs USB itself and
   shows that port in wireless mode only (below); a UART console works in both modes. The console's
   USB IDs can be changed (`CONFIG_OPENSC2_USB_CONSOLE_VID`, `_PID`), but the PC tools
   (`tools/hwtest`, `tools/match_steam_version.py`) then refuse the board: they only talk to a
   console with the default IDs `2FE3:0004` and the product string "OpenSC2 console"
   ([NOTICE.md](../NOTICE.md)).

9. **Radio coexistence.** The library owns the radio while it is on. Its own Bluetooth
   (`CONFIG_OPENSC2_BLE`, not yet tested on hardware) takes turns with the puck's link: one at a
   time, the radio handed over at run time ([api.md](api.md#the-bluetooth-build)); that needs
   Nordic's SoftDevice Controller and no other user of the radio. Bluetooth of the application's
   own next to the puck's link is not supported (it would need ESB on MPSL timeslots,
   `CONFIG_ESB_MPSL_TIMESLOT`, untested). A board for the Bluetooth build also needs a free EGU
   (`egu3` by default) and the 32.768 kHz source of the table above (MPSL then calibrates an RC
   oscillator every 4 s).

## Wired mode

`CONFIG_OPENSC2_USB` turns itself on when the devicetree has the HID node below; without it every
boot is wireless and nothing else changes.

| Need | Why |
| --- | --- |
| A chip with a USB device controller: nRF52840, nRF52833 or nRF52820 | the wired Steam Controller 2 is a full-speed USB device. The nRF52832, 52811 and 52810 have no USB: wireless only. |
| The nRF52's own USB power detection (the POWER peripheral's `USBREGSTATUS`) | decides the mode at every boot, and catches a cable plugged in later. Other families (the nRF5340 detects USB power elsewhere) read "no USB power" until `osc2_usb_power()` in `lib/opensc2/mode.c` learns them; `config.boot_mode = OPENSC2_BOOT_WIRED` still works. |
| Zephyr's USB device stack (`CONFIG_USB_DEVICE_STACK_NEXT`) and its HID class | the device is built on them; the SuperMini's board files select the stack |
| A `zephyr,hid-device` node chosen as `opensc2,usb-hid`: 64-byte input and output reports, 1000 µs | the one HID interface ([api.md](api.md#wired-mode) has the node) |
| `CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT=n`, `CONFIG_USBD_HID_IN_BUF_COUNT=8`, `CONFIG_USBD_THREAD_STACK_SIZE=2048`, `CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=2048`; with `CONFIG_LOG`, `CONFIG_LOG_PROCESS_THREAD_STACK_SIZE=1536` | the library starts USB itself after deciding the mode (board defaults that start a serial port at boot must be off; the build checks); eight report buffers as on the real controller; commands write flash from the USB stack's thread; while wired the log thread also writes the log into the session log (the build checks; or turn `CONFIG_OPENSC2_USB_LOG_ZEPHYR` off) |
| Recommended: a RAM region chosen as `opensc2,retained-ram` | the USB session log, the warm-restart mark and the crash record (`CONFIG_OPENSC2_CRASH_RECORD`), in RAM that start-up does not clear; shrink `sram0` by its size. Place it outside the RAM your bootloader uses at a reset (below). Without a region the area is in the application's no-init RAM, which such a bootloader overwrites: the build warns, the session log and a crash record do not survive a reset, and on nRF52 the warm-restart mark defaults to GPREGRET2 instead. |
| A bootloader that leaves that RAM alone at a soft reset | Steam's "reboot into wireless mode" depends on the mark surviving the reboot. If yours clears the region, `CONFIG_OPENSC2_WARM_MARK_GPREGRET2` keeps the mark in a retention register instead (nRF52 only). |
| Optional: a mode switch or a button read at boot | `config.boot_mode`; the blank canvas uses its pairing button and an optional `mode-wireless-gpios` |

The [minimal sample](../samples/minimal) and the [template](../template) have all of it for the
SuperMini.

### Where the retained region may go

The Adafruit nRF52 UF2 bootloader (the SuperMini's; releases 0.6.0 to 0.11.0 on the nRF52840) runs at
every reset and uses RAM in two places (its
[linker script](https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/master/linker/nrf52840.ld),
and the end of `.bss` in its release images):

- from the start of RAM up to the end of its `.bss`, `0x2000CE2C` in 0.6.0 and `0x2000DEC0` in
  0.11.0: below `0x20008000` the SoftDevice of its over-the-air DFU mode and its double-reset word at
  `0x20007F7C`, then its data and its `.bss`, which it zeroes;
- its 8 KB stack, `0x2003E000`..`0x20040000`.

It writes GPREGRET (the request to open the UF2 drive), not GPREGRET2. The SuperMini overlays put the
16 KB region at `0x2002C000`..`0x20030000`, clear of both, and end the application's RAM below it
(`sram0` 176 KB). Keep any other placement out of those ranges and out of the application's RAM.

## Boards with more analog pins

The SuperMini brings out three analog pins, enough for one stick. Boards that expose more (the
Seeed XIAO nRF52840: A0..A5 on P0.02, P0.03, P0.28, P0.29, P0.04, P0.05; the nRF52840 DK: all eight)
can carry both sticks and analog triggers: add `channel@N` nodes and the names `right-stick-x`,
`right-stick-y`, `left-trigger`, `right-trigger` in the overlay. An external ADC through Zephyr's ADC
API works the same way (`io-channels = <&my_adc 0>`), minus the internal bias. None of these boards
has been tried.

If your port works, a [new board report](https://github.com/ded811/OpenSC2-Controller/issues/new/choose)
helps the next person.

Back to the [documentation map](README.md).
