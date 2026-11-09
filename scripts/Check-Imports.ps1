[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [string]$Dumpbin = 'dumpbin.exe'
)
$ErrorActionPreference = 'Stop'
$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
$listing = & $Dumpbin /nologo /imports $resolvedExecutable
if ($LASTEXITCODE -ne 0) { throw "dumpbin could not inspect $resolvedExecutable" }
$allowlistPath = Join-Path $PSScriptRoot 'Win7-Imports.txt'
$allowed = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
foreach ($entry in Get-Content -LiteralPath $allowlistPath) {
    $entry = $entry.Trim()
    if ($entry -and -not $entry.StartsWith('#')) {
        $parts = $entry.Split('!')
        if ($parts.Count -ne 2) { throw "Invalid import baseline entry: $entry" }
        [void]$allowed.Add($parts[0].ToLowerInvariant() + '!' + $parts[1])
    }
}
$library = ''
$libraryImportCount = 0
$totalImportCount = 0
$libraries = [System.Collections.Generic.HashSet[string]]::new()
$violations = [System.Collections.Generic.List[string]]::new()
$delaySection = $false
$protectedDelayLibraries = @('dwmapi.dll','gdiplus.dll','winmm.dll','wtsapi32.dll')
foreach ($line in $listing) {
    if ($line -match 'Section contains the following delay load imports:') { $delaySection = $true; continue }
    if ($line -match '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') {
        if ($library -and $libraryImportCount -eq 0) { $violations.Add("No named imports parsed for $library") }
        $library = $Matches[1].ToLowerInvariant()
        $libraryImportCount = 0
        [void]$libraries.Add($library)
        if (-not $delaySection -and $library -in $protectedDelayLibraries) {
            $violations.Add("$library must load through the system-directory delay hook")
        }
        if ($library -match '^(vcruntime|msvcp|ucrtbase|api-ms-|ext-ms-|clang_rt\.)') {
            $violations.Add("Non-portable runtime dependency: $library")
        }
        continue
    }
    $importPattern = if ($delaySection) { '^\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+([A-Za-z_?@$][A-Za-z0-9_?@$]*)\s*$' }
        else { '^\s+[0-9A-Fa-f]+\s+([A-Za-z_?@$][A-Za-z0-9_?@$]*)\s*$' }
    if ($library -and $line -match $importPattern) {
        $symbol = $Matches[1]
        $identity = "$library!$symbol"
        ++$libraryImportCount
        ++$totalImportCount
        if (-not $allowed.Contains($identity)) { $violations.Add("Unreviewed / unsupported import: $identity") }
    }
    if ($line -match '\[NONAME\]|Ordinal\s+[0-9]+') { $violations.Add("Ordinal imports are not allowed: $($line.Trim())") }
}
if ($library -and $libraryImportCount -eq 0) { $violations.Add("No named imports parsed for $library") }
if ($totalImportCount -eq 0) { throw 'No imports parsed; refusing to treat an empty or unrecognized listing as compatible.' }
$listing | Set-Content -LiteralPath "$resolvedExecutable.imports.txt" -Encoding utf8
if ($violations.Count -gt 0) { throw ($violations -join [Environment]::NewLine) }
Write-Host "Import baseline passed: $totalImportCount symbols, $($libraries.Count) system DLLs. Win7 runtime testing remains required."

