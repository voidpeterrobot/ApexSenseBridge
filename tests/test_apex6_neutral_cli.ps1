param([Parameter(Mandatory=$true)][string]$Executable,[Parameter(Mandatory=$true)][string]$QueryExecutable)
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('asb-neutral-cli-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
function Run-Tool([string]$Tool,[string[]]$Arguments,[bool]$Success) {
    $saved=$ErrorActionPreference; $ErrorActionPreference='Continue'
    $lines=@(& $Tool @Arguments 2>&1); $code=$LASTEXITCODE; $ErrorActionPreference=$saved
    if (($code -eq 0) -ne $Success) { throw "Unexpected exit $code for $Arguments : $lines" }
}
function Hash-Text([string]$text) {
    $sha=[Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text))).Replace('-','').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
$review=Join-Path $root 'synthetic.json'; $rehearsal=Join-Path $root 'synthetic'
Run-Tool $Executable @('prepare','--synthetic','--output',$review) $true
Run-Tool $Executable @('rehearse','--synthetic','--manifest',$review,'--output',$rehearsal) $true
$manifest=[IO.File]::ReadAllText($review) | ConvertFrom-Json
if ($manifest.schema -ne 'asb.apex6.neutral-review.v2' -or $manifest.access_mode -ne 'synthetic' -or $manifest.status -ne 'NEUTRAL_ONLY_REQUIRES_FRESH_APPROVAL') { throw 'Wrong neutral review identity' }
$blocked=Join-Path $root 'must-not-exist'
Run-Tool $Executable @('approve-neutral','--manifest',$review,'--output',$blocked) $false
Run-Tool $QueryExecutable @('execute-neutral','--manifest',$review,'--approval','absent','--output',$blocked) $false
Run-Tool $Executable @('--neutral-worker','--manifest',$review,'--approval','absent','--output',$blocked) $false
# All fixtures below retain non-openable synthetic device paths. We only test
# refusals BEFORE any discovery/open; never execute a valid physical approval.
$synthetic=[IO.File]::ReadAllText((Join-Path $rehearsal 'snapshot.asb'))
$physical=Join-Path $root 'physical-fixture.asb'
[IO.File]::WriteAllText($physical,$synthetic.Replace("ASB_APEX6_SNAPSHOT_V2`nsynthetic`n","ASB_APEX6_SNAPSHOT_V2`nshared`n"))
$physicalReview=Join-Path $root 'physical-fixture.json'
Run-Tool $Executable @('prepare','--snapshot',$physical,'--output',$physicalReview) $true
$text=[IO.File]::ReadAllText($physicalReview); $hash=Hash-Text $text
# Piped/noninteractive confirmation must not create consent.
Run-Tool $Executable @('approve-neutral','--manifest',$physicalReview,'--output',$blocked) $false
$approval=Join-Path $root 'approval.asb'
$now=[DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
foreach ($timestamp in @(0,($now + 3600))) {
    [IO.File]::WriteAllText($approval,"ASB_APEX6_APPROVAL_V2`n$hash`n$timestamp`n1 1 1 1 1 1 1 1 1 1`n")
    Run-Tool $Executable @('execute-neutral','--manifest',$physicalReview,'--approval',$approval,'--output',$blocked) $false
}
foreach ($index in 0..9) {
    $flags=@('1') * 10; $flags[$index]='0'; $joined=$flags -join ' '
    [IO.File]::WriteAllText($approval,"ASB_APEX6_APPROVAL_V2`n$hash`n$now`n$joined`n")
    Run-Tool $Executable @('execute-neutral','--manifest',$physicalReview,'--approval',$approval,'--output',$blocked) $false
}
$wrongHash='b' * 64
[IO.File]::WriteAllText($approval,"ASB_APEX6_APPROVAL_V2`n$wrongHash`n$now`n1 1 1 1 1 1 1 1 1 1`n")
Run-Tool $Executable @('execute-neutral','--manifest',$physicalReview,'--approval',$approval,'--output',$blocked) $false
$tampered=Join-Path $root 'tampered.json'
foreach ($changed in @($text.Replace('"access_mode":"shared"','"access_mode":"exclusive"'),$text.Replace('600','601'),($text + "`n"))) {
    [IO.File]::WriteAllText($tampered,$changed)
    Run-Tool $Executable @('execute-neutral','--manifest',$tampered,'--approval',$approval,'--output',$blocked) $false
}
$queryReview=Join-Path $root 'query-review.json'
Run-Tool $QueryExecutable @('prepare','--snapshot',$physical,'--output',$queryReview) $true
Run-Tool $Executable @('execute-neutral','--manifest',$queryReview,'--approval',$approval,'--output',$blocked) $false
$legacy=Join-Path $root 'legacy.asb'
[IO.File]::WriteAllText($legacy,$synthetic.Replace("ASB_APEX6_SNAPSHOT_V2`nsynthetic`n","ASB_APEX6_SNAPSHOT_V1`n"))
$legacyReview=Join-Path $root 'legacy.json'
Run-Tool $Executable @('prepare','--snapshot',$legacy,'--output',$legacyReview) $true
Run-Tool $Executable @('rehearse','--snapshot',$legacy,'--manifest',$legacyReview,'--output',(Join-Path $root 'legacy-rehearsal')) $true
Run-Tool $Executable @('approve-neutral','--manifest',$legacyReview,'--output',$blocked) $false
$gripReview=Join-Path $root 'grip-review.json'
Run-Tool $Executable @('prepare-grip','--synthetic','--output',$gripReview) $true
# Even a fresh legacy approval matching the scoped review cannot authorize it.
$gripHash=Hash-Text ([IO.File]::ReadAllText($gripReview))
[IO.File]::WriteAllText($approval,"ASB_APEX6_APPROVAL_V2`n$gripHash`n$now`n1 1 1 1 1 1 1 1 1 1`n")
Run-Tool $Executable @('execute-neutral','--manifest',$gripReview,'--approval',$approval,'--output',$blocked) $false
Run-Tool $Executable @('execute-grip','--manifest',$gripReview,'--approval',$approval,'--output',$blocked) $false
Run-Tool $Executable @('approve-neutral','--manifest',$gripReview,'--output',$blocked) $false
Run-Tool $Executable @('approve-grip','--manifest',$gripReview,'--output',$blocked) $false
$gripRehearsal=Join-Path $root 'grip-rehearsal'
Run-Tool $Executable @('rehearse-grip','--synthetic','--manifest',$gripReview,'--output',$gripRehearsal) $true
$gripText=[IO.File]::ReadAllText((Join-Path $gripRehearsal 'grip-baseline.asb'))
$gripPhysical=Join-Path $root 'grip-physical-fixture.asb'
[IO.File]::WriteAllText($gripPhysical,$gripText.Replace("grip-only`nsynthetic`nsynthetic`n","grip-only`nphysical`nshared`n"))
$gripPhysicalReview=Join-Path $root 'grip-physical-fixture.json'
Run-Tool $Executable @('prepare-grip','--baseline',$gripPhysical,'--output',$gripPhysicalReview) $true
$gripPhysicalText=[IO.File]::ReadAllText($gripPhysicalReview)
$gripObject=$gripPhysicalText | ConvertFrom-Json
if ($gripObject.schema -ne 'asb.apex6.grip-neutral-review.v2' -or $gripObject.status -ne 'NEUTRAL_ONLY_REQUIRES_FRESH_GRIP_APPROVAL') { throw 'Wrong scoped neutral review' }
$gripHash=Hash-Text $gripPhysicalText
Run-Tool $Executable @('approve-grip','--manifest',$gripPhysicalReview,'--output',$blocked) $false # redirected consent refused
foreach ($timestamp in @(0,($now + 3600))) {
    [IO.File]::WriteAllText($approval,"ASB_APEX6_GRIP_APPROVAL_V1`ngrip-only`n1`nASB_APEX6_APPROVAL_V2`n$gripHash`n$timestamp`n1 1 1 1 1 1 1 1 1 1`n")
    Run-Tool $Executable @('execute-grip','--manifest',$gripPhysicalReview,'--approval',$approval,'--output',$blocked) $false
}
[IO.File]::WriteAllText($approval,"ASB_APEX6_GRIP_APPROVAL_V1`ngrip-only`n0`nASB_APEX6_APPROVAL_V2`n$gripHash`n$now`n1 1 1 1 1 1 1 1 1 1`n")
Run-Tool $Executable @('execute-grip','--manifest',$gripPhysicalReview,'--approval',$approval,'--output',$blocked) $false
[IO.File]::WriteAllText($approval,"ASB_APEX6_APPROVAL_V2`n$gripHash`n$now`n1 1 1 1 1 1 1 1 1 1`n")
Run-Tool $Executable @('execute-grip','--manifest',$gripPhysicalReview,'--approval',$approval,'--output',$blocked) $false # matching legacy token
[IO.File]::WriteAllText($approval,"ASB_APEX6_GRIP_APPROVAL_V1`ngrip-only`n1`nASB_APEX6_APPROVAL_V2`n$gripHash`n$now`n1 1 1 1 1 1 1 1 1 1`n")
foreach ($changed in @($gripPhysicalText.Replace('grip-only','full-only'),$gripPhysicalText.Replace('600','601'),$gripPhysicalText.Replace('neutral-review.v2','neutral-review.v1'))) {
    [IO.File]::WriteAllText($tampered,$changed)
    Run-Tool $Executable @('execute-grip','--manifest',$tampered,'--approval',$approval,'--output',$blocked) $false
}
Run-Tool $QueryExecutable @('execute-grip','--manifest',$gripPhysicalReview,'--approval',$approval,'--output',$blocked) $false
Run-Tool $Executable @('--grip-neutral-worker','--manifest',$gripPhysicalReview,'--approval',$approval,'--output',$blocked) $false
$restoreReview=Join-Path $root 'restore-review.json'
Run-Tool $Executable @('prepare-grip-restore','--synthetic','--output',$restoreReview) $true
$restoreObject=[IO.File]::ReadAllText($restoreReview) | ConvertFrom-Json
if ($restoreObject.schema -ne 'asb.apex6.grip-restore-review.v1' -or $restoreObject.scope -ne 'grip-left-restore-only' -or $restoreObject.status -ne 'RESTORE_LEFT_ONLY_REQUIRES_FRESH_APPROVAL') { throw 'Wrong restore review scope' }
if ($restoreObject.phases.active.Count -ne 1 -or $restoreObject.phases.active[0].command -ne 83 -or $restoreObject.phases.active[0].payload_hex -ne '01100140' -or $restoreObject.limits.waveform_requests -ne 0 -or $restoreObject.limits.mode_requests -ne 1) { throw 'Restore plan broadened' }
$restoreRehearsal=Join-Path $root 'restore-rehearsal'
Run-Tool $Executable @('rehearse-grip-restore','--synthetic','--manifest',$restoreReview,'--output',$restoreRehearsal) $true
$restoreSummary=[IO.File]::ReadAllText((Join-Path $restoreRehearsal 'summary.json')) | ConvertFrom-Json
if (!$restoreSummary.complete -or $restoreSummary.query_write_attempts -ne 28 -or $restoreSummary.actuator_write_attempts -ne 1 -or $restoreSummary.physical_actuation_attempted -or $restoreSummary.evidence_scope -ne 'grip-left-restore-only') { throw 'Wrong restore rehearsal result' }
Run-Tool $Executable @('approve-grip-restore','--manifest',$restoreReview,'--output',$blocked) $false
Run-Tool $Executable @('rehearse-grip-restore','--synthetic','--manifest',$gripReview,'--output',$blocked) $false
Run-Tool $Executable @('rehearse-grip','--synthetic','--manifest',$restoreReview,'--output',$blocked) $false
$restorePhysicalReview=Join-Path $root 'restore-physical-fixture.json'
Run-Tool $Executable @('prepare-grip-restore','--baseline',$gripPhysical,'--output',$restorePhysicalReview) $true
$restoreText=[IO.File]::ReadAllText($restorePhysicalReview)
$restoreHash=Hash-Text $restoreText
Run-Tool $Executable @('rehearse-grip-restore','--baseline',$gripPhysical,'--manifest',$restorePhysicalReview,'--output',(Join-Path $root 'restore-physical-rehearsal')) $true
Run-Tool $Executable @('approve-grip-restore','--manifest',$restorePhysicalReview,'--output',$blocked) $false # redirected input
foreach ($timestamp in @(0,($now + 3600))) {
    [IO.File]::WriteAllText($approval,"ASB_APEX6_GRIP_RESTORE_APPROVAL_V1`ngrip-left-restore-only`n$restoreHash`n$timestamp`n1 1 1 1 1 1 1 1 1 1 1 1`n")
    Run-Tool $Executable @('execute-grip-restore','--manifest',$restorePhysicalReview,'--approval',$approval,'--output',$blocked) $false
}
foreach ($index in 0..11) {
    $flags=@('1') * 12; $flags[$index]='0'; $joined=$flags -join ' '
    [IO.File]::WriteAllText($approval,"ASB_APEX6_GRIP_RESTORE_APPROVAL_V1`ngrip-left-restore-only`n$restoreHash`n$now`n$joined`n")
    Run-Tool $Executable @('execute-grip-restore','--manifest',$restorePhysicalReview,'--approval',$approval,'--output',$blocked) $false
}
# A matching, fresh NEUTRAL token is still the wrong authorization type.
[IO.File]::WriteAllText($approval,"ASB_APEX6_GRIP_APPROVAL_V1`ngrip-only`n1`nASB_APEX6_APPROVAL_V2`n$restoreHash`n$now`n1 1 1 1 1 1 1 1 1 1`n")
Run-Tool $Executable @('execute-grip-restore','--manifest',$restorePhysicalReview,'--approval',$approval,'--output',$blocked) $false
[IO.File]::WriteAllText($approval,"ASB_APEX6_GRIP_RESTORE_APPROVAL_V1`ngrip-left-restore-only`n$restoreHash`n$now`n1 1 1 1 1 1 1 1 1 1 1 1`n")
foreach ($changed in @($restoreText.Replace('grip-left-restore-only','grip-only'),$restoreText.Replace('01100140','01110140'),$restoreText.Replace('\u000a','\n'),$restoreText.Replace('600','601'))) {
    [IO.File]::WriteAllText($tampered,$changed)
    Run-Tool $Executable @('execute-grip-restore','--manifest',$tampered,'--approval',$approval,'--output',$blocked) $false
}
Run-Tool $QueryExecutable @('execute-grip-restore','--manifest',$restorePhysicalReview,'--approval',$approval,'--output',$blocked) $false
Run-Tool $Executable @('execute-grip','--manifest',$restorePhysicalReview,'--approval',$approval,'--output',$blocked) $false
Run-Tool $Executable @('--grip-restore-worker','--manifest',$restorePhysicalReview,'--approval',$approval,'--output',$blocked) $false
$observeReview=Join-Path $root 'observe-physical-fixture.json'
Run-Tool $Executable @('prepare-grip-observe','--baseline',$gripPhysical,'--output',$observeReview) $true
$observeText=[IO.File]::ReadAllText($observeReview); $observeObject=$observeText | ConvertFrom-Json
$observeHash=Hash-Text $observeText
if ($observeObject.schema -ne 'asb.apex6.grip-observe-review.v1' -or $observeObject.scope -ne 'grip-left-restore-observe' -or $observeObject.limits.observe_ms -ne 500 -or $observeObject.limits.observe_reports -ne 32 -or $observeObject.phases.postflight.Count -ne 0 -or $observeObject.phases.active.Count -ne 1) { throw 'Wrong observation contract' }
$observeRehearsal=Join-Path $root 'observe-rehearsal'
Run-Tool $Executable @('rehearse-grip-observe','--baseline',$gripPhysical,'--manifest',$observeReview,'--output',$observeRehearsal) $true
$observeSummary=[IO.File]::ReadAllText((Join-Path $observeRehearsal 'summary.json')) | ConvertFrom-Json
$observeCapture=[IO.File]::ReadAllText((Join-Path $observeRehearsal 'restore-observation.json')) | ConvertFrom-Json
if ($observeSummary.complete -or $observeSummary.query_write_attempts -ne 14 -or $observeSummary.actuator_write_attempts -ne 1 -or $observeSummary.physical_actuation_attempted -or !$observeCapture.capture_complete -or $observeCapture.restoration_proven -or $observeCapture.additional_reports -ne 1 -or $observeCapture.first_reply -ne 'GPA6 zero-count envelope error 1') { throw 'Observation promoted to restoration success or missed late-ACK fixture' }
Run-Tool $Executable @('approve-grip-observe','--manifest',$observeReview,'--output',$blocked) $false
Run-Tool $Executable @('rehearse-grip-observe','--baseline',$gripPhysical,'--manifest',$restorePhysicalReview,'--output',$blocked) $false
Run-Tool $Executable @('rehearse-grip-restore','--baseline',$gripPhysical,'--manifest',$observeReview,'--output',$blocked) $false
foreach ($cmd in @('approve-grip-observe','execute-grip-observe','--grip-observe-worker')) { Run-Tool $QueryExecutable @($cmd) $false }
Run-Tool $Executable @('--grip-observe-worker','--manifest',$observeReview,'--approval',$approval,'--output',$blocked) $false
foreach ($timestamp in @(0,($now+3600))) {
    [IO.File]::WriteAllText($approval,"ASB_APEX6_GRIP_OBSERVE_APPROVAL_V1`ngrip-left-restore-observe`n$observeHash`n$timestamp`n1 1 1 1 1 1 1 1 1 1 1 1`n")
    Run-Tool $Executable @('execute-grip-observe','--manifest',$observeReview,'--approval',$approval,'--output',$blocked) $false
}
foreach ($index in 0..11) {
    $flags=@('1') * 12; $flags[$index]='0'; $joined=$flags -join ' '
    [IO.File]::WriteAllText($approval,"ASB_APEX6_GRIP_OBSERVE_APPROVAL_V1`ngrip-left-restore-observe`n$observeHash`n$now`n$joined`n")
    Run-Tool $Executable @('execute-grip-observe','--manifest',$observeReview,'--approval',$approval,'--output',$blocked) $false
}
# Fresh matching-hash tokens from the OTHER policy must fail before discovery.
[IO.File]::WriteAllText($approval,"ASB_APEX6_GRIP_RESTORE_APPROVAL_V1`ngrip-left-restore-only`n$observeHash`n$now`n1 1 1 1 1 1 1 1 1 1 1 1`n")
Run-Tool $Executable @('execute-grip-observe','--manifest',$observeReview,'--approval',$approval,'--output',$blocked) $false
[IO.File]::WriteAllText($approval,"ASB_APEX6_GRIP_OBSERVE_APPROVAL_V1`ngrip-left-restore-observe`n$restoreHash`n$now`n1 1 1 1 1 1 1 1 1 1 1 1`n")
Run-Tool $Executable @('execute-grip-restore','--manifest',$restorePhysicalReview,'--approval',$approval,'--output',$blocked) $false
foreach ($changed in @($observeText.Replace('"observe_ms":500','"observe_ms":501'),$observeText.Replace('"observe_reports":32','"observe_reports":33'),$observeText.Replace('01100140','01110140'))) {
    [IO.File]::WriteAllText($tampered,$changed)
    Run-Tool $Executable @('rehearse-grip-observe','--baseline',$gripPhysical,'--manifest',$tampered,'--output',$blocked) $false
}
if (Test-Path -LiteralPath $blocked) { throw 'Refused approval/execution caused output side effects' }
$supervised=Join-Path $root 'supervised'
Run-Tool $Executable @('selftest-supervisor','--output',$supervised) $true
Run-Tool $Executable @('selftest-supervisor','--simulate-timeout','--output',(Join-Path $root 'timeout')) $false
Write-Output "Neutral CLI synthetic rehearsal, approval refusals, legacy handling, manifest tampering and worker supervision passed; no hardware commands. Evidence: $root"
