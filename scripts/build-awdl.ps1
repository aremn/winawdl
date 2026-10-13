# Builds the AWDL daemon in awdl\ with MinGW-w64 GCC. Output lands in
# build\awdl. OWL is GNU C, which Microsoft's compiler does not accept.
param([string]$MinGw)
$ErrorActionPreference = 'Stop'

$repo  = Split-Path $PSScriptRoot -Parent
$build = "$repo\build\awdl"
$mingw = if ($MinGw) { $MinGw } else {
  $gcc = Get-Command gcc.exe -ErrorAction SilentlyContinue
  if (-not $gcc) { throw 'MinGW-w64 GCC is required. Pass -MinGw with its bin directory.' }
  Split-Path $gcc.Source -Parent
}
if (-not (Test-Path "$mingw\gcc.exe")) { throw "MinGW-w64 GCC not found in $mingw" }
if (-not (Test-Path "$repo\upstream\owl\radiotap\radiotap.c")) {
  throw 'OWL radiotap sources missing: run "git -C upstream/owl submodule update --init radiotap".'
}

# GCC loads its own helper DLLs from the directory it lives in.
$env:PATH = "$mingw;$env:PATH"

cmake -S "$repo\awdl" -B $build -G 'MinGW Makefiles' `
  "-DCMAKE_C_COMPILER=$mingw\gcc.exe" "-DCMAKE_MAKE_PROGRAM=$mingw\mingw32-make.exe" `
  -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }

cmake --build $build
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

# awdld loads wintun.dll from its own directory only.
$wintun = "$repo\deps\wintun\wintun.dll"
if (Test-Path $wintun) { Copy-Item $wintun $build -Force }

Get-ChildItem $build -Filter *.exe | Select-Object Name, Length
