param([Parameter(Mandatory = $true)][string]$Executable)
$ErrorActionPreference = 'Stop'
$tempFile = [IO.Path]::GetTempFileName()
function Make-Wav([int]$Channels, [int]$Frames) {
    $stream = New-Object IO.MemoryStream
    $writer = New-Object IO.BinaryWriter($stream)
    try {
        $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF'))
        $writer.Write([uint32](36 + $Channels * 2 * $Frames))
        $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
        $writer.Write([uint32]16)
        $writer.Write([uint16]1)
        $writer.Write([uint16]$Channels)
        $writer.Write([uint32]48000)
        $writer.Write([uint32](48000 * $Channels * 2))
        $writer.Write([uint16]($Channels * 2))
        $writer.Write([uint16]16)
        $writer.Write([Text.Encoding]::ASCII.GetBytes('data'))
        $writer.Write([uint32]($Channels * 2 * $Frames))
        $writer.Write((New-Object byte[] ($Channels * 2 * $Frames)))
        $writer.Flush()
        return ,$stream.ToArray()
    } finally { $writer.Dispose(); $stream.Dispose() }
}
function Run-Preview([byte[]]$Bytes, [bool]$Valid, [int]$OutputFrames = 0) {
    [IO.File]::WriteAllBytes($tempFile, $Bytes)
    # Capture stderr too, without PowerShell turning native diagnostics into
    # terminating errors. Check the native exit code explicitly afterwards.
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $lines = @(& $Executable render-wav $tempFile 2>&1)
    $code = $LASTEXITCODE
    $ErrorActionPreference = $savedPreference
    if (!$Valid) {
        if ($code -eq 0 -or ($lines -join '') -match '"complete":true') { throw 'Malformed WAV was accepted' }
        return
    }
    if ($code -ne 0) { throw "Valid WAV refused: $lines" }
    $summary = $lines[-1] | ConvertFrom-Json
    if (!$summary.complete -or $summary.output_frames -ne $OutputFrames -or $summary.overrange_samples -ne 0) { throw 'Wrong offline metrics' }
    $packets = [int][Math]::Ceiling($OutputFrames / 8.0)
    if ($lines.Count -ne $packets + 2 -or $summary.neutral_padding_frames -ne ($packets * 8 - $OutputFrames)) { throw 'Wrong packet/padding count' }
    for ($i = 1; $i -le $packets; $i++) {
        $packet = $lines[$i] | ConvertFrom-Json
        if ($packet.private_frame_hex -ne '5aa5571b9880808080808080808080808080808080808080808080808000000a') { throw 'Silence is not neutral grip-only data' }
        if ($packet.nominal_sample_offset -ne ($i - 1) * 8) { throw 'Packet ordering changed' }
    }
}
function Run-Strength([string[]]$Options, [bool]$Valid) {
    $savedPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $lines = @(& $Executable render-wav $tempFile @Options 2>&1)
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $savedPreference }
    if (!$Valid) {
        if ($code -eq 0 -or ($lines -join '') -match '"complete":true|"packet":') { throw 'Invalid strength was accepted or emitted packets' }
        return
    }
    if ($code -ne 0) { throw "Strength preview refused: $lines" }
    return @($lines | ForEach-Object { $_ | ConvertFrom-Json })
}
try {
    Run-Preview (Make-Wav 4 480) $true 10
    Run-Preview (Make-Wav 2 384) $true 8
    Run-Preview (Make-Wav 4 0) $false
    Run-Preview (Make-Wav 4 49) $false
    Run-Preview (Make-Wav 3 480) $false
    $bytes = Make-Wav 4 480
    $bytes[20] = 3 # float format
    Run-Preview $bytes $false
    $bytes = Make-Wav 4 480
    $bytes[24] = 1 # wrong rate
    Run-Preview $bytes $false
    $bytes = Make-Wav 4 480
    $bytes[40] = 255; $bytes[41] = 255; $bytes[42] = 255; $bytes[43] = 255
    Run-Preview $bytes $false
    Run-Preview ([byte[]](0,1,2,3)) $false
    $bytes = Make-Wav 4 480
    Run-Preview ([byte[]]$bytes[0..($bytes.Length-2)]) $false
    $dc = Make-Wav 2 960
    for ($offset = 44; $offset -lt $dc.Length; $offset += 4) {
        $dc[$offset] = 0; $dc[$offset + 1] = 128 # left -1
        $dc[$offset + 2] = 255; $dc[$offset + 3] = 127 # right nearly +1
    }
    [IO.File]::WriteAllBytes($tempFile, $dc)
    $limited = Run-Strength @('--gain','4','--peak-limit','0.0625') $true
    if ($limited[0].gain -ne 4 -or $limited[0].peak_limit -ne 0.0625 -or $limited[0].physical_output) { throw 'Strength metadata mismatch' }
    if ($limited[-1].clipped_samples -le 0 -or $limited[-1].overrange_samples -le 0 -or
        $limited[-1].peak_before_limit -lt 3.9 -or $limited[-1].peak_after_limit -ne 0.0625) { throw 'Missing limiter metrics' }
    $muted = Run-Strength @('--gain','0') $true
    if ($muted[-1].peak_after_limit -ne 0 -or $muted[-1].clipped_samples -ne 0) { throw 'Mute metrics mismatch' }
    foreach ($packet in $muted[1..($muted.Length-2)]) {
        if ($packet.private_frame_hex -ne '5aa5571b9880808080808080808080808080808080808080808080808000000a') { throw 'Zero gain is not neutral' }
    }
    $default = Run-Strength @() $true
    $explicit = Run-Strength @('--peak-limit','1','--gain','1') $true
    if (($default | ConvertTo-Json -Depth 5 -Compress) -ne ($explicit | ConvertTo-Json -Depth 5 -Compress)) { throw 'Default strength changed' }
    $maximum = Run-Strength @('--gain','12','--peak-limit','1') $true
    if ($maximum[0].gain -ne 12 -or $maximum[-1].peak_after_limit -ne 1 -or $maximum[-1].clipped_samples -le 0) { throw 'Strength bound failed' }
    foreach ($invalid in @('-1','12.01','16','nan','inf','1x','1,5','1e999')) {
        Run-Strength @('--gain',$invalid) $false
    }
    foreach ($invalid in @('-0.01','1.01','nan','inf')) { Run-Strength @('--peak-limit',$invalid) $false }
    Run-Strength @('--gain') $false
    Run-Strength @('--gain','1','--gain','2') $false
    Run-Strength @('--peak-limit','1','--peak-limit','0') $false
    Run-Strength @('--unknown','1') $false
    Write-Output 'Offline WAV and strength CLI cases passed; no playback or device calls'
} finally {
    Remove-Item -LiteralPath $tempFile -Force
}
