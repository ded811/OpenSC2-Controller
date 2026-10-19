## What this changes

<!-- One or two sentences. -->

## Checklist

- [ ] The host tests pass (`tests\host\run.ps1` or `sh tests/host/run.sh`), and so do
      `python -I tools/hwtest/run.py --selftest`, `--dry-run` and `python -I tools/check_links.py`.
- [ ] Every build I touched builds without compiler warnings and leaves no `partitions.yml`.
- [ ] The documentation and `CHANGELOG.md` (under "Unreleased") say what changed.
- [ ] Provenance: I wrote this myself, from public documentation and black-box tests; it contains
      no code, firmware, binaries or captures from Valve, and nothing from Valve's firmware or a
      reconstruction of it.
- [ ] No identifying data: no serials, bond IDs, puck serials, Steam IDs, user folders or unmasked
      Steam log lines.

## Hardware it ran on

<!-- The board, the parts, the puck firmware, the Steam version, and which tests ran. "None" is
fine: say so. -->
