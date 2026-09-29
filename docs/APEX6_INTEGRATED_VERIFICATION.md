# Apex6 integrated beta verification

Recorded 2026-09-29. This records automated checks and limited supervised
testing, not complete hardware qualification or publication approval. Public
findings are summarized in [APEX6_FINDINGS.md](APEX6_FINDINGS.md); per-device
evidence, configuration snapshots and operator logs are excluded from Git.

## Automated checks

| Check | Recorded result |
| --- | --- |
| Windows x64 Release, experimental options disabled | Passed |
| Windows x64 Release, experimental diagnostics enabled | Passed |
| Normal native CTest suite | 27/27 passed, no skips |
| Full native CTest suite | 41/41 passed, no skips |
| Tray and Playnite Release builds | Passed |
| Shared managed Apex6 settings/control suite | Passed |
| Managed Tray suite, including whitelist and UAC handling | 148 assertions passed |
| One-hour simulated mixed PCM/HID continuous lifecycle | Passed in both native configurations |
| Matching libVIIPER asb9 raw ABI | Passed without host attachment |
| Version contract | Passed, 0.6.3 |
| Portable payload verification and diagnostic-runner exclusion | Passed |
| Playnite package root structure | Passed |
| Bundled prerequisite hashes | Matched pinned versions |

The simulated hour advances a fake clock; it is not a one-hour physical run.
It bounds feedback queues and retained fake transport history and exercises late
dispatch without catch-up bursts. Native tests cover framing and write deadlines.
Callback tests use a fake DLL, including malformed feedback and teardown races.
Isolation tests cover exact state matching, preservation of unrelated changes,
stale ownership and reused process IDs. Driver-level crash restoration and live
configuration/writer changes still require qualification.

Run native suites with `ctest --test-dir <build-directory> -C Release
--output-on-failure`. Managed projects are `tests/Apex6ManagedTests.csproj` and
`tests/ApexSenseBridgeTray.LearningTests.csproj`. Test RAM snapshots and identities
are synthetic; encoder vectors and selected identity-free protocol reports are
documented in [the fixture notes](../tests/fixtures/apex6/README.md).

## Supervised hardware observations

The operator confirmed independent inputs/triggers, PCM left/right/both/zero,
HID output held beyond two seconds, explicit HID zero, PCM takeover without
mixing, fresh HID resumption, gain changes, mute/reset and orderly Q/UI recovery.
Machine postflight matches and operator observations are separate evidence.

Early sessions stopped on incomplete/late native writes. One native-game write
completed all 33 bytes but was observed 3.642 ms beyond the 4-ms deadline.
Reconnecting restored input; restoring vibration required a controller restart.
The fail-stop policy and native deadline were preserved. Scoped MMCSS scheduling
was added to the vendor worker. A subsequent Endfield session completed about
13 minutes of streaming (92,635 packets), stopped on game exit, matched postflight
and had operator-reported normal operation/recovery. Sampled private memory was
55.9–57.9 MiB. This single run does not prove the timing failure cannot recur.

Deliberate USB disconnect caused native read/write failures and recovery-required
status, with no speculative commands or automatic restart. Reconnection restored
controls; a controller restart restored feedback. A fresh session confirmed mute
at gain 0, reset to gain 1 and normal input after UI shutdown.

The captured zero-count/value-1 restore envelopes remain **unverified**, even
when postflight matches and the operator reports normal recovery. They must not
be relabeled as successful restoration acknowledgements.

## Control center and whitelist

Tray embeds executable selection, Launch, beta consent, live gain, mute/reset
and source status in its main window. Playnite embeds the same controls in
extension settings. Exact executable paths are stored per user; consent and gain
use shared atomic settings. Hidden or unloaded panels stop polling, and refreshed
preferences do not echo commands to the engine.

Automated tests cover old-settings migration, exact-path matching, persistence,
readiness before process creation, preparation/launch failure cleanup, UAC error
740 retry, cancelled-UAC cleanup and tracking game exit with background detection
disabled. Managed tests cover concurrent preferences, stale-session IPC rejection
and embedded controls. The dark layout was inspected using the offscreen preview
helper without creating a bridge or game process.

The latest whitelist/UAC launch flow still needs a supervised real-game check.
The earlier successful native-game session used manual pre-start activation.
Flydigi Space Station and its service were subsequently started at the operator's
request with the bridge off; no completed conflict-test observation was reported.
Do not infer coexistence or successful startup refusal from that setup alone.

## Remaining acceptance work

- Supervised whitelist launch, elevation approval/cancellation and game-exit stop.
- Playnite host pre-launch, ownership conflicts and global maintenance stop.
- Real isolation health changes: competing writer/service, proxy appearance,
  allowlist changes, stale watchdog state and crash restoration.
- Longer physical reliability testing after the scheduling mitigation.

Follow the [beta checklist](APEX6_INTEGRATED_BETA.md). Packages were built for
local review, not installed or published. Per-build hashes belong with local
artifacts, not in this source-level report. Rebuild packages after source changes.
The result remains an integrated beta.

## Pre-commit cleanup verification

After replacing historical RAM/configuration fixtures and Windows instance
suffixes with synthetic data, the x64 Release rebuild and all 41 native tests
passed again, including the simulated hour. Tracked and untracked source passed
Git whitespace checks; local documentation links resolved. The public candidate
tree was reviewed for personal paths, device-instance suffixes, unit/container
identifiers and raw evidence. Obsolete session reports and the stray diagnostic
output were removed; useful findings are consolidated in the public summary.
Ignored local evidence and build/package directories are not commit inputs.
