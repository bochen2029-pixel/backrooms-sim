# Backrooms Sim — Progress Report

**As of 2026-06-13** · Built autonomously by Claude Code, milestone by milestone, gate-driven.
Backup remote: **https://github.com/bochen2029-pixel/backrooms-sim** (private).

## Status at a glance

| Milestone | What it delivers | Gate | State |
|---|---|---|---|
| **M0** | Scaffold + verification harness | `gate.ps1 -Milestone M0` | ✅ `m0-green` |
| **M1** | Win32 window, D3D12 device, headless PNG | M1 | ✅ `m1-green` |
| **M2** | Sim core: tick, camera, collision, replay | M2 | ✅ `m2-green` |
| **M3** | Infinite chunk streaming + telemetry | M3 | ✅ `m3-green` |
| **M4** | Level-0 generator: maze, doorways, walk-bot | M4 | ✅ `m4-green` |
| **M5** | Procedural materials + raster lighting | M5 | 🟡 in progress (textures done) |
| M6–M12 | audio · biomes · VHS post · DXR · soak · Director · acceptance | — | ⬜ pending |

**5 milestones green and pushed.** Each is verified by a machine-checkable gate
(`scripts/gate.ps1 -Milestone M<N>` exits 0) and tagged; the remote is the backup.

All numbers below are from real gate runs on the dev machine (RTX 4070 Ti SUPER,
VS 2022 17.14, Windows 11, vcpkg static triplet).

---

## What each milestone delivered

### M0 — Scaffold + Verification Harness
The harness comes before the product. CMake/Ninja/vcpkg skeleton across all 10
inventory modules with correct dependency arrows; `/W4 /WX /permissive-`,
`/fp:strict` on the sim core. Real **PCG64** RNG in `core` (the determinism
oracle). `hashdiff` + `goldgen` tools. Catch2 under CTest. `build`/`quickcheck`/
`precommit`/`gate` scripts, plus a self-test that proves a deliberately failing
test blocks a commit. INV-5 core-isolation and module-inventory grep gates.

### M1 — Window, D3D12 Device, Headless Mode
`render_d3d12::Renderer` (opaque PIMPL — no D3D12/Win32 leaks): DXGI adapter
selection (picks the RTX, WARP fallback), device, **debug layer + InfoQueue +
DRED**, fence-synced clear frame. Headless offscreen RT with 256-byte
row-pitch-correct readback → PNG; windowed flip-discard swapchain. **Gate:**
frame-0 PNG bit-identical ×3 + golden, **zero debug-layer messages**, 60 s
memory soak (372,750 frames, +1.6 MB → flat).

### M2 — Sim Core: Camera, Input, Collision, Replay
`core`: fixed **120 Hz tick**, first-person walk camera, **capsule-vs-AABB
collision** (per-axis swept + substepped: no penetration at any speed, sliding
preserves tangential velocity, no floor tunneling), gravity/jump, hardcoded test
room, per-tick **WorldState hash**, **input replay** record/playback. Renderer
gained real geometry (lit, depth-tested room → golden). **Headline proof:** seed
777 / 3000 ticks → hash `0e6105f7c33e525b`, **bit-identical across record + 2
replays in 3 separate processes** (INV-1 across process boundaries).

### M3 — Infinite Chunk Streaming
`gen::GenerateChunk` pure/deterministic (seam-correct); `stream::StreamManager`
— a `(2r+1)²` ring with a **background worker pool**, bounded residency,
fully decoupled from the sim. Renderer draws resident chunks via a
**persistently-mapped vertex-buffer pool** (allocation-free stream-in). Telemetry
frame CSV. **Gate:** 1000-chunk regen bit-identical, seam match, **p99 frame <
2× median @1440p** (best-of-2 + `timeBeginPeriod`), **600 s memory soak**
(344,855 frames, +0.86 MB → flat).

### M4 — Level-0 Generator: Rooms, Doorways, Connectivity
Each chunk is a **spanning-tree maze** (provably connected, zero sealed boxes) +
extra carves + 4 **edge-hash doorways** that neighbours agree on without
communicating. `validate_connectivity` + `ValidateChunkGeometry`. The wanderer
collides with generated walls (gathered by the app, keeping `core` gen-free).
**Walk-bot v1** traverses **1 km × 5 seeds with zero stuck events,
deterministically**. Top-down ortho debug golden. **Gate:** 10,000-chunk
connectivity + geometry, walk-bot, top-down goldens (seeds 1 & 7).

### M5 — Procedural Materials + Raster Lighting (🟡 in progress)
**Done:** procedural textures — wallpaper, carpet, ceiling tile, fluorescent
panel, baseboard (`render_d3d12/texgen.*`, D3D12-free, deterministic per
(kind, seed); determinism unit test green).
**Remaining:** GPU texture-array upload + SRV/sampler, chunk UV/material + a
ceiling with fluorescent panels, the textured + **forward fluorescent lighting**
shader with **seeded deterministic flicker** (computed in `core`, replayable),
the 5-pose × 3-seed goldens, the luminance-histogram gate, and the **≥120 FPS @
1440p** perf gate. (The top-down debug render must skip the new ceiling so the
maze stays visible; the M4 top-down golden will be re-captured via `goldgen` +
an ADR.)

---

## Architecture & invariants (the rules that keep it coherent)

- **Modules** (`docs/ARCHITECTURE.md` §4): `core` (depends on nothing) · `gen` ·
  `stream` · `render_d3d12` · `render_dxr` · `audio` · `telemetry` · `director` ·
  `app` (composition root) · `tools`. Dependency arrows point downward only; CI
  checks the inventory matches the directory listing.
- **Contracts** (`contracts/*.h`, header-only `contracts` target): `geometry_v1`,
  `world_view_v1`, `replay_v1`, `chunk_gen_v1`, `stream_events_v1`, `telemetry_v1`
  are live; `audio_events_v1` (M6) and `director_v1` (M11) pending.
- **Key invariants** held and gated: INV-1 determinism (per-binary, bit-exact
  across runs/replays/processes), INV-2 generation purity, INV-3 connectivity
  (zero sealed boxes), INV-4 bounded memory (streaming ring), INV-5 core
  isolation (grep-gated), INV-7 headless verification, INV-8 golden integrity
  (only `goldgen` writes `/goldens`, always with a DECISIONS.md entry).
- **Decisions:** ADR-001..024 in `docs/DECISIONS.md` (each summarised in
  ARCHITECTURE.md §8). Notable: vcpkg static triplet, `/fp:strict` core, the
  "test-the-gate" canary, the M3 hitch metric (p99 @1440p, NFR §9), the M4 maze +
  collision design, far-chunk float-precision deferral (camera-relative
  rendering still to come).

## Verification harness

Every feature ships with a headless path before any visual polish. The gate
runner (`scripts/gate.ps1`) is law — a milestone is done only when its gate
exits 0. A git **pre-commit hook** runs build + tests, and a PostToolUse hook
rebuilds after every edit. Goldens are bit-exact per-GPU; the perf/jitter gates
use p99 at the target resolution to stay non-flaky.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1            # build
ctest --test-dir build --output-on-failure                                       # tests
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gate.ps1 -Milestone M4   # a gate
# regression sweep = run gate.ps1 for M0..M4 (M3 accepts -StreamSoakSeconds N to shorten the soak)
```

App modes built so far: `--headless` · `--window` · `--scene` · `--sim` ·
`--stream` · `--walkbot` · `--topdown` (see `app/MODULE.md`).

## Two real bugs caught (and fixed properly, not tuned around)

1. **Far-chunk float precision** tripped the M4 geometry validator (a 0.3 m wall
   measured 0.312 m at 320 km from origin). Fixed with validator thresholds
   sitting between the 0.3 m wall and 4 m cell; far-chunk camera-relative
   rendering remains a documented deferral.
2. **Em-dash in a Catch2 test name** made `catch_discover_tests` register a name
   that ctest's re-invocation couldn't match (Unicode arg round-trip), so a
   correct connectivity test "failed" only under ctest. Keep test names ASCII.

## What's next

- **Finish M5** (the larger half): GPU textures + forward fluorescent lighting +
  flicker + goldens + luminance + 120 FPS gate. See the M5 section in
  `docs/SESSION_LOG.md` for the precise remaining steps.
- **M6** procedural audio (offline WAV gate) · **M7** biomes/set pieces/verticality
  · **M8** VHS post + HUD · **M9** DXR path-traced mode · **M10** 8 h walk-bot
  soak · **M11** the Director (local LLM) · **M12** integration + 12 h acceptance.

## How to continue (next session)

1. Read `docs/ARCHITECTURE.md`, the latest `docs/SESSION_LOG.md` entry, and the
   M5 section of `docs/MILESTONES.md`.
2. Continue M5 from the textures already in `render_d3d12/texgen.*` (green).
3. Run `gate.ps1 -Milestone M5` until exit 0, regression-sweep M0–M4, tag
   `m5-green`, push, write the SESSION_LOG entry.

_The repo is the memory. Last fully-green tag: `m4-green`. Never resume from a
broken state — revert to the last green tag if needed._
