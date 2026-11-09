[CmdletBinding()]
param([string]$Version)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
. (Join-Path $PSScriptRoot 'BuildSource.ps1')
$buildSource = Get-VisPickBuildSource $projectRoot
$binaries = @('VisPick.exe', 'VisPick-x86.exe')
$checksums = foreach ($binary in $binaries) {
    $source = Join-Path $projectRoot "dist\$binary"
    if (-not (Test-Path -LiteralPath $source -PathType Leaf) -or -not (Test-Path -LiteralPath "$source.build.json" -PathType Leaf)) {
        throw 'Run build.ps1 -Analyze for both Release architectures before packaging.'
    }
    $build = Get-Content -LiteralPath "$source.build.json" -Raw | ConvertFrom-Json
    $file = Get-Item -LiteralPath $source
    if (-not $Version) { $Version = $build.version }
    $expectedArchitecture = if ($binary -eq 'VisPick.exe') { 'x64' } else { 'x86' }
    if ($build.name -ne $binary -or $build.architecture -ne $expectedArchitecture -or $build.version -ne $Version -or
        $file.VersionInfo.ProductVersion -ne $Version -or $file.Length -gt 2MB -or $build.bytes -ne $file.Length -or
        $build.source_sha256 -ne $buildSource -or $build.sha256 -ne (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash) {
        throw "Release binary does not match the current build inputs: $binary. Rebuild both architectures."
    }
    [ordered]@{ name = $binary; bytes = $file.Length; sha256 = $build.sha256 }
}
if ($Version -notmatch '^\d+\.\d+\.\d+(?:-[a-zA-Z0-9.-]+)?$') { throw 'Invalid package version.' }
$packageName = "VisPick-$Version"
$packageRoot = Join-Path $projectRoot "dist\$packageName"
$archive = Join-Path $projectRoot "dist\$packageName.zip"
if ((Test-Path -LiteralPath $packageRoot) -or (Test-Path -LiteralPath $archive)) {
    throw 'Package already exists. Preserve it and choose a new version or move the old package explicitly.'
}
[void][IO.Directory]::CreateDirectory($packageRoot)
foreach ($relative in @('dist\VisPick.exe','dist\VisPick-x86.exe','CHANGELOG.md','res\default.ini')) {
    $destination = if ($relative.StartsWith('dist\')) { Join-Path $packageRoot ([IO.Path]::GetFileName($relative)) } else { Join-Path $packageRoot $relative }
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination))
    Copy-Item -LiteralPath (Join-Path $projectRoot $relative) -Destination $destination
}
$readme = Get-Content -LiteralPath (Join-Path $projectRoot 'README.md') -Raw
($readme -split '(?m)^## 构建\r?$', 2)[0].TrimEnd() | Set-Content -LiteralPath (Join-Path $packageRoot 'README.md') -Encoding utf8
$revision = $null
if (Get-Command git -ErrorAction SilentlyContinue) {
    $gitRevision = & git -C $projectRoot rev-parse --verify HEAD 2>$null
    if ($LASTEXITCODE -eq 0 -and $gitRevision) { $revision = $gitRevision.Trim() }
}
[ordered]@{
    version = $Version; created_utc = [DateTime]::UtcNow.ToString('o')
    source_revision = $revision; source_sha256 = $buildSource
    release_status = if ($Version.Contains('-')) { 'candidate' } else { 'stable' }
    files = @($checksums)
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $packageRoot 'manifest.json') -Encoding utf8
Compress-Archive -LiteralPath $packageRoot -DestinationPath $archive -CompressionLevel Optimal
Write-Host $archive
Get-FileHash -LiteralPath $archive -Algorithm SHA256
