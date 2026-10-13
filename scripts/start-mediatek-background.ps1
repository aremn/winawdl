# SPDX-License-Identifier: MIT
#Requires -RunAsAdministrator
param([switch]$Once,[int]$Seconds = 82800)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$state = (Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'winawdl\radio')
Start-Transcript -Path (Join-Path $state 'controller.log') -Force | Out-Null
trap {
  @{
    state = 'failed'
    error = $_.Exception.ToString()
    finished = (Get-Date).ToString('o')
  } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $state 'radio-status.json')
  Stop-Transcript | Out-Null
  exit 1
}
$install = Get-Content -LiteralPath (Join-Path $root 'installation.json') -Raw | ConvertFrom-Json
# Only the owner can request a stop; it grants no arbitrary privileged action.
$security = [Security.AccessControl.EventWaitHandleSecurity]::new()
foreach ($sid in 'S-1-5-18','S-1-5-32-544') {
  $security.AddAccessRule([Security.AccessControl.EventWaitHandleAccessRule]::new(
      [Security.Principal.SecurityIdentifier]::new($sid),[Security.AccessControl.EventWaitHandleRights]::FullControl,'Allow'))
}
$security.AddAccessRule([Security.AccessControl.EventWaitHandleAccessRule]::new(
    [Security.Principal.SecurityIdentifier]::new($install.ownerSid),
    ([Security.AccessControl.EventWaitHandleRights]::Modify -bor [Security.AccessControl.EventWaitHandleRights]::Synchronize),'Allow'))
$created = $false
$stop = [Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,'Global\winawdl-mt7925-stop',[ref]$created,$security)
if (-not $created) {
  $stop.Dispose()
  throw 'Another radio controller is active.'
}
try {
  # Recover a interrupted binding before accessing OEM-specific controls.
  $saved = Join-Path $state 'recovery-state.json'
  if (Test-Path -LiteralPath $saved) {
    $record = Get-Content -LiteralPath $saved -Raw | ConvertFrom-Json
    $service = (Get-PnpDeviceProperty -InstanceId $record.instanceId -KeyName DEVPKEY_Device_Service).Data
    if ($service -ne 'mtkwecx') {
      & (Join-Path $PSScriptRoot 'restore-mediatek-radio.ps1') -Immediate -StatePath $saved
      if ($LASTEXITCODE -ne 0) {
        throw 'The previous radio session requires recovery.'
      }
    }
  }
  $retries = 0
  do {
    & (Join-Path $PSScriptRoot 'start-mediatek-session.ps1') -Seconds $Seconds
    $result = Get-Content -LiteralPath (Join-Path $state 'radio-status.json') -Raw | ConvertFrom-Json
    if ($result.state -ne 'completed') {
      if (-not $Once -and $retries -lt 3 -and ($result.error -match '^MediaTek radio session failed: Low physical pages:' -or $result.error -eq 'An active wired connection is required.') -and -not $stop.WaitOne(30000)) {
        ++$retries
        continue
      }
      break
    }
    $retries = 0
  } while (-not $Once -and -not $stop.WaitOne(3000))
} catch {
  @{
    state = 'failed'
    error = $_.Exception.Message
    finished = (Get-Date).ToString('o')
  } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $state 'radio-status.json')
} finally {
  $stop.Dispose()
}
