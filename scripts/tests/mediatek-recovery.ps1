# SPDX-License-Identifier: MIT
# Execute the real recovery function with local stubs; no driver is touched.
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$fixture = Join-Path $root 'build\recovery-check'
$null = New-Item -ItemType Directory -Path $fixture -Force
$analysis = $fixture
$tools = @{DevCon = 'Test-DevCon'}
$dotnet = 'Test-Control'
$component = 'unused'

$tokens = $null
$errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
  (Join-Path $root 'scripts\restore-mediatek-radio.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Recovery script cannot be parsed.' }
$function = $ast.Find({param($node)
  $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Restore-RadioState'
}, $true)
. ([scriptblock]::Create($function.Extent.Text))

$script:calls = @()
$script:bound = 'own.inf'
$script:service = 'WUDFRd'
$script:base = 4096
$script:devconExit = 0
function Get-PnpDeviceProperty {
  param($InstanceId, $KeyName)
  $data = if ($KeyName -eq 'DEVPKEY_Device_Service') { $script:service } else { $script:bound }
  [pscustomobject]@{Data = $data}
}
function Test-DevCon {
  param($Action, $Inf, $HardwareId)
  $script:calls += 'bind'
  $global:LASTEXITCODE = $script:devconExit
  if (-not $script:devconExit) { $script:bound = 'oem-test.inf'; $script:service = 'mtkwecx' }
}
function Test-Control {
  param($Component, $Command, $Output)
  $script:calls += $Command
  $global:LASTEXITCODE = 0
  if ($Command -eq '--register-probe') {
    @{rings = @(@{queue = 15; count = 256; base = $script:base})} |
      ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $Output
  }
}
function Get-ScheduledTask { return $null }
function pnputil.exe { '<PnpUtil />'; $global:LASTEXITCODE = 0 }
function Assert-Condition([bool]$Condition, [string]$Message) {
  if (-not $Condition) { throw $Message }
}
function Assert-Failure([scriptblock]$Action, [string]$Expected) {
  try { & $Action } catch {
    if ($_.Exception.Message -like $Expected) { return }
    throw
  }
  throw 'The operation unexpectedly succeeded.'
}
function Reset-Case {
  $script:calls = @()
  $script:bound = 'own.inf'
  $script:service = 'WUDFRd'
  $script:base = 4096
  $script:devconExit = 0
  if (Test-Path -LiteralPath $saved.workerRelease) { Remove-Item -LiteralPath $saved.workerRelease }
}
$saved = [pscustomobject]@{
  instanceId = 'synthetic-device'
  hardwareId = 'synthetic-hardware'
  originalInf = 'oem-test.inf'
  restoreInf = 'synthetic.inf'
  wlanSnapshot = 'synthetic-settings.json'
  workerRelease = (Join-Path $analysis 'memory-release.signal')
  workerTask = 'winawdl-mt7925-mcu-00000000000000000000000000000000'
  hostAdapterGuid = $null
  firewallRules = @()
  addedStores = @()
}

Reset-Case
Restore-RadioState $saved
Assert-Condition (Test-Path -LiteralPath $saved.workerRelease) 'Verified OEM queues did not permit release.'
Assert-Condition (($script:calls -join ',') -eq 'bind,--restore-settings,--register-probe,--probe') 'Recovery phases ran out of order.'

Reset-Case
$script:base = 0
Assert-Failure { Restore-RadioState $saved } '*ring did not confirm recovery*'
Assert-Condition (-not (Test-Path -LiteralPath $saved.workerRelease)) 'Memory release was permitted without a valid OEM ring.'

Reset-Case
$script:bound = 'new-oem.inf'
$script:service = 'mtkwecx'
Assert-Failure { Restore-RadioState $saved } '*new binding was preserved*'
Assert-Condition ($script:calls.Count -eq 0) 'An updated OEM binding was changed.'

Reset-Case
$script:devconExit = 1
Assert-Failure { Restore-RadioState $saved } '*devcon exit 1*'
Assert-Condition (-not (Test-Path -LiteralPath $saved.workerRelease)) 'Memory release was permitted after binding failure.'

Reset-Case
$saved.hostAdapterGuid = '00000000-0000-0000-0000-000000000000'
Assert-Failure { Restore-RadioState $saved } '*Unknown host adapter recovery target*'
$saved.hostAdapterGuid = $null
Write-Output 'PASS: recovery ordering, DMA release guard, OEM update preservation, binding failure, adapter ownership.'

# Missing install inputs must be rejected before a running radio is interrupted.
$tokens = $null
$errors = $null
$installer = [Management.Automation.Language.Parser]::ParseFile(
  (Join-Path $root 'scripts\install-mediatek-background.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Installer cannot be parsed.' }
$function = $installer.Find({param($node)
  $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Test-StageInputs'
}, $true)
. ([scriptblock]::Create($function.Extent.Text))
$DotnetRoot = Join-Path $fixture 'dotnet'
$PythonRoot = $null
$DevConPath = $null
Assert-Failure { Test-StageInputs } '*Stage requires*'
$PythonRoot = Join-Path $fixture 'python'
$DevConPath = Join-Path $fixture 'devcon.exe'
$source = Join-Path $fixture 'source'
Assert-Failure { Test-StageInputs } '*input is missing*'
Write-Output 'PASS: missing runtime arguments and missing files are rejected during preflight.'
