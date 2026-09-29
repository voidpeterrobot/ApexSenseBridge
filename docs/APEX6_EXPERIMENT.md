# Apex6Pro: shared readback and separately gated neutral experiment

This guide covers opt-in diagnostic tools. For normal application sessions, use
the [integrated beta guide](APEX6_INTEGRATED_BETA.md). The
[engineering findings](APEX6_FINDINGS.md) summarize physical observations and
unresolved restore replies. Diagnostic scopes and approvals remain separate from
integrated sessions; the older commands below retain their strict reply policy.

The query executable supports explicit shared/exclusive Windows readback and
offline neutral rehearsal. Its `execute` command stays locked, and its native
adapter independently rejects actuator/profile writes. A separate opt-in
`ApexSenseBridgeApex6NeutralExperiment.exe` can run only a reviewed neutral
lifecycle with fresh interactive approval. Neither is a production haptic driver;
nonzero playback, live games, receiver qualification and adaptive triggers remain
out of scope. Implementing/building these tools is not approval to run hardware.

## Build

Experimental targets are opt-in and are not included in installers or portable
packages. The existing capture and legacy bridge link graphs remain isolated.
Both options default OFF. This workspace explicitly enables them for offline
build/testing. VS 2026 remains supported. No driver/service changes are needed
to build, and these binaries are not added to release packages.

```powershell
& 'C:\Program Files\CMake\bin\cmake.exe' -S . -B build-win -DASB_BUILD_APEX6_EXPERIMENTS=ON -DASB_BUILD_APEX6_NEUTRAL_EXPERIMENTS=ON
& 'C:\Program Files\CMake\bin\cmake.exe' --build .\build-win --config Release --target ApexSenseBridgeApex6Experiment ApexSenseBridgeApex6NeutralExperiment ApexSenseBridgeApex6ExperimentTests ApexSenseBridgeApex6WindowsIoTests ApexSenseBridgeApex6NeutralIoTests ApexSenseBridgeApex6LockTests --parallel
& 'C:\Program Files\CMake\bin\ctest.exe' --test-dir .\build-win -C Release --output-on-failure
```

## Offline review and rehearsal (no controller needed)

All output paths must be new; parent directories must already exist. Never
overwrite an earlier failed run. These example names can be changed for repeats.

```powershell
$tool = '.\build-win\Release\ApexSenseBridgeApex6Experiment.exe'
& $tool prepare --synthetic --output .\tmp\apex6-neutral-review-01.json
& $tool rehearse --synthetic --manifest .\tmp\apex6-neutral-review-01.json --output .\tmp\apex6-neutral-rehearsal-01
& $tool selftest-supervisor --output .\tmp\apex6-supervisor-01
& $tool selftest-supervisor --simulate-timeout --output .\tmp\apex6-supervisor-timeout-01
```

The last command intentionally fails after terminating its sleeping fake worker;
inspect `supervisor.json`. It must retain deadline/termination uncertainty and
must not infer device recovery. These commands use only a synthetic identity and
historical fixture RAM data, never a live device. The fake session makes 123
requests: 51 preflight, 21 active, 51 postflight. Compared with the reference
snapshot algorithm, each snapshot adds a final info query to detect firmware or
capability changes during acquisition; the 21-command neutral sequence is unchanged.

`prepare` produces canonical JSON with exact ordered Windows reports, baseline,
limits, unresolved risks and executable/source/baseline hashes. `rehearse` rejects
any difference from a regenerated review file. The file is **review-only**, not
an approval. `prepare` and `rehearse` never create consent. Reviews bind the
executable: a query-tool review cannot authorize the neutral executable.

## Next hardware operation: direct-USB readback only

Run this only when ready to authorize bounded vendor **queries**. It does not
send haptics, but it does write query packets to the vendor HID interface.

1. Connect the Apex6Pro directly by USB cable. Disconnect its receiver. Confirm
   ordinary input works; do not alter firmware, profiles or controller modes.
2. Close known controller-writing applications yourself. Do not kill unrelated
   processes, disable services, or change HidHide/driver/security settings.
   Shared readback qualification does not require first identifying every
   background handle owner; a shared handle is not evidence of an active writer.
3. List candidate vendor interfaces:

   ```powershell
   & $tool list
   ```

4. Copy the exact **vendor interface instance** printed by this tool. Do not use
   the earlier `IG_00` XInput instance or select a device by VID/PID alone.

   ```powershell
   & $tool snapshot --device 'EXACT_VENDOR_INSTANCE_FROM_LIST' --access shared --direct-usb-confirmed --output .\tmp\apex6-shared-readback-01
   ```

   If enumeration/layout validation is absent or ambiguous, stop and retain the
   diagnostic. Do not guess a report ID, use the legacy bridge, or try nearby
   command parameters. The confirmation flag records an operator choice; the
   reported connection byte is not sufficient to establish cable versus receiver.

5. Require exit code 0, `supervisor.json` with no deadline/wait failure, and
   `worker/summary.json` plus `worker/manifest.json` marked complete. Review the
   raw trace and numeric RAM data. The baseline is `worker/snapshot.asb`.

6. Review raw replies and stable UID/firmware/configuration. If successful, obtain
   a second **separately initiated and authorized** snapshot in a new directory.
   Compare stable snapshot fields (the canonical snapshot excludes battery/MAC).
   A failure stops qualification; do not automatically retry. Two matching runs
   increase confidence but cannot prove reply ownership without transaction IDs.

7. Prepare and rehearse using the separate neutral executable, still offline:

   ```powershell
   $neutral = '.\build-win\Release\ApexSenseBridgeApex6NeutralExperiment.exe'
   & $neutral prepare --snapshot .\tmp\apex6-shared-readback-01\worker\snapshot.asb --output .\tmp\apex6-shared-neutral-review-01.json
   & $neutral rehearse --snapshot .\tmp\apex6-shared-readback-01\worker\snapshot.asb --manifest .\tmp\apex6-shared-neutral-review-01.json --output .\tmp\apex6-shared-neutral-rehearsal-01
   ```

Keep all files local: they may include device UID and configuration data. A fresh
consistent snapshot proves readback, not mode entry, normal-rumble restoration,
safe all-disabled behavior or actuator fidelity. On query failure, preserve the
run and investigate; there is no automatic reopen or retry. `--access exclusive`
remains the default when omitted for compatibility. Shared mode is explicitly
requested, not a fallback; it changes sharing flags, not query/error policy.

## Physical neutral test: separate approval required

Do not run until readback and rehearsal have been reviewed and normal vibration
has been independently confirmed. Neutral samples are **not** proof of no motion:
entry/exit/restoration change runtime motor state. Historical restore errors and
the all-disabled frame remain unresolved; neither is suppressed or removed.

In an interactive PowerShell console, review the exact manifest and answer every
confirmation truthfully. Type the entire displayed manifest SHA256 at the end:

```powershell
& $neutral approve-neutral --manifest .\tmp\apex6-shared-neutral-review-01.json --output .\tmp\apex6-shared-neutral-approval-01.asb
# Only after explicit approval, within five minutes:
& $neutral execute-neutral --manifest .\tmp\apex6-shared-neutral-review-01.json --approval .\tmp\apex6-shared-neutral-approval-01.asb --output .\tmp\apex6-shared-neutral-live-01
```

There is no `--yes` or redirected-input approval. The approval file is a local
operator record, not a signature or security boundary against someone editing
their own files. Do not fabricate it or treat build/test success as consent.
The worker selects the manifest-bound device/access mode; there are no override
flags. It validates approval before opening and again before entry, then performs
fresh preflight, the exact 21-command active sequence, and equal postflight RAM.
Mode status errors and envelope errors remain failures. An unexpected failure
means no further traffic, including cleanup: power off/disconnect if actuator
state is uncertain. No automatic retry or recovery is attempted.

Physical `complete:true` means protocol checks passed, not normal vibration
recovery. `normal_vibration_verified` remains false and actuator uncertainty is
retained after entry. Record operator-observed input/vibration recovery separately.

## Diagnosing an exclusive-open failure (no controller reports)

After confirming direct USB with the receiver unplugged, use the exact vendor
instance from `list`. This is a separately authorized diagnostic, not a snapshot
retry or a shared-access fallback for the query transport:

```powershell
& $tool access-probe --device 'EXACT_VENDOR_INSTANCE_FROM_LIST' --direct-usb-confirmed --output .\tmp\apex6-access-probe-01
```

Discovery/capability inspection uses metadata access only. That handle closes
before seven fixed `CreateFileW`/`CloseHandle` trials: metadata/shared-read-write,
then read-only and read-write access, each with exclusive, share-read, and
share-read-write modes. Every successful handle closes before the next trial.
There are **no report reads/writes or feature/output-report calls** on these
handles, including when write access is requested. No service or setting changes.
The existing hidden worker supervisor bounds observation to 35 seconds plus a
one-second termination observation; kernel completion is not guaranteed.

Inspect `worker/access-probe.json`, `worker/summary.json`, the hash manifest, and
`supervisor.json`. A complete matrix can contain failed opens; it does not mean
the interface is usable. Only sharing/access-denied errors permit the next
distinct case. An unexpected open error or close failure stops the matrix.
No handles are passed to the query transport and no snapshot is acquired.

Sharing compatibility is bilateral: both requested access and sharing must be
compatible with existing opens ([Microsoft CreateFileW documentation](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)).
Shared success does not identify the owner, prove absence of competing writers,
or qualify response attribution. This diagnostic cannot acquire a baseline or
approve neutral execution. Rebuilding invalidates previous review manifests.

## Implemented safeguards

- Exact instance/container selection and explicit exclusive/shared vendor handle. A fresh
  descriptor-derived Windows capability signature establishes unique input/output
  IDs and 33–65-byte reports including the ID byte. This is NOT a raw descriptor
  hash. Unknown/ambiguous layouts are rejected. Removal loses the session; no rebind.
- Device type 150 and haptic-grip capability, 16-byte UID, seven big-endian firmware
  words, three format versions, active slot/CRCs, and RAM IDs 1/4/5/6 are validated.
  Snapshot consistency ignores volatile battery/MAC fields, not firmware or UID.
- Query transport accepts only exact forms of queries 01/04/07/A1/A3. The neutral
  executable is linked to a separately compiled native adapter. Both its session
  and adapter enforce the baseline-derived ordered plan, including exact neutral
  frames and saved restoration. No arbitrary/nonzero/trigger/profile writes.
- A machine-wide named mutex, keyed by normalized nonzero container GUID, covers
  snapshots, probes and physical neutral sessions. It fails without waiting on
  contention, access failure or observed abandoned ownership. It coordinates
  cooperating tools only, not third-party processes, and is not a persistent
  recovery journal after all handles close. No device permissions are loosened.
- Session-wide monotonic deadlines, 128-attempt snapshot bounds and 600-ms
  exchange ceilings; 64-report drain cap; known stale matching replies stop the
  session. Unexpected replies stop immediately, rather than consuming a flood.
  Same-opcode attribution cannot be guaranteed by a protocol without transaction IDs.
- No write fallback or retry. Short/late writes, malformed replies, removal,
  trace failures and changed baselines latch a permanent stop. Failed attempts
  consume budget. No destructor sends restoration/cleanup commands.
- Each overlapped operation owns its buffer/event. Empty polls retain their
  pending read. Cancellation is checked without an indefinite completion wait;
  unresolved memory and handles remain alive until worker process teardown.
  This intentional terminal retention is not a reusable recovery mechanism.
- Physical queries run in a hidden, job-supervised child: 30-second protocol
  phase, 35-second parent envelope including discovery/open/finalization, followed
  by at most a one-second termination observation. Kernel calls and termination
  cannot be promised a hard bound. The hidden worker refuses unsupervised launch.
- Neutral supervision allows 80 seconds for a 75-second session (30/15/30), plus
  one-second termination observation. Maximum four mode and three waveform
  commands; cleanup starts by ten seconds into active phase. Parent status uses
  `physical_actuation_attempted:null` for neutral runs rather than inventing
  certainty about a terminated worker. Inspect its evidence and remain fail-safe.
- Raw reports are queued before parsing/discarding, and write intent before
  submission. Trace is bounded to 8 MiB including a 64-KiB metadata reserve, with
  a 1-MiB writer queue. Queue/storage failure stops further traffic; a complete
  result requires successful trace finalization and SHA-256 evidence manifests.
- Source digest is built from the CMake source inventory in
  `build-win/apex6-source-hashes.txt`; the executable itself is hashed at prepare
  time. Rebuilding invalidates earlier review manifests.
- V2 snapshots/reviews/evidence record access mode and bind it into hashes.
  V1 snapshots remain usable offline with unknown acquisition mode, never for
  physical neutral approval. Synthetic snapshots cannot authorize hardware.
- Summaries record query/actuator adapter-write attempts separately from command
  intent, requested flags, cooperative-lock status, and physical/simulated I/O.
  Counts are host attempts, not USB delivery or firmware acceptance. Hardware
  ownership, absence of other writers and response attribution are never claimed.
  Pure open/close probes use access mode `unknown` at the summary level; their
  per-case flags are in `access-probe.json`.

## Input-only coexistence diagnostic

Use only with the exact direct-USB instance selected by `list` and the receiver
unplugged. This diagnostic does not acquire a baseline or authorize effects:

```powershell
.\build-win\Release\ApexSenseBridgeApex6Experiment.exe listen `
  --device 'EXACT_INSTANCE_ID_FROM_LIST' --direct-usb-confirmed `
  --output .\tmp\NEW_LISTEN_DIRECTORY
```

`listen` fixes access to `GENERIC_READ` with `FILE_SHARE_READ | FILE_SHARE_WRITE`;
it accepts no access override. A separate native write guard rejects every report.
It uses the same exact-instance/capability check and cooperative container lock.
It issues only report reads, stops at three seconds or 64 reports, and runs in a
hidden worker with a ten-second parent envelope. Pending read cancellation is
observed for up to 50 ms; unresolved kernel-owned storage is retained until exit.
Unexpected I/O or evidence failure stops the listener, without retries or queries.

`trace.jsonl` labels reports `unsolicited_input`; `listen.json` records bounds and
counts. `io-timing.json` records native operation IDs, host submit/return and
completion-observation times, transferred counts, errors and completed raw buffers.
It also preserves a read that completes during cancellation, which is not included
in the regular listener report count. These are **not USB arrival timestamps**.
Snapshot and neutral workers also export this timing evidence, but their execution
gates and strict reply validation are unchanged. Timing storage is fixed-size;
exhaustion stops submissions and prevents a complete result.

Unsolicited reports establish incoming traffic without this tool sending queries;
they do not identify a writer or prove a previous mismatched reply's cause.
Silence cannot establish exclusive ownership or absence of background traffic.
Any further query or neutral run needs its own authorization and prerequisites.

## One-shot RAM-5 diagnostic

After separate operator authorization, `examine-ram5` accepts the same exact-device,
direct-USB confirmation and new-output-directory options as `listen`. It fixes
shared read/write access and uses a separately restricted native factory: only
the exact A3 payload `01 05` may be written, once, with no retries. No arbitrary
command, RAM-ID or access-mode options are accepted. The native guard rejects
changed reports and any second write, including after a failed first attempt.

It listens for 250 ms before sending, then captures input for 600 ms measured from
write intent (including the write). The overall cap is 64 input reports; a
pre-listen flood prevents the write. I/O/evidence failures stop the diagnostic.
Malformed or mismatching reports are retained during the bounded post-write window
without additional writes. This diagnostic-only behavior does not change Session's
strict fail-stop reply handling. The hidden worker has a ten-second parent envelope.

`ram5-diagnostic.json` distinguishes capture completion from matching reply
candidates. Even a matching candidate proves neither attribution nor acquisition
of a complete baseline. No snapshot or neutral authorization is produced. Inspect
the raw trace and native timing, including possible shutdown-tail input, before
deciding on another separately authorized experiment.

## Validation and outstanding gates

VS 2026 Release: **30/30 CTests passed**. The inert adapter suites now also cover
input-only write rejection, listener time/report bounds, failed reads, cancellation,
trace failure, host timing order and timing-buffer exhaustion. The original 27
CTests plus three new suites cover cross-process
locking, native neutral report-order rejection, and neutral CLI approval gates.
Tests exercise every request and trace-event failure in rehearsal and authorized
fake-physical lifecycles, legacy codecs, expiry before entry, mode propagation,
manifest/approval tampering, and successful/terminated fake workers. Tests use no
physical HID opens; `selftest-readback --access shared --output NEW_DIRECTORY`
also tests parent/worker propagation without hardware and saves a synthetic-only
snapshot, never a physical baseline.

Still outstanding:

1. Actual Windows descriptor/readback qualification on the direct USB connection.
2. Operator review of restoration errors, all-disabled uncertainty and fail-stop
   policy before generating five-minute approval for the neutral-only runner.
3. Observed neutral lifecycle, unchanged postflight RAM and independently confirmed
   normal vibration recovery. A write completion or exit code is insufficient.
4. Differential DSP validation against the recovered Python reference, followed
   by separately reviewed finite left/right/stereo replay manifests. Nonzero
   physical replay and real packet timing are not implemented by this milestone.
5. The previous host PCM stall and native-game capture gates remain open for a
   later live bridge. No package or compatibility claim was upgraded here.
