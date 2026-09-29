param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

# New diagnostic data, NOT the original REPORT.md fixture. No playback or device I/O.
$ErrorActionPreference = 'Stop'
$target = [IO.Path]::GetFullPath($OutputPath)
$frames = 96000 # Two seconds, including 250 ms silence at either end.
$stream = [IO.File]::Open($target, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
$writer = [IO.BinaryWriter]::new($stream)
try {
    $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF'))
    $writer.Write([uint32](36 + $frames * 8))
    $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
    $writer.Write([uint32]16)
    $writer.Write([uint16]1)
    $writer.Write([uint16]4)
    $writer.Write([uint32]48000)
    $writer.Write([uint32]384000)
    $writer.Write([uint16]8)
    $writer.Write([uint16]16)
    $writer.Write([Text.Encoding]::ASCII.GetBytes('data'))
    $writer.Write([uint32]($frames * 8))
    for ($frame = 0; $frame -lt $frames; $frame++) {
        $left = 0
        $right = 0
        if ($frame -ge 12000 -and $frame -lt 84000) {
            # Distinct signed integer patterns expose channel swaps and scaling.
            $left = (($frame * 17) % 1023) - 511
            $right = (($frame * 31 + 73) % 769) - 384
            if ($frame -lt 36000) { $right = 0 }
            if ($frame -ge 60000) { $left = 0 }
        }
        $writer.Write([int16]0)
        $writer.Write([int16]0)
        $writer.Write([int16]$left)
        $writer.Write([int16]$right)
    }
} finally {
    $writer.Dispose()
    $stream.Dispose()
}
Write-Output "Created NEW diagnostic fixture (not the report fixture): $target"
Write-Output 'PCM: 48000 Hz, four channels, s16le, 96000 frames; channels 0/1 silent; peak <= 511.'
