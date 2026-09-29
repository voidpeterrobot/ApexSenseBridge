param(
    [Parameter(Mandatory=$true)][string]$Device,
    [Parameter(Mandatory=$true)][string]$InputDevice,
    [string]$Executable = "$PSScriptRoot\..\build-win\Release\ApexSenseBridgeApex6LiveBridge.exe",
    [string]$Library = "$PSScriptRoot\..\build-win\Release\libVIIPER.dll",
    [ValidateRange(0,60)][int]$Seconds = 60,
    [ValidateRange(0,12)][double]$Gain = 1,
    [switch]$Fixture,
    [switch]$Quiet,
    [switch]$IsolatePhysical,
    [string]$XInputDevice,
    [string]$GameExecutable,
    [string]$HidHideCli = 'C:\Program Files\Nefarius Software Solutions\HidHide\x64\HidHideCLI.exe',
    [string]$Output = "$PSScriptRoot\..\artifacts\apex6-live-$(Get-Date -Format yyyyMMdd-HHmmss)"
)
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $Output) { throw 'Output must be a new directory.' }
$null = New-Item -ItemType Directory -Path $Output
$Output = (Resolve-Path -LiteralPath $Output).Path
$isolationPlan=$null
$isolationLock=$null
$isolationLocked=$false
$result=1
$isolationArgs=@()
try {
if (-not $IsolatePhysical) { throw 'Live v2 requires -IsolatePhysical and the exact XUSB function; unisolated physical execution is disabled.' }
if ($IsolatePhysical) {
    . "$PSScriptRoot\Apex6HidHide.ps1"
    $Executable=(Resolve-Path -LiteralPath $Executable).Path
    $HidHideCli=(Resolve-Path -LiteralPath $HidHideCli).Path
    $isolationLock=[Threading.Mutex]::new($false,'Local\ApexSenseBridge-Apex6-HidHide-session')
    try { $isolationLocked=$isolationLock.WaitOne(0) } catch [Threading.AbandonedMutexException] { $isolationLocked=$true }
    if (-not $isolationLocked) { throw 'Another temporary HidHide session is active.' }
    $isolationRoot=Join-Path $Output 'physical-isolation'
    $null=New-Item -ItemType Directory -Path $isolationRoot
    $before=Get-Apex6HidHideState $HidHideCli
    if ($GameExecutable) { Assert-Apex6GameClosed $GameExecutable $before.Apps | ConvertTo-Json | Set-Content -LiteralPath "$isolationRoot\game-closed-before-isolation.json" -Encoding UTF8 }
    Assert-Apex6WritersStopped
    $targets=@(Get-Apex6IsolationTargets $Device $InputDevice $XInputDevice)
    $plan=New-Apex6HidHidePlan $before $Executable $InputDevice $XInputDevice $targets -Strict -AdministrativeCli $HidHideCli
    $inputContainer=(Get-PnpDeviceProperty -InstanceId $InputDevice -KeyName DEVPKEY_Device_ContainerId -ErrorAction Stop).Data
    $xinputContainer=(Get-PnpDeviceProperty -InstanceId $XInputDevice -KeyName DEVPKEY_Device_ContainerId -ErrorAction Stop).Data
    $xinputService=(Get-PnpDeviceProperty -InstanceId $XInputDevice -KeyName DEVPKEY_Device_Service -ErrorAction Stop).Data
    if (-not $inputContainer -or $inputContainer -ne $xinputContainer -or $xinputService -notmatch '^xusb(21|22)$') { throw 'XUSB and HID input are not verified functions of the same Apex6.' }
    $statePath=Join-Path $isolationRoot 'snapshot.json'
    $owner=[Diagnostics.Process]::GetCurrentProcess()
    $isolationToken=[Guid]::NewGuid().ToString('N')
    [ordered]@{ OwnerPid=$PID;OwnerStartTicks=$owner.StartTime.ToUniversalTime().Ticks;Cli=$HidHideCli;Plan=$plan;Token=$isolationToken } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $statePath -Encoding UTF8
    $watchScript=Join-Path $PSScriptRoot 'Watch-Apex6HidHide.ps1'
    $null=Start-Process powershell.exe -WindowStyle Hidden -ArgumentList "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$watchScript`" -StatePath `"$statePath`"" -PassThru
    $readyDeadline=[DateTime]::UtcNow.AddSeconds(10)
    while (-not (Test-Path -LiteralPath "$isolationRoot\watchdog-ready.json")) {
        if ([DateTime]::UtcNow -gt $readyDeadline -or (Test-Path -LiteralPath "$isolationRoot\watchdog-error.txt")) { throw 'HidHide recovery watchdog did not start; no isolation changes applied.' }
        Start-Sleep -Milliseconds 50
    }
    # Set rollback ownership before the first mutation, including partial failure.
    $isolationPlan=$plan
    Enable-Apex6HidHidePlan $HidHideCli $plan | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$isolationRoot\active.json" -Encoding UTF8
    $leaseDeadline=[DateTime]::UtcNow.AddSeconds(10)
    while(-not(Test-Path -LiteralPath "$isolationRoot\lease.txt")){
        if([DateTime]::UtcNow -gt $leaseDeadline -or (Test-Path -LiteralPath "$isolationRoot\isolation-failure.txt")){throw 'Strict isolation health check failed.'}
        Start-Sleep -Milliseconds 50
    }
    $isolationArgs=@('--isolation-lease',"$isolationRoot\lease.txt",'--isolation-token',$isolationToken)
    $positive=@(& $Executable check-input --input-device $InputDevice 2>&1)
    $positiveCode=$LASTEXITCODE
    $positive | Set-Content -LiteralPath "$isolationRoot\bridge-input.txt" -Encoding UTF8
    if ($positiveCode -ne 0) { throw 'Bridge cannot read the physical controller after isolation.' }
    $vendorProbe=@(& $Executable check-vendor-access --device $Device 2>&1)
    $vendorCode=$LASTEXITCODE
    $vendorProbe | Set-Content -LiteralPath "$isolationRoot\vendor-exclusive.txt" -Encoding UTF8
    if($vendorCode -ne 0){throw 'Exclusive vendor access unavailable; another writer or driver may hold the interface.'}
    $probe=Join-Path (Split-Path -Parent $Executable) 'ApexSenseBridgeCapture.exe'
    if (-not $plan.Strict -and $before.Apps -contains $probe) { throw 'Independent visibility probe is already whitelisted; cannot verify game isolation.' }
    $negativeErrorPreference=$ErrorActionPreference
    try { $ErrorActionPreference='Continue';$negative=@(& $probe input-status --input-device $InputDevice --seconds 1 2>&1);$negativeCode=$LASTEXITCODE } finally { $ErrorActionPreference=$negativeErrorPreference }
    $negative | Set-Content -LiteralPath "$isolationRoot\unlisted-input.txt" -Encoding UTF8
    if ($negativeCode -eq 0 -or ($negative -join "`n") -notmatch 'exact Apex6Pro gamepad instance/container required|no unique VID/PID-matched XInput slot') { throw 'Independent process visibility probe did not confirm physical controller isolation.' }
    & "$PSScriptRoot\Test-Apex6XInputIsolation.ps1" -Output "$isolationRoot\unlisted-xinput.json" -RequireNoControllers
}
$fixtureArgs = @()
if ($Fixture) {
    if ($Seconds -lt 20) { throw 'Fixture qualification requires at least 20 seconds.' }
    # New deterministic 20-second 4-channel s16 PCM: ten-second neutral lead
    # permits entry preflight, two seconds left, two right, four alternating,
    # two neutral. No physical I/O occurs while generating the file.
    $fixturePath = "$Output\left-right-alternating.wav"
    $writer = [IO.BinaryWriter]::new([IO.File]::Open($fixturePath, [IO.FileMode]::CreateNew))
    $frames = 48000 * 20
    try {
        $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF'))
        $writer.Write([uint32](36 + $frames * 8))
        $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
        $writer.Write([uint32]16); $writer.Write([uint16]1); $writer.Write([uint16]4)
        $writer.Write([uint32]48000); $writer.Write([uint32]384000)
        $writer.Write([uint16]8); $writer.Write([uint16]16)
        $writer.Write([Text.Encoding]::ASCII.GetBytes('data')); $writer.Write([uint32]($frames * 8))
        for ($i = 0; $i -lt $frames; $i++) {
            $secondsIn = $i / 48000.0
            $left = 0; $right = 0
            if ($secondsIn -ge 10 -and $secondsIn -lt 18) {
                $value = [int16][Math]::Round(2048 * [Math]::Sin(2 * [Math]::PI * 125 * $secondsIn))
                if ($secondsIn -lt 12 -or ($secondsIn -ge 14 -and ([int][Math]::Floor(($secondsIn - 14) * 2) % 2) -eq 0)) { $left = $value } else { $right = $value }
            }
            $writer.Write([int16]0); $writer.Write([int16]0)
            $writer.Write([int16]$left); $writer.Write([int16]$right)
        }
    } finally { $writer.Dispose() }
    $fixtureArgs = @('--fixture', $fixturePath)
}
& $Executable prepare --device $Device --input-device $InputDevice --library $Library --seconds $Seconds --gain ($Gain.ToString([Globalization.CultureInfo]::InvariantCulture)) --output "$Output\prepare" @fixtureArgs @isolationArgs
if ($LASTEXITCODE -ne 0) { throw 'Fresh baseline preparation failed; stop testing.' }
$review = "$Output\prepare\worker\review.asb"
& $Executable rehearse --manifest $review --output "$Output\rehearsal"
if ($LASTEXITCODE -ne 0) { throw 'Offline rehearsal failed; physical execution is unavailable.' }
& $Executable approve --manifest $review --rehearsal "$Output\rehearsal\rehearsal.asb" --output "$Output\approval.asb"
if ($LASTEXITCODE -ne 0) { throw 'Live approval was not granted.' }
if ($IsolatePhysical -and $GameExecutable) { Assert-Apex6GameClosed $GameExecutable (Get-Apex6HidHideState $HidHideCli).Apps | ConvertTo-Json | Set-Content -LiteralPath "$isolationRoot\game-closed-before-execute.json" -Encoding UTF8 }
if ($Quiet) { Clear-Host; Write-Host 'DualSense to Apex6 grips. + / - strength; 0 mute; 1 reset; Q stop; Ctrl+C cancel.' }
& $Executable execute --manifest $review --approval "$Output\approval.asb" --output "$Output\execute"
$result = $LASTEXITCODE
} finally {
    try {
        if ($null -ne $isolationPlan) {
            try {
                $after=Restore-Apex6HidHidePlan $HidHideCli $isolationPlan
                [ordered]@{ by='launcher';after=$after;utc=[DateTime]::UtcNow.ToString('o') } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$isolationRoot\restored.json" -Encoding UTF8
            } catch {
                $_.Exception.ToString() | Set-Content -LiteralPath "$isolationRoot\restore-error.txt" -Encoding UTF8
                [IO.File]::WriteAllText("$isolationRoot\recovery-requested",'launcher restoration failed')
                throw
            }
        } elseif ($IsolatePhysical -and $isolationRoot -and (Test-Path -LiteralPath "$isolationRoot\snapshot.json")) {
            [IO.File]::WriteAllText("$isolationRoot\restored.json",'{"by":"launcher","mutations_applied":false}')
        }
    } finally { if ($isolationLocked) { $isolationLock.ReleaseMutex() };if ($null -ne $isolationLock) { $isolationLock.Dispose() } }
}
if ($Quiet) {
    Write-Host 'Bridge stopped. Evidence saved to' $Output
} elseif ($result -eq 2) {
    Write-Host 'Completed diagnostic. Software evidence does not prove recovery.'
    $assessment = Read-Host 'Describe fixture/game event, input forwarding, left/right identity, gain changes, orderly stop, normal vibration afterward, and any unexpected behavior'
    [ordered]@{ schema='asb.apex6.live.operator.v1'; recorded_utc=[DateTime]::UtcNow.ToString('o'); assessment=$assessment; worker_evidence_modified=$false } |
        ConvertTo-Json | Set-Content -LiteralPath "$Output\operator-assessment.json" -Encoding UTF8
} else {
    Write-Host 'Failure: physical state may be uncertain. Disconnect/power off. No automatic reconnect or cleanup.'
}
exit $result
