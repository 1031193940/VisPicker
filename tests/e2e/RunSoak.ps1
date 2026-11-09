[CmdletBinding()]
param(
    [ValidateRange(1, 604800)][int]$Seconds = 60,
    [ValidateRange(1, 3600)][int]$IntervalSeconds = 30,
    [string]$Output,
    [string]$Executable,
    [switch]$DiagnoseMemory,
    [switch]$DiagnoseIsolatedMemory
)
$ErrorActionPreference = 'Stop'
if ($DiagnoseMemory -and $DiagnoseIsolatedMemory) { throw 'Choose one memory diagnostic mode.' }
$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if (-not $Executable) { $Executable = Join-Path $projectRoot 'out\build\x64-Release\VisPickSoak.exe' }
$resolvedExecutable = [System.IO.Path]::GetFullPath($Executable)
if (-not (Test-Path -LiteralPath $resolvedExecutable -PathType Leaf)) {
    throw "Build the VisPickSoak target first: $resolvedExecutable"
}
if (-not $Output) { $Output = Join-Path $projectRoot ('out\reports\soak-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
$resolvedOutput = [System.IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath (Join-Path $resolvedOutput 'report.json')) {
    throw 'Choose a fresh output directory; previous soak reports are never overwritten.'
}
[System.IO.Directory]::CreateDirectory($resolvedOutput) | Out-Null
$metadata = [ordered]@{
    executable = $resolvedExecutable
    executable_sha256 = (Get-FileHash -LiteralPath $resolvedExecutable -Algorithm SHA256).Hash
    requested_seconds = $Seconds
    interval_seconds = $IntervalSeconds
    memory_diagnostics_enabled = [bool]($DiagnoseMemory -or $DiagnoseIsolatedMemory)
    gif_validation_process = if ($DiagnoseMemory) { 'soak' } else { 'persistent_child' }
    scheduling_tolerance_seconds = 0.250
    started_utc = [DateTime]::UtcNow.ToString('o')
    coverage = 'Owned passive 256x144 pattern; production capture, annotation, PNG, pin create/close, one-second GIF/WIC. No clipboard or full interactive Overlay workflow.'
    eight_hour_requirement = 'Optional benchmark: actual elapsed time at least 28800 seconds, 30-second cadence, and no diagnostic instrumentation.'
    timing_method = 'Fixed interval start schedule, no catch-up cycles; starts more than 250ms late fail. GIF duration is compared with actual capture timestamps within one 10ms GIF tick.'
}
$metadata | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $resolvedOutput 'invocation.json') -Encoding UTF8
Write-Host "Running soak for $Seconds seconds; output: $resolvedOutput"
Write-Host 'A small passive test pattern and the production recording border/bar appear briefly. Ctrl+C stops and retains the partial report.'
Push-Location $projectRoot
try {
    $soakArguments = @('--seconds', $Seconds, '--interval-seconds', $IntervalSeconds, '--output', $resolvedOutput)
    if ($DiagnoseMemory) { $soakArguments += '--diagnose-memory' }
    if ($DiagnoseIsolatedMemory) { $soakArguments += '--diagnose-isolated-memory' }
    & $resolvedExecutable @soakArguments 2>&1 |
        Tee-Object -FilePath (Join-Path $resolvedOutput 'console.log')
    $processExitCode = $LASTEXITCODE
} finally {
    Pop-Location
}
$reportPath = Join-Path $resolvedOutput 'report.json'
if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) { throw "Soak exited without a report (exit $processExitCode)." }
$report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
Write-Host "elapsed=$($report.elapsed_seconds)s cycles=$($report.samples.Count) passed=$($report.passed) eight_hour_passed=$($report.eight_hour_passed)"
if ($processExitCode -ne 0) { throw "Soak exited with $processExitCode; inspect $reportPath and console.log." }
