# contracts/

Boundary contracts (ARCHITECTURE.md §5). One header (or schema file) per
boundary, semver-versioned, each with contract tests in `tests/contract/`.
A boundary header is added in the milestone that first crosses that boundary;
every change to one reconciles ARCHITECTURE.md §5 in the same commit
(Iron Rule 7).

| Contract file | Boundary | Introduced |
|---|---|---|
| `world_view_v1.h` | core → renderers (read-only snapshot) | M2 |
| `replay_v1.h` | input + Event Log serialization | M2 |
| `chunk_gen_v1.h` | gen → stream/render | M3 |
| `stream_events_v1.h` | stream → renderers | M3 |
| `audio_events_v1.h` | core → audio | M6 |
| `telemetry_v1.h` | all → telemetry | M3 |
| `director_v1/` | core ⇄ director (JSON Schemas) | M11 |

M0: none yet (the sim core exposes only the RNG, which is internal, not a
cross-module boundary contract).
