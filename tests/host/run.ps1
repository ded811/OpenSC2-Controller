<#
.SYNOPSIS
  Build and run the host tests on Windows with a gcc on PATH (for example MSYS2's ucrt64 gcc).
  Two binaries: "plain" (the library as it is built without the hardware test hooks) and "hooks"
  (CONFIG_OPENSC2_TEST_HOOKS, with the hooks' cases). Both run every other case, and both write
  the transcript of the unarmed command channel; the run fails when the two differ by one byte.
  A third binary, the firmware emulator (fwemu.c: the hooks binary with a stdin/stdout main), lets
  the hardware test suite's runner run its groups against the library's own code: when Python is
  on PATH, its self-tests and its dry run against the emulator run last (docs/testing.md), with
  the self-tests of tools\match_steam_version.py.
.EXAMPLE
  From the repository folder. Windows does not run scripts by default; started this way,
  PowerShell runs this one (no administrator rights needed):

  powershell -NoProfile -ExecutionPolicy Bypass -File tests\host\run.ps1
  powershell -NoProfile -ExecutionPolicy Bypass -File tests\host\run.ps1 -Cc C:\msys64\ucrt64\bin\gcc.exe
  powershell -NoProfile -ExecutionPolicy Bypass -File tests\host\run.ps1 -NoRunner   # the C tests only
.NOTES
  Copyright (c) 2026 the OpenSC2-Controller authors
  SPDX-License-Identifier: MIT
#>
param([string]$Cc = 'gcc', [switch]$NoRunner)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$tmp = [System.IO.Path]::GetTempPath()
$lib = Join-Path $root 'lib\opensc2'
$shim = Join-Path $PSScriptRoot 'shim'
$vectors = Join-Path $root 'tests\vectors\hwtest'
$flags = @('-std=c11', '-Wall', '-Wextra', '-Wshadow', '-Werror', "-I$shim", "-I$root\include",
    "-I$lib")
$libSources = @((Join-Path $PSScriptRoot 'stubs.c'),
    "$lib\codec.c", "$lib\lizard.c", "$lib\settings_defaults.c", "$lib\fusion.c",
    "$lib\usb_desc.c", "$lib\usb_rules.c", "$lib\mode_rules.c", "$lib\retained.c",
    "$lib\id_rules.c", "$lib\commands.c", "$lib\report_rules.c", "$lib\test_core.c",
    "$lib\proto_rules.c", "$lib\key_rules.c", "$lib\crash_rules.c")
$sources = @((Join-Path $PSScriptRoot 'run_tests.c')) + $libSources
$hooks = @('-DCONFIG_OPENSC2_TEST_HOOKS=1', '-DCONFIG_OPENSC2_TEST_RING_SIZE=512',
    '-DCONFIG_OPENSC2_TEST_SEQ_DEPTH=4', "$lib\test_notes.c",
    (Join-Path $PSScriptRoot 'hooks_stub.c'))
# the emulator: the Kconfig defaults of the app (a 2 KB ring, 8 sequence steps)
$emu = @('-DOSC2_HOST_FWEMU', '-DCONFIG_OPENSC2_TEST_HOOKS=1',
    '-DCONFIG_OPENSC2_TEST_RING_SIZE=2048', '-DCONFIG_OPENSC2_TEST_SEQ_DEPTH=8',
    "$lib\test_notes.c", (Join-Path $PSScriptRoot 'hooks_stub.c'),
    (Join-Path $PSScriptRoot 'fwemu.c'))

$plain = Join-Path $tmp 'opensc2_host_tests_plain.exe'
$withHooks = Join-Path $tmp 'opensc2_host_tests_hooks.exe'
$fwemu = Join-Path $tmp 'opensc2_fwemu.exe'
$plainLog = Join-Path $tmp 'opensc2_host_transcript_plain.txt'
$hooksLog = Join-Path $tmp 'opensc2_host_transcript_hooks.txt'

& $Cc @flags @sources -lm -o $plain
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $Cc @flags @sources @hooks -lm -o $withHooks
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $Cc @flags @libSources @emu -lm -o $fwemu
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$failed = 0
& $plain --vectors $vectors --transcript $plainLog
if ($LASTEXITCODE -ne 0) { $failed = 1 }
& $withHooks --vectors $vectors --transcript $hooksLog
if ($LASTEXITCODE -ne 0) { $failed = 1 }

# The unarmed command channel, byte for byte: both binaries must have answered alike.
if ((Get-FileHash $plainLog).Hash -ne (Get-FileHash $hooksLog).Hash) {
    Write-Host 'FAIL cmd_differential: the transcripts of the two binaries differ'
    Compare-Object (Get-Content $plainLog) (Get-Content $hooksLog) | Select-Object -First 4 |
        ForEach-Object { Write-Host "  $($_.SideIndicator) $($_.InputObject)" }
    $failed = 1
} else {
    Write-Host "PASS the two binaries' transcripts are the same ($((Get-Item $plainLog).Length) bytes)"
}

# The hardware test suite's runner: its self-tests (with the emulator) and a dry run against the
# emulator, which must pass with nothing failing. No device is opened.
$python = Get-Command python -ErrorAction SilentlyContinue
if ($NoRunner) {
    Write-Host 'SKIP the runner (-NoRunner)'
} elseif (-not $python) {
    Write-Host 'SKIP the runner: no python on PATH'
} else {
    $runPy = Join-Path $root 'tools\hwtest\run.py'
    $results = Join-Path $tmp 'opensc2_hwtest_results'
    $env:OPENSC2_FWEMU = $fwemu
    & python -I $runPy --selftest
    if ($LASTEXITCODE -ne 0) { Write-Host 'FAIL the runner self-tests'; $failed = 1 }
    & python -I $runPy --dry-run --fw-emu $fwemu --lizard --yes --results $results
    if ($LASTEXITCODE -ne 0) { Write-Host 'FAIL the dry run against the emulator'; $failed = 1 }
    Remove-Item Env:\OPENSC2_FWEMU
    & python -I (Join-Path $root 'tools\match_steam_version.py') --selftest
    if ($LASTEXITCODE -ne 0) { Write-Host 'FAIL the match_steam_version.py self-tests'; $failed = 1 }
}
exit $failed
