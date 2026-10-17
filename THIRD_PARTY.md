# Third-party components

The source in this repository is MIT licensed (see [LICENSE](LICENSE)). A built firmware
(`zephyr.uf2`) also contains code from the projects below, under their own licences. Their licence
texts ship with the SDK you build with (nRF Connect SDK v3.4.0: `zephyr/LICENSE`, `nrf/LICENSE`,
`modules/hal/nordic/LICENSE`).

| Component | Licence | Where it comes from |
| --- | --- | --- |
| Zephyr RTOS (kernel, drivers, USB device stack, CDC-ACM, input subsystem, sensor API, shell, logging, settings, NVS) | Apache-2.0 | the SDK |
| nRF Connect SDK glue, MPSL and the Enhanced ShockBurst (ESB) radio library | Nordic 5-clause (`LicenseRef-Nordic-5-Clause`): use is limited to Nordic Semiconductor devices, and the licence text must come with binaries | the SDK |
| nrfx peripheral drivers and the nRF MDK | BSD-3-Clause | the SDK |
| picolibc (C library) | BSD-3-Clause | the Zephyr SDK toolchain |
| The `promicro_nrf52840` board definition | Apache-2.0 | Zephyr (used as is, not copied here) |

Anyone who redistributes a built image must pass on the Nordic 5-clause licence text with it. That
licence also limits the ESB part of a binary to Nordic chips and forbids reverse engineering that
part.

The SuperMini boards use the Adafruit nRF52 bootloader (under its own licence, not part of this
image); it is already on the board and is neither modified nor redistributed here.

Names of reports, buttons and settings follow SDL's open-source Steam Controller 2 driver
(`SDL_hidapi_steam_triton.c`, `steam/controller_structs.h`, `steam/controller_constants.h`, zlib
licence). No SDL code is included.
