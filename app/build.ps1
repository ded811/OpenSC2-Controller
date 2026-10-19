# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT

<#
.SYNOPSIS
  Build the OpenSC2 firmware in this folder (app/, the blank canvas) for the SuperMini nRF52840.
.DESCRIPTION
  A shortcut for ..\build.ps1 -Project app (or -Project app\test-charger.conf with -TestCharger).
  The result is a .uf2 to copy onto the board's USB drive:
    with -OutDir <folder>:  <folder>\app\zephyr\zephyr.uf2
    without -OutDir:        <repository>\build\app\app\zephyr\zephyr.uf2
                            (-TestCharger: <repository>\build\app_test-charger\app\zephyr\zephyr.uf2)
  Needs nRF Connect SDK v3.4.0 (README.md, "Build and flash").
.PARAMETER OutDir
  The build folder. Keep it short on Windows (for example C:\b\osc2): the 260-character path limit
  breaks deep build folders.
.PARAMETER Pristine
  Start the build folder over.
.PARAMETER TestCharger
  Build the charger TEST build instead (test-charger.conf on top of prj.conf): a testing aid for a
  board without buttons, not the real controller's behaviour and not part of any release
  (README.md, "Test build: the charger fallback"). Give it a build folder of its own.
.PARAMETER Board
  The Zephyr board target. Default: promicro_nrf52840/nrf52840/uf2 (the SuperMini). The overlay in
  boards/ is for the SuperMini only; another board needs its own (docs/porting.md).
.PARAMETER NcsRoot
  The folder nRF Connect SDK v3.4.0 is installed in. Default: C:\ncs.
.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -OutDir C:\b\osc2 -Pristine

  In this folder. Windows does not run scripts by default; started this way, PowerShell runs this
  one (no administrator rights needed).
.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -TestCharger -OutDir C:\b\osc2-chg -Pristine
.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -OutDir C:\b\osc2 -NcsRoot D:\ncs
#>
param(
    [string]$OutDir,
    [switch]$Pristine,
    [switch]$TestCharger,
    [string]$Board,
    [string]$NcsRoot
)
$args2 = @{ Project = 'app' }
if ($TestCharger) { $args2.Project = 'app\test-charger.conf' }
if ($OutDir) { $args2.OutDir = $OutDir }
if ($Pristine) { $args2.Pristine = $true }
if ($Board) { $args2.Board = $Board }
if ($NcsRoot) { $args2.NcsRoot = $NcsRoot }
& (Join-Path $PSScriptRoot '..\build.ps1') @args2
exit $LASTEXITCODE
