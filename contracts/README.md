# contracts/

Boundary contracts (ARCHITECTURE.md §5). One header (or schema file) per
boundary, semver-versioned, each with contract tests in `tests/contract/`.
A boundary header is added in the milestone that first crosses that boundary;
every change to one reconciles ARCHITECTURE.md §5 in the same commit
(Iron Rule 7).

| Contract file | Boundary | Status |
|---|---|---|
| `world_view_v1.h` | core → renderers (read-only snapshot) | **present (M2)** |
| `replay_v1.h` | input + Event Log serialization | **present (M2)** |
| `chunk_gen_v1.h` | gen → stream/render | **present (M3)** |
| `stream_events_v1.h` | stream → renderers | **present (M3)** |
| `audio_events_v1.h` | core → audio | M6 |
| `telemetry_v1.h` | all → telemetry | **present (M3)** |
| `director_v1/` | core ⇄ director (JSON Schemas) | M11 |

Headers are consumed via the `contracts` INTERFACE target as
`#include "contracts/<name>.h"`. `world_view_v1` grows additively (resident
chunks + lights) in M3/M5; `replay_v1` gains the Event Log layer later.
