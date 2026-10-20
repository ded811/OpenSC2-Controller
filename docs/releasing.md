# Releasing

The maintainer's checklist for the first release and for each release after it, step by step: what
to check, build and publish, and in which order.

The commands are for PowerShell on Windows, in the repository folder, with nRF Connect SDK v3.4.0
in `C:\ncs\v3.4.0`.

The first release goes out in three parts, in this order: the pages OpenSC2 cites are checked and
the repository is pushed ([A](#a-check-commit-and-push)), v0.2.0 is released while the repository
is still private ([B](#b-release-v020-while-the-repository-is-private)), and only then does the
repository become public ([C](#c-make-the-repository-public)). So no reader meets a tag that does
not exist yet. The research repository stays private for now: OpenSC2's pages say so where they
link to it, and [D](#d-when-the-research-repository-goes-public) says what to change when it goes
public.

## Versions

| What | Rule |
| --- | --- |
| Release tags | `vX.Y.Z`; the first public release is `v0.2.0`. A GitHub pre-release while the version is 0.x. |
| The revision string Steam reads (`0xAE` tag 3, `0xF2`) | `osc2-X.Y.Z`, at most 12 characters (the build checks): `CONFIG_OPENSC2_GIT_REVISION` in `lib/opensc2/Kconfig` and in `app/prj.conf` |
| bcdDevice and the build time | never the version: they mirror the real controller, and Steam compares the build time |
| The test suite's `suite_version` | its own number (0.1.0), not the release's |
| The release asset | `opensc2-supermini-nrf52840-vX.Y.Z.uf2` (the default `app/` build, test hooks on: the tested configuration), with `SHA256SUMS.txt`, `THIRD_PARTY_LICENSES.txt`, `LICENSE` and `NOTICE.md` |

## A. Check, commit and push

1. **The pages OpenSC2 cites.** Counterpuck is public. The research repository
   `ded811/steam-controller-2-reverse-engineered` stays private for now: the README, `NOTICE.md`,
   `CONTRIBUTING.md`, `docs/protocol.md` and `docs/api.md` link to it and say that it is not public
   yet, and `docs/protocol.md` states the essentials itself, so a reader needs none of those links
   to use OpenSC2. The links stay, so that they work the day it goes public.
   1. **Counterpuck's docs commit with the GET-record fix is pushed**: its `docs/wire-formats.md`
      says that the GET record (record 3) carries one byte that Valve's puck leaves unset, not the
      opcode, as `docs/protocol.md` says. Check the page on GitHub in a private browser window,
      logged out.
   2. **Later, when the research repository goes public**: reword the "not public yet" notes and
      check its links with `tools/check_links.py --research`
      ([D](#d-when-the-research-repository-goes-public)).

2. **Review.** Read the whole diff. Check that every statement about hardware in the documentation
   matches [What works](../README.md#what-works): nothing stale, nothing beyond it. Then search
   the tree for anything private: your user name, your e-mail address, your user folder, Steam IDs,
   serial numbers other than the `OSC...` and `OSB...` development form, Bluetooth addresses, and
   references to private folders. Put your own user name and e-mail address in the first command:

   ```powershell
   git grep --untracked -n -I -i -e 'C:[\\/]Users' -e 'your-user-name' -e 'your-email-address'
   git grep --untracked -n -I -E '7656119[0-9]{10}'
   git grep --untracked -n -I -w -E '[0-9A-Z]{13}' | Select-String -NotMatch 'OSC|OSB|tests/codec/src/cases.h'
   git grep --untracked -n -I -i -E '([0-9a-f]{2}:){5}[0-9a-f]{2}|C34F12ABCDEF|D2A1B2C3D4E5'
   ```

   Expected hits, all made up: the length placeholder `"0000000000000"` of the USB serial string
   in `lib/opensc2/usb.c`; the tests' fake puck and console serials (`PUCK000000001`,
   `FAKEPUCK00001`, `FAKEPUCK00002`, `SIMCONSOLE001`); the masking self-tests' fake user, folder
   and Steam ID in `tools/hwtest/selftest/test_mask.py` (the serial validator's test strings in
   `tests/codec/src/cases.h` are filtered out above); and the made-up Bluetooth addresses
   `C3:4F:12:AB:CD:EF` (also written `C34F12ABCDEF`) and `D2A1B2C3D4E5` of the Bluetooth simulator
   and its self-tests (`tools/hwtest/simble.py`, `mask.py`, `proto/console.py`, `selftest/`; this
   page names them too). Any other address is a real one: mask it.

3. **Stage and commit.**

   ```powershell
   git add -A
   git status --ignored
   git update-index --chmod=+x tests/host/run.sh tools/hwtest/run.py tools/match_steam_version.py tools/check_links.py tools/collect_licenses.py
   git commit
   ```

   `git status --ignored` must show `build/`, `tools/hwtest/results/` and the `__pycache__` folders
   only as ignored, never as staged.

4. **Check a fresh clone** against a stock nRF Connect SDK v3.4.0: clone into an empty folder and
   run everything from the clone. Each build must exit 0, print `UF2 ready`, and log no compiler
   `warning:` and no `CMake Warning` (`build.ps1` itself stops when it finds a `partitions.yml`).

   ```powershell
   git clone C:\src\OpenSC2-Controller C:\src\osc2-check
   cd C:\src\osc2-check
   $projects = 'app', 'app\test-charger.conf', 'app\sensors.conf', 'app\imu-stream.conf',
       'app\expander.conf', 'app\haptics.conf', 'app\ble.conf', 'app\dock.conf',
       'app\battery.conf', 'app\watchdog.conf', 'app\all.conf', 'samples\minimal',
       'samples\buttons_sticks', 'samples\touchpad', 'samples\imu', 'samples\rumble',
       'samples\rgb_led', 'samples\haptics', 'samples\haptics\drv2605.conf', 'template'
   foreach ($p in $projects) {
       $out = 'C:\b\chk-' + (($p -replace '\.conf$', '') -replace '[\\/]', '-')
       powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Project $p -OutDir $out -Pristine 2>&1 |
           Tee-Object "$out.log" | Out-Null
       "{0}: exit {1}" -f $p, $LASTEXITCODE
       Select-String -Path "$out.log" -Pattern 'warning:', 'CMake Warning', 'UF2 ready'
   }
   powershell -NoProfile -ExecutionPolicy Bypass -File tests\host\run.ps1
   python -I tools\hwtest\run.py --selftest
   python -I tools\hwtest\run.py --dry-run
   python -I tools\check_links.py
   ```

5. **Push `main`.** Creating the GitHub repository is done: `ded811/OpenSC2-Controller` exists as
   a private repository, `origin` points to it and `main` is on it. Push the commit of step 3:

   ```powershell
   git push origin main
   ```

   The repository remains private until v0.2.0 is released ([C](#c-make-the-repository-public)): the
   documentation pins the tag `v0.2.0` (`docs/integration.md`, `template/west.yml`) and the
   CHANGELOG links to it, so the tag must exist before anyone can read them. Check its description
   and topics in its settings:
   - Description: "Make an nRF52840 board act as a Steam Controller 2: Zephyr module for nRF
     Connect SDK v3.4.0, SuperMini firmware, template, samples, hardware test suite. Unofficial."
   - Topics: `zephyr`, `nrf52840`, `nrf-connect-sdk`, `steam-controller`, `game-controller`,
     `enhanced-shockburst`, `usb-hid`, `supermini`.

## B. Release v0.2.0 (while the repository is private)

For the first release the repository is still private during these steps; for later releases it
is public, and the same steps apply.

1. **Prepare the commit.** Check that the date of the release in `CHANGELOG.md` is the day you tag
   (change it if not) and that `lib/opensc2/Kconfig` and `app/prj.conf` give the revision string
   `osc2-0.2.0`, and commit. The tag and the asset both come from this commit.
2. **Pin the build time.** A build reports the time it was configured, unless the build pins it.
   Use the release commit's time, which CMake's `string(TIMESTAMP)` takes from
   `SOURCE_DATE_EPOCH`:

   ```powershell
   $env:SOURCE_DATE_EPOCH = git log -1 --format=%ct
   '0x{0:X8}' -f [int64]$env:SOURCE_DATE_EPOCH
   ```

   (Or set `CONFIG_OPENSC2_BUILD_TIME` to a fixed value.) The value must be at least Steam's current
   controller firmware (`0x6AC686B3` at Steam's October 2026 release) and above `0x6A18D057`, at or
   before which Steam forces an update.
3. **Build the asset**, in the same PowerShell window:

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Project app -OutDir C:\b\rel -Pristine
   New-Item -ItemType Directory -Force C:\b\rel-assets | Out-Null
   Copy-Item C:\b\rel\app\zephyr\zephyr.uf2 C:\b\rel-assets\opensc2-supermini-nrf52840-v0.2.0.uf2
   Select-String -Path C:\b\rel\app\zephyr\zephyr.bin -SimpleMatch -Pattern 'Users', 'your-user-name'
   ```

   The search must find nothing. It reads `zephyr.bin`, the same image as one piece (a `.uf2`
   splits it into blocks, so a string could fall across two). Never attach a `.elf` or `.map` file:
   they contain your local folders.
4. **Check that exact file on hardware.** Flash it on a SuperMini and check: it enumerates as
   `28DE:1302`; Steam pairs it over USB-C; it reboots wireless and connects through the puck; Steam
   shows no update prompt; `opensc2 id` shows the pinned build time. Then run the suite with every
   opt-in, Steam closed:

   ```powershell
   python -I tools\hwtest\run.py --through-puck --flash --lizard --with-steam --steam-control
   ```

   Keep the masked `report.md`. Then bring the documents that name the tested build up to date:
   the table in [the results so far](testing.md#results-so-far) (the date, the revision
   `osc2-0.2.0`), and the sentences that say the results come from development builds of `app/`
   for 0.2.0 (`README.md`, What works and Testing; `CHANGELOG.md`; `app/README.md`;
   `template/README.md`; `docs/integration.md`), which then name this release's image. If any
   result differs, say so there too. A documentation commit after the tag is fine; a firmware
   change means starting this section again.
5. **The other assets**, into the same folder:

   ```powershell
   python -I tools\collect_licenses.py --ncs C:\ncs\v3.4.0 --out C:\b\rel-assets\THIRD_PARTY_LICENSES.txt
   Copy-Item LICENSE, NOTICE.md C:\b\rel-assets
   cd C:\b\rel-assets
   $sums = Get-ChildItem -File | Where-Object Name -ne 'SHA256SUMS.txt' |
       ForEach-Object { '{0}  {1}' -f (Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLower(), $_.Name }
   [IO.File]::WriteAllText("$PWD\SHA256SUMS.txt", ($sums -join "`n") + "`n")
   ```

6. **Tag and publish.** Tag the commit of step 1, then push it and the tag:

   ```powershell
   git tag -a v0.2.0 -m "OpenSC2 v0.2.0"
   git push origin main
   git push origin v0.2.0
   ```

   On GitHub, draft a release from the tag, attach the five files, paste the notes (below), mark it
   as a pre-release and publish it. Then download the `.uf2` again and compare its SHA-256 with
   `SHA256SUMS.txt`.

## C. Make the repository public

Once, after the first release.

1. **Check that the release is there.** `git ls-remote --tags origin v0.2.0` prints the tag, and
   the release page lists the five files.
2. **Make the repository public**: in its settings, General, Danger Zone, Change repository
   visibility. Then, under its security settings, enable private vulnerability reporting
   ([SECURITY.md](../SECURITY.md) points to it).
3. **Look at it as a stranger would**, in a private browser window, logged out: the README,
   `docs/protocol.md` (it must read complete without the research repository, whose links do not
   open while it is private), and the release with its `.uf2`.
4. **Optional:** try the west workspace way once, with `template/west.yml`
   ([integration.md](integration.md#way-3-a-west-workspace)), and then remove its "Not built this
   way yet" note from `docs/integration.md`.
5. **Cross-link.** Add a one-line link to OpenSC2 in Counterpuck's README (the research
   repository's link comes in [D](#d-when-the-research-repository-goes-public)).

## D. When the research repository goes public

Later, once `ded811/steam-controller-2-reverse-engineered` is public, after its own privacy and
provenance check:

1. **Optional: complete its documentation** with what OpenSC2's pages say is not in it yet: the
   form of the report descriptor's vendor items and the order of its reports (its collection sizes
   are already there, on the puck's USB page). The ranges of settings 9, 11, 12, 14, 45, 70 and 71
   and lizard mode's key-slot rule, its scales and when its mouse reports go out, which also first
   came from the same authors' study, are in it now.
2. **Reword the notes that say it is not public.** This lists every one:

   ```powershell
   git grep -n -i -E 'not public yet|to be published|once (it is|they are) public|stays private|while it is private'
   ```

   Reword each for a public repository and keep every link: "to be published in ...; not public
   yet" becomes "published in ...", the notes that its links do not open yet (in `README.md` and
   `docs/protocol.md`) go, and so does "not public yet" in the comments of the code and the tests.
   The list includes this page: bring its introduction, step A.1 and step C.3 up to date too. If
   you did step 1, also reword the sentences that say those facts are not in that documentation,
   with links to where they now are (in `README.md`, `NOTICE.md`, `CONTRIBUTING.md`,
   `docs/protocol.md`, `include/opensc2/codec.h`, `lib/opensc2/usb_desc.c` and
   `tests/codec/src/cases_usb.h`).
3. **Check every research link** against a fresh clone of the published repository, not against
   your working tree:

   ```powershell
   git clone https://github.com/ded811/steam-controller-2-reverse-engineered C:\src\research-check
   python -I tools\check_links.py --research C:\src\research-check
   ```

   Then, in a private browser window, logged out, open a research link from `docs/protocol.md`:
   the page must open at its section.
4. **Commit and push** the change of step 2, as a commit of its own (`git push origin main`).
5. **Cross-link.** Add a one-line link to OpenSC2 in the research repository's `docs/README.md`.

## Release notes template

Before you paste it, check its numbers against the runs of step B.4.

````markdown
OpenSC2 makes an nRF52840 board act as a Steam Controller 2. Steam pairs it with Valve's puck over a
USB-C cable, like a real controller, and it then connects through the puck. It is a library (a
Zephyr module for nRF Connect SDK v3.4.0), a ready-to-build firmware for the SuperMini nRF52840, a
template for your own project, seven samples and an automated hardware test suite.

> Unofficial: not affiliated with or endorsed by Valve. Steam and Steam Controller are trademarks of
> Valve Corporation. So that Steam recognises it, the firmware uses Valve's USB IDs (`28DE:1302`)
> and the real controller's USB strings, for personal interoperability and research use. It
> contains no Valve code or binaries. See NOTICE.md.

## What works

This release's `.uf2` was tested on a SuperMini nRF52840 with nothing wired to it, with Valve's
puck on its October 2026 firmware and Steam on a Windows PC.

- It enumerates over USB exactly like a real Steam Controller 2 (`28DE:1302`).
- Steam pairs it with Valve's puck over the USB-C cable, like a real controller.
- It then reboots into wireless mode and connects through the puck.
- Steam shows no "needs an update" prompt for a freshly flashed board: it reports a build time newer
  than Steam's current controller firmware.
- The automated hardware test suite passed every test with every opt-in on: 165 of 165 in a run
  started wireless, and 31 of 31 for the `modes` and `cmd` groups in a run started wired. It covers
  inputs injected and checked wired and through the puck, battery, rumble and haptics, LEDs,
  settings, all vendor commands Steam sends, lizard mode with nothing reaching the PC, reboots, mode
  switches, reconnects, and a run with Steam open.
- The charger test aid (a separate test build, not this `.uf2`): on a plain USB wall charger it
  switches to wireless and connects through the puck.

## Not proven yet

- Built, not yet tried with real parts: real buttons, sticks, trackpads, IMU, rumble motor, RGB LED
  and battery hardware (the drivers in `app/` and the samples have never run on such parts).
- Built, not run on hardware: the samples and the template.
- Built, not yet tested on hardware: everything new since 0.2.0 that this release's run did not
  cover (CHANGELOG.md, [Unreleased] until the release): the system state machine (power, chords,
  the status LED's patterns, cues), calibration and raw inputs, lizard mode's processing and the
  rules on every state report, crash records, and as opt-in builds Bluetooth LE, the haptics
  synthesiser, the dock's UART, the battery build and the watchdog.
- Built, untested with Steam: pairing over the air (the puck's pairing window).
- Not tested: other boards; Linux and macOS as the PC (the PC side was tested with Steam on
  Windows).
- Not implemented, or different from the real controller: docs/protocol.md lists every gap.

## What you need

- The SuperMini nRF52840, a Pro Micro-sized nRF52840 board with the nice!nano pinout and the
  Adafruit nRF52 UF2 bootloader. Nothing needs to be wired to try it. It has three analog-capable
  pins, so the default firmware carries one analog stick.
- Valve's puck (tested with its October 2026 firmware), a Windows PC with Steam, and a USB-C data
  cable.

## Flash it

Short RST to GND twice quickly: a USB drive appears. Copy `opensc2-supermini-nrf52840-v0.2.0.uf2`
onto it; the board restarts with the new firmware. Check the download first: on Windows,
`Get-FileHash -Algorithm SHA256 opensc2-supermini-nrf52840-v0.2.0.uf2` must print the hash in
`SHA256SUMS.txt`; on Linux, `sha256sum -c --ignore-missing SHA256SUMS.txt` must say `OK`.

## Add it in Steam

1. Plug the puck into the PC and start Steam. Switch a real Steam Controller 2 off and keep it
   unplugged while you add the board.
2. In Steam, start adding a controller. Steam asks you to connect it with a USB-C cable.
3. Plug the board in with a USB-C data cable (not a charge-only cable). It comes up wired: the LED
   turns on once the PC has set the board up.
4. Steam pairs the board with the puck over the cable and reboots it into wireless mode: the LED
   goes dark, blinks slowly while the board looks for the puck, then stays on. The board is now
   connected through the puck.
5. From then on the board connects through the puck by itself whenever it starts wireless. Plugged
   into a PC again it comes up wired; hold the pairing button (on a bare board: P0.31 against GND)
   while plugging in to keep it wireless.

## The test hooks

The default `app/` build has the hardware test hooks on (`CONFIG_OPENSC2_TEST_HOOKS=y`), so the test
suite checks the very image you run. They do nothing until the suite arms them; arming lives in RAM
and ends at every reboot, or 15 s after the PC goes quiet. The one exception is a boot-time
injection of the system state machine the armed suite stored for the next boot: a reset before
the arming ends makes that boot run on the injected power source, unarmed (SECURITY.md). The key
that arms them is published with the code: it prevents accidents, it is not a lock. For a board you
give to someone else, build with `CONFIG_OPENSC2_TEST_HOOKS=n`.

## Steam's update prompt

This image reports its own build time. If Steam later ships a newer controller firmware and offers
the board an update, declining is always safe; `tools/match_steam_version.py` gives the board
Steam's value (docs/troubleshooting.md, "Steam asks to update the controller").

## Problems and reports

Use the issue forms: a bug, a hardware report, or a new board. Mask serials before you post
(docs/troubleshooting.md, "What can I share in an issue?").
````

Back to the [documentation map](README.md).
