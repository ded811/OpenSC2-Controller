# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT

<#
.SYNOPSIS
  Load an nRF Connect SDK toolchain (v3.4.0 unless -NcsVersion says otherwise) into the current
  PowerShell session, plus ZEPHYR_BASE, so that west, CMake and the compiler of that toolchain work
  here.
.DESCRIPTION
  Install nRF Connect SDK v3.4.0 first: with nRF Connect for VS Code (Install SDK, then v3.4.0), or
  on the command line: nrfutil install sdk-manager, then nrfutil sdk-manager install v3.4.0. The
  default folder is C:\ncs, with the SDK in its v3.4.0 subfolder; pass -NcsRoot <folder> for
  another one.

  The script finds the toolchain for the version in <NcsRoot>\toolchains\toolchains.json and sets
  the variables listed in that toolchain's environment.json, then ZEPHYR_BASE to
  <NcsRoot>\<version>\zephyr.

  Dot-source it (build.ps1 does this itself). Windows does not run scripts by default; the first
  line allows them in this PowerShell window only, without administrator rights:
    Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
    . .\ncs_env.ps1
  The window's $ErrorActionPreference is left as it was.
.PARAMETER NcsRoot
  The folder that holds the SDK's version folders and its toolchains folder. Default: C:\ncs.
.PARAMETER NcsVersion
  The SDK version. Default: v3.4.0, the one OpenSC2 is built with.
#>
param(
    [string]$NcsRoot = 'C:\ncs',
    [string]$NcsVersion = 'v3.4.0'
)
# Dot-sourced, this script runs in the caller's scope: keep the caller's error preference and give
# it back at the end, also when the script stops with an error (build.ps1 sets Stop itself).
$osc2CallerErrorAction = $ErrorActionPreference
$ErrorActionPreference = 'Stop'
try {
    $installHint = "Install nRF Connect SDK $NcsVersion first (nRF Connect for VS Code, or nrfutil sdk-manager install $NcsVersion), or pass -NcsRoot with the folder it is in."
    Remove-Item env:ZEPHYR_BASE -ErrorAction SilentlyContinue
    $index = Join-Path $NcsRoot 'toolchains\toolchains.json'
    if (-not (Test-Path $index)) { throw "No toolchain index at $index. $installHint" }
    $bundle = (Get-Content $index -Raw | ConvertFrom-Json) |
        ForEach-Object { $_.toolchains } |
        Where-Object { $_.ncs_versions -contains $NcsVersion } |
        Select-Object -First 1
    if (-not $bundle) { throw "No toolchain for $NcsVersion listed in $index. $installHint" }
    $tc = Join-Path $NcsRoot ("toolchains\" + $bundle.identifier.bundle_id)
    $envDef = Get-Content (Join-Path $tc 'environment.json') -Raw | ConvertFrom-Json
    foreach ($v in $envDef.env_vars) {
        if ($v.type -eq 'string') {
            $value = $v.value
        } else {
            $value = ($v.values | ForEach-Object { Join-Path $tc $_ }) -join ';'
        }
        if ($v.existing_value_treatment -eq 'prepend_to' -and (Get-Item "env:$($v.key)" -ErrorAction SilentlyContinue)) {
            $value = $value + ';' + (Get-Item "env:$($v.key)").Value
        }
        Set-Item "env:$($v.key)" $value
    }
    $env:ZEPHYR_BASE = Join-Path $NcsRoot "$NcsVersion\zephyr"
    if (-not (Test-Path $env:ZEPHYR_BASE)) { throw "SDK not found at $env:ZEPHYR_BASE. $installHint" }
    Write-Host "nRF Connect SDK $NcsVersion environment loaded (toolchain $($bundle.identifier.bundle_id), ZEPHYR_BASE=$env:ZEPHYR_BASE)"
} finally {
    $ErrorActionPreference = $osc2CallerErrorAction
    Remove-Variable osc2CallerErrorAction -ErrorAction SilentlyContinue
}
