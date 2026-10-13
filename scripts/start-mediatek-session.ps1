# SPDX-License-Identifier: MIT
#Requires -RunAsAdministrator
param([ValidateRange(30,82800)][int]$Seconds = 82800)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$analysis = Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'winawdl\radio'
$package = Join-Path $repo 'build\mediatek-radio\package'
$stem = 'winawdl_mt7925_mcu'
$env:DOTNET_ROOT = Join-Path $repo 'runtime\dotnet'
$env:DOTNET_CLI_HOME = $analysis
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE = 'false'
$dotnet = Join-Path $env:DOTNET_ROOT 'dotnet.exe'
$component = Join-Path $repo 'build\mediatek\winawdl-mediatek.dll'
$tools = @{
  DevCon = (Join-Path $repo 'runtime\devcon.exe')
}
$log = Join-Path $analysis 'session.log'
$result = Join-Path $analysis 'radio-status.json'
$status = @{
  state = 'preflight'
  timestamp = (Get-Date).ToString('o')
}
$status | ConvertTo-Json | Set-Content -LiteralPath $result
Start-Transcript -Path $log -Force | Out-Null
$watchdog = $null
$done = $null
$ready = $null
$saved = $null
$bindingChanged = $false
try {
  $wired = @(Get-NetAdapter -Physical | Where-Object {
      $_.Status -eq 'Up' -and $_.InterfaceDescription -notmatch 'Wi-Fi|Wireless|802\.11'
    })
  if (-not $wired) {
    throw 'An active wired connection is required.'
  }
  $profiles = @(Get-ChildItem "$repo\mediatek\profiles\*.json" | ForEach-Object {
      Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json
    })
  $cards = @(Get-PnpDevice -PresentOnly | Where-Object {
      $instance = $_.InstanceId
      @($profiles | Where-Object {
          $instance.StartsWith($_.hardwarePrefix + '&REV_', [StringComparison]::OrdinalIgnoreCase) -or $instance.StartsWith($_.hardwarePrefix + '\', [StringComparison]::OrdinalIgnoreCase)
        }).Count -gt 0
    })
  if ($cards.Count -ne 1) {
    throw 'Exactly one supported MediaTek PCI function is required.'
  }
  $card = $cards[0]
  $original = (Get-PnpDeviceProperty -InstanceId $card.InstanceId -KeyName DEVPKEY_Device_DriverInfPath).Data
  $service = (Get-PnpDeviceProperty -InstanceId $card.InstanceId -KeyName DEVPKEY_Device_Service).Data
  if ($service -ne 'mtkwecx') {
    throw 'This test requires the inspected OEM driver binding.'
  }
  $platform = @{
    originalDmaPolicy = (Get-PnpDeviceProperty -InstanceId $card.InstanceId -KeyName DEVPKEY_Device_DmaRemappingPolicy).Data
  }
  $guard = Get-CimInstance -ClassName Win32_DeviceGuard -Namespace root\Microsoft\Windows\DeviceGuard
  $platform.vbsStatus = $guard.VirtualizationBasedSecurityStatus
  $platform.securityServicesRunning = @($guard.SecurityServicesRunning)
  try {
    $platform.secureBoot = Confirm-SecureBootUEFI
  } catch {
    $platform.secureBoot = $null
  }
  $manifest = Get-Content -LiteralPath "$package\package-manifest.json" -Raw | ConvertFrom-Json
  if ($manifest.kind -ne 'mcu' -or $manifest.awdlService -ne $true) {
    throw 'The MediaTek radio package is invalid.'
  }
  $wintun = Join-Path $repo 'build\awdl\wintun.dll'
  $pinnedWintun = 'E5DA8447DC2C320EDC0FC52FA01885C103DE8C118481F683643CACC3220DAFCE'
  if ((Get-FileHash -LiteralPath $wintun).Hash -ne $pinnedWintun -or $manifest.wintunSha256 -ne $pinnedWintun -or
    (Get-AuthenticodeSignature -LiteralPath $wintun).Status -ne 'Valid') {
    throw 'The official signed Wintun DLL was not verified.'
  }
  if (@(Get-NetAdapter -IncludeHidden | Where-Object {
        [guid]$_.InterfaceGuid -eq [guid]'2be941ea-a750-450a-ba41-fdd8df0870b6'
      }).Count) {
    throw 'The AWDL host adapter already exists; it will not be replaced.'
  }
  foreach ($file in $manifest.files.PSObject.Properties) {
    if ($file.Name -notin @("$stem.dll","$stem.inf","$stem.cat",'probe-signing.cer')) {
      throw 'Unknown radio package file.'
    }
    if ((Get-FileHash -LiteralPath (Join-Path $package $file.Name)).Hash -ne $file.Value) {
      throw 'The staged package changed.'
    }
  }
  $snapshot = Join-Path $analysis 'before-proxy.json'
  & $dotnet $component --probe $snapshot
  if ($LASTEXITCODE -ne 0) {
    throw 'OEM preflight failed.'
  }
  $settings = Get-Content -LiteralPath $snapshot -Raw | ConvertFrom-Json
  if ($settings.instanceId -ne $card.InstanceId) {
    throw 'The OEM snapshot belongs to another PCI device.'
  }
  $profile = Get-Content -LiteralPath (Join-Path $repo ('mediatek\profiles\'+$settings.id+'.json')) -Raw | ConvertFrom-Json
  if (@($settings.radios | Where-Object {
        $_.Software -ne 2
      }).Count -ne 0) {
    throw 'The radio session requires software Wi-Fi off.'
  }
  $backup = Join-Path $analysis ('oem-driver-backup-'+$original)
  $null = New-Item -ItemType Directory -Path $backup -Force
  pnputil.exe /export-driver $original $backup
  if ($LASTEXITCODE -ne 0) {
    throw 'The original driver package could not be backed up.'
  }
  $restore = @(Get-ChildItem -LiteralPath $backup -Recurse -File -Filter '*.inf')
  if ($restore.Count -ne 1) {
    throw 'The original driver backup is ambiguous.'
  }
  $run = [guid]::NewGuid().ToString('N')
  $saved = @{
    runId = $run
    recoveryTask = "winawdl-mt7925-recover-$run"
    kind = 'Mcu'
    instanceId = $card.InstanceId
    hardwareId = $profile.hardwarePrefix
    originalInf = $original
    restoreInf = $restore[0].FullName
    wlanSnapshot = $snapshot
    certificateThumbprint = $manifest.certificateThumbprint
    addedStores = @()
    doneEvent = "Local\winawdl-mt7925-proxy-done-$run"
    readyEvent = "Local\winawdl-mt7925-proxy-ready-$run"
  }
  $saved.hostAdapterGuid = '2be941ea-a750-450a-ba41-fdd8df0870b6'
  $saved.firewallRules = @("winawdl-mt7925-airdrop-tcp-$run","winawdl-mt7925-airdrop-udp-$run")
  $client = Join-Path $repo 'build\mediatek-radio\winawdl_mediatek.exe'
  if ((Get-FileHash -LiteralPath $client).Hash -ne $manifest.workerSha256 -or
    (Get-FileHash -LiteralPath (Join-Path $repo 'build\awdl\awdld.exe')).Hash -ne $manifest.awdlDaemonSha256) {
    throw 'The packaged radio executables changed.'
  }
  & $client --verify-policy-key
  if ($LASTEXITCODE) {
    throw 'The OEM DMA policy differs from the validated configuration.'
  }
  if ($settings.id -notmatch '^mt7925-[0-9.]+$') {
    throw 'Unrecognized reviewed driver profile.'
  }
  $profile = Get-Content -LiteralPath (Join-Path $repo ('mediatek\profiles\'+$settings.id+'.json')) -Raw | ConvertFrom-Json
  if ((Get-FileHash -LiteralPath (Join-Path ([Environment]::GetFolderPath('Windows')) 'System32\drivers\mtkwlan.dat')).Hash -ne $profile.firmwareBundleSha256 -or $settings.firmware -ne $profile.firmwareVersion) {
    throw 'The firmware bundle or loaded build changed.'
  }
  $saved.workerTask = "winawdl-mt7925-mcu-$run"
  $saved.workerRelease = Join-Path $analysis 'memory-release.signal'
  foreach ($path in $saved.workerRelease,(Join-Path $analysis 'session-result.json')) {
    if (Test-Path -LiteralPath $path) {
      Remove-Item -LiteralPath $path
    }
  }
  foreach ($store in 'Root','TrustedPublisher') {
    if (-not (Test-Path -LiteralPath "Cert:\LocalMachine\$store\$($manifest.certificateThumbprint)")) {
      $saved.addedStores += $store
    }
  }
  $saved.doneEvent = "Global\winawdl-mt7925-proxy-done-$run"
  $saved.readyEvent = "Global\winawdl-mt7925-proxy-ready-$run"
  $saved.heartbeatEvent = "Global\winawdl-mt7925-heartbeat-$run"
  $saved.nativeStopEvent = "Global\winawdl-mt7925-native-stop-$run"
  $heartbeat = [Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::AutoReset,$saved.heartbeatEvent)
  $nativeStop = [Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$saved.nativeStopEvent)
  $requestedStop = [Threading.EventWaitHandle]::OpenExisting('Global\winawdl-mt7925-stop')
  $state = Join-Path $analysis 'recovery-state.json'
  $saved | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $state
  $done = [Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$saved.doneEvent)
  $ready = [Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$saved.readyEvent)
  $shell = (Get-Process -Id $PID).Path
  # Task Scheduler keeps recovery independent of the controlling process.
  $recoveryArguments = '-NoProfile -ExecutionPolicy Bypass -File "'+(Join-Path $PSScriptRoot 'restore-mediatek-radio.ps1')+'" -StatePath "'+$state+'"'
  $recoveryAction = New-ScheduledTaskAction -Execute $shell -Argument $recoveryArguments
  $recoveryPrincipal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
  $recoverySettings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew
  Register-ScheduledTask -TaskName $saved.recoveryTask -Action $recoveryAction -Principal $recoveryPrincipal -Settings $recoverySettings -Description 'Independent MediaTek radio recovery.' | Out-Null
  $watchdog = $true
  Start-ScheduledTask -TaskName $saved.recoveryTask
  if (-not $ready.WaitOne(10000)) {
    throw 'Driver recovery watchdog did not confirm readiness.'
  }
  foreach ($store in $saved.addedStores) {
    Import-Certificate -FilePath "$package\probe-signing.cer" -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
  }
  $signature = Get-AuthenticodeSignature -LiteralPath "$package\$stem.dll"
  if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ne $manifest.certificateThumbprint) {
    throw 'The probe DLL signature was not verified.'
  }
  pnputil.exe /add-driver "$package\$stem.inf"
  if ($LASTEXITCODE -ne 0) {
    throw 'The radio package could not be staged.'
  }
  $status.state = 'switching'
  $status | ConvertTo-Json | Set-Content -LiteralPath $result
  & $tools.DevCon update "$package\$stem.inf" $saved.hardwareId
  if ($LASTEXITCODE -ne 0) {
    throw "The radio driver did not bind without reboot; devcon exit $LASTEXITCODE"
  }
  $bound = (Get-PnpDeviceProperty -InstanceId $card.InstanceId -KeyName DEVPKEY_Device_DriverInfPath).Data
  if ($bound -eq $original) {
    throw 'The card still uses the OEM driver.'
  }
  $bindingChanged = $true
  $status.boundInf = $bound
  $policy = Get-PnpDeviceProperty -InstanceId $card.InstanceId -KeyName DEVPKEY_Device_DmaRemappingPolicy -ErrorAction SilentlyContinue
  $platform.probeDmaPolicy = $policy.Data
  $platform.probeDmaPolicyReadError = if ($policy -and $null -ne $policy.Data) {
    $null
  } else {
    'Unavailable; not evidence of an unmapped DMA domain.'
  }
  $platform.directPhysicalDmaValidated = $false
  $platform | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $analysis 'proxy-platform.json')
  $action = New-ScheduledTaskAction -Execute $client -Argument ('--awdl-service '+$saved.nativeStopEvent)
  $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
  $taskSettings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew
  Register-ScheduledTask -TaskName $saved.workerTask -Action $action -Principal $principal -Settings $taskSettings -Description 'MT7925 radio session; memory held until DMA shutdown or OEM recovery.' | Out-Null
  Start-ScheduledTask -TaskName $saved.workerTask
  $deadline = (Get-Date).AddSeconds(($Seconds+30))
  $firewallAdded = $false
  do {
    Start-Sleep -Milliseconds 250
    $heartbeat.Set() | Out-Null
    if ($requestedStop.WaitOne(0) -or ((Get-Date) -gt $deadline.AddSeconds(-30))) {
      $nativeStop.Set() | Out-Null
    }
    $task = Get-ScheduledTask -TaskName $saved.workerTask
    $info = Get-ScheduledTaskInfo -TaskName $saved.workerTask
    if (-not $firewallAdded) {
      $hostAdapter = Get-NetAdapter -IncludeHidden | Where-Object {
        [guid]$_.InterfaceGuid -eq [guid]$saved.hostAdapterGuid
      }
      if ($hostAdapter) {
        $pythonPath = Join-Path $repo 'runtime\python\python.exe'
        if (-not (Test-Path -LiteralPath $pythonPath)) {
          throw 'The reviewed AirDrop runtime is missing.'
        }
        $runtimeManifest = Get-Content -LiteralPath (Join-Path (Split-Path $pythonPath -Parent) 'runtime-manifest.json') -Raw | ConvertFrom-Json
        if ((Get-FileHash -LiteralPath $pythonPath -Algorithm SHA256).Hash -ne $runtimeManifest.files.'python.exe') {
          throw 'AirDrop runtime differs from its staged hash.'
        }
        New-NetFirewallRule -Name $saved.firewallRules[0] -DisplayName 'winawdl AirDrop TCP' -Direction Inbound -Action Allow -Protocol TCP -LocalPort 8771 -InterfaceAlias $hostAdapter.Name -Program $pythonPath -Profile Any -PolicyStore PersistentStore | Out-Null
        New-NetFirewallRule -Name $saved.firewallRules[1] -DisplayName 'winawdl AirDrop mDNS' -Direction Inbound -Action Allow -Protocol UDP -LocalPort 5353 -InterfaceAlias $hostAdapter.Name -Program $pythonPath -Profile Any -PolicyStore PersistentStore | Out-Null
        foreach ($ruleName in $saved.firewallRules) {
          $activeRule = Get-NetFirewallRule -PolicyStore ActiveStore -Name $ruleName
          if ($activeRule.Action -ne 'Allow' -or $activeRule.Enabled -ne 'True') {
            throw 'The temporary AirDrop firewall rule is not active.'
          }
        }
        $firewallAdded = $true
        $status.firewallRulesInstalled = $true
        $status.state = 'radio-session-running'
        $status | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $result
      }
    }
    if ($task.State -ne 'Running' -and $info.LastRunTime.Year -gt 2000) {
      break
    }
  } while ((Get-Date) -lt $deadline)
  $status.workerExitCode = $info.LastTaskResult
  if ($task.State -eq 'Running') {
    throw 'The radio worker is still waiting; restoring the OEM driver before releasing memory.'
  }
  $data = Get-Content -LiteralPath (Join-Path $analysis 'session-result.json') -Raw | ConvertFrom-Json
  if ($info.LastTaskResult -ne 0) {
    throw "MediaTek radio session failed: $($data.error)"
  }
  if (-not $data.dmaValidated -or -not $data.dmaStopped -or -not $data.allPagesReleased) {
    throw 'MCU completion or memory cleanup was not confirmed.'
  }
  $status.state = 'session-completed'
} catch {
  $status.state = 'failed'
  $status.error = $_.Exception.Message
} finally {
  if ($nativeStop) {
    $nativeStop.Set() | Out-Null
  }
  if ($bindingChanged -and $saved -and -not $watchdog) {
    try {
      & (Join-Path $PSScriptRoot 'restore-mediatek-radio.ps1') -StatePath $state -Immediate
      if ($LASTEXITCODE) {
        throw 'Radio recovery failed.'
      }
    }
    catch {
      $status.state = 'recovery-failed'
      $status.recoveryError = $_.Exception.Message
    }
  }
  if ($done) {
    $done.Set() | Out-Null
  }
  if ($watchdog) {
    $recoveryDeadline = (Get-Date).AddSeconds(45)
    $recoveryConfirmed = $false
    $watchdogReport = Join-Path $analysis 'recovery-result.json'
    do {
      if (Test-Path -LiteralPath $watchdogReport) {
        try {
          $recoveryResult = Get-Content -LiteralPath $watchdogReport -Raw | ConvertFrom-Json
          if ($recoveryResult.runId -eq $saved.runId) {
            $recoveryConfirmed = $recoveryResult.originalDriverRestored -and $recoveryResult.wlanSettingsRestored -and $recoveryResult.temporaryTrustRemoved
            break
          }
        } catch {
        }
      }
      Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $recoveryDeadline)
    if (-not $recoveryConfirmed) {
      $status.state = 'recovery-failed'
      $status.watchdogError = 'Independent recovery was not confirmed.'
    }
  }
  if ($status.state -eq 'session-completed') {
    $status.state = 'completed'
  }
  $status.finished = (Get-Date).ToString('o')
  $status | ConvertTo-Json | Set-Content -LiteralPath $result
  if ($done) {
    $done.Dispose()
  }
  if ($ready) {
    $ready.Dispose()
  }
  foreach ($handle in @($heartbeat,$nativeStop,$requestedStop)) {
    if ($handle) {
      $handle.Dispose()
    }
  }
  Stop-Transcript | Out-Null
}
