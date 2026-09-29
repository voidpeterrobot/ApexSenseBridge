# Capture-backed silent grip lifecycle

The software diagnostic is implemented and offline-tested. One supervised silent
physical run is **qualified on 2026-09-28**, with matching postflight evidence and
operator-confirmed normal input/vibration and no unexpected behavior. See the
[run result](APEX6_FINDINGS.md). Recovery was unverified when
the official capture was collected; the operator confirmed readiness before this
new experiment. The anomalous reply meaning remains unresolved.

This diagnostic is isolated from production output. It adds no waveform playback,
adaptive triggers, driver changes or timeout tuning. It does not reproduce the
complete official lifecycle, which included other mode writes and pipelining.

## Capture evidence

The full capture is private and is not distributed with this repository.
SHA256: `a701b3fe7cee7d29301e19660dd4efe0b7d19dbf80dbcd183ea092d72a8a1a45`.

The small regression fixture is [OfficialSilent.json](../tests/fixtures/apex6/OfficialSilent.json),
with a generated C++ companion. It records frame numbers, directions, relative
microseconds, exact 32-byte USB bodies, and equivalent 33-byte Windows reports
with explicit report ID `00`. The extractor validates USBPcap framing and the
source hash; it does not scan for arbitrary leading bytes to discard.

Selected requests: entry frame 203, combined exit 371, left restore 421, right
restore 425. Selected inbound frames: 204, 209, 373, 376, 422, 426 (normal ACK
envelopes), 429 and 431 (exact zero-count/value-1 envelopes). These are an ordered
subset, **not request/reply pairs**. Official pipelining prevents one-to-one
attribution. The bytes do not establish the firmware meaning of the anomalous
replies or prove physical recovery.

To regenerate with an installed Python interpreter:

```powershell
python scripts/extract_apex6_silent_fixture.py tmp/official-silent.pcap tests/fixtures/apex6/OfficialSilent.json
```

## Exact contract

Scope: `grip-mode-only-lifecycle-v1`. Existing neutral, restore-only and observation
review/approval files cannot authorize this scope. The query executable supports
offline preparation/rehearsal but rejects approval, execution and actuator workers.
Physical execution requires the separately built supervised actuator executable
`ApexSenseBridgeApex6NeutralExperiment.exe`.

1. Acquire the existing 14-query grip baseline; compare it exactly with the review.
2. Send `53 / 01 12 02 40 00`; require a normal success ACK.
3. Immediately send `53 / 01 12 00`; require a normal success ACK.
4. Send baseline-derived `53 / 01 10 01 40`, then `53 / 01 11 01 40` serially.
5. Acquire 14 postflight queries and compare identity, firmware, UID, format
   versions, profile CRCs and RAM6 bytes/fingerprint with the baseline.

This version accepts only saved mode `01`, parameter `40`, targets `10` and `11`,
and 33-byte Windows reports with ID `00`. There are exactly four actuator writes,
28 query writes and zero waveform writes. No dwell, `57` neutral/all-disabled
frame, trigger target, configuration write, retry or speculative cleanup is allowed.
The native adapter independently enforces the entire ordered report sequence.

Individual restores require a full write and a timely complete reply. A normal
success ACK or the **exact** captured 32-byte zero-count/value-1 envelope permits
the next planned operation. The latter is classified
`captured_zero_count_value_1_unverified`; it never means restored or successful
restoration. Existing strict parsing behavior is unchanged for other experiments.

Unknown envelopes, malformed replies, normal nonzero status, missing/late replies,
stale same-opcode traffic, device loss and I/O/trace faults latch failure and stop
all traffic. The exchange deadline remains 600 ms, the active ceiling is five
seconds, and the session ceiling remains 75 seconds. No latched failure is cleared.

## Operator workflow

Before acquisition, confirm power-cycle recovery after the official capture and
ordinary input/normal vibration. Close official software and other known controller
writers. Connect direct USB and unplug the receiver. Use the existing query tool
to obtain **two independent matching fresh baselines**:

```powershell
$query = '.\build-win\Release\ApexSenseBridgeApex6Experiment.exe'
& $query grip-baseline --device 'EXACT_VENDOR_INSTANCE' --access shared --direct-usb-confirmed --output .\tmp\fresh-grip-01
& $query grip-baseline --device 'EXACT_VENDOR_INSTANCE' --access shared --direct-usb-confirmed --output .\tmp\fresh-grip-02
```

After reviewing both acquisitions, run this launcher in an interactive console:

```powershell
.\scripts\Invoke-Apex6GripLifecycleTest.ps1 `
  -Baseline .\tmp\fresh-grip-01\worker\grip-baseline.asb `
  -Confirmation .\tmp\fresh-grip-02\worker\grip-baseline.asb `
  -Output .\tmp\grip-lifecycle-new
```

The launcher requires different baseline files modified within five minutes,
compares them, prepares the exact review, rehearses the captured anomalous case,
and presents three grouped approval prompts: setup/recovery, exact review/risks,
and the full manifest SHA256. Approval binds the baseline, exact sequence, reply
policy, source and executable hashes, and expires after five minutes. The worker
rechecks the baseline before any actuator write and rechecks expiry before entry.
Configuration changes invalidate old baselines and approvals.

Approvals are single-use: the native authorization is shared across copies and
consumed once; the worker also creates a container-locked, exclusive `CREATE_NEW`
claim named `grip-lifecycle-used-<approval-sha256>.asb` alongside its executable.
Keep those claims; copying an approval to another filename does not permit replay.
The executable directory must be writable. These local claims are operational
replay guards, not tamper-proof security against an operator deleting evidence.

Commands are `prepare-grip-lifecycle`, `rehearse-grip-lifecycle`,
`approve-grip-lifecycle`, and `execute-grip-lifecycle`. The first two accept the
existing `--synthetic` or `--baseline` options. Rehearsal/execute return **2** for a
completed diagnostic whose physical recovery remains unverified, **1** for failure.
Preparation and approval return 0 on success. Existing command exit codes remain
unchanged. A launcher or automation must explicitly handle diagnostic exit 2.

## Evidence and qualification

`grip-lifecycle.json` contains `sequence_complete` (four active operations completed),
both `restore_replies`, `postflight_matches`, `restoration_verified` (always false),
`device_state_uncertain` and `failure`. Raw reports and mode classifications remain
in `trace.jsonl`; native timing remains in `io-timing.json`. The manifest hashes
these files. A diagnostic is complete only when the active sequence, postflight
comparison and evidence finalization all complete without failure.

Use `Show-Apex6ExperimentDiagnostics.ps1 -Run <run-directory>` to validate saved
file hashes and display the distinct exit-2 interpretation. Matching RAM alone
must never mark physical restoration verified.

After a run, obtain and retain the operator's report of ordinary input, normal
vibration and unexpected movement alongside the software evidence. Any unexpected
behavior ends physical testing. On failure, disconnect/power off; do not retry or
send speculative cleanup. Physical qualification needs a completed run, matching
postflight evidence and observed recovery. Only then plan a separately bounded
grip waveform test. Unrestricted game playback remains disabled.

Offline verification: full Release build and all 31 CTest tests, including capture
framing/classification, old strict reply rejection, all restore ACK/anomaly
combinations, fail-stop fault injection, native exact-sequence/replay guards,
approval scope/expiry/hash checks and exit-2 diagnostics. The subsequent physical
qualification is recorded separately in the linked run result.
