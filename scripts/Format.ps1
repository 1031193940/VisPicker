[CmdletBinding()]
param([switch]$Check, [string]$ClangFormat = 'clang-format')
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$version = & $ClangFormat --version
if ($LASTEXITCODE -ne 0 -or $version -notmatch 'version 18\.1\.8(?:\s|$)') {
    throw 'Use clang-format 18.1.8 (VS 2022 LLVM tools or the pinned clang-format Python package).'
}
$files = @(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'src'), (Join-Path $projectRoot 'tests') -Recurse -File |
    Where-Object { $_.Extension -in '.cpp', '.h' -and $_.FullName -notmatch '[\\/]third_party[\\/]' } |
    Sort-Object FullName)
$failed = $false
foreach ($file in $files) {
    [string[]]$arguments = if ($Check) { @('--dry-run', '--Werror') } else { @('-i') }
    & $ClangFormat @arguments --style=file $file.FullName
    if ($LASTEXITCODE -ne 0) { $failed = $true }
}
if ($failed) { throw 'Formatting failed. Run scripts/Format.ps1 with clang-format 18.1.8.' }
Write-Host "Formatting $(if ($Check) { 'check' } else { 'update' }) passed for $($files.Count) project files."
