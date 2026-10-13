# Locates MSVC and the Windows kit, sets INCLUDE and LIB, and returns tool paths.
# vcvars64.bat is not used because it fails in non-interactive shells.
function Initialize-Toolchain {
  param([string]$UmdfVersion, [string]$WdkRoot = $env:WINAWDL_WDK_ROOT)

  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer vswhere was not found.' }
  $vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (-not $vsRoot) { throw 'An installed MSVC toolchain was not found.' }
  $msvc = Get-ChildItem "$vsRoot\VC\Tools\MSVC" -Directory |
          Sort-Object Name | Select-Object -Last 1
  if (-not $msvc) { throw "MSVC toolset not found under $vsRoot" }

  $kit = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
  $kitVersion = Get-ChildItem "$kit\Include" -Directory |
                Where-Object { Test-Path "$($_.FullName)\um\windows.h" } |
                Sort-Object Name | Select-Object -Last 1 -ExpandProperty Name
  if (-not $kitVersion) { throw "Windows SDK headers not found under $kit" }

  $include = @("$($msvc.FullName)\include", "$kit\Include\$kitVersion\ucrt",
               "$kit\Include\$kitVersion\um", "$kit\Include\$kitVersion\shared")
  $lib = @("$($msvc.FullName)\lib\x64", "$kit\Lib\$kitVersion\ucrt\x64",
           "$kit\Lib\$kitVersion\um\x64")

  if ($UmdfVersion) {
    $umdfInclude = "$kit\Include\wdf\umdf\$UmdfVersion"
    $umdfLib = "$kit\Lib\wdf\umdf\x64\$UmdfVersion"
    $portableWdk = $WdkRoot
    if (-not (Test-Path $umdfInclude) -and (Test-Path "$portableWdk\Include\wdf\umdf\$UmdfVersion")) {
      $umdfInclude = "$portableWdk\Include\wdf\umdf\$UmdfVersion"
      $umdfLib = "$portableWdk\Lib\wdf\umdf\x64\$UmdfVersion"
    }
    if (-not (Test-Path $umdfInclude)) {
      throw "UMDF $UmdfVersion headers not found. Is the Windows Driver Kit installed?"
    }
    $include += $umdfInclude
    $lib += $umdfLib
  }

  $env:INCLUDE = $include -join ';'
  $env:LIB = $lib -join ';'

  $paths = @{
    Cl       = "$($msvc.FullName)\bin\Hostx64\x64\cl.exe"
    InfVerif = "$kit\Tools\$kitVersion\x64\infverif.exe"
    DevCon   = "$kit\Tools\$kitVersion\x64\devcon.exe"
    SignTool = "$kit\bin\$kitVersion\x64\signtool.exe"
    Inf2Cat  = "$kit\bin\$kitVersion\x86\Inf2Cat.exe"
    StampInf = "$kit\bin\$kitVersion\x64\stampinf.exe"
  }
  $portableWdk = $WdkRoot
  if (Test-Path -LiteralPath "$portableWdk\tools\$kitVersion\x64\infverif.exe") {
    foreach ($name in 'InfVerif','DevCon') {
      if (-not (Test-Path -LiteralPath $paths[$name])) { $paths[$name] = "$portableWdk\tools\$kitVersion\x64\$($name.ToLower()).exe" }
    }
    if (-not (Test-Path -LiteralPath $paths.Inf2Cat)) { $paths.Inf2Cat = "$portableWdk\bin\$kitVersion\x86\Inf2Cat.exe" }
    if (-not (Test-Path -LiteralPath $paths.StampInf)) { $paths.StampInf = "$portableWdk\bin\$kitVersion\x64\stampinf.exe" }
  }
  $paths
}
