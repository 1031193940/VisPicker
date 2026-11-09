# Deterministic code-drawn icon, no external graphics tools or downloaded artwork.
$ErrorActionPreference = 'Stop'
$iconPath = Join-Path $PSScriptRoot '..\res\VisPick.ico'
$stream = [System.IO.File]::Create($iconPath)
$writer = [System.IO.BinaryWriter]::new($stream)
try {
    $size = 32
    $maskBytes = 128
    $imageBytes = 40 + $size * $size * 4 + $maskBytes
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]1)
    $writer.Write([byte]$size); $writer.Write([byte]$size)
    $writer.Write([byte]0); $writer.Write([byte]0)
    $writer.Write([uint16]1); $writer.Write([uint16]32)
    $writer.Write([uint32]$imageBytes); $writer.Write([uint32]22)
    $writer.Write([uint32]40); $writer.Write([int]$size); $writer.Write([int]($size * 2))
    $writer.Write([uint16]1); $writer.Write([uint16]32); $writer.Write([uint32]0)
    $writer.Write([uint32]($size * $size * 4))
    1..4 | ForEach-Object { $writer.Write([uint32]0) }
    for ($vertical = $size - 1; $vertical -ge 0; --$vertical) {
        for ($horizontal = 0; $horizontal -lt $size; ++$horizontal) {
            $inside = $horizontal -ge 2 -and $horizontal -lt 30 -and $vertical -ge 2 -and $vertical -lt 30
            $corner = (($horizontal -ge 7 -and $horizontal -le 9) -or ($horizontal -ge 22 -and $horizontal -le 24)) -and (($vertical -ge 7 -and $vertical -le 13) -or ($vertical -ge 19 -and $vertical -le 24))
            $corner = $corner -or ((($vertical -ge 7 -and $vertical -le 9) -or ($vertical -ge 22 -and $vertical -le 24)) -and (($horizontal -ge 7 -and $horizontal -le 13) -or ($horizontal -ge 19 -and $horizontal -le 24)))
            if ($inside) {
                if ($corner) { $writer.Write([uint32]4294967295) }
                else { $writer.Write([uint32]4282022654) }
            } else { $writer.Write([uint32]0) }
        }
    }
    for ($row = 0; $row -lt $size; ++$row) {
        for ($columnByte = 0; $columnByte -lt 4; ++$columnByte) {
            $mask = 0
            for ($bit = 0; $bit -lt 8; ++$bit) {
                $column = $columnByte * 8 + $bit
                if ($row -lt 2 -or $row -ge 30 -or $column -lt 2 -or $column -ge 30) { $mask = $mask -bor (128 -shr $bit) }
            }
            $writer.Write([byte]$mask)
        }
    }
} finally { $writer.Dispose(); $stream.Dispose() }
