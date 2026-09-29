# Apex6Pro grip-haptics readback

This is the implemented, query-only start of step 2. It does not vibrate the
controller, change motor mode, or qualify physical haptic playback. It reads only
identity/version/profile checks and RAM 6, the motor settings needed for grip
restoration. RAM 1/4/5 are deliberately not acquired. Their outstanding issues
are deferred, not prerequisites for grip feedback.

## Implemented commands

From the repository root, using the opt-in experiment build:

```powershell
$tool = '.\build-win\Release\ApexSenseBridgeApex6Experiment.exe'
& $tool list
# Copy the exact current vendor instance from list; direct USB, receiver unplugged.
$instance = 'EXACT_VENDOR_INSTANCE_FROM_LIST'
& $tool grip-baseline --device $instance --direct-usb-confirmed --access shared --output .\tmp\grip-readback-A
# Review successful supervisor/worker summaries before initiating confirmation.
& $tool grip-baseline --device $instance --direct-usb-confirmed --access shared --output .\tmp\grip-readback-B
& $tool compare-grip --baseline .\tmp\grip-readback-A\worker\grip-baseline.asb --confirmation .\tmp\grip-readback-B\worker\grip-baseline.asb
& $tool prepare-grip --baseline .\tmp\grip-readback-A\worker\grip-baseline.asb --output .\tmp\grip-review.json
& $tool rehearse-grip --baseline .\tmp\grip-readback-A\worker\grip-baseline.asb --manifest .\tmp\grip-review.json --output .\tmp\grip-rehearsal
```

All output paths must be new. Stop on an incomplete run; no automatic retry or
access fallback is implemented. `compare-grip` compares physical-labelled
canonical content, not provenance signatures: independently review the two run
traces, supervisor results and integrity manifests. Copying one baseline twice
does not constitute two acquisitions.

The native adapter enforces exactly 14 ordered query writes. It refuses unrelated
RAM, mode commands, waveforms, malformed frames and extra writes. Acquiring RAM 6
requires length 64, consistent slot/fingerprint/CRC32, layout 2, valid restoration
entries, and unchanged identity, firmware, format versions and five config CRC16s.
Shared access and the cooperative lock are retained; neither proves sole ownership.

Artifacts use `ASB_APEX6_GRIP_BASELINE_V1` and
`asb.apex6.grip-neutral-review.v2`. The baseline records physical/synthetic origin
and explicit `grip-only` scope. Restoration entries are derived from the actual
mapping bytes and revalidated during decoding. Evidence manifests hash the
baseline, raw trace, native timing and summary.

Offline rehearsal simulates 14 preflight + 21 neutral lifecycle + 14 postflight
requests. Its saved acquisition is always labelled synthetic, including when
rehearsing a physical baseline. It cannot establish motor recovery.

## Isolated left-grip restoration diagnostic

Following the failed neutral lifecycle, a separate restore-only route isolates
the saved left-grip command from the preceding streaming/stop sequence. It is
not a vibration test or an authorization to repeat the full neutral lifecycle.
After an operator-confirmed power cycle and normal-input/vibration check, obtain
two fresh matching grip readbacks as above, then use the **neutral executable**:

```powershell
$tool = '.\build-win\Release\ApexSenseBridgeApex6NeutralExperiment.exe'
& $tool prepare-grip-restore --baseline .\tmp\grip-readback-A\worker\grip-baseline.asb --output .\tmp\restore-review.json
& $tool rehearse-grip-restore --baseline .\tmp\grip-readback-A\worker\grip-baseline.asb --manifest .\tmp\restore-review.json --output .\tmp\restore-rehearsal
# Only after successful evidence/rehearsal review, in a local interactive console:
.\scripts\Invoke-Apex6GripRestoreTest.ps1 -Manifest .\tmp\restore-review.json -Output .\tmp\restore-result
```

Use new output paths and stop on any failure. The distinct review schema
`asb.apex6.grip-restore-review.v1` and approval token
`ASB_APEX6_GRIP_RESTORE_APPROVAL_V1` cannot authorize the neutral lifecycle or be
replaced by its approvals. Approval expires after five minutes and is rechecked
before the mode command. Redirected consent is refused.
The restore-only console groups approval into three prompts: setup/readiness,
combined review/risks, and the exact review SHA256. Each YES covers all displayed
items in that group; a refusal or end-of-input cancels without creating approval.

Both session and native transport enforce 14 fresh preflight queries, exactly
one baseline-derived left-grip restore, then 14 postflight queries on success.
No streaming entry, waveform, combined exit, right-grip restore or RAM 1/4/5
access is permitted. Any unexpected response stops all traffic immediately;
there is no retry or speculative cleanup. Disconnect/power off after a failure.
Protocol success still requires an independent normal-input/vibration check and
does not qualify streaming or establish physical recovery by itself.

For detailed **offline** diagnostics of a saved run (no controller access):

```powershell
.\scripts\Show-Apex6ExperimentDiagnostics.ps1 -Run .\tmp\restore-result\run
```

This verifies recorded file hashes and prints the worker error, per-operation
timings, last raw TX/RX, mode-envelope fields and native I/O completion details.
The restore wrapper also prints these details automatically after a failed run.

## Bounded restore-reply observation diagnostic

The isolated restore reproduced the same zero-count/error-1 envelope. The new
observation route is a different diagnostic policy, NOT a retry of the old test
and NOT permission to ignore that envelope. Keep the device disconnected during
offline preparation. Its commands are:

```powershell
$tool = '.\build-win\Release\ApexSenseBridgeApex6NeutralExperiment.exe'
& $tool prepare-grip-observe --synthetic --output .\tmp\observe-synthetic-review.json
& $tool rehearse-grip-observe --synthetic --manifest .\tmp\observe-synthetic-review.json --output .\tmp\observe-synthetic-rehearsal
```

Rehearsal simulates the recorded zero-count envelope followed by one normal
ACK-shaped frame. This is a test fixture, not a prediction of firmware behavior.
It must retain the first error and never report restoration success. Rehearsal
exit 0 means the synthetic capture completed; its result summary remains unresolved.

Before physical use: power-cycle, independently verify ordinary input and normal
vibration, connect direct USB with receiver unplugged, and obtain two NEW matching
grip readbacks. Substitute their physical baseline for `--synthetic` when preparing
and rehearsing a new review. Do not reuse the previous failed run's approval.
After evidence review, use a local interactive console:

```powershell
.\scripts\Invoke-Apex6GripObserveTest.ps1 -Manifest .\tmp\your-new-observe-review.json -Output .\tmp\your-new-observe-result
```

The separate `asb.apex6.grip-observe-review.v1` review and
`ASB_APEX6_GRIP_OBSERVE_APPROVAL_V1` token disclose the receive-after-error policy
in the same three grouped approval prompts. They cannot be substituted for the
older restore or neutral approvals, even with a matching hash. The query-only
executable still cannot approve or execute physical commands.

Session and native transport allow exactly 14 preflight query writes and ONE
baseline-derived left restore. There is no postflight, right restore, waveform,
streaming entry/exit or retry. After a full write and timely complete first reply,
the session permanently forbids further exchanges and records at most 32 additional
raw reports within 500 ms of observation start, also bounded by the active phase.
Initial transport/timeout, preflight or evidence failures do not start observation.
The receive window ends on deadline/report limit, device removal, clock reversal,
I/O failure or evidence failure. Pending I/O is finalized by the existing bounded
native cancellation path; unresolved cancellation cannot count as complete capture.

`restore-observation.json` reports capture completion separately from restoration.
Raw tail bytes/statuses are in `trace.jsonl`; native completions/cancellation are in
`io-timing.json`. All are covered by the evidence manifest. A late ACK never clears
the first error, proves recovery or permits another write. A physical diagnostic
returns nonzero even when capture completes because restoration stays unresolved.
The wrapper explains this distinction and displays offline diagnostics afterward.
Disconnect immediately if behavior is unexpected; otherwise power off/disconnect
after capture and report the results. No automatic retry or speculative cleanup.

The first physical observation recorded zero additional reports but ended slightly
before its monotonic deadline. The updated native wait logic keeps the same pending
operation across early timer wakes, retaining the original deadline and allowing
at most eight wait calls. It never resubmits the command or read to extend the
window. Wait failure, timeout, device/completion failure and unresolved cancellation
remain distinct; pathological wakes/clock reversal fail closed.
`asb.apex6.host-io-timing.v2` records wait begin/return, requested milliseconds,
absolute deadline, result/error, early-wake markers, and cancellation request and
completion separately. Empty polls are not logged repeatedly. The formatter keeps
v1 recordings readable but explicitly marks their missing wait metadata. Existing
failed recordings are unchanged; rebuilding requires a new review and approval.

## Related output paths

Later lifecycle and streaming findings are summarized in
[APEX6_FINDINGS.md](APEX6_FINDINGS.md). Normal application sessions use the
[integrated beta](APEX6_INTEGRATED_BETA.md); this diagnostic retains its own
approval and reply policy.

`approve-grip` and `execute-grip` are available only in
`ApexSenseBridgeApex6NeutralExperiment.exe`; the query executable still refuses.
Old full-snapshot approvals cannot enable this new route. Mode/restoration errors
remain relevant to safe playback and must not be ignored. General USB tracing,
exclusive access, unrelated RAM and adaptive triggers are not part of this work.

## Physical neutral lifecycle (not a nonzero vibration test)

**Historical hardware result:** the first scoped run failed at left-grip restoration
after successful entry/combined-exit replies. Do not repeat the commands below
until that contract has been reviewed and a new test is explicitly approved.
See [the public findings](APEX6_FINDINGS.md) for the restoration limitation.

Prepare the review and rehearse it with the **neutral executable**, not the query
executable: reviews bind the source and executable hashes and cannot be exchanged
between builds. Existing v1 scoped reviews are rehearsal-only history; regenerate
v2 and rehearse after rebuilding. Use two matching physical readbacks and inspect
their complete trace/supervisor/integrity evidence.

The neutral runner now accepts a distinct `ASB_APEX6_GRIP_APPROVAL_V1` token.
Interactive approval includes an explicit acknowledgement that RAM 1/4/5 are not
acquired. The token binds the review hash, expires after five minutes, and is
checked again before entry. Neither synthetic baselines, legacy full-audit tokens,
query-binary reviews, modified reviews nor redirected console consent authorize it.

The session and native adapter both enforce exactly 49 ordered requests:
14 grip preflight queries, 21 active lifecycle requests, 14 grip postflight queries.
The active phase permits four mode commands and three neutral waveform frames;
no nonzero waveform, trigger enable, unrelated RAM or persistent write is allowed.
The existing bounded I/O, cooperative lock and supervised worker remain in force.
No retry or follow-up cleanup is sent after an unexpected failure.

Once prepared, an operator can start one approval-and-test session in a local
interactive PowerShell window using the wrapper below (substitute newly prepared
paths; do not launch a second instance while a test is already open):

```powershell
.\scripts\Invoke-Apex6GripNeutralTest.ps1 `
  -Manifest .\tmp\your-grip-neutral-review.json `
  -Output .\tmp\your-new-grip-neutral-result
```

This opens no device until the operator explicitly confirms the displayed review
and hash. Verify ordinary input/normal vibration beforehand; keep direct USB and
the receiver unplugged, and be ready to disconnect/power off. The all-disabled
frame remains unvalidated and historical mode/restoration errors unresolved; the
approval calls these out. On error, disconnect/power off and retain evidence; do
not retry. On protocol success, independently check ordinary input and normal
vibration and report the observation. Protocol completion alone is not physical
recovery or step-2 haptic playback success.
