<#
.SYNOPSIS
  Build the OpenSC2 blank canvas for the SuperMini / Pro Micro nRF52840 (UF2 bootloader).
  The result is a .uf2 to copy onto the board's USB drive. Same options as ..\build.ps1.
.EXAMPLE
  .\build.ps1
  .\build.ps1 -OutDir C:\b\osc2 -Pristine
#>
param(
    [string]$OutDir,
    [switch]$Pristine
)
$args2 = @{ Project = 'app' }
if ($OutDir) { $args2.OutDir = $OutDir }
if ($Pristine) { $args2.Pristine = $true }
& (Join-Path $PSScriptRoot '..\build.ps1') @args2
exit $LASTEXITCODE
