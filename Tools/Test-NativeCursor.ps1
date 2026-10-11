$ErrorActionPreference = 'Stop'
$repository = Split-Path $PSScriptRoot -Parent
$source = Join-Path $repository 'Plugins\uWindowCapture\uWindowCapture'
$output = Join-Path $repository 'Builds\NativeCursorTests'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$env:PATH = (Split-Path $vswhere -Parent) + ';' + $env:PATH
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw 'Visual Studio C++ build tools are required.' }
$vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
$sources = @(Get-ChildItem -LiteralPath $source -Filter '*.cpp' -File | ForEach-Object { '"' + $_.FullName + '"' })
$sources += '"' + (Join-Path $repository 'Tests\Native\CursorRegressionTests.cpp') + '"'
$build = 'call "' + $vcvars + '" >nul && cd /d "' + $output +
    '" && cl /nologo /std:c++17 /EHsc /MT /O2 /DUWC_NATIVE_TESTS /I"' + $source +
    '" /I"' + (Join-Path $source 'Include') + '" ' + ($sources -join ' ') +
    ' /Fe:NativeCursorTests.exe /link windowsapp.lib d3d11.lib dxgi.lib dwmapi.lib user32.lib gdi32.lib ole32.lib'
& $env:ComSpec /c $build
if ($LASTEXITCODE -ne 0) { throw 'Native cursor regression build failed.' }
& (Join-Path $output 'NativeCursorTests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Native cursor regression tests failed.' }
