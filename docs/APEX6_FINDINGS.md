# Apex6 Pro engineering findings

This is the public summary of development and supervised testing through
2026-09-29. It replaces the per-session reports and early roadmap. For current
operation, use [the integrated beta guide](APEX6_INTEGRATED_BETA.md); for test
coverage and outstanding qualification, see [verification](APEX6_INTEGRATED_VERIFICATION.md).
Raw captures, device identities, configuration snapshots and operator logs stay
outside the public source tree.

## Protocol and restoration

- GPA6 uses 32-byte USB bodies and 33-byte Windows HID reports, including report
  ID zero. Do not infer reply attribution from pipelined official traffic.
- Two matching startup baselines and a fresh preflight protect entry. Preserve
  identity, firmware, layout, configuration and RAM consistency checks; never
  substitute a saved baseline from another controller or session.
- A silent entry/combined-exit/left-restore/right-restore sequence completed with
  matching postflight and operator-observed normal input and vibration.
- Individual restores can return the exact zero-count/value-1 envelope. Keep its
  classification `captured_zero_count_value_1_unverified`. Matching postflight
  and observed recovery do not establish the firmware meaning of that reply.
- An early pulse run changed RAM6 offsets 55–58 and normal vibration required a
  controller restart. Later runs matched postflight and recovered normally.
  Takeover-toggle comparisons had different initial states and a competing
  service present; they do not establish causation or writer coexistence.

## Timing, strength and feedback

- The first low-amplitude pulse stopped on accumulated dispatch lateness after
  four waveform packets, although each completed within the native deadline.
  Moving preparation before the final wait improved synthetic real-clock trials
  from 0/20 to 20/20 completed. These are host timings, not motor measurements.
- Integrated sessions record dispatch lateness, preserve minimum 8-ms spacing
  and avoid catch-up bursts. The native 4-ms waveform-write deadline remains a
  fail-stop condition. It is distinct from dispatch lateness.
- Native-game testing exposed late write completion. Scoped MMCSS scheduling on
  the vendor worker was followed by a successful approximately 13-minute run;
  this is a mitigation, not proof that deadline failures cannot recur.
- Weak but perceptible pulses established delivery. A fixed gain-4 comparison
  felt stronger and recovered normally. Gain is a software multiplier, not a
  calibrated motor-force percentage. Live gain is 0–12, defaults to 1, ramps over
  100 ms, and is applied once after resampling with a fixed 0.75 ceiling.
- Accepted HID motor commands remain held until changed. Silence, PCM underruns
  and stale PCM drops must not clear held HID state. Active PCM clears prior HID
  state and has priority; a fresh HID command is required afterward. Do not mix
  PCM with synthesized HID output.

## Application and recovery

- Apex6 requires isolation before game launch. An executable whitelist alone
  cannot make late process detection safe. Tray prepares the bridge before its
  Launch action; Playnite uses its pre-launch hook. Apex4/5 retain their existing
  background activation behavior.
- Protected game-process queries can fail while the game is still alive. Tray
  uses PID enumeration as a fallback and allows a bounded process-handoff grace
  period instead of immediately treating access denial as game exit.
- Games requiring elevation receive UAC after bridge readiness. Only error 740
  causes an elevation retry; cancellation rolls back the prepared session.
- Flydigi Space Station, its service and competing writers must be stopped.
  Starting the official application/service for a conflict check is not evidence
  that coexistence or startup refusal has been physically qualified.
- Disconnect and native I/O faults stop the session without automatic reconnect
  or speculative recovery writes. Reconnecting USB restored input in observed
  fault cases; restoring normal vibration sometimes required a full controller
  restart. Ordinary stop and fault recovery are separate qualification results.

## Public repository boundary

The later [2.4 GHz dongle investigation](APEX6_DONGLE_INVESTIGATION.md) records
matching framing/baselines, a validated UID query between baseline boundaries,
and a neutral experiment that stopped on a missing combined-exit reply. A passive
official dongle capture also contains fewer mode replies than requests; reply
attribution and the cause remain unresolved. A revised neutral diagnostic with
validated UID queries between shutdown modes completed with matching postflight;
its individual restore replies remained unverified. A fixed low-strength left
pulse and subsequent right/both pulses completed with matching postflight and
operator-confirmed correct-side output, complete stop and normal operation
without restart. The operator then reported successful Endfield gameplay, but
closed the console rather than using Q; no final report was produced, and normal
vibration required a controller restart. Tray/CLI now expose a separately selected,
bounded dongle beta. Game-session orderly shutdown remains unverified. Full wireless support
remains unqualified. Descriptor matching alone cannot enforce the
previously tested direct-USB scope.

Keep model VID/PID values, interface-role rules, standard API GUIDs and protocol
constants: they describe supported hardware, not an individual unit. Examples
and tests use placeholders or synthetic identities. RAM test fixtures are
synthetic, including their CRCs, rather than retail configuration dumps.

Do not commit local device paths, instance/container IDs, serials, unit IDs,
configuration snapshots, HidHide recovery state, approvals, raw USB/audio
captures, logs, per-user settings or local package hashes. Diagnostic artifacts
may contain these even when their filenames look harmless. Keep them in ignored
local output directories and review any evidence separately before sharing it.
