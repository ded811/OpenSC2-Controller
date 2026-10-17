<#
.SYNOPSIS
  Build the OpenSC2 firmware (app/) or one of the samples for the SuperMini / Pro Micro nRF52840
  with the Adafruit UF2 bootloader. Needs a stock nRF Connect SDK v3.4.0 (no patches); the
  toolchain is loaded from C:\ncs by ncs_env.ps1.
.EXAMPLE
  .\build.ps1                               # app/ -> build\app\app\zephyr\zephyr.uf2
  .\build.ps1 -Project samples\minimal
  .\build.ps1 -Project app -OutDir C:\b\osc2 -Pristine   # a short folder (Windows path limit)
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
if (-not $OutDir) {
    $OutDir = Join-Path $PSScriptRoot ('build\' + (($Project -replace '[\\/:]+', '_').Trim('_')))
}
. (Join-Path $PSScriptRoot 'ncs_env.ps1') -NcsRoot $NcsRoot -NcsVersion 'v3.4.0'

$p = if ($Pristine) { 'always' } else { 'auto' }
west build -b $Board -p $p -d $OutDir $src
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if (Test-Path (Join-Path $OutDir 'partitions.yml')) {
    throw 'Partition Manager output found (partitions.yml): sysbuild.conf is missing or stale; use -Pristine.'
}
$app = Join-Path $OutDir (Split-Path $src -Leaf)
if (-not (Test-Path (Join-Path $app 'zephyr'))) { $app = $OutDir }
$uf2 = Join-Path $app 'zephyr\zephyr.uf2'
if (Test-Path $uf2) {
    Write-Host "`nUF2 ready: $uf2" -ForegroundColor Green
    Write-Host 'Double-tap RST (short RST to GND twice), then copy the .uf2 onto the drive that appears.'
}
