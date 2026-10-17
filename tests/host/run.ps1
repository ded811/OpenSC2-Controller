<#
.SYNOPSIS
  Build and run the codec tests on Windows with a gcc on PATH (for example MSYS2's ucrt64 gcc).
.EXAMPLE
  .\run.ps1
  .\run.ps1 -Cc C:\msys64\ucrt64\bin\gcc.exe
#>
param([string]$Cc = 'gcc')
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$out = Join-Path ([System.IO.Path]::GetTempPath()) 'opensc2_host_tests.exe'
$lib = Join-Path $root 'lib\opensc2'
& $Cc -std=c11 -Wall -Wextra -Wshadow -Werror "-I$root\include" "-I$lib" `
    (Join-Path $PSScriptRoot 'run_tests.c') "$lib\codec.c" "$lib\lizard.c" `
    "$lib\settings_defaults.c" "$lib\fusion.c" -lm -o $out
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $out
exit $LASTEXITCODE
