# ARCHITECTURE.md — Backrooms Sim

> Canonical truth of the system. Every Claude Code session loads this file first.
> Companion documents: docs/MILESTONES.md (build sequence + gates), docs/DECISIONS.md (full ADRs), docs/SESSION_LOG.md (session continuity).

---

## 1. System Intent

A native Windows, GPU-accelerated **walking simulation of the Backrooms**: an unbounded, deterministic, procedurally generated liminal-space world that never ends and never repeats. The wanderer walks forever through fluorescent-lit yellow rooms and stranger biomes beneath them; there is no exit, no win state, no combat. A local LLM ("the Director") personalizes ambience, intercom voice lines, and found notes based on the wanderer's actual journey. The renderer showcases RTX hardware: raster mode by default, real-time path-traced mode where the fluorescent panels are the true light sources.

This is a **demonstration and visualization, not a game**. It is built end-to-end by Claude Code with zero human verification until final acceptance; therefore **every requirement in this document is machine-verifiable**, and the build is governed by the gates in MILESTONES.md.

Every future decision is checked against this paragraph. Anything that adds game mechanics, networked features, asset pipelines, or human-only verification steps is out of scope.

## 2. Ubiquitous Glossary

Terms below are canonical. Forbidden synonyms must not appear in code, docs, or commit messages.

| Term | Definition | Forbidden synonyms |
|---|---|---|
| **Chunk** | 32×32 m world unit; atomic unit of generation and streaming | tile, sector, block |
| **Cell** | 1×1 m subdivision inside a chunk used by the layout solver | tile, square |
| **ChunkKey** | (level, cx, cz) integer triple identifying a chunk | coord, id |
| **Level** | Vertical stratum with its own generator parameters (Level 0, Level −1, …) | floor, stage |
| **Biome** | Regional generator style chosen by low-frequency noise (ClassicYellow, Cubicles, Garage, Poolrooms, Pipes) | theme, zone, region |
| **Set piece** | Rare macro-structure injected by the generator (PillarHall, Stairwell, FloodedSection) | landmark, POI |
| **WorldSeed** | 64-bit master seed; sole source of all generated content | seed value, random seed |
| **Wanderer** | The first-person capsule entity (human- or bot-driven) | player, character, user |
| **Walk-bot** | Autonomous wanderer controller used by soak tests | bot, AI player |
| **Tick** | Fixed 120 Hz simulation step | frame, update |
| **Frame** | One render iteration; decoupled from ticks | tick |
| **Director** | LLM module that emits Directives from WandererSummaries | AI, narrator, DM |
| **Directive** | Schema-validated JSON command from the Director, with expiry tick | command, action |
| **The Voice** | Caption channel rendering Director intercom/radio lines | dialogue, NPC |
| **Wanderer Note** | Generated text bound to a location hash, cached after first generation | lore, note item |
| **Event Log** | Serialized stream of committed Directives + sim events; replays consume this, never the model | history, journal |
| **Replay** | Recorded input stream + Event Log that reproduces a run bit-identically | demo, recording |
| **Golden** | Stored reference artifact (image, state hash, WAV spectrum, converged render) | snapshot, baseline |
| **Gate** | Machine-checkable milestone exit criteria executed by `scripts/gate.ps1 M<N>` | check, test suite |

## 3. Domain Model & Invariants

### 3.1 Invariants (non-negotiable; every module respects these)

- **INV-1 Determinism.** World state hash after N ticks is a pure function of (WorldSeed, input stream, Event Log). No wall-clock reads in sim code, no unseeded RNG, no nondeterministic float paths (sim core compiles `/fp:strict`; sim tick runs single-threaded; RNG is seeded PCG64 owned by core).
- **INV-2 Generation purity.** `GenerateChunk(WorldSeed, ChunkKey)` is a pure, total function. No neighbor queries; cross-seam agreement is achieved only through shared edge hashes derived from (WorldSeed, edge id).
- **INV-3 Connectivity.** Every chunk is internally traversable; every open edge connects to its neighbor's open edge; no sealed regions ever. The wanderer can always keep walking.
- **INV-4 Boundedness.** Memory is bounded by the streaming ring; chunk generation respects its worker-time budget; nothing grows with distance traveled.
- **INV-5 Isolation of the sim core.** `/core` has zero includes from render, audio, director, or OS UI. Director output enters the sim **only** as validated Event Log entries; raw model text never crosses the boundary.
- **INV-6 Fallbacks.** The sim runs fully with `--no-director` and with the raster renderer alone. DXR and the Director are enhancements, never dependencies.
- **INV-7 Headless parity.** Every observable behavior has a headless verification path (offscreen render, replay hash, telemetry, offline WAV). If it can't be verified headlessly, it doesn't ship.
- **INV-8 Golden integrity.** Goldens change only via the `goldgen` tool accompanied by a DECISIONS.md entry in the same commit. Editing goldens to make a failing gate pass is forbidden.

### 3.2 Core entities

- **WorldState** — wanderer kinematics, current ChunkKey, tick counter, RNG state, active directive set, odometer. Hashable per tick.
- **ChunkData** — cell grid (walkable/solid), wall/floor/ceiling geometry description, light placements, biome id, set-piece annotations, content hash.
- **WandererSummary** — rolling digest for the Director: time walked, distance, route-loop count, dwell points, biome history, recent events.

### 3.3 State machines

```
App:       Boot → StreamWarmup → Wandering → Shutdown
Chunk:     Unloaded → Generating → Ready → ResidentGPU → Evicted   (Evicted → Generating on revisit; regeneration is bit-identical per INV-2)
Directive: Idle → Summarizing → Generating → Validating → { Committed → EventLog | Rejected → log+drop }
Renderer:  Raster ⇄ PathTraced   (mode switch never touches sim state)
```

## 4. Module Inventory

Layers per methodology: L1 core domain, L2 infrastructure, L3 integration, L4 intelligence. CI enforces that this table matches the directory listing.

| Module | Layer | Purpose | Depends on |
|---|---|---|---|
| `core` | L1 | Tick loop, RNG, wanderer kinematics, capsule collision, WorldState + hashing, Event Log consumption | — |
| `gen` | L1 | Chunk layout solver, biome field, set pieces, connectivity + geometry validators | core (types) |
| `stream` | L2 | Chunk ring management, worker pool, residency events | core, gen |
| `render_d3d12` | L2 | Raster renderer, procedural textures, fluorescent lighting, VHS post stack, HUD | core (read-only view), stream (events) |
| `render_dxr` | L2 | BLAS/TLAS management, path-traced mode, temporal accumulation | render_d3d12 (device/share), stream |
| `audio` | L2 | Procedural hum/drone synthesis, footsteps, raycast room-probe reverb, offline WAV render | core (events) |
| `telemetry` | L2 | Counters, timers, CSV/JSON output, minidump capture | — (interface in contracts) |
| `director` | L4 | llama.cpp host, WandererSummary→Directive pipeline, schema validation, note cache, Event Log emission | core (contract only), telemetry |
| `app` | L2 | Win32 shell, config, composition root, CLI flags (`--headless`, `--replay`, `--no-director`, `--render-wav`) | all |
| `tools` | dev | `hashdiff`, `goldgen`, `walkbot`, `contactsheet`, `wavcheck` | varies |

Dependency rule: arrows point downward only; `core` depends on nothing; nothing depends on `app`.

## 5. Boundary Contracts

One header (or schema file) per boundary in `/contracts`, semver-versioned, each with contract tests in `/tests/contract`.

| Contract file | Boundary | Version |
|---|---|---|
| `contracts/chunk_gen_v1.h` | gen → stream/render | 1.0 |
| `contracts/stream_events_v1.h` | stream → renderers | 1.0 |
| `contracts/world_view_v1.h` | core → renderers (read-only snapshot) | 1.0 |
| `contracts/audio_events_v1.h` | core → audio | 1.0 |
| `contracts/replay_v1.h` | input + Event Log serialization | 1.0 |
| `contracts/telemetry_v1.h` | all → telemetry | 1.0 |
| `contracts/director_v1/` | core ⇄ director (JSON Schemas: `wanderer_summary.schema.json`, `directive.schema.json`) | 1.0 |

### 5.1 Key signatures

```cpp
// contracts/chunk_gen_v1.h — pure, total, deterministic (INV-2)
struct ChunkKey  { int32_t level; int64_t cx, cz; };
ChunkData GenerateChunk(uint64_t worldSeed, ChunkKey key);
uint64_t  ChunkContentHash(const ChunkData&);   // used by determinism gates

// contracts/world_view_v1.h — renderer-facing snapshot, immutable per frame
struct WorldView { CameraPose cam; std::span<const ResidentChunk> chunks;
                   std::span<const LightInstance> lights; uint64_t tick; };
```

### 5.2 Director JSON contract (sketch; full schemas in contracts/director_v1/)

```json
// WandererSummary v1 (sim → director)
{ "v":1, "tick":123456, "minutes_walked":42.5, "odometer_m":3120,
  "route_loops":3, "dwell_points":[{"chunk":[0,12,-4],"seconds":95}],
  "biome_history":["ClassicYellow","Garage"], "recent_events":["LevelTransition"] }

// Directive v1 (director → sim; expiry mandatory; unions are closed)
{ "v":1, "id":"d-0042", "expires_tick":131456, "type":"FlickerBurst",
  "params":{"chunk":[0,12,-4],"intensity":0.7} }
// Allowed types: FlickerBurst | DistantSoundCue | BiomeBias | IntercomLine | PlaceNote
```

Validation: directives failing schema or sanity lint (unknown type, expired on arrival, out-of-range params, contradiction with active set) are **rejected and logged**, never partially applied.

## 6. Event Catalog

| Event | Producer | Consumers | Schema |
|---|---|---|---|
| ChunkGenerated / ChunkEvicted | stream | render_*, telemetry | stream_events_v1 |
| FootstepTick | core | audio | audio_events_v1 |
| BiomeEntered / SetPieceEntered / LevelTransition | core | audio, director, telemetry | replay_v1 |
| DirectiveCommitted | director | core (Event Log), telemetry | director_v1 |
| LightFlickerBurst / DistantSoundCue | core (from directives) | render_*, audio | replay_v1 |
| NoteDiscovered | core | render_* (Voice/caption), telemetry | replay_v1 |

## 7. Error Taxonomy

| Code | Meaning | Class | Retryable | Response |
|---|---|---|---|---|
| E_GPU_DEVICE_LOST | D3D12 device removed | Fatal | no | minidump → clean exit (soak harness restarts + logs) |
| E_GPU_OOM | Allocation failure | Fatal | no | minidump → clean exit |
| E_GEN_BUDGET | Chunk gen exceeded worker budget | Recoverable | yes | re-queue once; telemetry counter; gate watches p95 |
| E_GEN_INVALID | Validator failed on generated chunk | Fatal-in-dev | no | abort with repro seed+key printed (generator bug, must fix) |
| E_STREAM_STALL | Ring starved (wanderer outran generation) | Recoverable | yes | hold wanderer at boundary; telemetry; gate watches count |
| E_AUDIO_UNDERRUN | Mix buffer starved | Degraded | n/a | log + counter; gate asserts zero in soak |
| E_DIR_SCHEMA | Directive failed validation | Recoverable | no | reject + log sample |
| E_DIR_TIMEOUT | Model exceeded latency budget | Recoverable | skip | drop request; counter |
| E_DIR_UNAVAILABLE | Model failed to load / VRAM pressure | Degraded | no | disable Director for session (INV-6) |
| E_REPLAY_VERSION | Replay/Event Log version mismatch | Fatal | no | exit with message |

Convention: errors cross module boundaries as typed results, never exceptions; `app` maps them to exit codes for the gate scripts.

## 8. Technology Decisions (ADR summaries — full text in DECISIONS.md)

| ADR | Decision | Rationale |
|---|---|---|
| 001 | **From-scratch C++20, not UE5** | Build is agent-driven: 100% text artifacts, seconds-long compiles, no binary .uassets, total determinism control |
| 002 | **D3D12 + DXR, not Vulkan** | Windows-only target; DXR maturity; debug layer + DRED output is itself a machine gate |
| 003 | **Fixed 120 Hz tick; `/fp:strict` sim core; single-threaded tick; PCG64 RNG** | INV-1 determinism is the test oracle for the whole build |
| 004 | **Procedural-only assets** | Textures/geometry generated in code; no asset pipeline; entire look is diffable and seedable |
| 005 | **llama.cpp, local small instruct model, CUDA offload** | Native C/C++, runs beside renderer; Event-Log isolation preserves INV-1; kill switch per INV-6 |
| 006 | **miniaudio** | Single-file C, easy offline WAV render path for audio gates |
| 007 | **CMake + Ninja + vcpkg (manifest), MSVC, warnings-as-errors** | Reproducible one-command builds from fresh clone |
| 008 | **Catch2 under CTest** | Single `ctest` entry point for gate scripts |
| 009 | **Goldens versioned in repo; goldgen-only updates** | INV-8; protects gates from being gamed |
| 010 | **vcpkg pinned baseline + self-bootstrapping location** | Reproducible deps; fresh clone self-bootstraps; vcpkg never vendored |
| 011 | **x64-windows-static triplet + static CRT** | Self-contained exes the gate scripts run directly; no DLL deployment |
| 012 | **"test-the-gate" canary (DISABLED CTest test, run directly)** | Proves failure detection without making `ctest` red |
| 013 | **Warnings-as-errors scoped to our code via `/external` flags** | `/WX` protects our code without third-party header noise |
| 014 | **M1 memory soak measures process private bytes** | Release `/MT` CRT has no debug heap; private-bytes delta preserves the "flat memory" intent |
| 015 | **M1 frame-0 golden (320×180 clear)** | First committed headless golden; bit-identical across runs (INV-8) |
| 016 | **M2 collision: AABB-proxy capsule, per-axis swept + substepped** | Robust no-penetration/sliding/no-tunneling on axis-aligned geometry |
| 017 | **M2 contracts world_view_v1 + replay_v1** | First boundary headers crossed; shared `contracts` INTERFACE target |
| 018 | **M2 test-room golden (640×360, lit, depth)** | First geometry golden; deterministic across runs (INV-8) |
| 019 | **M3 streaming: pure world-coord chunks + ring + worker pool** | Unbounded, seam-correct, bounded residency (INV-2/3/4); sim decoupled |
| 020 | **M3 chunk VB pool + warmup** | Allocation-free stream-in; p99/median ≈ 1.2x; no first-frame hitch |
| 021 | **M3 gate: p99 < 2× median @1280×720; private-bytes soak** | Aligns with NFR §9; non-flaky; soak duration parameterized |

## 9. Non-Functional Requirements

These numbers are the source of truth referenced by MILESTONES.md gates. Dev GPU = the RTX machine at C:\backrooms.

| NFR | Target |
|---|---|
| Raster frame rate | ≥ 120 FPS at 1440p |
| Path-traced mode | ≥ 60 FPS while moving; full accumulation when stationary |
| Frame-time stability | p99 frame < 2× median during active streaming |
| Chunk generation | p95 ≤ 4 ms worker time; zero E_STREAM_STALL in nominal walk speed |
| Memory | Plateau after ring fill; growth slope ≈ 0 over 8 h soak |
| Cold start | ≤ 5 s to Wandering state |
| Stability | 12 h unattended soak, zero crashes, Director ON |
| Director latency | p95 ≤ 5 s per directive; zero frame-time impact (async) |
| GPU validation | Zero D3D12 debug-layer errors **or warnings** in any gate run |
| Determinism | Bit-exact WorldState hash and chunk hashes across runs/replays |

## 10. Operational Requirements

- **Telemetry:** every run writes CSV/JSON (frame times, tick times, memory, chunk-gen times, error counters) to `/runs/<timestamp>/`; gate scripts parse these.
- **Crash handling:** unhandled fault → minidump in `/runs/.../crash/`; soak harness auto-restarts and logs.
- **Self-backup:** on every green gate, the agent tags (`m<N>-green`) and **pushes tags + branch to the remote**. Off-machine git remote is the backup; restore = clone + checkout tag.
- **Recovery rule:** on regression, revert to last green tag (Iron Rule 2 in CLAUDE.md) rather than debugging forward.
- **Goldens:** stored under `/goldens`, updated only by `goldgen` + DECISIONS.md entry (INV-8).
- **Config:** single `config.toml`; all CLI flags mirror config keys.

## 11. Open Questions (decide at the noted milestone, record as ADRs)

1. **PT denoising while moving** (M9): temporal accumulation only vs. SVGF-lite. Default plan: accumulation + reduced bounce count in motion.
2. **Director model selection** (M11): pick by VRAM headroom measured alongside PT mode on the dev GPU; 7–8B-class quantized is the working assumption.
3. **TTS for The Voice** (post-v1): captions only in v1.0.
4. **Poolrooms water rendering** (M7): screen-space approximation vs. deferring true water to post-v1.
