# First bounded grip waveform experiment

Historical v1 contract. Current pulse commands and the launcher target the
[v4 candidate](APEX6_GRIP_PULSE_V4.md). The v1 fixture below remains unchanged;
the command examples produce the current version, not the historical waveform.

Status: **runner verified offline; first approved physical attempt failed on
submission lateness; pulse remains unqualified**. See the
[physical result](APEX6_FINDINGS.md). Recovery after a power cycle
was operator-confirmed. Later scheduling and strength observations are summarized
in the findings; each physical diagnostic still needs a fresh scoped approval.
This plan follows the [qualified silent lifecycle](APEX6_FINDINGS.md).
That result supports entry/exit and observed recovery for one silent run. It does
not qualify waveform delivery, amplitude, timing or recovery after nonzero output.

## Fixed candidate

Use the separate scope `grip-left-pulse-v1`. Start with one brief left-channel
signal and retain the existing grip encoder's selector/enables. No game capture,
48-kHz resampling, live audio queue or FIR history participates in this first
endpoint probe. The later captured-PCM experiment will exercise that separate path.

| Setting | Fixed value |
| --- | --- |
| Grip sample rate / packet size | 1,000 samples/s; eight samples per packet |
| Signal | Eight cycles of a quantized 125-Hz sine, 64 ms nominal |
| Software peak before quantization | `1/32 = 0.03125` |
| Envelope | First and last cycles at `1/64`; middle six at `1/32` |
| Leading / trailing neutral | One / two packets: 8 ms / 16 ms nominal |
| Total nominal stream | 88 samples, 11 packets, 88 ms |
| Waveform header byte | `98`: selector 0, both grips enabled, trigger disabled |
| Right and trigger sample bytes | Always `80` |
| Repeat count / gain adjustment | None |

The right grip remains enabled with neutral data to preserve the existing
`gripWaveform` framing. “Left-channel” describes the injected samples; physical
channel separation must be observed. This software peak is an uncalibrated test
setting, not a motor-force percentage or a demonstrated physical safety threshold.
The short low-level signal may be imperceptible; that outcome is inconclusive.

The canonical cycle is `[0, sqrt(1/2), 1, sqrt(1/2), 0, -sqrt(1/2), -1, -sqrt(1/2)]`.
The current native encoder yields these exact left-channel byte tables:

| Packet kind | Eight left-channel bytes |
| --- | --- |
| Neutral | `80 80 80 80 80 80 80 80` |
| Half amplitude | `80 81 81 81 80 7e 7e 7e` |
| Full amplitude | `80 82 83 82 80 7d 7c 7d` |

The [design-only packet fixture](fixtures/grip-left-pulse-v1.json) contains every
32-byte USB body and corresponding 33-byte Windows report, including checksums.
Its SHA256 is `cb06525400813b38dda4026e9919a0f8031fe7b566130065e27fa62ae05bdf13`.
It is neither a physical capture nor an executable approval. It was generated
offline using the current `Gpa6.cpp::gripWaveform` and checked for all channel
bytes, selector/enables, lengths and checksums. No HID library or transport was
linked into the local generator.

## Implemented ordered contract

1. Acquire the fixed 14-query grip baseline and require an exact match with the
   reviewed fresh baseline. Require the already-qualified 33-byte ID-00 layout,
   saved modes `01`, parameters `40`, and restore targets `10`/`11`.
2. Send entry `53 / 01 12 02 40 00`; require a normal success ACK.
3. Submit the 11 exact fixture reports in order. Packet 0 is neutral; 1 and 8 are
   half amplitude; 2–7 are full amplitude; 9 and 10 are neutral. No waveform ACK is
   expected or inferred from a complete write.
4. After the final neutral packet's bounded hold, send combined exit
   `53 / 01 12 00`; require a normal success ACK.
5. Send baseline-derived left restore `53 / 01 10 01 40`, then right restore
   `53 / 01 11 01 40`, serially. Only these two steps admit the exact captured
   zero-count/value-1 envelope as unverified evidence, or a normal success ACK.
6. Acquire the existing 14-query postflight and compare identity, firmware, UID,
   format versions, profile CRCs and RAM6 with the baseline.

Normal completion means **43 report writes: 28 queries, four mode writes and
11 waveform writes**. Thus actuator-write count is 15. Do not add periodic queries
between waveform packets, all-disabled frames, trigger targets, configuration
writes, alternate selectors, arbitrary WAV loading, extra neutral packets or retries.
Neutral lead/tail packets are newly introduced waveform behavior and must be
disclosed in the pulse review; the silent run did not validate them.

## Timing and cancellation

These are acceptance limits for the separate pulse path. Existing lifecycle,
query and observation commands retain their current timing and behavior.

- Mode/query exchanges remain bounded by 600 ms. Pre/postflight phases remain
  30 seconds each; active phase is five seconds; total session is 75 seconds.
- Start packet 0 within 10 ms of the accepted entry reply, after readiness checks.
  Anchor waveform time `t0` at the first submission. For packet `k`, nominal due
  time is `t0 + 8*k` ms. Record actual native submission/completion times separately.
- Never submit early. Allow at most 2 ms lateness against the original schedule.
  Also require at least 8 ms since the preceding actual submission: wait until
  `max(nominal_due, previous_submit + 8 ms)`. If both constraints cannot be met,
  fail before submitting. Keep the original `t0`; do not reset phase to hide drift.
- Each waveform write must complete all 33 bytes within four ms of its submission,
  subject to the earlier active/session deadlines. A late complete write is still
  a failure; it is never resent. Mode/query deadlines are not shortened.
- After packet 10 completes, hold for eight ms and submit the combined exit
  within the following two ms. The waveform window through exit submission must
  be under 100 ms from `t0`. The exit reply retains its 600-ms exchange deadline.
- Recheck cancellation, device presence, trace health and timing after every wait
  and immediately before every submission. Use bounded monotonic waits, not timer
  resolution/service changes or busy-waiting without a cancellation check.

These bounds describe host behavior, not measured actuator duration. A blocked
host or lost device can prevent exit/restoration; process termination cannot
guarantee stopped motors. The operator must remain ready to disconnect/power off.

Cancellation before entry sends zero actuator writes. Cancellation after entry
latches stop and sends no further reports, including neutral/exit/restore. This
first version deliberately has no alternate cancellation cleanup sequence. It
requires operator disconnect/power-off and records an incomplete, uncertain run.
Do not clear a cancellation/failure latch when a late ACK arrives.

Any malformed, stale, missing or late mode/query reply, normal nonzero status,
unknown zero-count envelope, short/failed waveform write, device loss, timing
miss or trace/storage fault stops all traffic. During waveform delivery, any
unsolicited vendor report is retained raw and is fatal; it must not be silently
discarded or treated as a waveform ACK. Polling must remain bounded and must not
consume the submission timing allowance unnoticed.

## Implementation boundaries

The `prepare/rehearse/approve/execute-grip-pulse` commands use a distinct
approval type. Physical execution is only in the existing separately built,
supervised actuator executable. The query executable may prepare/rehearse and must
refuse pulse approval/execution/workers before discovery. Production and capture
executables remain output-isolated.

Implement a fixed-packet pulse runner and native ordered/timed guard. Do not widen
the existing `Session::exchange` three-waveform budget or its silent-lifecycle
reply exception globally. Restore exceptions must be bound to the explicit
restore steps; the silent runner's positional `index_ == 3 || index_ == 4` rule
cannot be copied into the longer pulse sequence.

Do not use `PacketScheduler::tick` unchanged: it discards missed/stale samples and
pads underruns. This experiment has an immutable in-memory packet list and must
fail on a missed deadline, missing packet or changed bytes. The native adapter
must independently reject changed amplitude/channel bytes, packet count/order,
too-short inter-submit spacing, excessive lateness, changed modes and extra writes.
Native timing must be checked immediately before OS submission, not only in the
outer scheduling loop.

Review/approval must bind exact reports and sample tables, timing/stop/reply policy,
baseline, scope, fixture hash and executable/source hashes. Preserve five-minute
expiry, interactive grouped prompts and single-use claims. Silent, neutral,
restore-only and observation approvals cannot authorize a pulse. Any amplitude,
duration, channel, packet or policy change requires a new scope/version or review
as appropriate, new rehearsal and fresh approval; no runtime gain knob.

The launcher should compare two independent fresh baselines, prepare/rehearse,
then present three prompts: readiness, exact pulse/risks, manifest SHA256. Before
running: normal input/vibration confirmed, direct USB, receiver unplugged, official
app and other known writers closed. Background ownership remains uncertain.

## Evidence and qualification

Keep hashed raw traces, native timing, exact review, approval and a dedicated pulse
result. Record mode/query/waveform write counts, waveform completion, actual packet
spacing/lateness, all restore classifications, postflight comparison, cancellation,
failure and `restoration_verified: false`. Preserve native finalization and trace
integrity status. Software completion still returns **2**, failure **1**; display
“diagnostic complete; operator qualification pending” rather than physical success.

Obtain the operator's pulse-location/intensity/stop observation immediately after
the run, followed by ordinary input/normal-vibration checks. Retain that report
separately from the immutable software result.

| Outcome | Required interpretation |
| --- | --- |
| Pass for this pulse | Complete trace and timing within bounds; correct 43-write sequence; matching postflight; operator identifies the intended brief left-grip pulse ending promptly, no unexpected behavior, normal input/vibration afterward |
| Inconclusive | Software complete and recovery normal, but pulse absent, too faint or location unclear; stop and review, no automatic repeat or gain increase |
| Failure / unexpected behavior | Any software fault, continued/strong/unexpected actuation, trigger movement, or input/vibration problem; end physical testing, disconnect/power off as needed, preserve evidence |

Even a pass qualifies only this fixed signal on this reviewed device/configuration.
Right-only, stereo differentiation, captured PCM and game feedback remain later
separate steps. Matching RAM alone is never physical recovery verification.

## Offline acceptance before offering a run

- Native encoder regression against all 11 fixture reports; exact right/trigger
  neutral bytes and disabled trigger bit; no all-disabled report or other selector.
- Fake-clock pacing at boundaries: on time, 2-ms lateness limit, early wake, clock
  reversal, completion at/after deadline, accumulated drift and no catch-up bursts.
  Exercise the final neutral hold and entry/exit timing bounds independently.
- Every write/read/trace failure; stale and unsolicited reports; missing replies;
  disconnect; changed baseline/profile/firmware; expiry immediately before entry;
  cancellation at each boundary. Assert no report after a latched stop.
- Both restore ACK/anomaly combinations, with the anomaly rejected on entry/exit
  and by all existing commands. Preserve raw classification on rejected replies.
- Native guard rejects missing/extra/reordered packets, changed channels/amplitude,
  changed restore parameters, waveform bursts, configuration/trigger commands,
  reused approvals and expired approvals. No HID opens in native hook tests.
- CLI isolation, canonical review regeneration, fixture/source/binary/hash binding,
  freshness/single-use handling, all result fields and distinct exit-code-2 display.
  Test launcher defaults in a real Windows PowerShell invocation to cover the
  previously observed `$PSScriptRoot` parameter-default failure.
- Run the full existing CTest suite plus new regressions. Review a successful
  offline rehearsal against fresh baselines before obtaining physical approval.

## Offline verification and operator entry point

The Release build and all **33 CTests** passed on 2026-09-28. The new portable
pulse tests performed 6,158 checks. Native hook tests use inert event handles,
never HID discovery or opens. Coverage includes every report-byte mutation,
write/read/trace faults, cancellation at every write boundary, mode reply policy,
changed baselines, pacing/hold boundaries, native submission guards, approval
expiry/reuse, fixture/source/executable binding, query executable isolation,
supervised cancellation, exit code 2, and the Windows PowerShell launcher default.
These tests do not establish physical timing or motor response.

The historical synthetic rehearsal completed without physical device access.
It records 43 simulated writes, both restore anomalies as unverified, matching
postflight, and `restoration_verified: false`. No device was opened. Its
`manifest.json` hashes the review, trace, summary and dedicated `grip-pulse.json`.

Implementation is in `src/apex6/experiment/Pulse.{h,cpp}` and
`PulseEvidence.{h,cpp}`. The native adapter owns an independent ordered/timed
guard. Scheduling uses a cancellable per-object Windows high-resolution timer;
there is no timer-resolution change or fallback. The supervisor passes a shared
cancellation event to its hidden worker. Physical execution still requires the
separate interactive approval; prior lifecycle tokens do not authorize it.

After operator readiness is confirmed and two new matching grip baselines have
been acquired, run this in an interactive PowerShell console (replace paths):

```powershell
& .\scripts\Invoke-Apex6GripPulseTest.ps1 `
  -Baseline .\tmp\fresh-baseline-1\worker\grip-baseline.asb `
  -Confirmation .\tmp\fresh-baseline-2\worker\grip-baseline.asb `
  -Output .\tmp\fresh-approved-pulse
```

The launcher resolves its default executable inside the script body, compares
distinct baselines no more than five minutes old, prepares and rehearses the
exact review, and then presents the three approval prompts. Approval expires
after five minutes and is consumed once. Ctrl+C stops subsequent traffic;
disconnect/power off after a failed or cancelled physical run. Preserve the
software evidence and record operator observations separately. An unclear pulse
is inconclusive and does not authorize a stronger or repeated test.

The original software milestone passed, but the first physical attempt exposed
accumulated scheduling delay. The next step is offline timing analysis and
verification before another fresh baseline/approval cycle. This document and
the offline rehearsal authorize no physical report writes.
