# contracts/

Boundary contracts (ARCHITECTURE.md §5). One header (or schema file) per
boundary, semver-versioned, each with contract tests in `tests/contract/`.
A boundary header is added in the milestone that first crosses that boundary;
every change to one reconciles ARCHITECTURE.md §5 in the same commit
(Iron Rule 7).

| Contract file | Boundary | Status |
|---|---|---|
| `geometry_v1.h` | shared primitive `BoxInstance` | **present (M4)** |
| `world_view_v1.h` | core → renderers (read-only snapshot) | **present (M2)** |
| `replay_v1.h` | input + Event Log serialization | **present (M2)** |
| `chunk_gen_v1.h` | gen → stream/render | **present (M3; +uv/material + fluorescent grid M5)** |
| `stream_events_v1.h` | stream → renderers | **present (M3)** |
| `audio_events_v1.h` | core → audio | **present (M6)** |
| `telemetry_v1.h` | all → telemetry | **present (M3)** |
| `director_v1.h` | sim ⇄ director (WandererSummary + Directive + DirectorEvent) | **present (M11)** |

Headers are consumed via the `contracts` INTERFACE target as
`#include "contracts/<name>.h"`. `chunk_gen_v1` grew additively in M5 — each
`ChunkVertex` carries `uv` + `material` (kMat* ids), and the fluorescent ceiling
grid is shared via `is_fluorescent_cell` / `fluorescent_light_pos` so `gen`'s
tiles and the renderer's forward lights agree without a separate lights channel.
`replay_v1` gained the **Director Event Log** in M11 (additive within v1): a
`DirectorLogHeader` + `DirectorEvent` records recorded live and consumed on replay
with the model offline — the channel through which a stochastic LLM stays INV-1
bit-exact (proven: record vs replay combined-hash identical).
