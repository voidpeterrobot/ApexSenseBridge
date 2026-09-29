param([Parameter(Mandatory=$true)][string]$Executable)
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('asb-apex6-cli-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
function Run-Tool([string[]]$Arguments, [bool]$Success) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $lines = @(& $Executable @Arguments 2>&1)
    $code = $LASTEXITCODE
    $ErrorActionPreference = $saved
    if (($code -eq 0) -ne $Success) { throw "Unexpected exit $code for $Arguments : $lines" }
}
$review = Join-Path $root 'review.json'
$capture = Join-Path $root 'rehearsal'
Run-Tool @('prepare','--synthetic','--output',$review) $true
$manifest = [IO.File]::ReadAllText($review) | ConvertFrom-Json
if ($manifest.status -ne 'REVIEW_ONLY_PHYSICAL_EXECUTION_LOCKED' -or $manifest.phases.active.Count -ne 21) { throw 'Wrong review contract' }
Run-Tool @('rehearse','--synthetic','--manifest',$review,'--output',$capture) $true
$summary = [IO.File]::ReadAllText((Join-Path $capture 'summary.json')) | ConvertFrom-Json
if (!$summary.complete -or $summary.physical_queries -or $summary.physical_actuation_attempted -or $summary.normal_vibration_verified) { throw 'Rehearsal falsely claims hardware evidence' }
$integrity = [IO.File]::ReadAllText((Join-Path $capture 'manifest.json')) | ConvertFrom-Json
$sha = [Security.Cryptography.SHA256]::Create()
try {
    foreach ($file in $integrity.files) {
        $bytes = [IO.File]::ReadAllBytes((Join-Path $capture $file.name))
        $hash = [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-','').ToLowerInvariant()
        if ($hash -ne $file.sha256 -or $bytes.Length -ne $file.bytes) { throw 'Evidence integrity mismatch' }
    }
} finally { $sha.Dispose() }
Run-Tool @('rehearse','--snapshot',(Join-Path $capture 'snapshot.asb'),'--manifest',$review,'--output',(Join-Path $root 'roundtrip')) $true
Run-Tool @('prepare','--synthetic','--output',$review) $false # must not overwrite
Run-Tool @('rehearse','--synthetic','--output',$capture) $false
$tampered = Join-Path $root 'tampered.json'
[IO.File]::WriteAllText($tampered, [IO.File]::ReadAllText($review).Replace('600','601'))
Run-Tool @('rehearse','--synthetic','--manifest',$tampered,'--output',(Join-Path $root 'must-not-exist')) $false
if (Test-Path -LiteralPath (Join-Path $root 'must-not-exist')) { throw 'Tampered manifest caused side effects' }
Run-Tool @('execute','--output',(Join-Path $root 'execute-must-not-exist')) $false
Run-Tool @('snapshot','--device','not-a-device','--output',(Join-Path $root 'snapshot-must-not-exist')) $false
foreach ($mode in @('synthetic','unknown','auto','SHARED')) {
    Run-Tool @('snapshot','--device','not-a-device','--direct-usb-confirmed','--access',$mode,'--output',(Join-Path $root 'snapshot-must-not-exist')) $false
}
Run-Tool @('execute-neutral','--output',(Join-Path $root 'execute-must-not-exist')) $false
Run-Tool @('access-probe','--device','not-a-device','--direct-usb-confirmed','--access','shared','--output',(Join-Path $root 'probe-must-not-exist')) $false
Run-Tool @('--snapshot-worker','--device','not-a-device','--output',(Join-Path $root 'worker-must-not-exist')) $false
Run-Tool @('access-probe','--device','not-a-device','--output',(Join-Path $root 'probe-must-not-exist')) $false
Run-Tool @('--access-probe-worker','--device','not-a-device','--output',(Join-Path $root 'probe-worker-must-not-exist')) $false
Run-Tool @('listen','--device','not-a-device','--output',(Join-Path $root 'listen-must-not-exist')) $false
Run-Tool @('listen','--device','not-a-device','--direct-usb-confirmed','--access','shared','--output',(Join-Path $root 'listen-must-not-exist')) $false
Run-Tool @('listen','--device','not-a-device','--direct-usb-confirmed','--synthetic','--output',(Join-Path $root 'listen-must-not-exist')) $false
Run-Tool @('--listen-worker','--device','not-a-device','--output',(Join-Path $root 'listen-must-not-exist')) $false
if (Test-Path -LiteralPath (Join-Path $root 'listen-must-not-exist')) { throw 'Invalid listener command caused side effects' }
Run-Tool @('examine-ram5','--device','not-a-device','--output',(Join-Path $root 'ram5-must-not-exist')) $false
Run-Tool @('examine-ram5','--device','not-a-device','--direct-usb-confirmed','--access','shared','--output',(Join-Path $root 'ram5-must-not-exist')) $false
Run-Tool @('examine-ram5','--device','not-a-device','--direct-usb-confirmed','--synthetic','--output',(Join-Path $root 'ram5-must-not-exist')) $false
Run-Tool @('--ram5-worker','--device','not-a-device','--output',(Join-Path $root 'ram5-must-not-exist')) $false
if (Test-Path -LiteralPath (Join-Path $root 'ram5-must-not-exist')) { throw 'Invalid RAM5 diagnostic caused side effects' }
Run-Tool @('access-probe','--synthetic','--direct-usb-confirmed','--device','not-a-device','--output',(Join-Path $root 'probe-synthetic-must-not-exist')) $false
foreach ($name in @('execute-must-not-exist','snapshot-must-not-exist','worker-must-not-exist','probe-must-not-exist','probe-worker-must-not-exist','probe-synthetic-must-not-exist')) {
    if (Test-Path -LiteralPath (Join-Path $root $name)) { throw 'Locked/invalid command had side effects' }
}
Run-Tool @('prepare','--synthetic','--snapshot','missing','--output',(Join-Path $root 'ambiguous')) $false
Run-Tool @('rehearse','--synthetic','--unknown') $false
$supervised = Join-Path $root 'supervised'
Run-Tool @('selftest-supervisor','--output',$supervised) $true
$supervisor = [IO.File]::ReadAllText((Join-Path $supervised 'supervisor.json')) | ConvertFrom-Json
if (!$supervisor.worker_exited -or $supervisor.deadline_or_wait_failure) { throw 'Supervised synthetic worker failed' }
$timeoutRoot = Join-Path $root 'timeout'
Run-Tool @('selftest-supervisor','--simulate-timeout','--output',$timeoutRoot) $false
$supervisor = [IO.File]::ReadAllText((Join-Path $timeoutRoot 'supervisor.json')) | ConvertFrom-Json
if (!$supervisor.deadline_or_wait_failure -or !$supervisor.termination_requested -or !$supervisor.recovery_not_inferred) { throw 'Supervisor timeout uncertainty was hidden' }
foreach ($mode in @('exclusive','shared')) {
    $readbackRoot=Join-Path $root ('readback-'+$mode)
    Run-Tool @('selftest-readback','--access',$mode,'--output',$readbackRoot) $true
    $readback=[IO.File]::ReadAllText((Join-Path $readbackRoot 'worker\summary.json')) | ConvertFrom-Json
    if (!$readback.complete -or $readback.access_mode -ne $mode -or $readback.query_write_attempts -ne 51 -or $readback.physical_queries -or $readback.physical_actuation_attempted) { throw 'Access mode lost in supervised fake readback' }
    if ($readback.requested_share_flags -ne $(if ($mode -eq 'shared') { 3 } else { 0 })) { throw 'Wrong share flags' }
    $snapshotText=[IO.File]::ReadAllText((Join-Path $readbackRoot 'worker\snapshot.asb'))
    if (!$snapshotText.StartsWith("ASB_APEX6_SNAPSHOT_V2`nsynthetic`n")) { throw 'Fake readback exported as physical baseline' }
}
$gripReview=Join-Path $root 'grip-review.json'
$gripRehearsal=Join-Path $root 'grip-rehearsal'
Run-Tool @('prepare-grip','--synthetic','--output',$gripReview) $true
$gripManifest=[IO.File]::ReadAllText($gripReview) | ConvertFrom-Json
if ($gripManifest.schema -ne 'asb.apex6.grip-neutral-review.v2' -or $gripManifest.scope -ne 'grip-only' -or $gripManifest.status -ne 'REHEARSAL_ONLY_PHYSICAL_EXECUTION_LOCKED' -or $gripManifest.full_configuration_preservation_proven) { throw 'Wrong grip scope/review' }
if ($gripManifest.phases.preflight.Count -ne 14 -or $gripManifest.phases.active.Count -ne 21 -or $gripManifest.phases.postflight.Count -ne 14) { throw 'Wrong scoped lifecycle counts' }
Run-Tool @('rehearse-grip','--synthetic','--manifest',$gripReview,'--output',$gripRehearsal) $true
$gripSummary=[IO.File]::ReadAllText((Join-Path $gripRehearsal 'summary.json')) | ConvertFrom-Json
if (!$gripSummary.complete -or $gripSummary.evidence_scope -ne 'grip-only' -or $gripSummary.query_write_attempts -ne 42 -or $gripSummary.actuator_write_attempts -ne 7 -or $gripSummary.physical_queries -or $gripSummary.physical_actuation_attempted) { throw 'Wrong scoped rehearsal result' }
$gripBaseline=Join-Path $gripRehearsal 'grip-baseline.asb'
if (![IO.File]::ReadAllText($gripBaseline).StartsWith("ASB_APEX6_GRIP_BASELINE_V1`ngrip-only`nsynthetic`nsynthetic`n")) { throw 'Synthetic grip provenance lost' }
Run-Tool @('rehearse-grip','--baseline',$gripBaseline,'--manifest',$gripReview,'--output',(Join-Path $root 'grip-roundtrip')) $true
# Exercise the SAME canonical decoder used by physical approval, not just text equality.
# The generator emits Unicode LF escapes; an earlier reader incorrectly expected \\n.
$gripReviewText=[IO.File]::ReadAllText($gripReview)
if (!$gripReviewText.Contains('\u000a')) { throw 'Expected canonical LF escape in generated review' }
$wrongEscape=Join-Path $root 'wrong-escape.json'
[IO.File]::WriteAllText($wrongEscape,$gripReviewText.Replace('\u000a','\n'))
Run-Tool @('rehearse-grip','--synthetic','--manifest',$wrongEscape,'--output',(Join-Path $root 'wrong-escape-output')) $false
if (Test-Path -LiteralPath (Join-Path $root 'wrong-escape-output')) { throw 'Noncanonical escape caused output side effects' }
Run-Tool @('prepare','--snapshot',$gripBaseline,'--output',(Join-Path $root 'grip-as-full')) $false
Run-Tool @('prepare-grip','--baseline',(Join-Path $capture 'snapshot.asb'),'--output',(Join-Path $root 'full-as-grip')) $false
Run-Tool @('compare-grip','--baseline',$gripBaseline,'--confirmation',$gripBaseline) $false
Run-Tool @('rehearse-grip','--synthetic','--manifest',$review,'--output',(Join-Path $root 'wrong-grip-manifest')) $false
$gripTampered=Join-Path $root 'grip-tampered.json'
[IO.File]::WriteAllText($gripTampered,[IO.File]::ReadAllText($gripReview).Replace('grip-only','full-only'))
Run-Tool @('rehearse-grip','--synthetic','--manifest',$gripTampered,'--output',(Join-Path $root 'tampered-grip-output')) $false
Run-Tool @('execute-grip','--manifest',$gripReview,'--approval','missing','--output',(Join-Path $root 'execute-grip-output')) $false
Run-Tool @('grip-baseline','--device','not-a-device','--output',(Join-Path $root 'invalid-grip')) $false
Run-Tool @('grip-baseline','--device','not-a-device','--direct-usb-confirmed','--output',(Join-Path $root 'invalid-grip')) $false
Run-Tool @('grip-baseline','--device','not-a-device','--direct-usb-confirmed','--access','exclusive','--output',(Join-Path $root 'invalid-grip')) $false
Run-Tool @('--grip-baseline-worker','--device','not-a-device','--access','shared','--output',(Join-Path $root 'invalid-grip')) $false
foreach ($name in @('grip-as-full','full-as-grip','wrong-grip-manifest','tampered-grip-output','execute-grip-output','invalid-grip')) {
    if (Test-Path -LiteralPath (Join-Path $root $name)) { throw 'Invalid grip command caused side effects' }
}
$gripReadback=Join-Path $root 'grip-readback'
Run-Tool @('selftest-grip-baseline','--output',$gripReadback) $true
$gripReadbackSummary=[IO.File]::ReadAllText((Join-Path $gripReadback 'worker\summary.json')) | ConvertFrom-Json
if (!$gripReadbackSummary.complete -or $gripReadbackSummary.query_write_attempts -ne 14 -or $gripReadbackSummary.actuator_write_attempts -ne 0 -or $gripReadbackSummary.physical_queries) { throw 'Scoped query selftest not exactly 14 simulated queries' }
$gripIntegrity=[IO.File]::ReadAllText((Join-Path $gripReadback 'worker\manifest.json')) | ConvertFrom-Json
if (!($gripIntegrity.files | Where-Object name -eq 'grip-baseline.asb')) { throw 'Grip artifact missing from integrity manifest' }
Write-Output "Offline CLI review/rehearsal, tampering, and lock checks passed. Evidence retained: $root"
