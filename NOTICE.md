# Notice

OpenSC2-Controller is an unofficial project. It is not affiliated with, endorsed by or supported by
Valve Corporation. Steam and Steam Controller are trademarks of Valve Corporation. They are used
here only to say what this project works with.

So that Steam recognises it, the firmware presents Valve's USB vendor and product IDs (`28DE:1302`)
and the real controller's USB strings ("Valve Software", "Steam Controller"), and it answers
Steam's commands the way a real Steam Controller 2 does. This is for personal interoperability and
research use.

In the same way, Bluetooth LE (`CONFIG_OPENSC2_BLE`, in the default `app/` build) presents itself
as the real controller does over Bluetooth, so that hosts that look for a Steam Controller 2
(Windows, SDL's Android and Apple back ends, Steam) find it: the name "Steam Ctrl (BT) " followed
by the board's serial, Valve's Bluetooth company ID (`0x055D`) in the advertised manufacturer data, the PnP ID
`28DE:1303` with the model "Steam Controller" in its Device Information Service, and the UUIDs of
the real controller's vendor GATT service (`100F6Cxx-1735-4313-B402-38567131E5F3`), which SDL's
Android and Apple back ends look for. The first three are Kconfig options
(`CONFIG_OPENSC2_BLE_NAME_PREFIX`, `_COMPANY_ID`, `_PNP_VENDOR`, `_PNP_PRODUCT`, `_MODEL`) that you
can change, and the vendor service can be left out (`CONFIG_OPENSC2_BLE_VENDOR_SERVICE=n`).

OpenSC2 is original code. It was written from the protocol documentation the same authors wrote by
studying the controller's and the puck's firmware (to be published in
[steam-controller-2-reverse-engineered](https://github.com/ded811/steam-controller-2-reverse-engineered);
not public yet), plus black-box tests and USB captures of Steam with a real controller. A few facts
first came directly from the same authors' study of the controller's firmware rather than from
that documentation: details of the report descriptor, which it does not give in full yet, and the
ranges of seven settings and lizard mode's key-slot rule, its scales and when its mouse reports go
out, which it has now ([docs/protocol.md](docs/protocol.md#where-this-library-had-to-fill-gaps)
lists each). It is not a clean-room implementation. It contains no Valve code or binaries.

The haptics synthesiser (`CONFIG_OPENSC2_HAPTICS_SYNTH`) plays sounds of OpenSC2's own: its two
clicks, built-in scripts 1 to 17, the cues and the two drop sounds are made by its own code and
`tools/gen_haptics_tables.py`. None of the real controller's recordings, click samples or script
step data are in this repository; what follows the real controller is how the output reports are
played (their formats, timings and gains), as the protocol documentation describes it.

In wireless mode the board's debug serial port uses other USB IDs: `2FE3:0004` by default. `2FE3`
is the Zephyr Project's vendor ID, which Zephyr uses for its own samples and asks products not to
use; `0004` is the ID of Zephyr's USB console sample. Change them with
`CONFIG_OPENSC2_USB_CONSOLE_VID` and `CONFIG_OPENSC2_USB_CONSOLE_PID` if you need to. OpenSC2's PC
tools (`tools/hwtest`, `tools/match_steam_version.py`) only talk to a console with the default IDs
and the product string "OpenSC2 console".
