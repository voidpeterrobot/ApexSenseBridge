param([string]$Executable,[string]$QueryExecutable)
$ErrorActionPreference='Stop'
$root=Join-Path ([IO.Path]::GetTempPath()) ('asb-lifecycle-cli-'+[Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
function Run-Tool([string]$Tool,[string[]]$Arguments,[int]$Expected) {
    $saved=$ErrorActionPreference; $ErrorActionPreference='Continue'
    $lines=@(& $Tool @Arguments 2>&1); $code=$LASTEXITCODE; $ErrorActionPreference=$saved
    if ($code -ne $Expected) { throw "Unexpected exit $code for $Arguments : $lines" }
    if ($Expected -eq 2 -and ($lines -join '') -notmatch 'DIAGNOSTIC COMPLETE.*UNVERIFIED') { throw 'Exit 2 message missing' }
}
function Hash-Text([string]$text) {
    $sha=[Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text))).Replace('-','').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
$review=Join-Path $root 'review.json'; $rehearsal=Join-Path $root 'rehearsal'; $blocked=Join-Path $root 'must-not-exist'
Run-Tool $Executable @('prepare-grip-lifecycle','--synthetic','--output',$review) 0
Run-Tool $Executable @('rehearse-grip-lifecycle','--synthetic','--manifest',$review,'--output',$rehearsal) 2
$r=[IO.File]::ReadAllText($review) | ConvertFrom-Json
if ($r.scope -ne 'grip-mode-only-lifecycle-v1' -or $r.phases.preflight.Count -ne 14 -or $r.phases.postflight.Count -ne 14 -or $r.phases.active.Count -ne 4) { throw 'Wrong lifecycle plan' }
if (($r.phases.active.payload_hex -join ',') -ne '0112024000,011200,01100140,01110140') { throw 'Wrong mode writes' }
if ($r.limits.exchange_ms -ne 600 -or $r.limits.active_seconds -ne 5 -or $r.limits.session_seconds -ne 75 -or $r.limits.waveform_writes -ne 0) { throw 'Wrong limits' }
$s=[IO.File]::ReadAllText((Join-Path $rehearsal 'summary.json')) | ConvertFrom-Json
$result=[IO.File]::ReadAllText((Join-Path $rehearsal 'grip-lifecycle.json')) | ConvertFrom-Json
if (!$s.complete -or $s.query_write_attempts -ne 28 -or $s.actuator_write_attempts -ne 4 -or $s.physical_actuation_attempted -or $s.report_io_kind -ne 'simulated') { throw 'Wrong rehearsal counters' }
if (!$result.sequence_complete -or !$result.postflight_matches -or $result.restoration_verified -or !$result.device_state_uncertain -or $result.failure) { throw 'Wrong diagnostic result' }
if (($result.restore_replies -join ',') -ne 'captured_zero_count_value_1_unverified,captured_zero_count_value_1_unverified') { throw 'Anomaly evidence lost' }
$display=& (Join-Path $PSScriptRoot '..\scripts\Show-Apex6ExperimentDiagnostics.ps1') -Run $rehearsal
if (($display -join '') -notmatch 'DIAGNOSTIC COMPLETE \(exit 2\).*UNVERIFIED') { throw 'Display mislabels diagnostic' }
foreach ($command in @('approve-grip-lifecycle','execute-grip-lifecycle','--grip-lifecycle-worker')) {
    Run-Tool $QueryExecutable @($command,'--manifest',$review,'--approval','absent','--output',$blocked) 1
}
Run-Tool $Executable @('approve-grip-lifecycle','--manifest',$review,'--output',$blocked) 1
Run-Tool $Executable @('--grip-lifecycle-worker','--manifest',$review,'--approval','absent','--output',$blocked) 1
# Non-openable synthetic binding throughout. Only refusal paths before discovery.
$physical=Join-Path $root 'physical-fixture.asb'
[IO.File]::WriteAllText($physical,$r.baseline.Replace("grip-only`nsynthetic`nsynthetic`n","grip-only`nphysical`nshared`n"))
$physicalReview=Join-Path $root 'physical-review.json'
Run-Tool $Executable @('prepare-grip-lifecycle','--baseline',$physical,'--output',$physicalReview) 0
Run-Tool $Executable @('approve-grip-lifecycle','--manifest',$physicalReview,'--output',$blocked) 1
$text=[IO.File]::ReadAllText($physicalReview);$hash=Hash-Text $text
$approval=Join-Path $root 'approval.asb';$now=[DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
$prefix="ASB_APEX6_GRIP_LIFECYCLE_APPROVAL_V1`ngrip-mode-only-lifecycle-v1`n"
$oldPrefix="ASB_APEX6_GRIP_RESTORE_APPROVAL_V1`ngrip-left-restore-only`n"
foreach ($time in @(0,($now+3600))) {
    [IO.File]::WriteAllText($approval,"$prefix$oldPrefix$hash`n$time`n1 1 1 1 1 1 1 1 1 1 1 1`n")
    Run-Tool $Executable @('execute-grip-lifecycle','--manifest',$physicalReview,'--approval',$approval,'--output',$blocked) 1
}
foreach ($index in 0..11) {
    $flags=@('1')*12;$flags[$index]='0';$joined=$flags -join ' '
    [IO.File]::WriteAllText($approval,"$prefix$oldPrefix$hash`n$now`n$joined`n")
    Run-Tool $Executable @('execute-grip-lifecycle','--manifest',$physicalReview,'--approval',$approval,'--output',$blocked) 1
}
foreach ($old in @($oldPrefix,"ASB_APEX6_GRIP_OBSERVE_APPROVAL_V1`ngrip-left-restore-observe`n","ASB_APEX6_GRIP_APPROVAL_V1`ngrip-only`n1`nASB_APEX6_APPROVAL_V2`n")) {
    [IO.File]::WriteAllText($approval,"$old$hash`n$now`n1 1 1 1 1 1 1 1 1 1 1 1`n")
    Run-Tool $Executable @('execute-grip-lifecycle','--manifest',$physicalReview,'--approval',$approval,'--output',$blocked) 1
}
$wrong='b'*64
[IO.File]::WriteAllText($approval,"$prefix$oldPrefix$wrong`n$now`n1 1 1 1 1 1 1 1 1 1 1 1`n")
Run-Tool $Executable @('execute-grip-lifecycle','--manifest',$physicalReview,'--approval',$approval,'--output',$blocked) 1
# Valid token bytes below are used only with altered reviews, which must fail before discovery.
[IO.File]::WriteAllText($approval,"$prefix$oldPrefix$hash`n$now`n1 1 1 1 1 1 1 1 1 1 1 1`n")
$tampered=Join-Path $root 'tampered.json'
foreach ($changed in @($text.Replace('01100140','01100141'),$text.Replace('600','601'),$text.Replace('individual restores admit','all commands admit'),$text.Replace('"source_sha256":"','"source_sha256":"00'),$text.Replace('"executable_sha256":"','"executable_sha256":"00'))) {
    [IO.File]::WriteAllText($tampered,$changed)
    Run-Tool $Executable @('execute-grip-lifecycle','--manifest',$tampered,'--approval',$approval,'--output',$blocked) 1
}
$queryReview=Join-Path $root 'query-review.json'
Run-Tool $QueryExecutable @('prepare-grip-lifecycle','--baseline',$physical,'--output',$queryReview) 0
Run-Tool $Executable @('execute-grip-lifecycle','--manifest',$queryReview,'--approval',$approval,'--output',$blocked) 1
if (Test-Path -LiteralPath $blocked) { throw 'Refused command produced evidence or launched a worker' }
Write-Output "Lifecycle CLI regressions passed; zero HID opens. Evidence: $root"
