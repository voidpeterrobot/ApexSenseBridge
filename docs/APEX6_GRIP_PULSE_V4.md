# Fixed 12x comparison pulse

The operator reported a stronger but still weak pulse at gain 4 and requested
16x, then changed the request to **12x before any new physical run**. The final
candidate is 12 times the original 1/16 base, or three times the preceding gain-4
candidate: software peak/ceiling 0.75, with half-strength edge cycles at 0.375.
The shared gain stage now accepts finite gains in [0,12]; output remains bounded
by a finite ceiling in [0,1]. No scale beyond the encoder range is introduced.

Current pulse commands target `grip-left-pulse-v4`. Old v1/v2/v3 approvals are
rejected. The physical candidate remains fixed: one 64-ms, 125-Hz left tone,
11 waveform packets, four mode writes and 28 queries. All deadlines, native
sequence guards, reply policies, neutral right-grip data and disabled trigger
enable are unchanged. No automatic replay or gain increase is available.

The prior v3 run completed
with matching postflight and exit 2; all seven recorded file sizes/hashes matched.
The operator confirmed ordinary input/vibration without a restart and no unexpected
right-grip/trigger or continued vibration. Its worker restoration flag remains
false; feedback is stored separately in `operator-report.json`.

V4 left sample tables (neutral, edge, middle) are:

- `8080808080808080`
- `80a1afa1805e505e`
- `80c3dfc3803c203c`

The middle cycles range from `20` to `df`. Software amplitude is not a calibrated
motor-force claim. Planned clipping and overrange are zero because the source
peaks exactly at the ceiling. The independently generated exact packet fixture is
[grip-left-pulse-v4.json](fixtures/grip-left-pulse-v4.json), SHA256
`3f3d5e06ae2c6acd956f3df9fbf9d6914cf7414d9423acfe3925b1cc171650ba`.

Fresh matching baselines, successful offline checks/rehearsal and a separate
five-minute single-use interactive approval precede execution. Compare pulse
strength/location/stop and ordinary vibration before restarting afterward.

Verification on 2026-09-29: Release build and all **33 CTests passed**, including
12x packet fixtures, older approval rejection, gain/limit tampering and native
rejection of full-scale packets. Two fresh baselines matched and a simulated
43-write rehearsal completed. This document does not claim physical qualification
of the fixed v4 pulse. Later live-strength checks are recorded in
[the integrated verification summary](APEX6_INTEGRATED_VERIFICATION.md).
