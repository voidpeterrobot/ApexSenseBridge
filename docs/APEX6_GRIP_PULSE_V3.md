# Fixed 4x strength comparison pulse

Historical v3 candidate: its physical run completed with matching postflight;
the operator felt stronger feedback and confirmed normal input/vibration without
restart or unexpected movement. Current commands now target the operator-requested
[v4 12x comparison](APEX6_GRIP_PULSE_V4.md).

The operator requested a physical test of the offline strength update and selected
"4x for a more obvious comparison". The v3 pulse commands used the separate
`grip-left-pulse-v3` scope. Previous v1/v2 approvals are rejected. Their fixtures
and saved evidence remain historical artifacts.

The base waveform retains the v2 1/16 peak with half-strength edges. The shared
`GripStrength` stage used by the WAV resampler applies gain **4**, then a symmetric
peak limit of **1/4** before quantization. This yields a 1/4 middle-cycle peak and
1/8 edge peak, with zero clipping and zero full-scale overrange for this signal.
It tests the gain stage on a known tone; it does not test live PCM capture or
resampling. Software amplitude is not calibrated motor force. The fixed candidate
has no runtime amplitude argument or automatic gain increase.

Duration, frequency, packet count and timing limits are unchanged: one 64-ms
125-Hz left-grip tone; 11 waveform packets including neutral lead/tail; four mode
writes; 28 queries; right samples neutral; trigger enable clear. The existing
entry/exit/restore sequence and fatal-failure policy remain in force.

The independent native transport enforces the exact reports and sequence. Review
binds base amplitude, gain, peak limit, policy, signal metrics, every packet,
baseline and executable/source/fixture hashes. Fresh matching baseline acquisitions
and the launcher's three interactive approvals precede physical execution.

Verification on 2026-09-29: Release build and all **33 CTests passed**, including
shared-strength regressions, exact fixture bytes, v1/v2 approval rejection,
gain/limit review tampering, and native rejection of prior-strength packets.
Fresh matching physical baselines and a 43-write simulated rehearsal preceded
the successful supervised comparison summarized in [the findings](APEX6_FINDINGS.md).

Exact left sample tables (neutral, edge, middle):

- `8080808080808080`
- `808b8f8b80747074`
- `80969f9680696069`

Fixture: [grip-left-pulse-v3.json](fixtures/grip-left-pulse-v3.json), SHA256
`42c70cb58780713d1a2ea32cc8f004a6328de6270485e47fbe5d11c2f8f6d155`.

Afterward compare perceived strength/location/stop, ordinary input and normal
vibration before restarting, and software postflight. A completed diagnostic still
returns exit 2 and retains unverified restore envelopes as evidence.
