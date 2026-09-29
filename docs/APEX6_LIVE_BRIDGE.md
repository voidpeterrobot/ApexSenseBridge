# Experimental live grip bridge

`ApexSenseBridgeApex6LiveBridge` is a separate, unpackaged Stage 2 experiment.
It forwards physical input to a raw-capable virtual DualSense and routes original
48-kHz four-channel s16 PCM channels 2/3 through the existing FIR and 48:1
resampler to Apex6 grips. V2 also translates valid motor-strength commands from
that same virtual DualSense into grip waveforms. Trigger output remains disabled.
The regular bridge and capture recorder remain output-isolated from this target.

Use the asb9 raw-capable backend for audio source tests. Earlier asb8 completions
used independent fixed 2-ms timers, allowing 10-ms PCM requests to arrive much
faster than real time and overwhelm the bridge's 40-ms backlog policy. Asb9
serializes completions by valid packet sample duration without changing sample
values or channels. This applies only while raw capture is registered. Initial
host pipeline bursts and scheduler-related slow delivery remain possible; the
bridge still enforces its queue/age policy. This correction is not physical grip
qualification.

Build with `ASB_BUILD_APEX6_EXPERIMENTS=ON` and
`ASB_BUILD_APEX6_LIVE_BRIDGE=ON`. Run the complete CTest suite before hardware.
For an offline 60-second simulation with evidence:

```powershell
.\build-win\Release\ApexSenseBridgeApex6LiveBridge.exe rehearse --output .\artifacts\live-rehearsal-new
```

The interactive launcher takes exact vendor and gamepad instance IDs (the two
interfaces must have the same container). Existing experiment `list` and capture
`list-inputs --json` commands can identify them. Direct USB, receiver unplugged,
other controller writers closed, observed ordinary input/vibration, and active
operator supervision are required.

```powershell
.\scripts\Invoke-Apex6LiveBridge.ps1 -Device 'EXACT_VENDOR_INSTANCE' `
    -InputDevice 'EXACT_GAMEPAD_INSTANCE' -XInputDevice 'EXACT_XUSB_MI_00_INSTANCE' `
    -IsolatePhysical -Fixture
```

`-Fixture` generates a new signed 125-Hz fixture: ten seconds neutral, two left,
two right, four alternating (half-second segments), two neutral. The base peak
is 1/16; default gain is 1. It is played in exclusive four-channel PCM through
the audio endpoint identified by this virtual DualSense's unique serial. Its
path and hash are included in the review. No default playback endpoint is used.
For the native-game qualification, omit `-Fixture`, launch the game manually,
and reproduce one documented native DualSense haptic event after input is ready.

For browser audio/haptic testing, verify that Windows exposes the current virtual
DualSense playback endpoint as four channels (quadraphonic), then select that
endpoint explicitly in the tester's audio player and enable its haptic output.
Selecting the controller through WebHID and moving vibration sliders exercises
the separate HID motor-strength path added in v2. It does not exercise raw PCM.
Four-channel configuration is required for the audio test, not the HID test.
Each capture currently creates a fresh virtual serial, so recheck the current
endpoint and browser output selection after restarting. The capture ABI's fixed
four-channel format does not prove the Windows shared-mode mixer is configured
for four channels. The controlled fixture uses exclusive four-channel playback.

The launcher runs `prepare`, `rehearse`, `approve`, and `execute`. Preparation
acquires two fresh matching 14-query grip baselines. Approval requires a real
console, explicit setup/risk confirmations, and the matching successful rehearsal.
The review hash is automatically bound to approval; manual hash entry is not
required. Reviews and approvals must be used within five minutes. Execution
claims a single-use approval file and rechecks the device, baseline, executable,
compiled-source identity, libVIIPER and optional fixture hashes. Pulse tokens are
rejected by the distinct `apex6-live-grips-v2` scope. V1 live approvals are also
rejected because the feedback and isolation contracts have changed.

During execution, `+` / `-` change gain by 0.5, `0` requests mute, `1` requests
gain 1, and `Q` requests orderly shutdown. Every gain target ramps from its
current value over 100 output samples (100 ms), bounded to 0..12. The software
ceiling is fixed at 0.75 with clipping and pre/post-limit peaks recorded. This is
not a calibrated motor-force measurement. There is no automatic normalization.
Ctrl+C requests immediate fail-stop, including suppression of physical cleanup.

After virtual attachment and input readiness, valid PCM or a nonzero supported
HID motor command must arrive within
120 seconds. No actuator write precedes eligible feedback. A fresh 14-query preflight
immediately precedes entry; pre-entry PCM is discarded. The streaming timer
starts at entry, lasts at most 60 seconds, and permits at most 7,500 stream
packets plus one neutral lead and two neutral tail packets. Actual submissions
are at least 8 ms apart, each dispatch can be at most 2 ms late, and waveform
writes must complete within 4 ms. There are no catch-up bursts or periodic
configuration queries. The native transport independently enforces order,
grip-only enable bits, neutral trigger bytes, checksum, quantized amplitude,
deadlines, four mode writes, and 28 queries.

Callbacks perform bounded validation and owned copying into a 4-MiB queue;
individual records are limited to 1 MiB. Sequence loss, overflow, invalid PCM,
disconnect, cancellation, native I/O or trace faults latch failure. Trailing URB
padding is never PCM. Failed packets are fatal and never played. Stale records
and processed backlogs exceeding 40 ms are discarded; generation/gap changes
reset queued samples and FIR history together. Underruns send neutral samples.

Duration expiry or Q discards audio, sends the two neutral tail packets, holds
for 8 ms, exits grip mode, restores saved left then right mode, and compares
postflight. Captured anomalous restore envelopes remain unverified evidence.
Failure sends no speculative physical cleanup or reconnect. Teardown stops the
input thread, neutralizes virtual input, quiesces callbacks, detaches, and checks
audio defaults. A job supervisor caps the worker at 240 seconds and itself at
245 seconds; a forced termination cannot guarantee physical or audio recovery.

Worker evidence includes raw records, physical trace, native submission/completion
timing, gain targets, queue/drop/underrun and clipping metrics, stop reason,
restore classifications, postflight comparison, audio defaults, and file hashes.
Console key timestamps and supervisor results are separate. Operator reports
are saved outside worker evidence. Exit 2 means a completed diagnostic awaiting
operator assessment; exit 1 means failure. Matching RAM never proves recovery.

Physical qualification remains required: verify input forwarding, correct left,
right and alternating grip identity, live gain controls, Q shutdown, and ordinary
vibration afterward; then repeat with one native-game event. Record the game,
event and observations. Stage 2 is complete only after these observed results.
Peak calibration and further latency optimization remain follow-ups.

### Recorded web-audio qualification (2026-09-29)

With the asb9 backend and native Q timestamp-order correction, the operator
confirmed left/right grip separation. A subsequent run completed approximately
97 seconds of waveform output, gain increase,
mute, gain reset and Q shutdown. Its postflight matched; audio defaults and
HidHide were restored. The operator confirmed normal input and vibration without
power cycling afterward. Raw evidence and operator reports remain private.

The restore envelopes remain classified as captured anomalous/unverified; the
operator report is separate from worker evidence. Dropped samples, underruns and
history resets remain measurable. This is a successful web-audio functional
check. Later integrated native-game observations and remaining qualification are
recorded in [the verification summary](APEX6_INTEGRATED_VERIFICATION.md).

## Continuous source testing

Use `-Seconds 0 -Quiet` with the interactive launcher to enable grip transfer
until Q without a fixed session duration. The two interactive confirmations and
automatic integrity checks remain. `-Quiet` clears approval details once accepted
and omits the final diagnostic questionnaire. Gain still starts at 1 and stays
within 0..12, with the fixed 0.75 software ceiling.

Continuous mode also removes the 120-second PCM wait timeout and the 7,500-packet
total limit. The 8-ms minimum spacing, 2-ms dispatch lateness limit, 4-ms write
deadline, neutral trigger bytes, framing/checksum, exact lifecycle order, one
neutral lead, two neutral tails, four mode writes, and 28 queries remain enforced
independently by the native transport. Q before PCM exits without grip entry.

For this explicitly approved mode, authorization is activated once while fresh
and remains valid in that worker while waiting for sources. The single-use file,
hash checks, device checks, pinned library, and fresh baseline preflight still
apply. Approval cannot be activated after expiry or reused in another session.

The worker and supervisor use progress watchdogs (10 and 15 seconds), rather
than a maximum session duration. Worker progress comes from the main session
thread, not the input polling thread. Teardown remains bounded to 240/245 seconds;
the native tail/exit deadlines are unchanged. Parent loss or a stalled process
still fails closed. No reconnect or automatic retry is performed.

Raw and physical trace files stream to disk using bounded memory queues without
a cumulative storage budget in continuous mode. They grow for the duration of
the run; disk or queue failure stops output. Native timing records are drained
to `worker/native/trace.jsonl` through a bounded asynchronous writer, retaining
their submission/completion timestamps and wait metadata. Timed-mode budgets and
the standalone capture recorder's default limits remain unchanged.

Continuous rehearsal simulates five minutes, then requests Q and checks the
neutral tail, restore sequence, and postflight. This verifies software behavior;
it does not qualify indefinite physical operation or prove motor recovery.

```powershell
.\scripts\Invoke-Apex6LiveBridge.ps1 -Device 'EXACT_VENDOR_INSTANCE' `
    -InputDevice 'EXACT_GAMEPAD_INSTANCE' -Seconds 0 -Quiet -IsolatePhysical `
    -XInputDevice 'EXACT_XUSB_MI_00_INSTANCE' -GameExecutable 'C:\Games\Game.exe'
```

## Physical controller isolation for games

### Debug dispatch policy

The separate `build-win-live-debug` configuration can be built with
`ASB_APEX6_LIVE_DEBUG_NO_DISPATCH_LATENESS=ON` (default OFF). It uses scope
`apex6-live-grips-debug-no-dispatch-lateness-v2`, includes the build option in its
source identity, and binds `dispatch_late_us=disabled_debug` in fresh reviews.
Normal approvals cannot authorize it. Pass its live executable explicitly to
the launcher with `-Executable` and select the updated asb9 DLL with `-Library`.
Both artifact hashes remain bound to fresh approval.

This debug policy records waveform dispatches more than 2 ms late but permits
them, including late neutral-tail dispatches and late exit after the tail hold.
The native transport applies the same policy. Minimum 8-ms spacing from each
actual submission, minimum tail hold, 4-ms write completion deadline, 40-ms PCM
age limit, amplitude/framing guards, duration policy, cancellation, and lifecycle
budgets remain enforced. No catch-up bursts are allowed. `result.json` records
the policy and worker-observed late-dispatch count/maximum; the native trace
retains actual submission times. This debug build does not establish timing
qualification for the normal build.

V2 requires `-IsolatePhysical`. It uses the installed HidHide CLI to hide the
selected Apex6's same-container `IG_*` collections, `MI_00` XUSB function and exact
vendor HID interface. It temporarily permits the exact live executable plus
the exact HidHideCLI executable, which self-registers on every invocation.
Their container IDs and the `xusb21`/`xusb22` service are checked before hiding.
Verified Flydigi GeniTech roots and their USB/HID descendants are also hidden,
using the original bridge's root/service ancestry rule. VID/PID alone never
selects another physical controller or VIIPER device.
The vendor grip interface remains accessible to the bridge; the virtual DualSense
remains visible to applications. Live vendor I/O opens exclusively, without a
shared-access fallback. Baseline acquisition retains its query-only shared mode.
Start the
game after isolation is active; an already-running game can retain old handles.
With `-GameExecutable`, the launcher requires that game to be closed before
isolation and again after approval, and rejects a game on the HidHide allowlist.
It records both checks and never closes or launches the game automatically.
Without isolation, games can see both the physical Xbox-compatible controller
and virtual DualSense, switch button icons, and send ordinary rumble directly to
the physical controller without exercising this bridge's PCM transfer.

The launcher snapshots the existing application/device lists and cloak/inverse
states. It refuses pre-existing cloaking, inverse mode, or a nonempty hidden-device
list for separate review. Existing application registrations are temporarily
removed and restored at teardown. It
starts an independent hidden recovery process before any configuration change,
verifies the new configuration, and checks that the live executable can read the
physical input while the unlisted capture executable cannot. A separate direct
XInput probe checks all four slots and rejects any visible XInput controller.
This is an access
probe only; it sends no physical output. The selected native-game process must
also not be whitelisted by the user's existing configuration.

On completion or startup failure, the launcher removes its own additions,
restores removed allowlist entries and restores the cloak state. The recovery process does the same if the launcher
exits unexpectedly. Concurrent unrelated edits are preserved. Snapshots,
verification results, and restoration evidence are saved in `physical-isolation`
beside the worker evidence. This option does not install drivers, change controller
profiles, launch games, or restart Flydigi services. Isolation scripts are included
in the compiled source identity used for fresh review/approval.

## V2 HID rumble and isolation policy

The raw queue accepts feedback only from this session's virtual DualSense. HID
parsing accepts USB output report 0x02 (48-byte common form or 63/64-byte Windows
forms), on interrupt endpoint 3 or an exact HID SET_REPORT(Output 0x02) request
for interface 3. Compatible-vibration v1/v2 and haptics-select flags must request
rumble. LED, trigger, feature, other-interface and other-report data never become
motor output. Malformed supported reports fail the session. Original raw records
remain evidence. The parser follows the same compatible-rumble flag semantics
used by the repository's original DualSense feedback bridge, with an explicit
haptics-select check and raw USB envelope validation.

Large/low motor strength maps to the left grip with an 80-Hz sine; small/high
maps to the right with a 160-Hz sine. Full byte strength has a provisional peak
of 0.125 before the shared gain ramp and 0.75 limiter. These are synthesized
waveforms: strength-only reports do not contain an original waveform to forward.
The same single output worker and native grip framing guards handle PCM and HID.
No XInputSetState, alternate actuator commands or trigger output are introduced.

Motor state lasts until an explicit stop, generation change, active PCM takeover
or two seconds without a valid motor update. This bounded lease can shorten a
game's long, unrefreshed rumble command; continued effects require fresh commands.
Pre-entry PCM is discarded, while a fresh motor-state command may survive the
short entry preflight. Expired commands cannot restart output. PCM above 64 s16
counts on either haptic channel takes priority for 100 ms and clears HID state;
HID commands received during that interval are discarded. Resuming HID afterward
requires a fresh command. Silent/one-LSB PCM does not suppress rumble. Sources are
never summed, and suppressed HID state is never replayed when PCM ends.

The isolation watchdog checks the allowlist/cloak/target set, stopped Flydigi
service, known competing writers, new GeniTech roots and independent XInput
visibility. It renews a session-token lease, bound into review/approval. The input
thread checks it every 250 ms and latches failure on a failure marker, mismatched
token or lease older than five seconds. No filesystem or isolation queries are
added to the raw callback or physical output worker. Isolation changes are
detected on watchdog polls, not atomically at every input event. A failure keeps
isolation applied until worker teardown, then the launcher restores it.

HidHide does not identify the process that produced a virtual USB feedback report.
Any app allowed to write to the virtual DualSense can send a valid command. Close
other virtual-controller writers and start the game after isolation; existing
game/browser handles are not proven revoked by a new-process visibility probe.
Use `-GameExecutable` for the existing game-closed guard, and reopen the web
tester after isolation for a clean browser test. No service is automatically
restarted and no controller profile or driver is changed.

The asb9 raw backend ABI remains unchanged. V2 needs fresh physical qualification
for HID motor commands and the exclusive vendor open; earlier web PCM results do
not establish that new path. Parser, source arbitration, generation/lease expiry,
HidHide selection/rollback and the existing timing/isolation tests run offline.
