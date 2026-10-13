# SPDX-License-Identifier: MIT
#Requires -RunAsAdministrator
param([ValidateSet('Stage','Enable','Stop','Uninstall')][string]$Mode = 'Stage',[int]$TestSeconds = 360,[switch]$NoTest, [string]$DotnetRoot, [string]$PythonRoot, [string]$DevConPath)
$ErrorActionPreference = 'Stop'
$source = Split-Path $PSScriptRoot -Parent
Start-Transcript -Path (Join-Path $source 'build\installation.log') -Force | Out-Null
trap {
  Write-Output $_.Exception.ToString()
  Stop-Transcript | Out-Null
  exit 1
}
$target = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'winawdl'
$state = Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'winawdl\radio'
$shell = Join-Path ([Environment]::GetFolderPath('Windows')) 'System32\WindowsPowerShell\v1.0\powershell.exe'
$taskName = 'winawdl-MediaTek-Radio'
$guiName = 'winawdl-Background-App'
function Test-StageInputs {
  if (-not $DotnetRoot) {
    $script:DotnetRoot = Split-Path (Get-Command dotnet.exe -ErrorAction Stop).Source -Parent
  }
  if (-not $PythonRoot -or -not $DevConPath) {
    throw 'Stage requires -PythonRoot (Python 3.11) and -DevConPath (the signed WDK utility).'
  }
  $required = @(
    (Join-Path $DotnetRoot 'dotnet.exe'),
    (Join-Path $PythonRoot 'python.exe'),
    (Join-Path $PythonRoot 'python311.dll'),
    $DevConPath,
    "$source\build\mediatek-radio\winawdl_mediatek.exe",
    "$source\build\mediatek-radio\package\package-manifest.json",
    "$source\build\mediatek\winawdl-mediatek.dll",
    "$source\build\awdl\awdld.exe",
    "$source\build\awdl\wintun.dll",
    "$source\build\app\winawdl\winawdl.exe"
  )
  foreach ($path in $required) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
      throw "Stage input is missing: $path"
    }
  }
  foreach ($framework in 'Microsoft.NETCore.App','Microsoft.WindowsDesktop.App') {
    $versions = @(Get-ChildItem -LiteralPath (Join-Path $DotnetRoot "shared\$framework") -Directory |
        Where-Object {
        $_.Name -match '^10\.'
      })
    if (-not $versions.Count) {
      throw "The private runtime needs .NET 10 $framework."
    }
  }
  if ((Get-AuthenticodeSignature -LiteralPath $DevConPath).Status -ne 'Valid') {
    throw 'The WDK device utility has no valid signature.'
  }
}
function Protect-Folder([string]$path) {
  $null = New-Item -ItemType Directory -Path $path -Force
  $acl = [Security.AccessControl.DirectorySecurity]::new()
  $acl.SetAccessRuleProtection($true,$false)
  $acl.SetOwner([Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'))
  foreach ($sid in 'S-1-5-18','S-1-5-32-544','S-1-5-32-545') {
    $rights = if ($sid -eq 'S-1-5-32-545') {
      'ReadAndExecute'
    } else {
      'FullControl'
    }
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
        [Security.Principal.SecurityIdentifier]::new($sid),$rights,'ContainerInherit,ObjectInherit','None','Allow'))
  }
  Set-Acl -LiteralPath $path -AclObject $acl
}
function Stop-Radio {
  try {
    $event = [Threading.EventWaitHandle]::OpenExisting('Global\winawdl-mt7925-stop')
    $event.Set() | Out-Null
    $event.Dispose()
  } catch [Threading.WaitHandleCannotBeOpenedException] {
  }
  $deadline = (Get-Date).AddSeconds(60)
  do {
    $task = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    if (-not $task -or $task.State -ne 'Running') {
      return
    }
    Start-Sleep -Milliseconds 500
  } while ((Get-Date) -lt $deadline)
  throw 'Radio is still recovering; it was left running to protect DMA memory.'
}
function Stop-BackgroundApp {
  # Stop only this installation's GUI and engine after radio recovery.
  $paths = @("$target\build\app\winawdl\winawdl.exe", "$target\runtime\python\python.exe")
  Get-Process winawdl,python -ErrorAction SilentlyContinue | Where-Object {
    $_.Path -in $paths
  } | Stop-Process -ErrorAction Stop
  $deadline = (Get-Date).AddSeconds(10)
  do {
    $task = Get-ScheduledTask -TaskName $guiName -ErrorAction SilentlyContinue
    if (-not $task -or $task.State -ne 'Running') { return }
    Start-Sleep -Milliseconds 250
  } while ((Get-Date) -lt $deadline)
  throw 'The background app did not exit before updating its files.'
}
function Radio-Task([bool]$permanent) {
  $args = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "'+$target+'\scripts\start-mediatek-background.ps1"'
  if (-not $permanent) {
    $args += ' -Once -Seconds '+$TestSeconds
  }
  $action = New-ScheduledTaskAction -Execute $shell -Argument $args
  $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
  $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -StartWhenAvailable -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
  if ($permanent) {
    Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Settings $settings -Trigger (New-ScheduledTaskTrigger -AtStartup) -Force | Out-Null
  } else {
    Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Settings $settings -Force | Out-Null
  }
  $scheduler = New-Object -ComObject 'Schedule.Service'
  $scheduler.Connect()
  $registered = $scheduler.GetFolder('\').GetTask($taskName)
  $registered.SetSecurityDescriptor('O:BAG:SYD:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGX;;;'+$install.ownerSid+')',0)
}
if ($Mode -eq 'Stop') {
  Stop-Radio
  exit 0
}
if ($Mode -eq 'Uninstall') {
  Stop-Radio
  Stop-BackgroundApp
  $install = Get-Content -LiteralPath "$target\installation.json" -Raw | ConvertFrom-Json
  foreach ($name in $taskName,$guiName) {
    Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
  }
  # Preserve received files and the OEM package; only own scheduled startup is removed.
  @{
    state = 'uninstalled'
    filesPreserved = $true
    timestamp = (Get-Date).ToString('o')
  } | ConvertTo-Json | Set-Content -LiteralPath "$state\installation-status.json"
  exit 0
}
if ($Mode -eq 'Stage') {
  # Validate the package before interrupting an existing installation.
  Test-StageInputs
  Stop-Radio
  if ((Test-Path -LiteralPath $target) -and -not (Test-Path -LiteralPath "$target\installation.json")) {
    throw 'The target folder belongs to another installation.'
  }
  Stop-BackgroundApp
  $owner = [Security.Principal.WindowsIdentity]::GetCurrent()
  if ($owner.User.Value -eq 'S-1-5-18') {
    throw 'Run initial installation as the interactive owner.'
  }
  Protect-Folder $target
  Protect-Folder (Split-Path $state -Parent)
  Protect-Folder $state
  $install = @{
    schema = 1
    ownerSid = $owner.User.Value
    ownerName = $owner.Name
    installed = (Get-Date).ToString('o')
    experimental = $true
  }
  $install | ConvertTo-Json | Set-Content -LiteralPath "$target\installation.json"
  foreach ($folder in 'scripts','runtime','runtime\dotnet','runtime\python','build\mediatek-radio\package','build\mediatek','build\awdl','build\app\winawdl','airdrop','mediatek\profiles','licenses') {
    $null = New-Item -ItemType Directory -Path (Join-Path $target $folder) -Force
  }
  foreach ($name in 'airdrop-beacon.ps1','start-mediatek-session.ps1','restore-mediatek-radio.ps1','start-mediatek-background.ps1','launch-background-app.ps1','install-mediatek-background.ps1') {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination "$target\scripts" -Force
  }
  Copy-Item -LiteralPath "$source\build\mediatek-radio\winawdl_mediatek.exe" -Destination "$target\build\mediatek-radio" -Force
  Copy-Item "$source\build\mediatek-radio\package\*" "$target\build\mediatek-radio\package" -Force
  Copy-Item "$source\build\mediatek\*" "$target\build\mediatek" -Recurse -Force
  Copy-Item "$source\build\app\winawdl\*" "$target\build\app\winawdl" -Recurse -Force
  foreach ($name in 'awdld.exe','wintun.dll') {
    Copy-Item -LiteralPath "$source\build\awdl\$name" -Destination "$target\build\awdl" -Force
  }
  Get-ChildItem -LiteralPath "$source\airdrop" -File | Where-Object Extension -eq '.py' | Copy-Item -Destination "$target\airdrop" -Force
  Copy-Item "$source\mediatek\profiles\*.json" "$target\mediatek\profiles" -Force
  foreach ($part in 'LICENSE','airdrop\LICENSE','awdl\LICENSE','radio\LICENSE','deps\wintun\LICENSE.txt','LICENSES\BSD-3-Clause-Clear.txt') {
    if (Test-Path -LiteralPath (Join-Path $source $part)) {
      Copy-Item -LiteralPath (Join-Path $source $part) -Destination (Join-Path "$target\licenses" ($part.Replace('\','-'))) -Force
    }
  }
  if (-not $DotnetRoot) {
    $DotnetRoot = Split-Path (Get-Command dotnet.exe -ErrorAction Stop).Source -Parent
  }
  $dotnet = [IO.Path]::GetFullPath($DotnetRoot)
  Copy-Item -LiteralPath "$dotnet\dotnet.exe" -Destination "$target\runtime\dotnet" -Force
  Copy-Item -LiteralPath "$dotnet\host" -Destination "$target\runtime\dotnet" -Recurse -Force
  foreach ($framework in 'Microsoft.NETCore.App','Microsoft.WindowsDesktop.App') {
    $null = New-Item -ItemType Directory -Path "$target\runtime\dotnet\shared" -Force
    Copy-Item -LiteralPath "$dotnet\shared\$framework" -Destination "$target\runtime\dotnet\shared" -Recurse -Force
  }
  if (-not $DevConPath) {
    throw 'Pass -DevConPath with the signed WDK device utility.'
  }
  $devcon = [IO.Path]::GetFullPath($DevConPath)
  if ((Get-AuthenticodeSignature -LiteralPath $devcon).Status -ne 'Valid') {
    throw 'The Microsoft device utility is not signed.'
  }
  Copy-Item -LiteralPath $devcon -Destination "$target\runtime\devcon.exe" -Force
  if (-not $PythonRoot) {
    throw 'Pass -PythonRoot with a Python 3.11 installation.'
  }
  $python = [IO.Path]::GetFullPath($PythonRoot)
  foreach ($name in 'python.exe','python311.dll','python3.dll','vcruntime140.dll','vcruntime140_1.dll','LICENSE.txt') {
    Copy-Item -LiteralPath "$python\$name" -Destination "$target\runtime\python" -Force
  }
  & robocopy.exe "$python\Lib" "$target\runtime\python\Lib" /E /XD site-packages __pycache__ /XF '*.pyc' /NFL /NDL /NJH /NJS /NP | Out-Null
  if ($LASTEXITCODE -ge 8) {
    throw 'Python standard library copy failed.'
  }
  Copy-Item -LiteralPath "$python\DLLs" -Destination "$target\runtime\python" -Recurse -Force
  @('Lib','DLLs','..\..\airdrop','import site') | Set-Content -LiteralPath "$target\runtime\python\python311._pth" -Encoding ascii
  @{
    files = @{
      'python.exe' = (Get-FileHash -LiteralPath "$target\runtime\python\python.exe").Hash
    }
  } | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath "$target\runtime\python\runtime-manifest.json"
  & "$target\runtime\python\python.exe" -c 'import ssl,engine,receiver'
  if ($LASTEXITCODE -ne 0) {
    throw 'The private runtime failed its import check.'
  }
  # Reset copied ACLs: user-writable source files must never stay writable in SYSTEM's installation.
  & icacls.exe $target /reset /T /Q | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw 'Installed file permissions could not be reset.'
  }
  Protect-Folder $target
  Radio-Task $false
  if (-not $NoTest) {
    Start-ScheduledTask -TaskName $taskName
  }
} else {
  $install = Get-Content -LiteralPath "$target\installation.json" -Raw | ConvertFrom-Json
  Stop-Radio
  Radio-Task $true
  $action = New-ScheduledTaskAction -Execute $shell -Argument ('-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "'+$target+'\scripts\launch-background-app.ps1"')
  $principal = New-ScheduledTaskPrincipal -UserId $install.ownerSid -LogonType Interactive -RunLevel Limited
  $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -StartWhenAvailable -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
  Register-ScheduledTask -TaskName $guiName -Action $action -Principal $principal -Settings $settings -Trigger (New-ScheduledTaskTrigger -AtLogOn -User $install.ownerSid) -Force | Out-Null
  Start-ScheduledTask -TaskName $taskName
  $shortcutFolder = Join-Path ([Environment]::GetFolderPath('Programs')) 'winawdl'
  $null = New-Item -ItemType Directory -Path $shortcutFolder -Force
  $shortcut = (New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path $shortcutFolder 'winawdl.lnk'))
  $shortcut.TargetPath = $shell
  $shortcut.Arguments = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "'+$target+'\scripts\launch-background-app.ps1" -Show'
  $shortcut.WindowStyle = 7
  $shortcut.IconLocation = $target+'\build\app\winawdl\winawdl.exe,0'
  $shortcut.Save()
  Start-ScheduledTask -TaskName $guiName
}
@{
  state = $Mode
  root = $target
  timestamp = (Get-Date).ToString('o')
} | ConvertTo-Json | Set-Content -LiteralPath "$state\installation-status.json"
Stop-Transcript | Out-Null
$global:LASTEXITCODE = 0
