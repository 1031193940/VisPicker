param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateSet('crash','terminate','worker-exception','abort','invalid-parameter','purecall','concurrent','stack-overflow')][string]$Mode = 'crash'
)
$ErrorActionPreference = 'Stop'
$probeExecutable = (Resolve-Path -LiteralPath $Executable).Path
$testRoot = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $testRoot) { throw 'Choose a new output directory; existing diagnostics are preserved.' }
[void][IO.Directory]::CreateDirectory($testRoot)
$dumpDirectory = Join-Path $testRoot 'dumps'
$blockedDirectory = Join-Path $testRoot 'blocked-directory'
[IO.File]::WriteAllText($blockedDirectory, 'Preserve this regular file; it cannot be a dump directory.')

function Invoke-Probe([string]$mode) {
    # Windows file paths cannot contain quotes; quote complete paths for spaces.
    $arguments = "$mode `"$dumpDirectory`" `"$blockedDirectory`""
    $process = Start-Process -FilePath $probeExecutable -ArgumentList $arguments -PassThru -WindowStyle Hidden
    try {
        if (-not $process.WaitForExit(15000)) {
            $process.Kill()
            $process.WaitForExit()
            throw 'Crash probe timed out; no prompt or unbounded crash worker is permitted.'
        }
        return $process.ExitCode
    } finally { $process.Dispose() }
}

if ((Invoke-Probe '--install-only') -ne 0) { throw 'Normal crash-handler install/uninstall failed.' }
if (Test-Path -LiteralPath $dumpDirectory) { throw 'Normal installation unexpectedly created a diagnostic directory.' }
$expectedCode = @{
    'crash'='E0421001'; 'terminate'='E0421002'; 'worker-exception'='E0421003'; 'abort'='E0421003'; 'invalid-parameter'='E0421004'
    'purecall'='E0421005'; 'concurrent'='E0421001'; 'stack-overflow'='C00000FD'
}[$Mode]
$crashExit = Invoke-Probe "--$Mode"
if ($crashExit -eq 0) { throw 'Crash probe unexpectedly returned success.' }
$dumps = @(Get-ChildItem -LiteralPath $dumpDirectory -Filter '*.dmp' -File)
if ($dumps.Count -ne 1) { throw "Expected one dump, found $($dumps.Count)." }
$dump = $dumps[0]
$bytes = [IO.File]::ReadAllBytes($dump.FullName)
if ($bytes.Length -lt 32 -or [BitConverter]::ToUInt32($bytes, 0) -ne 0x504D444D) { throw 'Invalid minidump signature/header.' }
$streamCount = [BitConverter]::ToUInt32($bytes, 8)
$directoryOffset = [BitConverter]::ToUInt32($bytes, 12)
if ([uint64]$directoryOffset + [uint64]$streamCount * 12 -gt $bytes.Length) { throw 'Invalid minidump stream directory.' }
$exceptionFound = $false
$actualPathFound = $false
$architecture = $null
for ($streamIndex = 0; $streamIndex -lt $streamCount; ++$streamIndex) {
    $entry = [int]($directoryOffset + $streamIndex * 12)
    $type = [BitConverter]::ToUInt32($bytes, $entry)
    $size = [BitConverter]::ToUInt32($bytes, $entry + 4)
    $offset = [BitConverter]::ToUInt32($bytes, $entry + 8)
    if ([uint64]$offset + $size -gt $bytes.Length) { throw 'Minidump stream exceeds file bounds.' }
    if ($type -eq 6) {
        if ($size -lt 168) { throw 'Truncated exception stream.' }
        $exceptionCode = [BitConverter]::ToUInt32($bytes, [int]$offset + 8)
        $exceptionFound = $exceptionCode -eq [Convert]::ToUInt32($expectedCode, 16)
    } elseif ($type -eq 7) {
        if ($size -lt 2) { throw 'Truncated system information stream.' }
        $architecture = [BitConverter]::ToUInt16($bytes, [int]$offset)
    } elseif ($type -eq 11) {
        $recordedPath = [Text.Encoding]::Unicode.GetString($bytes, [int]$offset, [int]$size).TrimEnd([char]0)
        $actualPathFound = $recordedPath -eq $dump.FullName
    }
}
if (-not $exceptionFound) { throw 'Expected exception code was not preserved.' }
if (-not $actualPathFound) { throw 'Dump did not record its actual fallback path.' }
if ($architecture -notin @(0,9)) { throw 'Missing or unexpected x86/x64 architecture stream.' }
if ([IO.File]::ReadAllText($blockedDirectory) -ne 'Preserve this regular file; it cannot be a dump directory.') { throw 'Fallback changed the blocked primary file.' }
[ordered]@{
    checks_passed = $true
    normal_install_created_files = $false
    fallback_verified = $true
    mode = $Mode
    exception_code = "0x$expectedCode"
    processor_architecture = $architecture
    process_exit = $crashExit
    dump = $dump.FullName
    dump_bytes = $dump.Length
    sha256 = (Get-FileHash -LiteralPath $dump.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $testRoot 'result.json') -Encoding utf8
Get-Content -LiteralPath (Join-Path $testRoot 'result.json')
