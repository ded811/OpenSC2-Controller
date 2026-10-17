<#
.SYNOPSIS
  Load an nRF Connect SDK toolchain environment (v3.4.0 unless -NcsVersion says otherwise) into
  the current PowerShell session, the same way Toolchain Manager's terminal does
  (toolchains\<bundle>\environment.json), plus ZEPHYR_BASE.
  Dot-source it:  . .\ncs_env.ps1     (build.ps1 does this itself)
#>
param(
    [string]$NcsRoot = 'C:\ncs',
    [string]$NcsVersion = 'v3.4.0'
)
$ErrorActionPreference = 'Stop'
Remove-Item env:ZEPHYR_BASE -ErrorAction SilentlyContinue
$index = Join-Path $NcsRoot 'toolchains\toolchains.json'
if (-not (Test-Path $index)) { throw "No toolchain index at $index. Install NCS $NcsVersion with Toolchain Manager first." }
$bundle = (Get-Content $index -Raw | ConvertFrom-Json) |
    ForEach-Object { $_.toolchains } |
    Where-Object { $_.ncs_versions -contains $NcsVersion } |
    Select-Object -First 1
if (-not $bundle) { throw "No toolchain for $NcsVersion listed in $index." }
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
if (-not (Test-Path $env:ZEPHYR_BASE)) { throw "SDK not found at $env:ZEPHYR_BASE." }
Write-Host "NCS $NcsVersion environment loaded (toolchain $($bundle.identifier.bundle_id), ZEPHYR_BASE=$env:ZEPHYR_BASE)"
