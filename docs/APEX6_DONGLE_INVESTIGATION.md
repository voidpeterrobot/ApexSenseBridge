# Apex6 Pro 2.4 GHz dongle investigation

Status: the revised neutral sequence with validated UID boundaries completed
with matching postflight and operator-observed recovery without restart. A fixed
low-strength left, right and both-grip pulses also completed, with correct-side delivery and normal
recovery confirmed by the operator. Wireless support remains experimental. Findings below
were collected on 2026-09-30 with the operator confirming that the controller's
USB cable was disconnected and only the 2.4 GHz dongle was connected.

## Observed compatibility

- Windows exposed model VID/PID `37D7:2502`, the same pair used by the tested
  direct-USB connection. The vendor collection was `MI_02`, usage page `FFA0`,
  usage 1, with 33-byte input and output reports. The gamepad and vendor
  collections shared a container. A separate `MI_03` collection used usage page
  `FFEE` and 64-byte reports; it was not opened for protocol commands.
- A single GPA6 information query completed through exclusive vendor access.
  It reported device type 150, the haptic-grip capability bit, and connection
  field **1**. The existing prototype interprets 1 as wired and 2 as dongle.
  This observation means that mapping cannot reliably classify this setup.
- Two independent, query-only grip-baseline acquisitions completed. Each used
  the existing exact 14-query sequence, validated the 64-byte motor mapping,
  and completed native I/O finalization. Their canonical baselines matched.
- These checks sent no actuator, mode-change, profile-selection, or persistent
  configuration-write commands. No virtual controller or isolation session was
  started. Matching baselines establish query compatibility, not haptic delivery.
- With the operator reporting the controller powered off and the dongle still
  plugged in, the Apex6 vendor and gamepad collections disappeared from HID
  discovery. The unchanged input backend refused to open because no matching
  gamepad remained. This verifies absence at startup for orderly power-off;
  it does not verify an existing session's removal callback or sudden radio loss.
- After the operator turned the controller back on in dongle mode, a fresh
  instance of the unchanged input backend opened successfully and completed
  186 polls over approximately one second using its unique VID/PID-matched
  XInput path. This checks input availability, not individual button mappings
  or virtual-controller forwarding under isolation.

## Implications for the bridge

The existing discovery predicate also accepts these receiver descriptors. Its
"direct USB" comment describes the previously tested connection, not an enforced
transport distinction. Neither the USB parent ancestry nor the observed GPA6
connection field proves that the controller is attached by cable. Do not use
successful discovery as evidence that wireless operation is supported.

The PCM processing, held HID synthesis, strength controls and virtual DualSense
feedback pipeline could be reused if the receiver correctly carries the native
grip protocol. They converge on the same waveform output path; successful query
traffic does not validate that path's bandwidth, delivery, latency or recovery.

## Qualification still required

- Detect controller power-off or radio loss while the receiver remains plugged
  in. Check for retained device nodes, cached input, successful writes to an
  absent controller, and automatic reconnection. A running session must stop
  rather than resume output after an unobserved radio reconnect.
- Verify input forwarding and isolation with the receiver, including hotplug and
  any new proxy/controller interfaces. Preserve pre-launch isolation.
- Verify mode entry, neutral output and exit/restoration before nonzero output.
  Existing experimental direct-USB approval formats cannot authorize a wireless
  test by falsely supplying the direct-USB confirmation.
- Under supervision, verify left/right/both/zero, held HID, PCM takeover, fresh
  HID resume, gain/mute/reset, normal stop and controller recovery.
- Measure sustained output at minimum 8-ms spacing with the existing 4-ms native
  write deadline. Successful host writes alone do not prove radio delivery.
  Keep fail-stop behavior and do not relax deadlines to obtain a passing result.

Until these checks are complete, keep dongle support explicitly experimental and
bounded rather than claiming USB feature parity. Adaptive triggers and onboard-profile switching remain outside
the Apex6 feature set regardless of connection method.

## Opt-in neutral diagnostic

Build with `ASB_BUILD_APEX6_DONGLE_EXPERIMENT=ON` to obtain
`ApexSenseBridgeApex6DongleExperiment`. Normal builds default this option off;
the executable is not part of the portable/installer payload. It accepts only:

```text
ApexSenseBridgeApex6DongleExperiment neutral --dongle-confirmed --supervised --output NEW_DIRECTORY
```

Use `readback` in place of `neutral` for the query-only path. It cannot promote
the native transport to output mode.

`neutral-boundaries` is a separate supervised variant prepared after the passive
official capture. It inserts one UID query before each of the three shutdown
mode commands, validates the returned identity, and requires each mode reply as
before. Its scope is `apex6-dongle-neutral-uid-boundaries-v1`; it permits 46
lifecycle writes (31 queries, four mode commands, 11 neutral waveforms), plus
the unchanged 30 startup queries. The original `neutral` path remains available
for offline regression; do not repeat its known failed physical sequence.

Those flags attest to the actual dongle-only connection and a present operator.
Close games, the normal bridge, Flydigi Space Station/service and other controller
writers first. Observe the entire test and be ready to power off the controller
on a failure or unexpected movement. The diagnostic does not open a browser,
launch a game, or attach a virtual controller.

Each invocation obtains two new matching baselines, then repeats the preflight
immediately before entry. The diagnostic holds one exclusive handle throughout.
Each startup baseline is followed by a UID read checked against that baseline,
for 30 startup queries; this separates consecutive identity queries at the
baseline/preflight boundaries. The fixed output is one neutral lead, eight neutral
test packets and two neutral tail packets, followed by combined exit, saved
left/right restoration and matching postflight. The transport wrapper rejects
nonzero waveforms, more than 11 waveform packets or more than 43 lifecycle writes.
Triggers remain disabled. This is a neutral transport/lifecycle check, not a
nonzero pulse or live-feedback session.

The diagnostic reuses exclusive native transport, strict Apex6 isolation,
input/health monitoring, MMCSS scheduling, minimum 8-ms spacing, the native 4-ms
write deadline, fail-stop behavior, and the visibility recovery watchdog.
Dispatch lateness is recorded using the integrated beta policy. The one-second
active limit and 90-second process watchdog are additional bounds; neither
permits retries or automatic reconnection. Q requests orderly stop; cancellation
or faults stop traffic without speculative recovery commands.

It uses a separate `apex6-dongle-neutral-v1` evidence scope and accepts no legacy
approval files or direct-USB assertions. A completed command sequence and matching
postflight still leave physical recovery unverified. Preserve anomalous restore
reply classifications and collect the operator's input/vibration observations
separately before advancing to nonzero output.

Automated preparation passed the neutral diagnostic tests (including every
lifecycle write failing, cancellation at every step, disconnect, packet budgets,
nonzero rejection and anomalous restore classification), the existing live stream
and native deadline tests, the simulated one-hour test, and all five isolation
regressions. A CLI check also rejected `--direct-usb-confirmed` before device
access. These results do not constitute physical dongle output qualification.

The final native build completed. All 43 native checks passed across the full
suite and the targeted rerun of the pulse CLI check. That check initially failed
because Git had converted its byte-hashed JSON fixture to CRLF; restoring the
reviewed LF bytes and pinning their line endings in `.gitattributes` fixed it
without changing the fixture hash or weakening validation.

## Supervised neutral experiment outcome

### Instrumented evidence

- A sandbox-only startup attempt could not create the isolation recovery marker;
  no device commands were sent. The next attempt established isolation, completed
  one 14-query baseline, then timed out on the first identity query of the second
  baseline. There were no actuator commands.
- A separate query-only attempt retaining one exclusive handle reproduced that
  same boundary timeout. Handle reopening alone therefore did not explain it.
- Adding one validated UID query after each startup baseline allowed both
  baselines and both boundary queries to complete and match (30 queries). This
  supports a sequence-dependent issue with adjacent identical identity requests;
  duplicate suppression is a hypothesis, not an established firmware contract.
- The subsequent neutral run completed those 30 startup queries and the 14-query
  entry preflight. Grip entry returned a normal success acknowledgement. All 11
  neutral waveform writes completed within the native 4-ms deadline, with zero
  recorded dispatch-lateness events. Host completion does not prove radio delivery.
- The combined-exit write completed, but no reply arrived within the existing
  600-ms deadline. The runner stopped immediately: two mode writes total, no
  individual restoration commands, no postflight, no retries or follow-up device
  writes. Native cancellation and evidence finalization completed; isolation
  restoration reported success.

### Operator observations

The operator reported no unexpected movement during the neutral run and normal
input and vibration after fully powering off and restarting the controller.
These observations do not convert the missing exit reply into protocol success
or establish ordinary in-session restoration.

At this stage, nonzero pulses, PCM/HID output and live wireless sessions remained pending.
The receiver's mode-exit/acknowledgement behavior still needs a validated contract.
Do not retry the failed actuator sequence or treat a timeout as a successful exit.

## Passive official dongle reference

The operator used the official application's grip controls with games/audio
stopped and both trigger effects off, then reported: "Finished; master is OFF,
no unexpected movement." This is an operator observation, not a baseline or
postflight result. No custom controller commands were sent during recording.

The address-filtered USBPcap recording contained 2,130 complete records over
approximately 120 seconds. Its injected device descriptor matched the Apex6
model. No nonzero USB completion statuses were present. The recorder required
forced process termination at its duration limit; offline validation found no
partial records. Neither property proves that every firmware reply was delivered.

The relevant mode traffic was:

| Phase | Requests visible at USB | Replies visible before the next phase |
| --- | --- | --- |
| Enable | Trigger target `02`, mode `03`, parameters `00 40`; grip target `12`, mode `02`, parameters `40 00` | One normal success envelope |
| Disable, about 9.3 seconds later | Trigger target `02`, mode `00`; grip target `12`, mode `00` | One normal success envelope |
| Restore, about 85 ms after grip disable | Trigger targets `00` and `01`, mode `00`; grip targets `10` and `11`, mode `01`, parameter `40` | One normal success envelope and one zero-count/value-1 envelope |

All eight mode requests and four replies had valid checksums. There were no
waveform (`57`) writes. The official app sent trigger-mode commands despite the
operator keeping trigger effects off; this capture is therefore not a strictly
grip-only protocol sequence and must not be replayed as one.

A previously recorded wired official session contained 23 mode requests and 23
mode replies. Its final disable/restore phase had the same six requests as this
dongle capture, with six replies rather than three. Both captures pipeline
same-opcode requests without transaction identifiers. Do not assign individual
replies to requests solely by their order, or interpret the dongle's one disable
reply as a proven acknowledgement of grip exit.

Together with the repeated-baseline timeout, the reduced reply count supports
investigating suppression of identical responses in the receiver path. This is
a hypothesis: the trace cannot locate suppression, establish a general rule, or
exclude loss/coalescing. The zero-count/value-1 reply remains unverified even
though the official application emitted the surrounding sequence.

The prepared `neutral-boundaries` diagnostic tests whether a distinct, validated
UID response between shutdown operations allows each mode reply to arrive. This
is a bounded neutral-only hypothesis test, not an established receiver fix. It
keeps fresh matching baselines, supervision and explicit reply attribution;
missing replies are never accepted. The native submission guard rejects omitted,
repeated, misplaced or identity-mismatched boundary queries. Production USB
sessions retain their original sequence.

All 43 native regression checks passed after this change. Added tests cover
failure/cancellation at every one of the 46 lifecycle writes, wrong/missing UID
responses, missing exit acknowledgement, anomalous restore replies, and native
boundary ordering. A synthetic duplicate-response model fails on the original
sequence and completes with UID boundaries; this demonstrates the hypothesis
test, not real receiver behavior. Nonzero wireless output and integrated support
were still pending at that stage.

### Physical result of the UID-boundary neutral variant

An initial invocation found no eligible connected controller and stopped before
isolation or device commands. After the operator confirmed reconnection through
the dongle, one supervised `neutral-boundaries` run completed:

- Two startup baselines and their UID boundaries matched (30 queries).
- The lifecycle completed 31 queries, four mode writes and 11 neutral waveform
  writes. All native waveform deadlines passed, with zero dispatch-lateness
  events. The entry and combined-exit replies were accepted as normal success.
- Each shutdown mode operation followed a validated UID boundary. Both individual
  restoration replies retained the classification
  `captured_zero_count_value_1_unverified`; they are not ordinary success replies.
- The postflight baseline matched. Native finalization, evidence completion and
  physical-controller visibility restoration all reported success.

This establishes completion of the revised neutral protocol sequence on the
tested dongle connection. It supports the boundary-query hypothesis but does
not locate or prove duplicate-response suppression. It does not establish
nonzero haptic delivery or sustained wireless sessions. Operator confirmation of
ordinary input/vibration was collected separately: "No unexpected movement;
input and vibration work without restart." This does not reclassify either
anomalous restore reply.

### Fixed grip pulse diagnostics

`pulse-left` uses a distinct `apex6-dongle-left-pulse-v1` evidence scope and the
validated UID shutdown boundaries. It generates 32 fixed waveform packets
(256 ms of samples) at 80 Hz and peak sample amplitude 0.0625 on the left grip,
with no live feedback or gain control. The lead and two tail packets are neutral;
the right grip and trigger columns remain neutral. Dispatch delays may extend
wall-clock time; the existing one-second active bound, 8-ms minimum spacing and
4-ms native write deadline still apply.

Both session and native submission boundaries enforce the exact pulse pattern,
at most 35 waveforms and the original lifecycle ordering. There are at most 70
lifecycle writes (31 queries, four mode commands, 35 waveforms) and 30 startup
queries. Q may terminate the pulse early through the neutral tail and checked
shutdown; faults stop immediately without recovery writes. This mode is an
opt-in diagnostic, not packaged wireless support.

All 43 native regressions passed after adding the pulse, including failure and
cancellation at every lifecycle write, fixed-pattern/side/amplitude rejection,
early orderly stop, missing exit replies, native packet limits, spacing and the
4-ms write deadline. The subsequent supervised physical run completed all 30
startup queries and the 70 lifecycle writes. There were no dispatch-lateness
events or native deadline faults; postflight matched, and native/evidence
finalization and visibility restoration reported success. Both individual
restore replies remained `captured_zero_count_value_1_unverified`.

The operator separately reported: "Brief left-only pulse; stopped fully; normal
operation without restart." This establishes short, low-strength left-grip
delivery and observed recovery on the tested dongle connection. Held HID, PCM
takeover, live controls, sustained sessions and radio-loss behavior
remain unqualified. No integrated wireless enablement follows from this result.

The same bounded diagnostic now accepts `pulse-right` (160 Hz on the right) and
`pulse-both` (left 80 Hz plus right 160 Hz). Each uses its own evidence scope,
`apex6-dongle-right-pulse-v1` or `apex6-dongle-both-pulse-v1`, with the same
256-ms sample duration and peak amplitude 0.0625 per active grip. The selected
side is fixed before transport promotion; both validation layers reject packets
for another side. Triggers, waveform/write budgets, deadlines and UID-separated
shutdown remain unchanged.

All 43 native checks passed with these variants, including invalid-side rejection,
wrong-side rejection at both boundaries, exact per-side waveform composition,
all 70 write faults and cancellation points per variant, and native timing limits.
Each subsequent physical run completed all 30 startup queries and 70 lifecycle
writes, matched postflight, recorded zero dispatch-lateness events and met native
write deadlines. Native/evidence finalization and visibility restoration succeeded.
Both individual restore replies in each run remained classified as
`captured_zero_count_value_1_unverified`.

Operator observations were recorded separately:

- Right: "Right only; stopped fully; normal operation without restart."
- Both: "Both grips worked; stopped fully; normal operation without restart."

These results qualify the bounded low-strength left/right/both pulse checks only.
The ordinary integrated USB session still uses its original startup and shutdown
sequence. Before live wireless testing, a separately selected diagnostic session
must retain one exclusive handle across the 30-query startup, use validated UID
boundaries at shutdown, and enforce a bounded live-test duration. Successful
pulses do not authorize silently routing a receiver through the USB-only session.

## Experimental live game session

The opt-in dongle build also produces `ApexSenseBridgeApex6DongleLive`, separate
from the installed/packaged application. It reuses the integrated virtual
DualSense, physical input forwarding, raw PCM, held HID rumble, gain/mute controls,
audio-default protection, isolation monitoring, session ownership and maintenance
stop. Its launch contract is:

```text
ApexSenseBridgeApex6DongleLive bridge-triggers --dongle-confirmed --supervised --grip-gain 1 --telemetry-json PRIVATE_PATH
```

It requires both attestations and a telemetry destination before discovery and
rejects unrelated physical test commands. Normal `ApexSenseBridge` rejects these
diagnostic flags. It does not launch a game. Wait for readiness before manually
launching one, and keep competing writers closed. The matching asb9-or-later DLL
and build record are verified and pinned as in the integrated USB session.

The live diagnostic retains the exclusive handle through both startup baselines
and their validated UID boundaries (30 queries), idle readiness, fresh entry
preflight and shutdown. Shutdown uses a validated UID query before each mode
command, with no missing-reply bypass. The selected native guard permits up to
ten minutes of active feedback and twenty minutes total; the session requests
orderly stop slightly before the total native deadline. Q and maintenance stop
also shut down orderly. Disconnect, invalid feedback, native I/O failure and lost
isolation retain fail-stop behavior, with no automatic reconnection or speculative
recovery writes. The standard amplitude ceiling, 8-ms minimum spacing and 4-ms
native waveform-write deadline remain enforced.

The experimental session defaults to gain 1, supports the usual console controls,
and does not persist consent or gain changes into shared production settings.
It records bounded telemetry under `apex6-dongle-live-diagnostic-v1`. Anomalous
restore replies remain unverified evidence even if postflight and observations
are normal. Game exit alone is not a stop signal for this standalone console;
exit the game and press Q when finished, or allow the bounded session to end.

Preparation passed all 43 native regression checks, including a simulated
ten-minute dongle lifecycle, native startup promotion, shutdown UID boundaries,
early orderly stop, total-time rejection, and unchanged USB/capture regressions.
CLI checks rejected missing attestations, missing telemetry and unrelated
actuator commands, and confirmed normal builds reject diagnostic flags.

The first supervised live build reached readiness with isolation, baseline checks,
virtual attachment and input forwarding initialized. The operator subsequently
reported Endfield worked as expected. They ended the run by closing the console
window; input continued to work, but vibration returned only after restarting the
controller. No final session report was produced. This is gameplay evidence and
a forced-close recovery observation, not verified orderly shutdown or postflight.
The feedback source and exact session duration cannot be established from the
remaining instrumented evidence.

## Tray and normal CLI integration

The normal engine exposes this path only with `--apex6-dongle-beta`.
Tray persists an off-by-default **Use 2.4 GHz dongle (experimental)** checkbox
in the shared control center and passes the flag only for a selected Apex6.
It does not infer transport from the ambiguous VID/PID or connection field.
The existing Apex6 consent, global strength controls, pre-launch readiness,
session-token stop, game-exit handling and maintenance stop remain in use.
USB and Apex4/5 routing are unchanged. Playnite's Apex6 path remains USB-only.

The integrated dongle path uses the same 30-query startup and validated shutdown
boundaries as the live diagnostic and scope `apex6-dongle-beta-v1`. As of
2026-10-01, integrated sessions run continuously, without the former ten-minute
active or twenty-minute total cap; explicit CLI `--seconds` still applies.
The standalone diagnostic retains both bounds. The UI states the possible controller
restart after forced stop or disconnect. Normal builds and portable packages do
not require experimental CMake flags. The diagnostic binaries remain opt-in and
excluded from package payloads. Public evidence deliberately does not upgrade
the successful game observation into full wireless qualification.

The shared control center offers **Start bridge** beside **Stop bridge**, using
the existing manual pre-start lifecycle without launching a game. Stop clears
the manual activation setting and requests orderly shutdown. Users should wait
for readiness before opening a game or tester. Component watchdogs, isolation
checks, native write deadlines and shutdown UID boundaries remain enforced.

### Reference tooling

`scripts/Record-Apex6UsbReference.ps1` records one freshly enumerated USB address
for a bounded duration/size. It sends no controller commands and does not launch
applications or change services. Select the address from a fresh USBPcap mapping;
never copy a previous machine's device address. Its output includes raw identity
data and belongs in ignored private storage.

`scripts/summarize_apex6_reference.py` validates PCAP record framing, the selected
model descriptor and single-address scope. It exports mode traffic and aggregate
command/waveform counts without unit identifiers or configuration-query bodies.
It deliberately does not infer reply attribution or physical recovery. Synthetic
tests cover malformed/truncated captures, scope rejection, privacy, waveform
validation and the distinction between normal and anomalous mode envelopes.

Raw discovery output and baselines contain local identities and configuration.
Keep them in ignored local diagnostic output; do not commit them to this public
repository.
