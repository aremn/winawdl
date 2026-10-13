# Builds the Windows app in app\ with the .NET SDK, version 10 or newer, from
# what that SDK ships: nothing is downloaded. Output lands in build\app\winawdl.
# -Check then runs what can run with no radio: the app's own checks, and its
# real window, kept off screen, against the AirDrop engine over loopback.
param([switch]$Check)
$ErrorActionPreference = 'Stop'

$repo = Split-Path $PSScriptRoot -Parent
if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) { throw 'The .NET SDK 10 or newer is required.' }

dotnet build "$repo\app\winawdl.csproj" -c Release -nologo -v q
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
$app = "$repo\build\app\winawdl\winawdl.exe"

if ($Check) {
  foreach ($mode in 'check', 'drive') {
    $out = "$repo\build\app\$mode"
    # The app has no console, so its exit is waited for and its log read.
    $run = Start-Process $app -ArgumentList "--$mode", "`"$out`"" -WindowStyle Hidden -Wait -PassThru
    Get-Content "$out\$mode.log" | Select-String -CaseSensitive '^(FAIL|STOPPED|ALL .*PASSED|\d+ FAILED)'
    if ($run.ExitCode -ne 0) { throw "$mode failed: read $out\$mode.log" }
  }
}

Get-Item $app | Select-Object FullName, Length
