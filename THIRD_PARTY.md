# Third-party components

What a built OpenSC2 image contains besides OpenSC2's own code, under which licences, and where
their texts are: for anyone who passes a built image on.

> [!NOTE]
> Unofficial: not affiliated with or endorsed by Valve. Steam and Steam Controller are trademarks of
> Valve Corporation. So that Steam recognises it, the firmware uses Valve's USB IDs (`28DE:1302`)
> and the real controller's USB strings, for personal interoperability and research use. It
> contains no Valve code or binaries. See [NOTICE.md](NOTICE.md).

The source in this repository is MIT licensed ([LICENSE](LICENSE)). A built image (`zephyr.uf2`)
also contains code from the projects below. The paths are those of nRF Connect SDK v3.4.0, relative
to its folder (for example `C:\ncs\v3.4.0`); each one was checked in that release.

| Component | Licence | Text |
| --- | --- | --- |
| OpenSC2 (the library, the firmware, the samples, the template) | MIT | [`LICENSE`](LICENSE) |
| Zephyr RTOS: the kernel, drivers, the USB device stack, CDC-ACM, the input subsystem, the sensor API, the shell, logging, settings, NVS, the nrfx glue, the `promicro_nrf52840` board files (used from Zephyr, not copied here) | Apache-2.0 | `zephyr/LICENSE` |
| nRF Connect SDK code: Enhanced ShockBurst (ESB), the MPSL glue, the CryptoCell entropy driver, the multithreading lock | LicenseRef-Nordic-5-Clause | `nrf/LICENSE` |
| MPSL, Nordic's binary Multiprotocol Service Layer library, which ESB's build links (with its front-end module library) | LicenseRef-Nordic-5-Clause | `nrfxlib/mpsl/license.txt` (the licence tag; its text is `nrfxlib/LICENSE`), and an ARM Ltd BSD-3-Clause notice in `nrfxlib/mpsl/LICENSE-ATTRIBUTION.txt` |
| nrf_cc310_platform, Nordic's binary CryptoCell 310 platform library, which the entropy driver uses for random numbers | LicenseRef-Nordic-5-Clause | `nrfxlib/crypto/nrf_cc310_platform/license.txt` (the licence tag; its text is `nrfxlib/LICENSE`) |
| nrfx drivers and the nRF MDK | BSD-3-Clause | the header of each file, for example `modules/hal/nordic/nrfx/nrfx.h` and `modules/hal/nordic/nrfx/bsp/stable/mdk/system_nrf52840.c` |
| CMSIS 6 core headers | Apache-2.0 | `modules/hal/cmsis_6/LICENSE` |
| picolibc, the C library (from the toolchain) | BSD-style licences, listed file by file | `modules/lib/picolibc/COPYING.picolibc` |
| libgcc, GCC's runtime library (from the toolchain) | GPL-3.0 with the GCC Runtime Library Exception 3.1 | the toolchain; not in the SDK's repositories |

The samples and your own builds may link more of Zephyr and the SDK (sensor and input drivers, PWM,
I2C). Each of those files names its licence in its header (`SPDX-License-Identifier`). The opt-in
builds of `app/` link more too, and the table above and `tools/collect_licenses.py` cover only the
default image: the Bluetooth build (`app/ble.conf`, `app/all.conf`) adds Zephyr's Bluetooth host
(Apache-2.0), Nordic's SoftDevice Controller, a binary library under LicenseRef-Nordic-5-Clause
(`nrfxlib/softdevice_controller/license.txt`), and the SDK's crypto (`nrf_security` with Mbed TLS,
Apache-2.0, and Nordic's binary `nrf_oberon` library, LicenseRef-Nordic-5-Clause,
`nrfxlib/crypto/nrf_oberon/license.txt`). Check those before you pass such a build on.

What the licences ask of copies in binary form, in short (the licence texts decide; this is not
legal advice):

- **Apache-2.0** (Zephyr, CMSIS): section 4(a) asks that recipients get a copy of the licence.
- **BSD-3-Clause** (nrfx, the nRF MDK, the MPSL attribution, much of picolibc): clause 2 asks that
  binaries reproduce the copyright notice, the conditions and the disclaimer in their documentation
  or other materials.
- **LicenseRef-Nordic-5-Clause** (ESB and the other Nordic parts): clause 2 asks the same of
  binaries, except as embedded in a Nordic chip in a product or in a software update for one; clause
  3 forbids using Nordic's name to endorse the product; clause 4 limits use to Nordic Semiconductor
  chips; clause 5 forbids reverse engineering, modifying or disassembling software provided in
  binary form under it (and the other ways its text lists), which MPSL and the CryptoCell library
  are.
- **MIT** (OpenSC2): the copyright notice and the permission notice go with copies of the software.

Every release carries `THIRD_PARTY_LICENSES.txt` next to the `.uf2`: the texts above in one file,
made with `tools/collect_licenses.py` from the SDK the image was built with:

```powershell
python -I tools\collect_licenses.py --ncs C:\ncs\v3.4.0 --out C:\b\rel\THIRD_PARTY_LICENSES.txt
```

The SuperMini boards use the Adafruit nRF52 bootloader (under its own licence, not part of this
image); it is already on the board and is neither modified nor redistributed here.

The names of reports, buttons and settings, and a few enum values and layouts (the charge states
of `EChargeState`, the gyro-mode bits `SETTING_GYRO_MODE_*`, the layout of `MsgHapticRumble`),
follow the declarations of SDL's open-source Steam Controller 2 driver
(`SDL_hidapi_steam_triton.c`, `steam/controller_structs.h`, `steam/controller_constants.h`, zlib
licence). No SDL code is included.

Back to the [documentation map](docs/README.md).
