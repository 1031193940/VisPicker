param([ValidateSet('x64','x86')][string]$Architecture = 'x64')
$ErrorActionPreference = 'Stop'
$locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $locator)) { throw 'Visual Studio Installer / vswhere.exe was not found.' }
$installation = & $locator -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Install VS2022 with Desktop development with C++ and MSVC 14.42.' }
$compilerRoot = Join-Path $installation 'VC\Tools\MSVC'
$toolset = Get-ChildItem -LiteralPath $compilerRoot -Directory | Where-Object Name -Like '14.42.*' | Sort-Object Name -Descending | Select-Object -First 1
if (-not $toolset) { throw 'The validated MSVC 14.42 toolset is missing. Install it through VS Installer.' }
$developerCommand = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
# Repeated architecture switches inherit the previous developer PATH. Keep its
# entries once so cmd.exe never expands a duplicated PATH beyond its line limit.
$env:PATH = ($env:PATH -split ';' | Select-Object -Unique) -join ';'
# The installation path comes from vswhere, not user-provided command text.
$environmentLines = & $env:ComSpec /d /s /c "`"$developerCommand`" -no_logo -arch=$Architecture -host_arch=x64 -vcvars_ver=14.42 >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the Visual Studio build environment.' }
foreach ($line in $environmentLines) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}
if ($env:VCToolsVersion -notlike '14.42.*') { throw "Unexpected toolset: $env:VCToolsVersion" }
$cmakeDirectory = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$ninjaDirectory = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$env:PATH = (@($cmakeDirectory, $ninjaDirectory) + ($env:PATH -split ';') | Select-Object -Unique) -join ';'
Write-Host "MSVC $($env:VCToolsVersion.TrimEnd('\')) / $Architecture"

