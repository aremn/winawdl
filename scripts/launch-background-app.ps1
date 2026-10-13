# SPDX-License-Identifier: MIT
param([switch]$Show)
$root = Split-Path $PSScriptRoot -Parent
$env:DOTNET_ROOT = Join-Path $root 'runtime\dotnet'
$env:DOTNET_ROOT_X64 = $env:DOTNET_ROOT
$env:DOTNET_MULTILEVEL_LOOKUP = '0'
[string[]]$appArgs = @()
if (-not $Show) {
  $appArgs = @('--background')
}
$start = @{
  FilePath = (Join-Path $root 'build\app\winawdl\winawdl.exe')
  WindowStyle = 'Hidden'
  Wait = $true
  PassThru = $true
}
if ($appArgs.Count) {
  $start.ArgumentList = $appArgs
}
$process = Start-Process @start
exit $process.ExitCode
