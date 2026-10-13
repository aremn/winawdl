# SPDX-License-Identifier: MIT
# Signs a local UMDF package. Installation and system trust are separate steps.
param([ValidatePattern('^\d+\.\d+\.\d+\.\d+$')][string]$Version = '0.4.0.0')
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $root 'scripts\toolchain.ps1')
$tools = Initialize-Toolchain -UmdfVersion '2.33'
$build = Join-Path $root 'build\mediatek-radio'
$package = Join-Path $build 'package'
$null = New-Item -ItemType Directory -Path $package -Force
foreach ($path in "$build\winawdl_mt7925_mcu.dll", "$build\winawdl_mediatek.exe", "$root\build\awdl\awdld.exe", "$root\build\awdl\wintun.dll") {
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
    throw "Build artifact is missing: $path"
  }
}
Copy-Item -LiteralPath "$build\winawdl_mt7925_mcu.dll" -Destination $package -Force
Copy-Item -LiteralPath "$PSScriptRoot\winawdl_mt7925_mcu.inf" -Destination $package -Force
$subject = 'CN=winawdl MT7925 radio'
$certificate = Get-ChildItem Cert:\CurrentUser\My | Where-Object {
  $_.Subject -eq $subject -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date).AddMonths(6)
} | Select-Object -First 1
if (-not $certificate) {
  $certificate = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy NonExportable -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(1)
}
& $tools.StampInf -f "$package\winawdl_mt7925_mcu.inf" -d * -v $Version
if ($LASTEXITCODE) {
  throw 'MediaTek INF stamping failed.'
}
& $tools.InfVerif /w "$package\winawdl_mt7925_mcu.inf"
if ($LASTEXITCODE) {
  throw 'MediaTek INF validation failed.'
}
& $tools.Inf2Cat "/driver:$package" /os:10_X64 /uselocaltime
if ($LASTEXITCODE) {
  throw 'MediaTek catalog generation failed.'
}
& $tools.SignTool sign /fd SHA256 /sha1 $certificate.Thumbprint /s My "$package\winawdl_mt7925_mcu.dll" "$package\winawdl_mt7925_mcu.cat"
if ($LASTEXITCODE) {
  throw 'MediaTek package signing failed.'
}
Export-Certificate -Cert $certificate -FilePath "$package\probe-signing.cer" | Out-Null
$hashes = @{
}
foreach ($name in 'winawdl_mt7925_mcu.dll','winawdl_mt7925_mcu.inf','winawdl_mt7925_mcu.cat','probe-signing.cer') {
  $hashes[$name] = (Get-FileHash -LiteralPath (Join-Path $package $name)).Hash
}
@{
  schema = 1
  kind = 'mcu'
  version = $Version
  awdlService = $true
  certificateThumbprint = $certificate.Thumbprint
  workerSha256 = (Get-FileHash -LiteralPath "$build\winawdl_mediatek.exe").Hash
  awdlDaemonSha256 = (Get-FileHash -LiteralPath "$root\build\awdl\awdld.exe").Hash
  wintunSha256 = (Get-FileHash -LiteralPath "$root\build\awdl\wintun.dll").Hash
  files = $hashes
} | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath "$package\package-manifest.json" -Encoding UTF8
