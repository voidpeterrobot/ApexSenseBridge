# Passive, address-filtered USBPcap recording. No controller output or service changes.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidatePattern('^\\\\\.\\USBPcap[1-9][0-9]*$')][string]$CaptureInterface,
    [Parameter(Mandatory=$true)][ValidateRange(1,127)][int]$DeviceAddress,
    [Parameter(Mandatory=$true)][string]$Output,
    [ValidateRange(3,180)][int]$Seconds = 120,
    [string]$CaptureExecutable = 'C:\Program Files\USBPcap\USBPcapCMD.exe'
)
$ErrorActionPreference = 'Stop'
$outputPath = [IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $outputPath) { throw 'Output directory must be new.' }
if ($outputPath.Contains('"') -or $outputPath.Contains([char]10) -or $outputPath.Contains([char]13)) { throw 'Invalid output path.' }
if (-not (Test-Path -LiteralPath $CaptureExecutable -PathType Leaf)) { throw 'USBPcapCMD is not installed.' }
[IO.Directory]::CreateDirectory($outputPath) | Out-Null
$capturePath = Join-Path $outputPath 'reference.pcap'
$stopPath = Join-Path $outputPath 'stop.request'
function Start-CaptureProcess([string]$arguments) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $CaptureExecutable
    $info.Arguments = $arguments
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    return [Diagnostics.Process]::Start($info)
}
$mapping = Start-CaptureProcess ('--extcap-interface ' + $CaptureInterface + ' --extcap-config')
if (-not $mapping.WaitForExit(10000)) { $mapping.Kill(); throw 'Capture mapping enumeration timed out.' }
$mappingText = $mapping.StandardOutput.ReadToEnd()
$mappingError = $mapping.StandardError.ReadToEnd()
[IO.File]::WriteAllText((Join-Path $outputPath 'mapping.txt'),$mappingText)
[IO.File]::WriteAllText((Join-Path $outputPath 'mapping-error.txt'),$mappingError)
if ($mapping.ExitCode -ne 0 -or $mappingText -notmatch ('\{value=' + $DeviceAddress + '\}\{display=[^\r\n]+\}\{enabled=true\}')) {
    throw 'Selected USB address is absent from the fresh mapping; no recording started.'
}
$capture = $null
$stopKind = 'not_started'
$forced = $false
$started = [DateTime]::UtcNow
try {
    $capture = Start-CaptureProcess ('-d ' + $CaptureInterface + ' --devices ' + $DeviceAddress + ' --inject-descriptors -o "' + $capturePath + '"')
    $stdout = $capture.StandardOutput.ReadToEndAsync()
    $stderr = $capture.StandardError.ReadToEndAsync()
    if ($capture.WaitForExit(500)) { throw 'USBPcap exited during startup; inspect capture logs.' }
    [IO.File]::WriteAllText((Join-Path $outputPath 'recording.json'),('{"pid":' + $capture.Id + ',"address":' + $DeviceAddress + '}'))
    Write-Output ('PASSIVE RECORDING ACTIVE: ' + $outputPath)
    Write-Output 'This records only the selected USB address. No HID output, application launch or service change is performed.'
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while (-not $capture.WaitForExit(250)) {
        if (Test-Path -LiteralPath $stopPath) { $stopKind = 'stop_marker'; break }
        if ($timer.Elapsed.TotalSeconds -ge $Seconds) { $stopKind = 'duration_limit'; break }
        if ((Test-Path -LiteralPath $capturePath) -and (Get-Item -LiteralPath $capturePath).Length -ge 33554432) { $stopKind = 'size_limit'; break }
    }
    if ($capture.HasExited) { $stopKind = 'capture_process_exited' }
} finally {
    if ($null -ne $capture) {
        if (-not $capture.HasExited) {
            # Some USBPcap versions require a console rather than redirected q.
            # Preserve forced-stop classification and validate complete records offline.
            try { $capture.StandardInput.WriteLine('q'); $capture.StandardInput.Flush() } catch {}
            if (-not $capture.WaitForExit(1500)) { $forced = $true; $capture.Kill(); $capture.WaitForExit() }
        }
        if ($null -ne $stdout) { [IO.File]::WriteAllText((Join-Path $outputPath 'stdout.txt'),$stdout.GetAwaiter().GetResult()) }
        if ($null -ne $stderr) { [IO.File]::WriteAllText((Join-Path $outputPath 'stderr.txt'),$stderr.GetAwaiter().GetResult()) }
    }
    [ordered]@{
        schema='asb.apex6.passive-usb-reference.v1'; started_utc=$started.ToString('o'); ended_utc=[DateTime]::UtcNow.ToString('o')
        capture_interface=$CaptureInterface; device_address=$DeviceAddress; stop_kind=$stopKind; forced_process_stop=$forced
        physical_success_inferred=$false; recovery_inferred=$false
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $outputPath 'session.json') -Encoding UTF8
}
Write-Output ('Recording ended: ' + $stopKind + '; forced process stop=' + $forced + '. Validate capture before drawing conclusions.')
