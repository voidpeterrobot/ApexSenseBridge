# Apex 6 haptics: offline implementation

The [integrated beta](APEX6_INTEGRATED_BETA.md) reuses this codec and DSP for
physical grip output. This guide describes the transport-free library and CLI.
`asb_apex6_offline` and `ApexSenseBridgeHapticOffline` have no HID, XInput,
VIIPER, audio playback, driver or device transport dependencies. The offline
executable does not open hardware. The integrated backend links the library;
the output-isolated capture executable remains separate. No hardware connection
is needed for the commands below.

## Implemented

- GPA6 private 32-byte frames and binary32-compatible waveform quantization.
- Strict compact/padded/V21 reply parsing. Zero-count envelope errors remain
  distinct from normal mode-status refusals; neither is treated as success.
- Active-slot/fingerprint/chunk/CRC validation for offline RAM assembly and
  conservative layout-2 grip restoration decoding. No profile writes.
- Signed stereo/four-channel 48-kHz s16le conversion: final two channels,
  independent 769-tap Blackman FIR histories, 350-Hz cutoff, 48:1 decimation.
  Fragment state persists; explicit reset discards history, phase and fragments.
  Overrange is counted before quantization; no RMS/rumble synthesis.
- Bounded fake-clock packet queue (default 40 ms, allowed 8–100 ms), oldest-data
  dropping, no catch-up bursts, sample aging and neutral underrun padding.
  This is a scheduling component, not an integrated live session. Future callers
  must reset the resampler AND queue on stream-generation changes or stale gaps.
- Offline WAV preview: selector 0, trigger column disabled, eight grip samples
  per packet. General selectors/enables exist only in the pure codec for fixture
  comparison, not as preview or device-output options.

## Build and test

```powershell
& 'C:\Program Files\CMake\bin\cmake.exe' --build .\build-win --config Release --target ApexSenseBridgeApex6OfflineTests ApexSenseBridgeHapticOffline --parallel
& 'C:\Program Files\CMake\bin\ctest.exe' --test-dir .\build-win -C Release --output-on-failure
```

Print a software-only packet preview (JSONL) from the diagnostic fixture:

```powershell
.\build-win\Release\ApexSenseBridgeHapticOffline.exe render-wav .\tmp\apex6-diagnostic-2s-v1.wav
```

Generate a local diagnostic WAV with `scripts/New-CaptureDiagnosticFixture.ps1`.
An offline render is not a replacement for live host/capture verification.

The preview accepts only regular RIFF WAV files up to 64 MiB, one canonical
16-byte PCM format chunk, 48 kHz, 16-bit, 2/4 channels, and whole 1-ms blocks.
It does not repair malformed formats, resample other rates or open endpoints.
Gain defaults to 1; `--gain 0..12` sets fixed software strength and
`--peak-limit 0..1` sets a symmetric ceiling (default 1). Both must be finite.
After filtering, the renderer multiplies signed samples by gain, then clips to
the ceiling before quantization. Zero remains neutral and channels remain
independent. These values are software amplitudes, not calibrated motor force or
approved physical settings. For example:

```powershell
.\build-win\Release\ApexSenseBridgeHapticOffline.exe render-wav input.wav --gain 2 --peak-limit 0.125
```

The header records both settings. The summary reports `clipped_samples`,
`peak_before_limit`, `peak_after_limit` and `overrange_samples` (samples exceeding
full scale before clipping). A peak limit can clip without full-scale overrange.
Metrics accumulate across feed calls and resets, like the existing frame counters.
Settings are fixed for the render: no automatic normalization or live gain changes.
The separately reviewed [v4 physical pulse candidate](APEX6_GRIP_PULSE_V4.md)
uses this same strength stage at a fixed operator-requested gain of 12; WAV options
do not change or authorize its physical sequence.
FIR delay is 8 ms; the tail is not flushed. Last packets use neutral padding.
Timing fields are nominal sample positions, NOT measured USB timing. A complete
summary and exit code 0 are both required; redirected output has no hash manifest.

## Verification and limits

2026-09-29 configurable-strength update: Release build and all **33 CTests passed**.
New DSP/CLI cases cover signed limiting, gain linearity below the limit, zero gain
and zero ceiling, silent-channel preservation, chunk invariance, clipping metrics,
default/explicit-setting equivalence and malformed/duplicate/nonfinite options.
Existing experiment, native guard and command-isolation regressions also pass.

Local VS 2026 Release validation on 2026-09-27: **23/23 CTests passed**,
including 3,671 native offline checks and 10 CLI cases. The existing 20 tests
also passed before edits. Both offline render smoke tests passed:

| Input | Input PCM frames | Output stereo frames | Packets | Neutral padding |
|---|---:|---:|---:|---:|
| New diagnostic, 2 s | 96,000 | 2,000 | 250 | 0 |
| Recovered original, 4.5 s | 216,000 | 4,500 | 563 | 4 |

Both reported zero pre-quantization overrange. Neither test opened hardware,
attached a virtual controller, played audio, or transmitted a packet.

The native tests cover all 71 supplied quantizer cases, all 48 encoder packets,
four synthetic RAM blocks and corruption cases, fragmented input,
independent channels, signed extrema, DC, impulse/group delay, tones, stopband,
reset, gain/overrange and bounded queue behavior. CLI tests reject malformed WAVs.
Source/linkage checks guard separation from the capture and bridge paths.
Attribution and exact source hashes are in `THIRD_PARTY_NOTICES.md` and
`tests/fixtures/apex6/README.md`.

No claim of bit-exact Python DSP equivalence, Windows physical I/O timing,
actuator response, restored normal rumble, or mechanical fidelity is made.
The prior live PCM stall remains unresolved; this work does not alter playback.

## Next gates (not implemented or authorized by this stage)

Update: the separate [query-only transport and neutral rehearsal milestone](APEX6_EXPERIMENT.md)
now implements the transport/readback software and fake-device fault tests below.
It has not performed live readback; physical neutral execution and nonzero replay
remain locked. This offline preview target and its safety boundary are unchanged.

1. A separate single-owner Windows experiment transport: exact HID descriptor
   and unit binding, raw trace before parsing, session-wide deadlines and command
   budgets, overlapped I/O lifetime/cancellation ownership, no retries/fallbacks.
2. Fake-transport fault matrix: floods, stale same-opcode replies, late completion,
   short writes, every mode failure, UID/profile changes and storage exhaustion.
   A same-opcode match cannot by itself establish reply attribution.
3. Fresh read-only identity/firmware/configuration baseline and concrete finite
   neutral-only manifest review. Historical RAM values are not restore defaults.
4. Separately approved neutral lifecycle with observed normal-vibration recovery;
   only afterwards consider a separately approved short low-amplitude pulse.

There is no `execute`, live haptic switch, physical transport or automatic cleanup
writer here. The report's old restoration errors and all-disabled uncertainty
remain open. Process termination cannot establish that physical motors stopped.
