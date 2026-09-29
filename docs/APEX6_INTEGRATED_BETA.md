# Apex6 Pro integrated beta

This is an opt-in development integration, pending complete physical qualification of the
integrated build. Existing Apex4/5 sessions retain their existing backend.

## Starting a session

Use direct USB. Close Flydigi Space Station and stop its service and other
controller writers. The bridge checks this and never restarts Flydigi or changes
its startup configuration. Disconnect unrelated XInput controllers. HidHide must
have no active/inverse or pre-existing hidden-device configuration for this beta.

In the Tray control center, enable **Apex6 Pro grip beta** once in the embedded
grip controls. Under **Game whitelist**, choose **Add game…** and select the actual
game `.exe`, then select it and click **Launch**. The bridge prepares isolation
and waits for readiness before starting the game. It stops when the game exits,
including when background game detection is disabled. Failed bridge startup
leaves the game closed; failed game launch stops the prepared bridge. Adding a
path does not launch anything. The list starts empty and preserves existing settings.
If Windows reports that the game requires administrator rights, Launch requests
UAC approval for the game only. Cancelling approval stops the prepared bridge.

Use actual game executables; launcher shortcuts, URLs and launch arguments are
not supported by this first launch-list UI. Games requiring a launcher should use
Playnite's pre-launch integration or manual preparation. Late automatic detection
still refuses to start Apex6 because it cannot isolate handles a running game
already opened. Apex4/5 background detection retains its existing behavior.

For manual preparation, close the game, turn on **Configuration → Force continuous
activation**, and wait for **ready, awaiting feedback** in the same control center
before starting the game yourself. Use **Stop bridge** for a manual stop.

In Playnite, use the embedded grip controls in extension settings and enable the
desired game's bridge profile. Playnite starts isolation in its pre-launch hook
and waits for readiness. Do not select an onboard Apex profile for this controller.

CLI example, after `ApexSenseBridge list`:

```powershell
.\ApexSenseBridge.exe bridge-triggers 0 --controller-model apex6-pro --apex6-beta-consent --grip-gain 1
```

Consent is stored once. Subsequent runs omit `--apex6-beta-consent`. With multiple
eligible controllers, an explicit current list index is required. The UI control
panel accepts that index; it never persists a machine-specific device instance ID.
Sessions run continuously unless `--seconds` is supplied. No game is launched by
the CLI or the implementation/build scripts.

## Output and controls

HID motor commands remain active until changed. Zero, relinquishing haptics mode,
active PCM takeover, generation changes, shutdown and faults clear held commands.
Quiet or discarded stale PCM does not cancel a valid held motor command. After
active PCM takes over, a new HID command is required to resume synthesized rumble.
The left and right synthesized carriers remain 80 Hz and 160 Hz.

The shared control center shows awaiting-feedback, PCM, HID, muted and failure
status alongside the gain slider, **Mute**, and **Reset to 1**. No separate Apex6
configuration window is opened. Playnite embeds the same controls in its settings.
Global gain ranges from 0 to 12 (default 1), with 0.5 UI steps, a 100 ms ramp and
the fixed 0.75 waveform ceiling. CLI keys: `+`/`-`, `0` mute, `1` reset, `Q` orderly
stop. Tray/Playnite stop and maintenance stop also use orderly shutdown. Ctrl+C,
disconnect, malformed feedback, failed I/O and lost isolation are fail-stop:
there is no automatic reconnect or speculative actuator recovery. In supervised
tests, USB reconnection alone restored input but a controller restart was needed
to restore vibration. After a fault, leave the bridge off, stop tester/game output,
restart/reconnect the controller and check normal behavior before starting again.

HID sliders do **not** need audio setup. PCM haptics require four-channel 48 kHz
audio routed to the virtual DualSense. Signed channels 2/3 are resampled and gain
is applied once afterward. Queues and PCM age remain bounded (40 ms); dispatch
lateness is counted without aborting solely for lateness. Minimum 8 ms spacing,
no catch-up bursts, and the native 4 ms waveform-write deadline remain enforced.
The vendor-I/O worker uses scoped Windows multimedia scheduling (Pro Audio/high
priority); this reduces scheduling contention without relaxing the native deadline.

## Lifecycle and compatibility

Before readiness, the engine verifies isolation, two matching startup baselines,
input and virtual attachment. An exclusive vendor handle remains held while
waiting indefinitely for eligible feedback. Waiting sends no actuator commands.
The fresh entry preflight occurs on that handle immediately before grip entry.
Only one worker submits vendor reports; input/control and watchdog paths are
separate. Executable/library hashes are pinned and rechecked at entry. The DLL
must also match the SHA-256 and asb9-or-later revision in its packaged build record.
Recovery serializes marker ownership with restoration and checks process creation
time, so a stale watchdog cannot claim a newer process that reused the same PID.
Input reuses the tested capture reader: exact-container HID where complete, or a
uniquely VID/PID-matched XInput source with removal monitoring when the HID
descriptor combines triggers. It never accepts an unchecked XInput slot.

The integrated backend requires raw audio/HID/event ABI 1 (asb9 or later), with
callback unregistration as a quiescence barrier. A sidecar or incompatible DLL is
rejected. Capture stays isolated from actuator libraries. Diagnostic runners and
their approval formats remain separate and are not included in normal packages.

Shared settings live in `%LOCALAPPDATA%\ApexSenseBridge\apex6-settings-v1.txt`.
Both applications update them under a named mutex with atomic replacement.
Gain IPC has its own version and session token; the existing readiness/status
layout remains unchanged. Routine telemetry is bounded in
`%LOCALAPPDATA%\ApexSenseBridge\Logs\apex6-last-session.json`, or the explicit
`--telemetry-json` path. Raw audio is diagnostic-only. Save session telemetry
before another session if retaining multiple qualification runs.

Restore classifications and raw mode replies are retained as unverified evidence,
even if postflight matches. Keep operator observations in a separate report;
neither successful postflight nor a normal subjective recovery verifies an
anomalous restore reply.

## Verification and remaining qualification

Automated coverage includes held HID beyond two seconds; zero, stale/silent PCM,
PCM takeover, generation and gain/mute; bounded queues; native framing/deadlines;
fault and orderly lifecycle; raw callback ownership/teardown and ABI rejection;
controller routing; concurrent settings and token-scoped IPC; strict isolation
state comparison and preservation of unrelated configuration during restoration.
A synthetic one-hour continuous mixed PCM/HID lifecycle checks memory bounds and
late dispatch handling. These tests do not substitute for driver/hardware tests.

Supervised checks on 2026-09-29 confirmed held HID, PCM takeover, gain/mute/reset,
Q/UI recovery, a roughly 13-minute Endfield streaming session and game-exit stop,
and deliberate disconnect followed by explicit controller recovery. Earlier runs
hit the native-write deadline; the successful longer run followed the scheduling
mitigation. A single successful run does not establish that the fault cannot recur.
Restore reply anomalies remain unverified. Full real-driver crash/configuration
and Playnite-host qualification remain pending.

Qualification checklist:

1. Web tester: hold a slider longer than two seconds; left/right/both/zero;
   PCM takeover; gain, mute and reset; orderly Q exit and observed recovery.
2. A repeatable native-game event, then a longer supervised physical session.
3. Controlled disconnect, explicit recovery/restart and no automatic reconnect.
4. Real HidHide/service/proxy changes, exclusive-open conflicts and crash
   restoration; verify unrelated concurrent configuration is preserved.
5. Tray/Playnite pre-launch readiness, game exit, maintenance stop and ownership
   conflicts with actual games. No existing game handle is claimed to be isolated.

Do not describe this as hardware-qualified until those observations are recorded.
Packaging produces local review artifacts only; installation/publication is a
separate action.
