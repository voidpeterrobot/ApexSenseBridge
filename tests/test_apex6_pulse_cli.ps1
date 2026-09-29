param([string]$Executable,[string]$QueryExecutable)
$ErrorActionPreference='Stop'
$root=Join-Path ([IO.Path]::GetTempPath()) ('asb-pulse-cli-'+[Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
function Run-Tool([string]$Tool,[string[]]$Arguments,[int]$Expected) {
    $saved=$ErrorActionPreference; $ErrorActionPreference='Continue'
    $lines=@(& $Tool @Arguments 2>&1); $code=$LASTEXITCODE; $ErrorActionPreference=$saved
    if ($code -ne $Expected) { throw "Unexpected exit $code for $Arguments : $lines" }
    if ($Expected -eq 2 -and ($lines -join '') -notmatch 'GRIP PULSE DIAGNOSTIC COMPLETE.*operator qualification pending') { throw 'Exit 2 message missing' }
}
function Hash-Text([string]$text) {
    $sha=[Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text))).Replace('-','').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
$review=Join-Path $root 'review.json'; $rehearsal=Join-Path $root 'rehearsal'; $blocked=Join-Path $root 'must-not-exist'
Run-Tool $Executable @('prepare-grip-pulse','--synthetic','--output',$review) 0
Run-Tool $Executable @('rehearse-grip-pulse','--synthetic','--manifest',$review,'--output',$rehearsal) 2
$r=[IO.File]::ReadAllText($review) | ConvertFrom-Json
if ($r.scope -ne 'grip-left-pulse-v4' -or $r.phases.preflight.Count -ne 14 -or $r.phases.postflight.Count -ne 14 -or $r.phases.active.Count -ne 15) { throw 'Wrong pulse plan' }
if (($r.phases.active[0,12,13,14].payload_hex -join ',') -ne '0112024000,011200,01100140,01110140') { throw 'Wrong mode writes' }
if ($r.limits.mode_query_exchange_ms -ne 600 -or $r.limits.waveform_write_ms -ne 4 -or $r.limits.active_seconds -ne 5 -or $r.limits.session_seconds -ne 75 -or $r.limits.waveform_writes -ne 11 -or $r.limits.total_writes -ne 43) { throw 'Wrong limits' }
$fixturePath=Join-Path $PSScriptRoot '..\docs\fixtures\grip-left-pulse-v4.json'
$fixture=[IO.File]::ReadAllText($fixturePath) | ConvertFrom-Json
$sha=[Security.Cryptography.SHA256]::Create()
try { $fixtureHash=[BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($fixturePath))).Replace('-','').ToLowerInvariant() }
finally { $sha.Dispose() }
if ($fixtureHash -ne $r.fixture_sha256) { throw 'Fixture hash mismatch' }
if ($r.signal.software_peak -ne 0.75 -or $r.signal.edge_peak -ne 0.375 -or $r.signal.tone_ms -ne 64 -or $r.signal.frequency_hz -ne 125) { throw 'Wrong v4 amplitude/duration' }
if (($r.signal.left_byte_tables -join ',') -ne '8080808080808080,80a1afa1805e505e,80c3dfc3803c203c') { throw 'Wrong v4 sample tables' }
if ($r.signal.gain -ne 12 -or $r.signal.base_peak -ne 0.0625 -or $r.signal.peak_limit -ne 0.75 -or
    $r.signal.clipped_samples -ne 0 -or $r.signal.overrange_samples -ne 0 -or
    $r.signal.peak_before_limit -ne 0.75 -or $r.signal.peak_after_limit -ne 0.75) { throw 'Wrong shared strength policy/metrics' }
for ($i=0;$i -lt 11;$i++) {
    $packet=$r.phases.active[$i+1]
    if ($packet.command -ne 87 -or $packet.windows_report_hex -ne $fixture.packets[$i].windows_report_hex -or $packet.nominal_due_us -ne $fixture.packets[$i].nominal_due_us) { throw "Packet $i differs from reviewed artifact" }
}
$s=[IO.File]::ReadAllText((Join-Path $rehearsal 'summary.json')) | ConvertFrom-Json
$result=[IO.File]::ReadAllText((Join-Path $rehearsal 'grip-pulse.json')) | ConvertFrom-Json
if (!$s.complete -or $s.query_write_attempts -ne 28 -or $s.actuator_write_attempts -ne 15 -or $s.physical_actuation_attempted -or $s.report_io_kind -ne 'simulated') { throw 'Wrong rehearsal counters' }
if (!$result.sequence_complete -or !$result.waveform_complete -or !$result.postflight_matches -or $result.restoration_verified -or !$result.operator_qualification_pending -or !$result.device_state_uncertain -or $result.cancelled -or $result.failure -or $result.packets.Count -ne 11) { throw 'Wrong pulse result' }
if (($result.restore_replies -join ',') -ne 'captured_zero_count_value_1_unverified,captured_zero_count_value_1_unverified') { throw 'Anomaly evidence lost' }
for ($i=0;$i -lt 11;$i++) { if ($result.packets[$i].native_submit_us -ne $i*8000 -or $result.packets[$i].lateness_us -ne 0) { throw 'Wrong simulated timing evidence' } }
$display=& (Join-Path $PSScriptRoot '..\scripts\Show-Apex6ExperimentDiagnostics.ps1') -Run $rehearsal
if (($display -join '') -notmatch 'GRIP PULSE DIAGNOSTIC COMPLETE \(exit 2\).*operator qualification pending') { throw 'Display mislabels pulse' }
if (($display -join '') -notmatch 'Reviewed strength: gain 12, base peak 0.0625, peak limit 0.75') { throw 'Strength display missing' }
foreach ($command in @('approve-grip-pulse','execute-grip-pulse','--grip-pulse-worker')) {
    Run-Tool $QueryExecutable @($command,'--manifest',$review,'--approval','absent','--output',$blocked) 1
}
Run-Tool $Executable @('approve-grip-pulse','--manifest',$review,'--output',$blocked) 1
Run-Tool $Executable @('--grip-pulse-worker','--manifest',$review,'--approval','absent','--output',$blocked) 1
# Non-openable synthetic identity. Exercise only paths that refuse before discovery.
$physical=Join-Path $root 'physical-fixture.asb'
[IO.File]::WriteAllText($physical,$r.baseline.Replace("grip-only`nsynthetic`nsynthetic`n","grip-only`nphysical`nshared`n"))
$physicalReview=Join-Path $root 'physical-review.json'
Run-Tool $Executable @('prepare-grip-pulse','--baseline',$physical,'--output',$physicalReview) 0
Run-Tool $Executable @('approve-grip-pulse','--manifest',$physicalReview,'--output',$blocked) 1
$text=[IO.File]::ReadAllText($physicalReview);$hash=Hash-Text $text
$approval=Join-Path $root 'approval.asb';$now=[DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
$prefix="ASB_APEX6_GRIP_PULSE_APPROVAL_V1`ngrip-left-pulse-v4`n1 1`n"
$oldPrefix="ASB_APEX6_GRIP_RESTORE_APPROVAL_V1`ngrip-left-restore-only`n"
# Even with current review hash and confirmations, older pulse scopes must refuse.
foreach ($oldScope in @('grip-left-pulse-v1','grip-left-pulse-v2','grip-left-pulse-v3')) {
    [IO.File]::WriteAllText($approval,"ASB_APEX6_GRIP_PULSE_APPROVAL_V1`n$oldScope`n1 1`n$oldPrefix$hash`n$now`n1 1 1 1 1 1 1 1 1 1 1 1`n")
    Run-Tool $Executable @('execute-grip-pulse','--manifest',$physicalReview,'--approval',$approval,'--output',$blocked) 1
}
foreach ($time in @(0,($now+3600))) {
    [IO.File]::WriteAllText($approval,"$prefix$oldPrefix$hash`n$time`n1 1 1 1 1 1 1 1 1 1 1 1`n")
    Run-Tool $Executable @('execute-grip-pulse','--manifest',$physicalReview,'--approval',$approval,'--output',$blocked) 1
}
foreach ($index in 0..11) {
    $flags=@('1')*12;$flags[$index]='0';$joined=$flags -join ' '
    [IO.File]::WriteAllText($approval,"$prefix$oldPrefix$hash`n$now`n$joined`n")
    Run-Tool $Executable @('execute-grip-pulse','--manifest',$physicalReview,'--approval',$approval,'--output',$blocked) 1
}
foreach ($flags in @('0 1','1 0','0 0')) {
    [IO.File]::WriteAllText($approval,"ASB_APEX6_GRIP_PULSE_APPROVAL_V1`ngrip-left-pulse-v4`n$flags`n$oldPrefix$hash`n$now`n1 1 1 1 1 1 1 1 1 1 1 1`n")
    Run-Tool $Executable @('execute-grip-pulse','--manifest',$physicalReview,'--approval',$approval,'--output',$blocked) 1
}
foreach ($old in @($oldPrefix,"ASB_APEX6_GRIP_LIFECYCLE_APPROVAL_V1`ngrip-mode-only-lifecycle-v1`n$oldPrefix","ASB_APEX6_GRIP_OBSERVE_APPROVAL_V1`ngrip-left-restore-observe`n","ASB_APEX6_GRIP_APPROVAL_V1`ngrip-only`n1`nASB_APEX6_APPROVAL_V2`n")) {
    [IO.File]::WriteAllText($approval,"$old$hash`n$now`n1 1 1 1 1 1 1 1 1 1 1 1`n")
    Run-Tool $Executable @('execute-grip-pulse','--manifest',$physicalReview,'--approval',$approval,'--output',$blocked) 1
}
$wrong='b'*64
[IO.File]::WriteAllText($approval,"$prefix$oldPrefix$wrong`n$now`n1 1 1 1 1 1 1 1 1 1 1 1`n")
Run-Tool $Executable @('execute-grip-pulse','--manifest',$physicalReview,'--approval',$approval,'--output',$blocked) 1
# Correct token is paired ONLY with altered reviews, which refuse before discovery.
[IO.File]::WriteAllText($approval,"$prefix$oldPrefix$hash`n$now`n1 1 1 1 1 1 1 1 1 1 1 1`n")
$tampered=Join-Path $root 'tampered.json'
foreach ($changed in @($text.Replace('01100140','01100141'),$text.Replace('8000','8001'),$text.Replace('unsolicited waveform-phase input fatal','allow unsolicited'),$text.Replace('80a1afa1','80838483'),$text.Replace('"gain":12','"gain":16'),$text.Replace('"peak_limit":0.75','"peak_limit":1'),$text.Replace('"fixture_sha256":"','"fixture_sha256":"00'),$text.Replace('"source_sha256":"','"source_sha256":"00'),$text.Replace('"executable_sha256":"','"executable_sha256":"00'))) {
    if ($changed -eq $text) { throw 'Tamper test did not change text' }
    [IO.File]::WriteAllText($tampered,$changed)
    Run-Tool $Executable @('execute-grip-pulse','--manifest',$tampered,'--approval',$approval,'--output',$blocked) 1
}
$queryReview=Join-Path $root 'query-review.json'
Run-Tool $QueryExecutable @('prepare-grip-pulse','--baseline',$physical,'--output',$queryReview) 0
Run-Tool $Executable @('execute-grip-pulse','--manifest',$queryReview,'--approval',$approval,'--output',$blocked) 1
if (Test-Path -LiteralPath $blocked) { throw 'Refused command produced evidence or launched a worker' }
# Exercise parameter defaults in Windows PowerShell without entering approval or opening hardware.
$launcher=Join-Path $PSScriptRoot '..\scripts\Invoke-Apex6GripPulseTest.ps1'
$saved=$ErrorActionPreference; $ErrorActionPreference='Continue'
$launchText=@(& powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $launcher -Baseline absent -Confirmation absent2 -Output $blocked 2>&1)
$launchCode=$LASTEXITCODE; $ErrorActionPreference=$saved
if ($launchCode -eq 0 -or ($launchText -join '') -match 'ParameterArgumentValidationErrorEmptyStringNotAllowed') { throw 'Launcher parameter default regression' }
# Real supervisor/job/handle inheritance; worker transport is always FakeIo.
Run-Tool $Executable @('selftest-grip-pulse','--output',(Join-Path $root 'supervised')) 2
Run-Tool $Executable @('selftest-grip-pulse-cancel','--output',(Join-Path $root 'cancelled')) 1
$cancelled=[IO.File]::ReadAllText((Join-Path $root 'cancelled\worker\grip-pulse.json')) | ConvertFrom-Json
if (!$cancelled.cancelled -or $cancelled.query_writes -ne 0 -or $cancelled.mode_writes -ne 0 -or $cancelled.waveform_writes -ne 0) { throw 'Inherited cancellation did not stop before traffic' }
Write-Output "Pulse CLI regressions passed; zero HID opens. Evidence: $root"
