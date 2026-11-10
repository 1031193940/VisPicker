param([ValidateSet('x64','x86')][string]$Architecture = 'x64')
$ErrorActionPreference = 'Stop'
$locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $locator)) { throw 'Visual Studio Installer / vswhere.exe was not found.' }
$installation = & $locator -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Install VS2022 with Desktop development with C++ (MSVC 14.44 recommended).' }
$compilerRoot = Join-Path $installation 'VC\Tools\MSVC'
# Prefer the newest installed VS2022 toolset in the supported build range.
# VS2026 / 14.50 and later no longer target Windows 7; imports are checked
# separately after building, and do not replace actual target-system testing.
$toolset = Get-ChildItem -LiteralPath $compilerRoot -Directory |
    Where-Object { $_.Name -match '^14\.4[2-9]\.\d+$' -and
        (Test-Path -LiteralPath (Join-Path $_.FullName "bin\Hostx64\$Architecture\cl.exe")) } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $toolset) { throw 'Install an MSVC 14.42–14.49 toolset through VS Installer; 14.44 is recommended.' }
$selectedToolset = $toolset.Name
$developerCommand = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
# Repeated architecture switches inherit the previous developer PATH. Keep its
# entries once so cmd.exe never expands a duplicated PATH beyond its line limit.
$env:PATH = ($env:PATH -split ';' | Select-Object -Unique) -join ';'
# The installation path comes from vswhere, not user-provided command text.
$environmentLines = & $env:ComSpec /d /s /c "`"$developerCommand`" -no_logo -arch=$Architecture -host_arch=x64 -vcvars_ver=$selectedToolset >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the Visual Studio build environment.' }
foreach ($line in $environmentLines) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}
if ($env:VCToolsVersion.TrimEnd('\') -ne $selectedToolset) { throw "Unexpected toolset: $env:VCToolsVersion (requested $selectedToolset)" }
$cmakeDirectory = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$ninjaDirectory = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$env:PATH = (@($cmakeDirectory, $ninjaDirectory) + ($env:PATH -split ';') | Select-Object -Unique) -join ';'
Write-Host "MSVC $($env:VCToolsVersion.TrimEnd('\')) / $Architecture"
if ([version]$selectedToolset -lt [version]'14.44') {
    Write-Warning 'This installed toolset predates the supported VS2022 17.14 channel. Update to MSVC 14.44 when available.'
}

