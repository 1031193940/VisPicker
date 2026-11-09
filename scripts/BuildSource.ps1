function Get-VisPickBuildSource([string]$ProjectRoot) {
    $inputFiles = @(Get-Item -LiteralPath (Join-Path $ProjectRoot 'CMakeLists.txt'),
        (Join-Path $ProjectRoot 'CMakePresets.json'), (Join-Path $ProjectRoot 'build.ps1'))
    foreach ($directory in @('src','res','scripts','tests')) {
        $inputFiles += Get-ChildItem -LiteralPath (Join-Path $ProjectRoot $directory) -Recurse -File
    }
    $lines = foreach ($file in $inputFiles | Sort-Object FullName) {
        $relative = $file.FullName.Substring($ProjectRoot.Length + 1).Replace('\','/')
        "$relative $((Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash)"
    }
    $hashAlgorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $digest = $hashAlgorithm.ComputeHash([Text.Encoding]::UTF8.GetBytes($lines -join "`n"))
        return [BitConverter]::ToString($digest).Replace('-','')
    } finally { $hashAlgorithm.Dispose() }
}
