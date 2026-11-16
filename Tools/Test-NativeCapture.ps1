param([int]$Windows = 8, [int]$Width = 1280, [int]$Height = 720, [int]$Seconds = 8, [int]$AnimatedWindows = $Windows,
    [int]$AdapterIndex = 0, [switch]$Desktop, [int]$MonitorIndex = -1, [switch]$DisableMips,
    [ValidateSet('Auto','WindowsGraphicsCapture','BitBlt')][string]$CaptureMode = 'Auto',
    [ValidateRange(0,3)][int]$InitialWgcFailures = 0)
$ErrorActionPreference = 'Stop'
$repository = Split-Path $PSScriptRoot -Parent
$source = Join-Path $repository 'Plugins\uWindowCapture\uWindowCapture'
$output = Join-Path $repository 'Builds\NativeCaptureTests'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$env:PATH = (Split-Path $vswhere -Parent) + ';' + $env:PATH
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw 'Visual Studio C++ build tools are required.' }
$vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
$sources = @(Get-ChildItem -LiteralPath $source -Filter '*.cpp' -File | ForEach-Object { '"' + $_.FullName + '"' })
$sources += '"' + (Join-Path $repository 'Tests\Native\CaptureThroughputTests.cpp') + '"'
$build = 'call "' + $vcvars + '" >nul && cd /d "' + $output +
    '" && cl /nologo /std:c++17 /EHsc /MT /O2 /DUWC_NATIVE_TESTS /I"' + $source +
    '" /I"' + (Join-Path $source 'Include') + '" ' + ($sources -join ' ') +
    ' /Fe:NativeCaptureTests.exe /link windowsapp.lib d3d11.lib dxgi.lib dwmapi.lib user32.lib gdi32.lib ole32.lib psapi.lib'
& $env:ComSpec /c $build
if ($LASTEXITCODE -ne 0) { throw 'Native capture throughput build failed.' }
$mode = @{ Auto = 3; WindowsGraphicsCapture = 2; BitBlt = 1 }[$CaptureMode]
& (Join-Path $output 'NativeCaptureTests.exe') $Windows $Width $Height $Seconds $AnimatedWindows $AdapterIndex ([int]$Desktop.IsPresent) $MonitorIndex ([int](!$DisableMips.IsPresent)) $mode $InitialWgcFailures
if ($LASTEXITCODE -ne 0) { throw 'Native capture throughput target failed.' }
