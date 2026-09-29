param([Parameter(Mandatory=$true)][string]$Run)
# Offline only: reads saved evidence; never opens a controller or runs the tool.
$ErrorActionPreference = 'Stop'
$runPath = (Resolve-Path -LiteralPath $Run).Path
$workerPath = Join-Path $runPath 'worker'
if (!(Test-Path -LiteralPath $workerPath -PathType Container)) { $workerPath = $runPath } # Flat offline rehearsal evidence
function Read-Json([string]$Path) {
    return ([IO.File]::ReadAllText($Path) | ConvertFrom-Json)
}
$manifest = Read-Json (Join-Path $workerPath 'manifest.json')
if ($manifest.schema -ne 'asb.apex6.evidence.v2') { throw 'Unsupported evidence schema.' }
$sha = [Security.Cryptography.SHA256]::Create()
try {
    foreach ($entry in $manifest.files) {
        if ([string]::IsNullOrEmpty($entry.name) -or [IO.Path]::GetFileName($entry.name) -ne $entry.name -or $entry.name -in @('.', '..') -or $entry.name.Contains(':')) {
            throw 'Invalid manifest file name.'
        }
        $bytes = [IO.File]::ReadAllBytes((Join-Path $workerPath $entry.name))
        $hash = [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-','').ToLowerInvariant()
        if ($bytes.Length -ne $entry.bytes -or $hash -ne $entry.sha256) { throw "Evidence integrity mismatch: $($entry.name)" }
    }
} finally { $sha.Dispose() }
foreach ($required in @('summary.json','trace.jsonl')) {
    if ($required -notin @($manifest.files.name)) { throw "Unhashed diagnostic input: $required" }
}
$summary = Read-Json (Join-Path $workerPath 'summary.json')
$timing = $null
if ('io-timing.json' -in @($manifest.files.name)) { $timing = Read-Json (Join-Path $workerPath 'io-timing.json') }
elseif ($summary.report_io_kind -eq 'physical') { throw 'Physical diagnostic is missing hashed native I/O timing.' }
$trace = @([IO.File]::ReadAllLines((Join-Path $workerPath 'trace.jsonl')) | ForEach-Object { $_ | ConvertFrom-Json })
$writes = @($trace | Where-Object { $_.event -eq 'write_attempt' })
Write-Output 'APEX6 SAVED-EVIDENCE DIAGNOSTIC (OFFLINE; NO DEVICE ACCESS)'
Write-Output "Evidence: $runPath"
Write-Output "Integrity: $(@($manifest.files).Count) recorded file sizes/hashes match (not a provenance signature)."
Write-Output "Scope: $($summary.evidence_scope); complete: $($summary.complete)"
Write-Output "Worker error: $($summary.error)"
Write-Output "Query attempts: $($summary.query_write_attempts); actuator attempts: $($summary.actuator_write_attempts)"
Write-Output "Actuation attempted: $($summary.physical_actuation_attempted); state uncertain: $($summary.device_state_uncertain)"
Write-Output "Report I/O kind: $($summary.report_io_kind)"
if ($summary.evidence_scope -eq 'grip-mode-only-lifecycle-v1') {
    if ('grip-lifecycle.json' -notin @($manifest.files.name)) { throw 'Missing hashed lifecycle result.' }
    $lifecycle = Read-Json (Join-Path $workerPath 'grip-lifecycle.json')
    Write-Output "Sequence complete: $($lifecycle.sequence_complete); postflight matches: $($lifecycle.postflight_matches)"
    Write-Output "Restore classifications: $($lifecycle.restore_replies -join ', ')"
    if ($summary.complete -and $lifecycle.sequence_complete -and $lifecycle.postflight_matches -and !$lifecycle.failure) {
        Write-Output 'DIAGNOSTIC COMPLETE (exit 2): physical recovery UNVERIFIED; recording was not interrupted.'
    } else { Write-Output "LIFECYCLE FAILED (exit 1): $($lifecycle.failure)" }
    Write-Output 'Restoration verified: false. Obtain operator input/vibration and unexpected-movement report; matching RAM alone is insufficient.'
}
if ($summary.evidence_scope -in @('grip-left-pulse-v1','grip-left-pulse-v2','grip-left-pulse-v3','grip-left-pulse-v4')) {
    if ('grip-pulse.json' -notin @($manifest.files.name)) { throw 'Missing hashed pulse result.' }
    $pulse = Read-Json (Join-Path $workerPath 'grip-pulse.json')
    if ($summary.evidence_scope -in @('grip-left-pulse-v3','grip-left-pulse-v4')) {
        if ('review.json' -notin @($manifest.files.name)) { throw 'Missing hashed strength review.' }
        $pulseReview = Read-Json (Join-Path $workerPath 'review.json')
        if ($pulseReview.scope -ne $summary.evidence_scope) { throw 'Pulse review scope mismatch.' }
        $signal = $pulseReview.signal
        Write-Output "Reviewed strength: gain $($signal.gain), base peak $($signal.base_peak), peak limit $($signal.peak_limit), edge peak $($signal.edge_peak)"
        Write-Output "Planned signal clipping: $($signal.clipped_samples); peak after limit: $($signal.peak_after_limit). Completion fields below determine what was sent."
    }
    Write-Output "Sequence complete: $($pulse.sequence_complete); waveform complete: $($pulse.waveform_complete); postflight matches: $($pulse.postflight_matches)"
    Write-Output "Restore classifications: $($pulse.restore_replies -join ', '); cancelled: $($pulse.cancelled)"
    if ($summary.complete -and $pulse.sequence_complete -and $pulse.waveform_complete -and $pulse.postflight_matches -and !$pulse.failure -and !$pulse.cancelled) {
        Write-Output 'GRIP PULSE DIAGNOSTIC COMPLETE (exit 2): operator qualification pending; physical recovery UNVERIFIED.'
    } else { Write-Output "GRIP PULSE FAILED (exit 1): $($pulse.failure)" }
    Write-Output ($pulse.packets | Format-Table index,native_submit_us,native_complete_us,nominal_us,lateness_us,spacing_us -AutoSize | Out-String)
    Write-Output 'Restoration verified: false. Report left pulse perception, right/trigger or unexpected movement, and ordinary input/vibration. Faint/unclear is inconclusive; no automatic retry or gain increase.'
}
if ($timing) { Write-Output "Native timing complete: $($timing.timing_complete); pending I/O resolved: $($timing.pending_io_resolved)" }
else { Write-Output 'No native timing: this is simulated/offline evidence, not a physical capture.' }
Write-Output "Source SHA256: $($manifest.source_sha256)"
Write-Output "Executable SHA256: $($manifest.executable_sha256)"
Write-Output 'Host-observed microseconds below are NOT USB arrival or motor-response times.'
if ($writes.Count) {
    $rows = foreach ($sent in $writes) {
        $received = @($trace | Where-Object { $_.operation -eq $sent.operation -and $_.event -eq 'receive' })
        $returned = @($trace | Where-Object { $_.operation -eq $sent.operation -and $_.event -eq 'write_return' })
        [pscustomobject]@{
            Op=$sent.operation
            Cmd= $(if ($sent.raw_hex.Length -ge 8) { $sent.raw_hex.Substring(6,2) } else { '?' })
            Bytes= $(if ($returned.Count) { $returned[0].detail } else { $null })
            WriteUs= $(if ($returned.Count) { $returned[0].time_us-$sent.time_us } else { $null })
            ReplyUs= $(if ($received.Count) { $received[0].time_us-$sent.time_us } else { $null })
        }
    }
    Write-Output ($rows | Format-Table -AutoSize | Out-String)
    $last = $writes[-1]
    Write-Output "Last attempted operation: $($last.operation); Windows report TX: $($last.raw_hex)"
    foreach ($rx in @($trace | Where-Object { $_.operation -eq $last.operation -and $_.event -eq 'receive' })) {
        Write-Output "Windows report RX: $($rx.raw_hex)"
        if ($rx.raw_hex -match '^005aa553[0-9a-f]{58}$') {
            $hex = $rx.raw_hex
            $count = [Convert]::ToInt32($hex.Substring(8,2),16)
            $index = [Convert]::ToInt32($hex.Substring(10,2),16)
            $value = [Convert]::ToInt32($hex.Substring(12,2),16)
            $sum=0
            for ($byte=3; $byte -lt 32; $byte++) { $sum += [Convert]::ToInt32($hex.Substring(2*$byte,2),16) }
            $checksum = [Convert]::ToInt32($hex.Substring(64,2),16)
            Write-Output "Mode envelope: count=$count; index=$index; value=$value; padded checksum matches=$(($sum -band 255) -eq $checksum)"
            if ($count -eq 0) { Write-Output 'Zero-count envelope: strict parser rejects it. Only approved lifecycle/pulse individual restore steps admit the exact captured bytes as unverified evidence; firmware meaning is NOT established.' }
        }
    }
}
if ($timing) {
    Write-Output 'Last native I/O events (997 at submit denotes pending I/O, not a failed completion):'
    Write-Output ($timing.events | Select-Object -Last 16 observed_us,event,native_operation,transferred,win32_error | Format-Table -AutoSize | Out-String)
    if ($timing.schema -eq 'asb.apex6.host-io-timing.v2') {
        Write-Output 'Last wait returns: 0=signaled, 258=timeout, 4294967295=WAIT_FAILED. Wait errors and cancellation errors are separate.'
        $waits = @($timing.events | Where-Object { $_.event -like '*wait_return' } | Select-Object -Last 8)
        $rows = foreach ($wait in $waits) {
            [pscustomobject]@{
                Operation=$wait.native_operation; Event=$wait.event; RequestedMs=$wait.requested_wait_ms;
                Result=$wait.wait_result; WaitError=$wait.win32_error;
                DeadlineUs=$wait.deadline_us; ReturnedUs=$wait.observed_us;
                RemainingUs=($wait.deadline_us-$wait.observed_us)
            }
        }
        Write-Output ($rows | Format-Table -AutoSize | Out-String -Width 220)
        Write-Output ('Early wake events: ' + @($timing.events | Where-Object { $_.event -eq 'wait_early_wake' }).Count)
    } else {
        Write-Output 'Legacy timing: original wait results/deadlines were not recorded; do not infer them from cancellation codes.'
    }
}
if ('restore-observation.json' -in @($manifest.files.name)) {
    $observation = Read-Json (Join-Path $workerPath 'restore-observation.json')
    Write-Output "Follow-up capture: started=$($observation.started); complete=$($observation.capture_complete); additional reports=$($observation.additional_reports); elapsed us=$($observation.elapsed_us); stop=$($observation.stop)"
    Write-Output "First-reply outcome: $($observation.first_reply); capture error: $($observation.error)"
    foreach ($rx in @($trace | Where-Object { $_.event -eq 'restore_observation_receive' -and $_.raw_hex })) {
        Write-Output "Follow-up RX at $($rx.time_us) us: $($rx.raw_hex)"
        if ($rx.raw_hex -eq '005aa5530100000000000000000000000000000000000000000000000000000054') {
            Write-Output 'Normal mode-ACK-shaped report observed. Attribution and physical recovery NOT proven; writes remain forbidden.'
        }
    }
} else {
    Write-Output 'No bounded post-error receive window was recorded by this run.'
}
Write-Output 'No success/recovery is inferred from write completion, a later ACK, or this report.'
