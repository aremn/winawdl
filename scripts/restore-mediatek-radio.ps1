# SPDX-License-Identifier: MIT
#Requires -RunAsAdministrator
param([Parameter(Mandatory)][string]$StatePath, [switch]$Immediate)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$analysis = Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'winawdl\radio'
$dotnet = Join-Path $repo 'runtime\dotnet\dotnet.exe'
$component = Join-Path $repo 'build\mediatek\winawdl-mediatek.dll'
$tools = @{
  DevCon = (Join-Path $repo 'runtime\devcon.exe')
}
function Restore-RadioState($saved) {
  $bound = (Get-PnpDeviceProperty -InstanceId $saved.instanceId -KeyName DEVPKEY_Device_DriverInfPath).Data
  if ($bound -ne $saved.originalInf -and
    (Get-PnpDeviceProperty -InstanceId $saved.instanceId -KeyName DEVPKEY_Device_Service).Data -eq 'mtkwecx') {
    throw 'The OEM driver changed during the session. Its new binding was preserved; update the reviewed program profile.'
  }
  if ($bound -ne $saved.originalInf) {
    & $tools.DevCon update $saved.restoreInf $saved.hardwareId
    if ($LASTEXITCODE -ne 0) {
      throw "OEM restoration requires attention; devcon exit $LASTEXITCODE"
    }
  }
  for ($attempt = 0; $attempt -lt 10; $attempt++) {
    & $dotnet $component --restore-settings $saved.wlanSnapshot
    if ($LASTEXITCODE -eq 0) {
      break
    }
    if ($attempt -eq 9) {
      throw 'OEM WLAN settings could not be restored.'
    }
    Start-Sleep -Milliseconds 500
  }
  $service = (Get-PnpDeviceProperty -InstanceId $saved.instanceId -KeyName DEVPKEY_Device_Service).Data
  $bound = (Get-PnpDeviceProperty -InstanceId $saved.instanceId -KeyName DEVPKEY_Device_DriverInfPath).Data
  if ($service -ne 'mtkwecx' -or $bound -ne $saved.originalInf) {
    throw 'The original driver binding was not confirmed.'
  }
  $expectedRelease = Join-Path $analysis 'memory-release.signal'
  if ($saved.workerRelease -ne $expectedRelease -or $saved.workerTask -notmatch '^winawdl-mt7925-mcu-[0-9a-f]{32}$') {
    throw 'Unknown MCU memory recovery target.'
  }
  $registers = Join-Path $analysis 'after-mcu-dma-registers.json'
  & $dotnet $component --register-probe $registers
  if ($LASTEXITCODE -ne 0) {
    throw 'OEM DMA restoration could not be read.'
  }
  $dma = Get-Content -LiteralPath $registers -Raw | ConvertFrom-Json
  $ring = $dma.rings | Where-Object queue -eq 15
  $pendingPath = Join-Path $analysis 'session-result.json'
  $pending = if (Test-Path -LiteralPath $pendingPath) {
    Get-Content -LiteralPath $pendingPath -Raw | ConvertFrom-Json
  } else {
    $null
  }
  if (-not $ring -or $ring.count -ne 256 -or $ring.base -eq 0 -or ($pending -and $pending.ringAddress -eq $ring.base)) {
    throw 'OEM MCU ring did not confirm recovery; allocated pages will stay held.'
  }
  Set-Content -LiteralPath $expectedRelease -Value 'OEM MCU ring has been restored.'
  $task = Get-ScheduledTask -TaskName $saved.workerTask -ErrorAction SilentlyContinue
  if ($task) {
    for ($attempt = 0; $attempt -lt 20 -and $task.State -eq 'Running'; $attempt++) {
      Start-Sleep -Milliseconds 250
      $task = Get-ScheduledTask -TaskName $saved.workerTask
    }
    if ($task.State -eq 'Running') {
      Stop-ScheduledTask -TaskName $saved.workerTask
    }
    Unregister-ScheduledTask -TaskName $saved.workerTask -Confirm:$false
  }
  $expectedSubject = 'CN=winawdl MT7925 radio'
  if ($saved.hostAdapterGuid) {
    if ($saved.hostAdapterGuid -ne '2be941ea-a750-450a-ba41-fdd8df0870b6') {
      throw 'Unknown host adapter recovery target.'
    }
    $hostAdapter = @(Get-NetAdapter -IncludeHidden | Where-Object {
        [guid]$_.InterfaceGuid -eq [guid]$saved.hostAdapterGuid
      })
    foreach ($adapter in $hostAdapter) {
      $instance = [string]$adapter.PnPDeviceID
      if (-not $instance -or (Get-PnpDeviceProperty -InstanceId $instance -KeyName DEVPKEY_Device_Service).Data -ne 'Wintun') {
        throw 'Unexpected host adapter service; left untouched.'
      }
      pnputil.exe /remove-device $instance
      if ($LASTEXITCODE -ne 0) {
        throw 'The owned temporary Wintun adapter could not be removed.'
      }
    }
    if (@(Get-NetAdapter -IncludeHidden | Where-Object {
          [guid]$_.InterfaceGuid -eq [guid]$saved.hostAdapterGuid
        }).Count) {
      throw 'The temporary host adapter is still present.'
    }
  }
  foreach ($ruleName in $saved.firewallRules) {
    if ($ruleName -notmatch '^winawdl-mt7925-airdrop-(tcp|udp)-[0-9a-f]{32}$') {
      throw 'Unknown firewall recovery target.'
    }
    $rule = Get-NetFirewallRule -Name $ruleName -ErrorAction SilentlyContinue
    if ($rule) {
      Remove-NetFirewallRule -Name $ruleName
    }
  }
  foreach ($store in 'Root','TrustedPublisher') {
    $target = "Cert:\LocalMachine\$store\$($saved.certificateThumbprint)"
    if ($saved.addedStores -contains $store -and (Test-Path -LiteralPath $target)) {
      $cert = Get-Item -LiteralPath $target
      if ($cert.Subject -ne $expectedSubject) {
        throw 'Unexpected temporary certificate; left untouched.'
      }
      Remove-Item -LiteralPath $target
    }
  }
  $store = [xml](pnputil.exe /enum-drivers /format xml | Out-String)
  $originalName = 'winawdl_mt7925_mcu.inf'
  $ours = @($store.PnpUtil.Driver | Where-Object {
      $_.OriginalName -eq $originalName
    })
  foreach ($entry in $ours) {
    pnputil.exe /delete-driver $entry.DriverName
    if ($LASTEXITCODE -ne 0) {
      throw 'The unused radio package could not be removed.'
    }
  }
  & $dotnet $component --probe (Join-Path $analysis 'after-proxy.json')
  if ($LASTEXITCODE -ne 0) {
    throw 'Post-restoration OEM control query failed.'
  }
}
$path = [IO.Path]::GetFullPath($StatePath)
if (-not $path.StartsWith([IO.Path]::GetFullPath($analysis) + '\', [StringComparison]::OrdinalIgnoreCase)) {
  throw 'Recovery state is outside the protected radio directory.'
}
$saved = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
if ($saved.runId -notmatch '^[0-9a-f]{32}$' -or $saved.kind -ne 'Mcu') {
  throw 'Invalid radio recovery record.'
}
$done = $ready = $heartbeat = $nativeStop = $null
try {
  if (-not $Immediate) {
    $done = [Threading.EventWaitHandle]::OpenExisting($saved.doneEvent)
    $ready = [Threading.EventWaitHandle]::OpenExisting($saved.readyEvent)
    $heartbeat = [Threading.EventWaitHandle]::OpenExisting($saved.heartbeatEvent)
    $nativeStop = [Threading.EventWaitHandle]::OpenExisting($saved.nativeStopEvent)
    $ready.Set() | Out-Null
    $lastBeat = Get-Date
    while (-not $done.WaitOne(1000)) {
      if ($heartbeat.WaitOne(0)) {
        $lastBeat = Get-Date
      }
      if (((Get-Date) - $lastBeat).TotalSeconds -gt 30) {
        break
      }
    }
    $nativeStop.Set() | Out-Null
    Start-Sleep -Seconds 8
  }
  Restore-RadioState $saved
  @{
    runId = $saved.runId
    originalDriverRestored = $true
    wlanSettingsRestored = $true
    temporaryTrustRemoved = $true
  } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $analysis 'recovery-result.json')
} catch {
  @{
    runId = $saved.runId
    originalDriverRestored = $false
    error = $_.Exception.Message
  } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $analysis 'recovery-result.json')
  exit 3
} finally {
  foreach ($handle in @($done,$ready,$heartbeat,$nativeStop)) {
    if ($handle) {
      $handle.Dispose()
    }
  }
  if (-not $Immediate -and $saved.recoveryTask -match '^winawdl-mt7925-recover-[0-9a-f]{32}$') {
    Unregister-ScheduledTask -TaskName $saved.recoveryTask -Confirm:$false -ErrorAction SilentlyContinue
  }
}
