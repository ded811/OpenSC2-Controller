# Releasing

The maintainer's checklist for the first push and for each release, step by step: what to check,
build and publish, and in which order.

The commands are for PowerShell on Windows, in the repository folder, with nRF Connect SDK v3.4.0
in `C:\ncs\v3.4.0`.

The first release goes out in three parts, in this order: the pages OpenSC2 cites are published
([A](#a-before-the-first-push)), the repository is pushed and v0.2.0 released while the repository
is still private ([B](#b-release-v020-while-the-repository-is-private)), and only then does the
repository become public ([C](#c-make-the-repository-public)). So no reader meets a link to a page
that is not there, or a tag that does not exist yet.

## Versions

| What | Rule |
| --- | --- |
| Release tags | `vX.Y.Z`; the first public release is `v0.2.0`. A GitHub pre-release while the version is 0.x. |
| The revision string Steam reads (`0xAE` tag 3, `0xF2`) | `osc2-X.Y.Z`, at most 12 characters (the build checks): `CONFIG_OPENSC2_GIT_REVISION` in `lib/opensc2/Kconfig` and in `app/prj.conf` |
| bcdDevice and the build time | never the version: they mirror the real controller, and Steam compares the build time |
| The test suite's `suite_version` | its own number (0.1.0), not the release's |
| The release asset | `opensc2-supermini-nrf52840-vX.Y.Z.uf2` (the default `app/` build, test hooks on: the tested configuration), with `SHA256SUMS.txt`, `THIRD_PARTY_LICENSES.txt`, `LICENSE` and `NOTICE.md` |

## A. Before the first push

1. **Publish the sources first.** The README, `NOTICE.md`, `CONTRIBUTING.md`, `docs/protocol.md`,
   `docs/api.md` and the issue forms link to the research repository, and OpenSC2's provenance
   rests on it ("the protocol documentation the same authors published"). Before anything else:
   1. **The research repository `ded811/steam-controller-2-reverse-engineered` is public**, after
      its own privacy and provenance check. If it has to stay private, OpenSC2 cannot go out as it
      is: its research links and its provenance sentences would have to change first.
   2. **The research repository's pending docs update is committed and pushed**, all five files of
      it: `docs/controller/03-usb-and-hid.md`, `docs/controller/04-radio.md`,
      `docs/controller/09-system.md`, `docs/firmware-changelog.md` and `docs/puck/04-radio.md`.
      OpenSC2 cites what it adds to the first: the sections "The wired session as Steam runs it",
      "Pairing a controller with a puck over the cable" and "Reboot into wireless mode", the
      October device class `00/00/00` next to July's `EF/02/01`, and the release reports when
      lizard mode turns off.
   3. **Counterpuck's `docs/wire-formats.md` fix is committed and pushed**: the GET record (record
      3) carries one byte that Valve's puck leaves unset, not the opcode, as `docs/protocol.md`
      says.
   4. **Check them as a stranger would.** In a private browser window, logged out, open
      <https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#the-wired-session-as-steam-runs-it>:
      the page must open at that section. Then check every research link against a fresh clone of
      the published repository, not against your working tree:

      ```powershell
      git clone https://github.com/ded811/steam-controller-2-reverse-engineered C:\src\research-check
      python -I tools\check_links.py --research C:\src\research-check
      ```

   5. **Optional: publish what the docs call "not yet on the published pages"**: the ranges of
      settings 9, 11, 12, 14, 45, 70 and 71; the report descriptor's collection sizes (67 and 41
      bytes), the form of its vendor items and the order of its reports; lizard mode's key-slot
      rule, its scales and when its mouse reports go out. Then turn that phrase into links (in
      `README.md`, `NOTICE.md`, `CONTRIBUTING.md`, `docs/protocol.md`, `docs/api.md`,
      `include/opensc2/settings.h`, `include/opensc2/codec.h`, `lib/opensc2/settings_defaults.c`,
      `lib/opensc2/usb_desc.c`, `lib/opensc2/lizard.c` and `tests/codec/src/cases_usb.h`) and run
      the check of step 4 again.

2. **Review.** Read the whole diff. Check that every statement about hardware in the documentation
   matches [What works](../README.md#what-works): nothing stale, nothing beyond it. Then search
   the tree for anything private: your user name, your e-mail address, your user folder, Steam IDs,
   serial numbers other than the `OSC...` and `OSB...` development form, and references to private
   folders. Put your own user name and e-mail address in the first command:

   ```powershell
   git grep --untracked -n -I -i -e 'C:[\\/]Users' -e 'your-user-name' -e 'your-email-address'
   git grep --untracked -n -I -E '7656119[0-9]{10}'
   git grep --untracked -n -I -w -E '[0-9A-Z]{13}' | Select-String -NotMatch 'OSC|OSB|tests/codec/src/cases.h'
   ```

   Expected hits, all made up: the length placeholder `"0000000000000"` of the USB serial string
   in `lib/opensc2/usb.c`; the tests' fake puck and console serials (`PUCK000000001`,
   `FAKEPUCK00001`, `FAKEPUCK00002`, `SIMCONSOLE001`); and the masking self-tests' fake user, folder
   and Steam ID in `tools/hwtest/selftest/test_mask.py` (the serial validator's test strings in
   `tests/codec/src/cases.h` are filtered out above).

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
   $projects = 'app', 'app\test-charger.conf', 'samples\minimal', 'samples\buttons_sticks',
       'samples\touchpad', 'samples\imu', 'samples\rumble', 'samples\rgb_led', 'template'
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

5. **Create the GitHub repository `ded811/OpenSC2-Controller` as a private repository.** It stays
   private until v0.2.0 is released ([C](#c-make-the-repository-public)): the documentation pins
   the tag `v0.2.0` (`docs/integration.md`, `template/west.yml`) and the CHANGELOG links to it, so
   the tag must exist before anyone can read them.
   - Description: "Make an nRF52840 board act as a Steam Controller 2: Zephyr module for nRF
     Connect SDK v3.4.0, SuperMini firmware, template, samples, hardware test suite. Unofficial."
   - Topics: `zephyr`, `nrf52840`, `nrf-connect-sdk`, `steam-controller`, `game-controller`,
     `enhanced-shockburst`, `usb-hid`, `supermini`.
   - Push `main`:

     ```powershell
     git remote add origin https://github.com/ded811/OpenSC2-Controller
     git push -u origin main
     ```

## B. Release v0.2.0 (while the repository is private)

For the first release the repository is still private during these steps; for later releases it
is public, and the same steps apply.

1. **Prepare the commit.** Set the date of the release in `CHANGELOG.md`, check that
   `lib/opensc2/Kconfig` and `app/prj.conf` give the revision string `osc2-0.2.0`, and commit. The
   tag and the asset both come from this commit.
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
3. **Look at it as a stranger would**, in a private browser window, logged out: the README, a research
   link from `docs/protocol.md`, and the release with its `.uf2`.
4. **Optional:** try the west workspace way once, with `template/west.yml`
   ([integration.md](integration.md#way-3-a-west-workspace)), and then remove its "Not built this
   way yet" note from `docs/integration.md`.
5. **Cross-links.** Add a one-line link to OpenSC2 in Counterpuck's README and in the research
   repository's `docs/README.md`.

## Release notes template

Before you paste it, check its numbers (169 of 169) against the run of step B.4.

````markdown
OpenSC2 makes an nRF52840 board act as a Steam Controller 2. Steam pairs it with Valve's puck over a
USB-C cable, like a real controller, and it then connects through the puck. It is a library (a
Zephyr module for nRF Connect SDK v3.4.0), a ready-to-build firmware for the SuperMini nRF52840, a
template for your own project, six samples and an automated hardware test suite.

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
- Steam's "needs an update" prompt goes away when the board reports a build time at least as new as
  Steam's current controller firmware.
- The automated hardware test suite passed 169 of 169 tests with every opt-in on: inputs injected
  and checked wired and through the puck, battery, rumble and haptics, LEDs, settings, all vendor
  commands Steam sends, lizard mode with nothing reaching the PC, reboots, mode switches,
  reconnects, and a run with Steam open.

## Not proven yet

- Built, not yet tried with real parts: real buttons, sticks, trackpads, IMU, rumble motor, RGB LED
  and battery hardware (the drivers in `app/` and the samples have never run on such parts); the
  charger test aid on a real charger.
- Built, not run on hardware: the samples and the template.
- Built, untested with Steam: pairing over the air (the puck's pairing window).
- Not tested: other boards; Linux and macOS as the PC (the PC side was tested with Steam on
  Windows).
- Not implemented: Bluetooth; the dock (pogo pads); lizard mode's smoothing, glide and haptic
  ticks; PCM haptics playback in the samples.

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
   is dark for half a second, then on with a short dark blink every 2 seconds.
4. Steam pairs the board with the puck over the cable and reboots it into wireless mode: the LED
   goes dark, blinks slowly while the board looks for the puck, then stays on. The board is now
   connected through the puck.
5. From then on the board connects through the puck by itself whenever it starts wireless. Plugged
   into a PC again it comes up wired; hold the pairing button (on a bare board: P0.31 against GND)
   while plugging in to keep it wireless.

## The test hooks

The default `app/` build has the hardware test hooks on (`CONFIG_OPENSC2_TEST_HOOKS=y`), so the test
suite checks the very image you run. They do nothing until the suite arms them; arming lives in RAM
and ends at every reboot, or 15 s after the PC goes quiet. The key that arms them is published with
the code: it prevents accidents, it is not a lock. For a board you give to someone else, build with
`CONFIG_OPENSC2_TEST_HOOKS=n`.

## Steam's update prompt

This image reports its own build time. If Steam later ships a newer controller firmware and offers
the board an update, declining is always safe; `tools/match_steam_version.py` gives the board
Steam's value (docs/troubleshooting.md, "Steam asks to update the controller").

## Problems and reports

Use the issue forms: a bug, a hardware report, or a new board. Mask serials before you post
(docs/troubleshooting.md, "What can I share in an issue?").
````

Back to the [documentation map](README.md).
