# Experimental Apex6Pro input / DualSense capture

This is a capture-first port, not an Apex6Pro actuator driver. It forwards a
selected controller's input to a virtual USB DualSense and records original host
feedback. It does not send Apex vendor commands, vibration, trigger effects,
profile writes, resets, or firmware updates. Do not add device type 150 to the
Apex4/5 identity allowlist or launch the legacy bridge against this device.

The [offline haptic implementation](APEX6_HAPTICS_OFFLINE.md) is a separate
target and is not linked into this recorder. It does not enable actuator output.

## Build and prerequisites

Build the native targets and the **matching** integrated backend. VS 2026 works
with CMake 4.2 or newer; this workspace was verified with global CMake 4.4.3.

```powershell
$env:Path = 'C:\Program Files\CMake\bin;' + $env:Path
.\scripts\build-windows.ps1
ctest --test-dir .\build-win -C Release --output-on-failure
.\scripts\build-libviiper-windows.ps1
```

The capture executable accepts only the raw ABI 1 exports (asb9 DLL). It does
not fall back to legacy energy summaries or a sidecar. The old bridge still uses
its original summary API. Both APIs are exported by the new DLL.

The asb9 backend paces raw-session audio completions serially by each request's
declared PCM duration. The previous fixed 2-ms, independently scheduled replies
could consume audio much faster than real time. Packet payloads and submission
timestamps remain unchanged; trailing padding is excluded from duration. Waiting
does not block input/control handling, and cancellation removes pending replies.
Initial host buffering can still produce a submission burst. Scheduler overhead
can make delivery slower than the nominal rate; no catch-up replies are sent.
Legacy summary-only sessions retain their existing completion behavior.

Backend build scripts preserve existing source directories. To rebuild from a
fresh checkout without deleting development work, pass `-SourceDirectory` with a
new directory. The authoritative backend changes are in the tracked VIIPER patch,
not in ignored `.tmp-*` source directories. DLL and sidecar provenance are written
to `LIBVIIPER-SOURCE.txt` and `VIIPER-SOURCE.txt` respectively.

Live attachment requires the USBip virtual-host driver. A physical Xbox/XInput
driver is not that prerequisite. Installing a driver, hiding a controller, or
changing game settings is an operator action, not performed by this program.

## Commands

From `build-win\Release`:

```powershell
# First host test: disconnect Apex and receiver, leave input synthetic.
.\ApexSenseBridgeCapture.exe record --input synthetic --seconds 10 --output C:\captures\new-run

# Explicit demo inputs (Cross, left stick, L2) on the virtual device only.
.\ApexSenseBridgeCapture.exe record --input synthetic --demo --seconds 10 --output C:\captures\new-demo

# Read-only discovery/probe; no virtual driver is needed for these two commands.
.\ApexSenseBridgeCapture.exe list-inputs --json
.\ApexSenseBridgeCapture.exe input-status --input-device 'EXACT_INSTANCE_ID' --seconds 15

# Forward one explicitly selected physical input source while recording.
.\ApexSenseBridgeCapture.exe record --input-device 'EXACT_INSTANCE_ID' --seconds 60 --output C:\captures\new-game

# Play the fixture to this session's virtual endpoint, never the default output.
.\ApexSenseBridgeCapture.exe record --input synthetic --fixture C:\fixtures\signal.wav --seconds 20 --output C:\captures\new-fixture
.\ApexSenseBridgeCapture.exe verify --capture C:\captures\new-fixture --fixture C:\fixtures\signal.wav
```

The output's parent directory must exist; the run directory must not. Optional
`--library PATH` selects an explicitly built raw-capable DLL. Timed duration is
1–3600 seconds; the fixed storage limit may end long recordings early and
incomplete. `--seconds 0` records until Q without a total duration/storage cap;
the memory queue stays bounded. Q completes the capture normally. Ctrl+C /
Ctrl+Break stop recording and mark it incomplete. Forced termination or
blocked filesystem/driver I/O cannot guarantee bounded cleanup; missing final
metadata or manifest always means incomplete. No physical recovery is attempted.

Input selection prefers the reviewed, read-only generic gamepad HID reader if it
exposes all six independent axes. Otherwise XInput requires one matching VID/PID
slot and one corresponding container. Its identity query is an undocumented
Windows extension: if unavailable or ambiguous, input selection fails, even if
one unrelated controller is connected. Slot numbers are never persisted. Selected
device removal invalidates the source; no automatic replacement is selected.

Mapping: A/B/X/Y → Cross/Circle/Square/Triangle; Back/View → touchpad click;
Start → Options; standard shoulders/stick clicks/D-pad; separate analog triggers
with digital bits above 30. Stick neutral is 128. No gyro, touch gesture, rear-button
or adaptive-trigger capability is claimed. On source loss, virtual controls are
released and the recording stops incomplete. Duplicate physical/game input must
be checked separately. No HidHide configuration is changed automatically.

## Evidence format and ABI 1

Each run contains `session.json`, `transfers.bin`, `events.jsonl`, and a final
`manifest.json` containing SHA-256 hashes of the other three files. Records are
copied into owned memory before callbacks return. Files are written by a separate
thread. Defaults: 64 MiB total including a 64 KiB final-metadata reserve, and
4 MiB queued memory (including record overhead). Continuous capture omits the
total storage cap, so files grow until stopped. Record/index reservations are
conservative; the effective payload budget is smaller. Exhaustion stops capture
incomplete; samples are never silently replaced with silence or old data dropped.

`transfers.bin` concatenates the exact ABI envelopes received from Go. Every
envelope preserves its original payload and packet descriptors. The JSONL index
contains byte offset/length, sequence, generation, timestamp, type, analysis
validity, and queue residence. Raw records with invalid packets are retained,
excluded from PCM statistics, and make the recording incomplete.

All integers are little endian. There are no serialized native structs.

| Offset | Width | Meaning |
|---|---|---|
| 0 | 4 | ASCII `ASBR` |
| 4 / 6 | 2 / 2 | ABI 1 / kind: audio 1, HID 2, event 3 |
| 8 / 12 | 4 / 4 | Total envelope bytes / header bytes (80) |
| 16 / 24 / 32 | 8 each | Sequence / stream generation / monotonic nanoseconds |
| 40 / 44 | 4 each | Endpoint number / USB/IP direction (OUT = 0) |
| 48 / 52 / 54 | 4 / 2 / 2 | Audio rate / channel count / bits per sample |
| 56 / 60 | 4 each | Packet descriptor count / payload byte count |
| 64 | 4 | Transfer status placeholder; unavailable at submission |
| 68 | 4 | Source: iso 1, interrupt 2, control 3, lifecycle 4 |
| 72 / 76 | 4 each | Flags = 1 (submission observation) / reserved = 0 |
| 80 onward | 16 per packet | Offset, requested length, observed actual length, signed status |
| After packets | variable | Original data bytes |

Maximum envelope: 1 MiB; maximum packets: 4096. Unknown ABI, type, format, size or
status semantics fail explicitly. PCM is 48 kHz, four interleaved signed s16le
channels. Requested OUT packet lengths drive analysis because these are USB/IP
**submission** records, not Windows completion reports. Observed actual/status
fields remain intact. Non-contiguous/overlapping packets, errors, or partial PCM
frames fail analysis; trailing URB padding is retained but not interpreted as PCM.
Channel indices 0–3 are authoritative; mechanical routing has not been validated.

HID interrupt payloads are unchanged. Control HID payloads consist of the exact
eight-byte USB setup packet followed by the unchanged data stage. One transfer is
recorded once, before summary parsing. Events contain two uint32 fields:
code/value. Codes: connected 1, disconnected 2, configuration 3, alternate setting
4, callback registration 5, capture failure 6. Alternate value packs interface in
the low byte and setting in the next byte. Generation advances on connection,
disconnection, configuration, and playback alternate-setting changes. Normal local
stop is represented by final session/manifest closure, not a fabricated USB reply.

Exports: `GetASBCaptureCapabilities(1)` must include bits 1/2/4 (audio/HID/events);
`SetDualSenseASBCaptureCallback` uses the legacy callback calling convention but
the new envelope; `AttachDualSenseASBDevice` attaches after registration and neutral
input setup. Unregister is a serialized callback barrier and must not be called
from inside a callback. The c-shared Go DLL stays resident until process exit.

## Validation boundaries

The recorder can be complete without receiving any game PCM. Completeness means
the observed bounded session was preserved, not that native game support or
physical haptics work. Session metadata labels host validation as unvalidated.

The fixture player uses a random per-session USB serial and audio-controller PnP
ancestry to associate exactly one render endpoint. It requires exclusive four-
channel 48-kHz s16le playback with channel mask 0x33. No mixing, default-device,
closest-format, stereo, or physical-endpoint fallback is allowed. Playback errors
and endpoint IDs are retained. Initial Windows host testing is described below;
repeatability is not yet a passed gate.

The verifier checks the canonical complete manifest and all hashes, then compares
one intact generation to all fixture frames. Only extra leading/trailing zero
frames are allowed. It never accepts tolerance-based scaling, inversion, swapped
channels, missing silence, or a partial fixture. The offline verifier currently
limits transfer evidence and WAV files to 64 MiB each.

The original report's 216,000-frame WAV was not supplied in this checkout.
Expected WAV SHA-256:
`667bd4cc16f0c6862eb5ed080620de9b57c2de7ec53b1732ea3e41185c77f5b0`.
Other fixtures can be tested, but verifier output labels `report_fixture:false`.
Synthetic injection and fake-DLL tests are not host-ingestion evidence.

For a newly generated, explicitly non-report diagnostic fixture, from the repo root:

```powershell
.\scripts\New-CaptureDiagnosticFixture.ps1 -OutputPath .\tmp\new-diagnostic.wav
.\build-win\Release\ApexSenseBridgeCapture.exe record --input synthetic --fixture .\tmp\new-diagnostic.wav --seconds 15 --output .\tmp\new-diagnostic-capture
.\build-win\Release\ApexSenseBridgeCapture.exe verify --capture .\tmp\new-diagnostic-capture --fixture .\tmp\new-diagnostic.wav
```

The generator refuses to overwrite files. It makes 96,000 four-channel frames
(two seconds), with silent channels 0/1 and distinct signed integer patterns on
2/3, maximum magnitude 511. It neither plays audio nor accesses devices.
On 2026-09-27, actual Windows exclusive playback and raw USB/IP capture matched
all frames byte-for-byte in five of six runs. One run timed out after retaining
95,616 frames and was correctly finalized incomplete. Its cause remains unresolved;
subsequent diagnostic builds add submitted/padding/capacity/period values to
deadline errors, without retries, sample repair, relaxed verification, or format
fallback. Do not treat the successful runs as repeatability certification or as
reproduction of the original report fixture. See [the findings](APEX6_FINDINGS.md)
and [current verification status](APEX6_INTEGRATED_VERIFICATION.md).

Before claiming support, finish: real attach/descriptor/feature-report inspection,
demo mapping, clean detach/no-orphan checks, exact original-fixture host ingestion,
full physical mapping/disconnect checks, then one installed native game's event.
Record and restore any game-specific translation/hiding settings. Do not use
desktop loopback, magnitude summaries, or ordinary rumble as evidence of PCM.

Physical GPA6 framing, DSP, bounded transport and experiments remain a separate
future workstream. Neutral lifecycle and nonzero effects each need separate review
and approval. This executable provides no switch to enable physical output.
