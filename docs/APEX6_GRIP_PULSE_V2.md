# Doubled-amplitude left-grip pulse candidate

Historical v2 documentation: current pulse commands now prepare the separately
scoped [v4 strength comparison](APEX6_GRIP_PULSE_V4.md).

The operator requested an amplitude increase after the
[v1 timing failure](APEX6_FINDINGS.md). The v2
`prepare/rehearse/approve/execute-grip-pulse` commands targeted the separate
scope **`grip-left-pulse-v2`**. This changes the fixed candidate, not the historical
v1 evidence. The [first v2 physical run](APEX6_FINDINGS.md)
completed all packets, and the operator felt a weak pulse, but RAM6 changed and
normal vibration was absent until restart. That run failed restoration. A later
operator-authorized [toggle-off repeat](APEX6_FINDINGS.md)
completed with matching postflight and reported normal vibration. Its initial RAM6
state differed, so the toggle's causal role remains unproven. Configurable strength
is now progressing in the offline renderer. A subsequent
[toggle-on repeat](APEX6_FINDINGS.md) also matched
postflight, and the operator accepted weak but perceptible pulse delivery.

| Setting | v1 | v2 |
| --- | --- | --- |
| Software peak | 1/32 (0.03125) | **1/16 (0.0625)** |
| First/last cycle peak | 1/64 | **1/32** |
| Tone | 125 Hz, 64 ms | Same |
| Stream | 11 packets, 88 ms nominal | Same |
| Channel configuration | Left signal; right neutral; trigger disabled | Same |
| Sequence | 28 queries, four modes, 11 waveform writes | Same |
| Timing | 8 ms minimum spacing, 2 ms maximum lateness, 4 ms write deadline | Same |

These are pre-quantization software amplitudes, not calibrated motor-force
percentages. The unchanged encoder gives these eight-byte left sample tables:

- Neutral: `80 80 80 80 80 80 80 80`
- Edge cycles: `80 82 83 82 80 7d 7c 7d`
- Middle cycles: `80 85 87 85 80 7a 78 7a`

The [v2 fixture](fixtures/grip-left-pulse-v2.json) records all 11 exact USB bodies
and Windows reports. SHA256:
`1a28e899fe4178cf79769392c5a15e4af5c1d243e38f2e4b3e3d50d7bdc836e4`.
The CLI regression compares each generated report, nominal due time, sample table
and amplitude against this fixture. It rejects a v1-scope approval even when
paired with the current review hash. Source/executable hashes also invalidate
prior approvals. The diagnostic display retains support for saved v1 evidence.

The amplitude change alone does **not** resolve the scheduling failure. The
scheduling correction below is now verified offline. The next physical attempt
requires two fresh matching baselines and a new interactive approval. No
deadline is relaxed and no automatic retry, gain adjustment or cleanup is added.
The launcher displays the current scope and amplitude; v2 is historical.

Verification on 2026-09-28: Release build and all **33 CTests passed**. The
synthetic rehearsal completed
the exact 43-write sequence with exit 2 and zero device opens. Fake-clock success
does not establish real Windows timing or physical pulse qualification.

## Minimal scheduling correction

The real-clock probe `ApexSenseBridgeApex6PulseTimingProbe` uses the real Windows
timer, synthetic replies and the independent native policy. It links no HID
transport/discovery. Its declared workload models 25 us per presence check and
1 ms write completion; these are test inputs, not measured controller guarantees.

The original scheduler failed all 20 trials on submission lateness.
Packet preparation and intent recording now
precede the final wait. The high-resolution timer wakes 1 ms early, followed by
at most 1 ms of cancellation/clock polling with a bounded iteration count. Final
device-presence, trace, input-drain and native timing checks remain after waiting.
No global timer-resolution or priority changes are made. The review binds this
wait policy, and the result records preparation, due, wait-return and final-check
timestamps separately in `dispatches` (zero denotes an unreached timestamp).

The revised scheduler completed **20/20 trials**, maximum accepted lateness
**775 us**, below the unchanged 2,000 us limit. Release and all **33 CTests** passed again,
including cancellation during/after waiting and reversed/stalled clock refusal.
This supports the next supervised experiment, not a hard real-time guarantee.
Further performance optimization is deferred at the operator's request.
