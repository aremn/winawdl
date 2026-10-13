# SPDX-License-Identifier: MIT
param([switch]$Check, [switch]$SkipDriver)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $root 'scripts\toolchain.ps1')
$tools = if ($SkipDriver) {
  Initialize-Toolchain
} else {
  Initialize-Toolchain -UmdfVersion '2.33'
}
$output = Join-Path $root 'build\mediatek-radio'
$null = New-Item -ItemType Directory -Path $output -Force
Push-Location $output
try {
  if (-not $SkipDriver) {
    & $tools.Cl /nologo /W4 /WX /wd4324 /O2 /GS /guard:cf /MT /DUNICODE /D_UNICODE /D_AMD64_ /D_WIN64 /DUMDF_VERSION_MAJOR=2 /DUMDF_VERSION_MINOR=33 /DWINAWDL_MT7925 /DMIN_BAR_LEN=0x200000u "$root\driver\regproxy\regproxy.c" /link /DLL "/OUT:$output\winawdl_mt7925_mcu.dll" /SUBSYSTEM:WINDOWS /GUARD:CF /INCREMENTAL:NO /OPT:REF /OPT:ICF WdfDriverStubUm.lib ntdll.lib mincore.lib
    if ($LASTEXITCODE) {
      throw 'MediaTek UMDF build failed.'
    }
  }
  $sources = @('main.cpp', 'worker.cpp', 'device_io.cpp') | ForEach-Object {
    Join-Path "$root\radio\mediatek" $_
  }
  & $tools.Cl /nologo /std:c++20 /EHsc /W4 /WX /O2 /MT @sources /link "/OUT:$output\winawdl_mediatek.exe" setupapi.lib advapi32.lib winmm.lib iphlpapi.lib ws2_32.lib
  if ($LASTEXITCODE) {
    throw 'MediaTek radio build failed.'
  }
  if ($Check) {
    & $tools.Cl /nologo /std:c++20 /EHsc /W4 /WX /O2 /MT "$root\radio\tests\mediatek_protocol_test.cpp" /link "/OUT:$output\mediatek_protocol_test.exe"
    if ($LASTEXITCODE) {
      throw 'MediaTek protocol test build failed.'
    }
    & "$output\mediatek_protocol_test.exe"
    if ($LASTEXITCODE) {
      throw 'MediaTek protocol checks failed.'
    }
  }
} finally {
  Pop-Location
}
