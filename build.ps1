[CmdletBinding()]
param(
    [ValidateSet('x64','x86','all')][string]$Architecture = 'all',
    [ValidateSet('Release','Debug')][string]$Configuration = 'Release',
    [switch]$Analyze,
    [switch]$RunDesktopTests
)
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
. (Join-Path $projectRoot 'scripts\BuildSource.ps1')
$buildSource = if ($Configuration -eq 'Release') { Get-VisPickBuildSource $projectRoot }
$architectures = if ($Architecture -eq 'all') { @('x64','x86') } else { @($Architecture) }
Push-Location $projectRoot
try {
    foreach ($targetArchitecture in $architectures) {
        . (Join-Path $projectRoot 'scripts\Enter-VsEnvironment.ps1') -Architecture $targetArchitecture
        $preset = "$targetArchitecture-$Configuration"
        $analysisOption = if ($Analyze) { 'ON' } else { 'OFF' }
        & cmake --preset $preset "-DVISPICK_ANALYZE=$analysisOption"
        if ($LASTEXITCODE -ne 0) { throw "Configure failed: $preset" }
        & cmake --build --preset $preset --parallel
        if ($LASTEXITCODE -ne 0) { throw "Build failed: $preset" }
        & ctest --preset $preset
        if ($LASTEXITCODE -ne 0) { throw "Unit tests failed: $preset" }
        $binaryDirectory = Join-Path $projectRoot "out\build\$preset"
        if ($Configuration -eq 'Release') {
            & (Join-Path $projectRoot 'scripts\Check-Imports.ps1') -Executable (Join-Path $binaryDirectory 'VisPick.exe')
            $distribution = Join-Path $projectRoot 'dist'
            New-Item -ItemType Directory -Force -Path $distribution | Out-Null
            $filename = if ($targetArchitecture -eq 'x64') { 'VisPick.exe' } else { 'VisPick-x86.exe' }
            $releaseBinary = Join-Path $binaryDirectory 'VisPick.exe'
            $binary = Get-Item -LiteralPath $releaseBinary
            if ($binary.Length -gt 2MB) { throw "$filename exceeds the 2 MiB size budget." }
            if ((Get-VisPickBuildSource $projectRoot) -ne $buildSource) { throw 'Build inputs changed during compilation. Rebuild before packaging.' }
            $version = $binary.VersionInfo.ProductVersion
            $checksum = (Get-FileHash -LiteralPath $releaseBinary -Algorithm SHA256).Hash
            $symbolDirectory = Join-Path $projectRoot "out\symbols\$version\$targetArchitecture\$checksum"
            [void][IO.Directory]::CreateDirectory($symbolDirectory)
            Copy-Item -LiteralPath (Join-Path $binaryDirectory 'VisPick.pdb') -Destination $symbolDirectory
            Copy-Item -LiteralPath $releaseBinary -Destination (Join-Path $distribution $filename)
            [ordered]@{
                name = $filename; architecture = $targetArchitecture; version = $version
                bytes = $binary.Length; sha256 = $checksum; source_sha256 = $buildSource
            } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $distribution "$filename.build.json") -Encoding utf8
        }
        if ($RunDesktopTests) {
            $reportDirectory = Join-Path $projectRoot "out\reports\$preset"
            & (Join-Path $binaryDirectory 'VisPickTest.exe') --all --output $reportDirectory
            if ($LASTEXITCODE -ne 0) { throw "Desktop validation failed; inspect $reportDirectory" }
        }
    }
} finally { Pop-Location }

