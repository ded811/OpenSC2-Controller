# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT

<#
.SYNOPSIS
  Build an OpenSC2 project into a .uf2 for the SuperMini nRF52840 (Zephyr board target
  promicro_nrf52840/nrf52840/uf2, Adafruit nRF52 UF2 bootloader): the firmware (app, the default),
  the template, a sample, or a project of your own.
.DESCRIPTION
  Needs an unmodified nRF Connect SDK v3.4.0. Install nRF Connect SDK v3.4.0 with nRF Connect for
  VS Code (Install SDK, then v3.4.0), or on the command line: nrfutil install sdk-manager, then
  nrfutil sdk-manager install v3.4.0. build.ps1 looks in C:\ncs; pass -NcsRoot <folder> for another
  one. ncs_env.ps1 loads the SDK's toolchain for the build.

  -Project is any folder with a CMakeLists.txt: a folder of this repository (app, template,
  samples\minimal, ...) or an absolute path, also to a folder outside the repository, such as your
  copy of template. A project outside the repository finds the library through
  ZEPHYR_EXTRA_MODULES or a modules\opensc2 folder (docs\integration.md).

  -Project may also name a Kconfig fragment inside a project folder: that folder is built with the
  fragment on top (Zephyr's EXTRA_CONF_FILE). app\test-charger.conf is such a fragment: the
  charger TEST build, a testing aid that is not the real controller's behaviour (app\README.md).

  The result is <OutDir>\<name of the project folder>\zephyr\zephyr.uf2 (sysbuild names the image
  after the folder). Without -OutDir the build folder is build\<Project with \ turned into _> in
  this repository: app gives build\app\app\zephyr\zephyr.uf2. Keep -OutDir short (C:\b\osc2):
  Windows' 260-character path limit breaks deep build folders.

  To flash: short RST to GND twice quickly: a USB drive appears. Copy zephyr.uf2 onto it; the
  board restarts with the new firmware.
.PARAMETER Project
  The project folder (or a Kconfig fragment in one), relative to this repository or absolute.
  Default: app.
.PARAMETER Board
  The Zephyr board target. Default: promicro_nrf52840/nrf52840/uf2 (the SuperMini). Other boards
  need their own overlay (docs\porting.md) and are untested.
.PARAMETER OutDir
  The build folder. Default: build\<Project> in this repository.
.PARAMETER NcsRoot
  The folder that holds the nRF Connect SDK's v3.4.0 and toolchains folders. Default: C:\ncs.
.PARAMETER Pristine
  Start the build folder over (west build -p always).
.EXAMPLE
  Windows does not run scripts by default; started this way, PowerShell runs this one (no
  administrator rights needed). In the repository folder:

  powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
      # app -> build\app\app\zephyr\zephyr.uf2
  powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Project app -OutDir C:\b\osc2 -Pristine
      # -> C:\b\osc2\app\zephyr\zephyr.uf2
  powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Project template -OutDir C:\b\tpl
      # -> C:\b\tpl\template\zephyr\zephyr.uf2
  powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Project samples\minimal -OutDir C:\b\osc2-min
      # -> C:\b\osc2-min\minimal\zephyr\zephyr.uf2
  powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Project C:\src\my-controller -OutDir C:\b\mine
      # -> C:\b\mine\my-controller\zephyr\zephyr.uf2
  powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Project app\test-charger.conf -OutDir C:\b\osc2-chg -Pristine
      # the charger test build
#>
param(
    [string]$Project = 'app',
    [string]$Board = 'promicro_nrf52840/nrf52840/uf2',
    [string]$OutDir,
    [string]$NcsRoot = 'C:\ncs',
    [switch]$Pristine
)
$ErrorActionPreference = 'Stop'
$src = if ([System.IO.Path]::IsPathRooted($Project)) { $Project } else { Join-Path $PSScriptRoot $Project }
$src = (Resolve-Path $src).Path
$extraConf = ''
if ((Test-Path $src -PathType Leaf) -and $src.EndsWith('.conf')) {
    # A Kconfig fragment: its folder is the project, the fragment goes on top of prj.conf. Passed
    # by its name (Zephyr reads it from the application folder), so a path with spaces is no issue.
    $extraConf = Split-Path $src -Leaf
    $src = Split-Path $src -Parent
    if (-not (Test-Path (Join-Path $src 'CMakeLists.txt'))) {
        throw "$extraConf must be in a project folder (one with a CMakeLists.txt)."
    }
    Write-Host "TEST BUILD: $(Split-Path $src -Leaf) with $extraConf on top (not the default firmware)" -ForegroundColor Yellow
}
if (-not $OutDir) {
    $OutDir = Join-Path $PSScriptRoot ('build\' + ((($Project -replace '\.conf$', '') -replace '[\\/:]+', '_').Trim('_')))
}
. (Join-Path $PSScriptRoot 'ncs_env.ps1') -NcsRoot $NcsRoot -NcsVersion 'v3.4.0'

$p = if ($Pristine) { 'always' } else { 'auto' }
# The build folder's CMake cache keeps the fragment of its last configuration: a default build
# never reuses a test build's folder as it is (nor the other way round), it starts over.
$cache = Join-Path $OutDir 'CMakeCache.txt'
if ($p -ne 'always' -and (Test-Path $cache)) {
    $hit = Select-String -Path $cache -Pattern '^EXTRA_CONF_FILE:[A-Z]+=(.*)$' | Select-Object -First 1
    $was = if ($hit) { $hit.Matches[0].Groups[1].Value } else { '' }
    if ($was -ne $extraConf) {
        Write-Host "The build folder was set up with EXTRA_CONF_FILE='$was': building it from scratch." -ForegroundColor Yellow
        $p = 'always'
    }
}
$westArgs = @('build', '-b', $Board, '-p', $p, '-d', $OutDir, $src)
if ($extraConf) { $westArgs += @('--', "-DEXTRA_CONF_FILE=$extraConf") }
west @westArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if (Test-Path (Join-Path $OutDir 'partitions.yml')) {
    throw 'Partition Manager output found (partitions.yml): sysbuild.conf is missing or stale; use -Pristine.'
}
$app = Join-Path $OutDir (Split-Path $src -Leaf)
if (-not (Test-Path (Join-Path $app 'zephyr'))) { $app = $OutDir }
$uf2 = Join-Path $app 'zephyr\zephyr.uf2'
if (Test-Path $uf2) {
    Write-Host "`nUF2 ready: $uf2" -ForegroundColor Green
    if ($extraConf) {
        Write-Host "This is the TEST build with $extraConf, not the default firmware (see app\README.md)." -ForegroundColor Yellow
    }
    Write-Host 'To flash: short RST to GND twice quickly; a USB drive appears. Copy zephyr.uf2 onto it.'
}
