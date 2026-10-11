param(
    [ValidateSet('x64', 'Win32')]
    [string[]]$Platforms = @('x64', 'Win32')
)

$ErrorActionPreference = 'Stop'
$repository = Split-Path $PSScriptRoot -Parent
$solutionDirectory = Join-Path $repository 'Plugins\uWindowCapture'
$project = Join-Path $solutionDirectory 'uWindowCapture\uWindowCapture.vcxproj'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw 'Visual Studio 2022 C++ build tools are required.' }
$msbuild = Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'
foreach ($platform in $Platforms) {
    $output = Join-Path $repository "Builds\Native\$platform"
    $intermediate = Join-Path $output 'obj'
    New-Item -ItemType Directory -Path $output, $intermediate -Force | Out-Null
    & $msbuild $project /nologo /verbosity:minimal /m `
        /p:Configuration=Release "/p:Platform=$platform" /p:PlatformToolset=v143 `
        "/p:SolutionDir=$solutionDirectory\" "/p:OutDir=$output\" "/p:IntDir=$intermediate\" `
        /p:PostBuildEventUseInBuild=false
    if ($LASTEXITCODE -ne 0) { throw "Native $platform build failed." }
    $architecture = if ($platform -eq 'x64') { 'x86_64' } else { 'x86' }
    $destination = Join-Path $repository "Assets\uWindowCapture\Plugins\$architecture\uWindowCapture.dll"
    Copy-Item -LiteralPath (Join-Path $output 'uWindowCapture.dll') -Destination $destination
    Get-FileHash -LiteralPath $destination -Algorithm SHA256
}
