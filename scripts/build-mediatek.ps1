# SPDX-License-Identifier: MIT
# Builds the OEM control helper; --self-test never accesses the adapter.
param([string]$Dotnet = 'dotnet', [switch]$Check)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$version = & $Dotnet --version
if ($LASTEXITCODE -or [version]$version -lt [version]'10.0') {
  throw 'The .NET SDK 10 or newer is required.'
}
$env:DOTNET_CLI_HOME = Join-Path $root 'build\dotnet-home'
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE = 'false'
$output = Join-Path $root 'build\mediatek'
& $Dotnet build "$root\mediatek\Winawdl.MediaTek.csproj" -c Release -o $output -nologo -v q
if ($LASTEXITCODE) {
  throw 'MediaTek control build failed.'
}
if ($Check) {
  & $Dotnet "$output\winawdl-mediatek.dll" --self-test
  if ($LASTEXITCODE) {
    throw 'MediaTek profile checks failed.'
  }
}
