[CmdletBinding()]
param(
    [ValidateSet('x64','x86')][string]$Architecture = 'x64',
    [ValidateSet('Release','Debug')][string]$Configuration = 'Release',
    [ValidateRange(0,3600)][int]$IdleSeconds = 0,
    [switch]$FullRecording,
    [string]$Output
)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
. (Join-Path $projectRoot 'scripts\Enter-VsEnvironment.ps1') -Architecture $Architecture
$binaryDirectory = Join-Path $projectRoot "out\build\$Architecture-$Configuration"
if (-not $Output) { $Output = Join-Path $projectRoot ("out\reports\product-$Architecture-$Configuration-" + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
$Output = [IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $Output) { throw 'Use a fresh output directory to preserve previous evidence.' }
[void][IO.Directory]::CreateDirectory($Output)
$results = [Collections.Generic.List[object]]::new()

function Invoke-Check([string]$Name, [string]$File, [string[]]$Arguments = @()) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    & (Join-Path $binaryDirectory $File) @Arguments > (Join-Path $Output "$Name.log") 2>&1
    $code = $LASTEXITCODE
    $results.Add([ordered]@{ name = $Name; exit_code = $code; seconds = $watch.Elapsed.TotalSeconds })
    $summary = [ordered]@{
        architecture = $Architecture; configuration = $Configuration
        recorded_utc = [DateTime]::UtcNow.ToString('o')
        product_sha256 = (Get-FileHash -LiteralPath (Join-Path $binaryDirectory 'VisPick.exe') -Algorithm SHA256).Hash
        physical_display_acceptance = 'not_assessed_by_this_runner'
        checks = @($results.ToArray())
    }
    $summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $Output 'checks.json') -Encoding utf8
    if ($code -ne 0) {
        Get-Content -LiteralPath (Join-Path $Output "$Name.log") -Tail 30
        throw "$Name failed ($code); inspect $Output"
    }
    Write-Host "$Name passed ($([math]::Round($watch.Elapsed.TotalSeconds, 2)) seconds)"
}

Push-Location $projectRoot
try {
    # These tests own their windows. Clipboard checks run in a separate window station.
    # Run serially because multiple desktop fixtures would invalidate pixel comparisons.
    Invoke-Check 'output' 'VisPickOutputTests.exe'
    Invoke-Check 'overlay' 'VisPickOverlayTests.exe' @('--visual-output', (Join-Path $Output 'overlay.png'))
    Invoke-Check 'editor' 'VisPickEditorTests.exe' @((Join-Path $Output 'editor.json'))
    Invoke-Check 'pins' 'VisPickPinTests.exe'
    $recordingArguments = @('--output', (Join-Path $Output 'recording'))
    if ($FullRecording) {
        $width = if ($Architecture -eq 'x64') { '1920' } else { '1280' }
        $height = if ($Architecture -eq 'x64') { '1080' } else { '720' }
        $recordingArguments += @('--seconds','60','--width',$width,'--height',$height)
    }
    Invoke-Check 'recording' 'VisPickRecordingIntegration.exe' $recordingArguments
    Invoke-Check 'lifecycle' 'VisPickAppLifecycleIntegration.exe' @((Join-Path $binaryDirectory 'VisPick.exe'), '--output', (Join-Path $Output 'lifecycle'))
    Invoke-Check 'resident' 'VisPickAppIntegration.exe' @((Join-Path $binaryDirectory 'VisPick.exe'), '--output', (Join-Path $Output 'resident'), '--idle-seconds', "$IdleSeconds")
} finally { Pop-Location }
