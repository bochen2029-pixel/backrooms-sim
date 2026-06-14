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
| **M5** | Procedural materials + raster fluorescent lighting | M5 | ✅ `m5-green` |
| **M6** | Procedural audio: synth, room-probe reverb, offline WAV | M6 | ✅ `m6-green` |
| **M7** | Biomes, set-piece pillars, verticality (level −1 stairwell) | M7 | ✅ `m7-green` |
| **M8** | VHS post-processing stack + HUD/timestamp | M8 | ✅ `m8-green` |
| **M9** | DXR path-traced mode (BLAS/TLAS, inline-RayQuery PT, accumulation) | M9 | ✅ `m9-green` |
| **M10** | Walk-bot soak + hardening (telemetry, contactsheet, minidump) | M10 | ✅ `m10-green` |
| **M11** | The Director + The Voice (local LLM via the KEEL sidecar) | M11 | ✅ `m11-green` |
| M12 | Integration · polish · 12 h acceptance | — | ⬜ pending |

**12 milestones green and pushed.** Each is verified by a machine-checkable gate
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

### M5 — Procedural Materials + Raster Fluorescent Lighting
It now *looks* like the backrooms. All materials are procedural (no asset files):
**yellow wallpaper, damp carpet, dark ceiling tiles, glowing fluorescent panels,
scuffed baseboards** (`render_d3d12/texgen.*`, D3D12-free, deterministic per
(kind, seed)). Each `ChunkVertex` carries uv + material; `GenerateChunk` paints
floor/walls and emits a **ceiling** with the fluorescent-tile grid (shared
verbatim with the renderer's lights; render-only, not collidable). The lit
pipeline uploads a **Texture2DArray** (fenced) and samples by (material, uv) with
a per-chunk hue tint. **Forward fluorescent lighting:** the renderer gathers the
ceiling-grid lights near the camera, each scaled by **`core::light_flicker(seed,
id, tick)`** — deterministic flicker computed in `core` so lit replays reproduce
exactly — over flat ambient, with a highlight knee that keeps the wallpaper's hue
instead of clipping to white. `render_topdown` discards the ceiling so the M4
maze goldens stay **bit-exact** (no re-capture). **Gate:** texture determinism,
**5 poses × 3 seeds lit goldens** (`goldens/m5/`, `goldgen`-captured) bit-identical
×2 + golden-matched + **luminance-histogram band** (no all-black/blown-out) +
debug-clean, **≥120 FPS @1440p** (best-of-2, measured **~179 FPS**), and a
regression pass over the M1/M2/M4 render goldens.

### M6 — Procedural Audio
Now it *sounds* like the backrooms: a 60 Hz fluorescent mains hum + harmonics, an
HVAC drone bed, footsteps timed to the walk, and reverb that opens up in larger
rooms — all procedural (no asset files), all deterministic, all verified headlessly
(no speakers). `core` derives footfalls as a pure floor of the odometer
(`footstep_count`) and the `audio_listener`, returning only contract types so it
stays audio-free (INV-5) and footsteps reproduce from a replay with no
replay-format change. The `audio` module is a deterministic block `Synth` (hum,
HVAC, footstep transients, Freeverb reverb sized by a 16-ray **room probe**),
header-only PCM16 WAV I/O, and a headless real-time **`AudioEngine`** (prebuffered
mixer thread fed lock-free from the sim). `app --render-wav` writes a bit-identical
WAV at exactly 400 audio frames/tick; `tools/wavcheck` runs a self-contained FFT.
**No new dependency** — miniaudio is deferred to real-time speaker playback
(headless-first). **Gate:** WAV deterministic ×2 + `wavcheck` (60 Hz fundamental +
harmonics over the noise floor, RMS silence check); footstep log **1:1** with the
replay; **soak with zero underruns** + audio-on tick time within 1.5× of off (the
audio thread never blocks the sim). Measured: 60 Hz at ~12000× the noise floor,
64/64 footsteps aligned, 0 underruns over 60 s (and a 10-min soak), tick-time delta
~0.4%.

### M7 — Biomes, Set Pieces, Verticality
The world gains character. A pure, **low-frequency biome field** (`gen/biome.*`,
coarse K=3 lattice + weighted CDF) paints five biomes in contiguous regions —
**classic yellow, cubicle farm, pipe corridors, parking garage, poolrooms** —
each with its own openness (carve ratio), tint, and pillar density, while every
biome reuses the **same edge-doorway protocol** so cross-biome seams still connect
(INV-3). Parking garages and pillar halls get free-standing **collidable columns**
(the geometry validator learned to tell a pillar from a wall). A **stairwell set
piece** descends to a dimmer **level −1** (levels stack in world Y; level 0 is
unchanged), and `app --descend` walks the wanderer down it under gravity —
deterministically. **Gate (all 4 exit criteria):** every biome passes the M4
validators (10k connectivity + geometry each), the realized **distribution over
102,400 chunks is within ±2 %** of the designed weights, the **stairwell descent**
reaches level −1 with cross-level connectivity + a reproducible hash, and each
biome has a **fixed-pose lit golden**. Two golden re-captures (tint, then pillars)
via `goldgen` + ADRs 031–033.

### M8 — VHS Post-Processing + HUD
The picture gets its analog-horror finish. A single **fullscreen post pass**
(`render_d3d12`) binds the scene as a texture and composites into a second target:
**barrel distortion**, **chromatic aberration**, **scanline/interlace flicker**,
**vignette**, and **seeded film grain** (`hash(px, py, seed + ⌊time·60⌋)`, so a
fixed time means fixed grain — goldens don't flake). A **HUD** is rasterised on
the CPU (a 5×7 bitmap font, `app/hud.*`) — `TIME HH:MM:SS`, seed, odometer, chunk,
level, FPS in CRT-phosphor green — and composited undistorted so it stays crisp.
Post is **off by default**, so every earlier golden is byte-unchanged. **Gate (all
4 exit criteria):** post ON/OFF A/B goldens bit-identical ×2 + golden-matched +
debug-clean; the **timestamp** renders the right sim time (verified via telemetry
*and* a pixel golden — OCR-free: 305,160 ticks → `00:42:23`); the post pass costs
**~0.65 ms at 1440p** (budget < 1.5 ms); and the seeded grain keeps the goldens
deterministic. ADR-034.

### M9 — DXR Path-Traced Mode
The maze can now be **path-traced**. `render_dxr` is self-contained (its own
`ID3D12Device5`) and consumes the same `ResidentChunk` geometry as raster, which
stays the default + fallback (INV-6). A runtime **DXC** wrapper compiles HLSL to
signed DXIL (SM 6.3 for the recursive primary-ray pass, **SM 6.5** for the inline
`RayQuery` path tracer); it's a system dependency, no vcpkg change (ADR-035).
`build_scene` builds a **BLAS per resident chunk + a TLAS**, concatenates every
chunk's vertices into one `StructuredBuffer`, and tags each instance's start vertex
offset in `InstanceID` so the shader reads per-hit normal/material via `(InstanceID
+ 3·PrimitiveIndex)`. The **path tracer** (inline `RayQuery`) treats the emissive
fluorescent ceiling grid as area lights (analytic **NEE + shadow rays** from the
`is_fluorescent_cell` formula), adds one cosine **diffuse-GI bounce** and a small
ambient floor, with **seeded per-(pixel,sample) RNG** and an **RGBA32F accumulation
buffer** across batched dispatches (resets on camera movement for interactive use).
**Gate (all 4 exit criteria):** (#1) cross-renderer **depth compare** — raster vs
DXR primary-hit NDC depth within epsilon over 5 poses (mean rel-err ~1e-5, proving
the AS holds exactly the streamed geometry); (#2) **converged golden** — 1024 spp at
3 poses, deterministic, mean-abs-diff < threshold (`goldens/m9/`); (#3) **interactive
PT** — **178.5 FPS @ 1440p** (1 spp/frame, ≥60 bar) + a **no-ghost** accumulation
reset (clean-vs-fresh 0, un-reset ghost 31); (#4) **TLAS rebuild under streaming** —
walk-bot **1 km in PT mode**, 13 rebuilds, debug/DRED-clean. ADR-036.

### M10 — Walk-Bot Soak + Long-Haul Hardening
Proof the world survives a long, unattended run. `app --soak` drives the
deterministic maze walk-bot over the **streaming raster renderer** (the shipping
path), writing the frame telemetry CSV, running **periodic connectivity audits**
(`gen::validate_connectivity` over the wanderer's region — a regression guard mid-
run), and dumping periodic **screenshots**. `scripts/soak.ps1` is the real harness:
it runs the soak with **auto-restart-and-log** on a captured crash, analyzes the
CSV (**FPS percentiles**, **steady-state memory spread** skipping the warm-up so
ring-fill isn't mistaken for a leak), and tiles the screenshots via the new
**`contactsheet`** tool (mechanical all-black/all-white screen). Crash forensics
live in `telemetry` (`crash.cpp` → `dbghelp` **`MiniDumpWriteDump`**): `app`
installs an unhandled-exception filter at startup that writes a minidump + marker
and exits with a known code, so a fault during the 8 h run leaves a post-mortem and
the harness restarts. The soak is **duration-parameterized** — `soak.ps1 -Hours 8`
is the full acceptance run, the gate runs `-Seconds 30` (like the M3 stream soak).
**Gate (all 3 exit criteria):** (#1) short soak — **1 %-low FPS ≥ 30** (measured
~125), **steady-state memory spread ≤ 48 MB** (measured ~1.7, flat), zero
audit-failures / stuck / debug; (#2) **contact sheet** mechanical screen (no
all-black/white) + agent visual review; (#3) **forced-crash drill** — minidump
captured, exit 70, clean auto-restart. ADR-037.

### M11 — The Director + The Voice (local LLM)
The world gets a mind — without surrendering determinism. An ambient game-master
reads a deterministic **WandererSummary** and emits a schema-validated **Directive**
(flicker a light sector, a distant sound, a biome lean, an intercom line, a wanderer
note). **The model runs in a separate process:** the Director routes to a local
**KEEL sidecar** (OpenAI-compatible HTTP, Qwen-9B, $0/local/sovereign) instead of an
embedded llama.cpp — a dependency *removal* (Catch2 + stb stay the only third-party
libs; WinHTTP is a system lib), and Backrooms is KEEL's first proof-of-cell (ADR-038).
Raw model text never crosses into the sim: a dependency-free JSON reader + a schema
validator reject anything off-schema; valid directives enter only as recorded
Event-Log entries (`replay_v1`) at a deterministic `effective_tick`, and they're
**presentation-layer only** so they never perturb the WorldState hash. The Voice
renders captions; notes cache per location hash; `--no-director` is the kill switch.
**Gate (all 5 exit criteria):** (#4, the sacred one) **replay bit-identical with the
model fully offline** — record a run with the Director live, replay it with KEEL
unreachable → identical combined hash (proven from every angle: WorldState folds in,
the director stream folds in, replay contacts KEEL zero times, unreachable → graceful
no-op); (#1) **100% schema-valid** directives + lint over 100 scenarios; (#3) **p95
latency ~0.5 s** (< 5 s); (#2) **async isolation** — the Director thread never stalls
the sim (integration overhead ~0; no new hitches; ambient-pacing frame-time within a
stated band of OFF — the live-inference baseline shift is shared-GPU contention, not a
stall, honestly reformulated in ADR-039); (#5) **`--no-director`** runs a clean soak.
ADR-038, ADR-039.

---

## Architecture & invariants (the rules that keep it coherent)

- **Modules** (`docs/ARCHITECTURE.md` §4): `core` (depends on nothing) · `gen` ·
  `stream` · `render_d3d12` · `render_dxr` · `audio` · `telemetry` · `director` ·
  `app` (composition root) · `tools`. Dependency arrows point downward only; CI
  checks the inventory matches the directory listing.
- **Contracts** (`contracts/*.h`, header-only `contracts` target): `geometry_v1`,
  `world_view_v1`, `replay_v1`, `chunk_gen_v1`, `stream_events_v1`, `telemetry_v1`,
  `audio_events_v1` are live; `director_v1` (M11) pending.
- **Key invariants** held and gated: INV-1 determinism (per-binary, bit-exact
  across runs/replays/processes), INV-2 generation purity, INV-3 connectivity
  (zero sealed boxes), INV-4 bounded memory (streaming ring), INV-5 core
  isolation (grep-gated), INV-7 headless verification, INV-8 golden integrity
  (only `goldgen` writes `/goldens`, always with a DECISIONS.md entry).
- **Decisions:** ADR-001..034 in `docs/DECISIONS.md` (each summarised in
  ARCHITECTURE.md §8). Notable: vcpkg static triplet, `/fp:strict` core, the
  "test-the-gate" canary, the M3 hitch metric (p99 @1440p, NFR §9), the M4 maze +
  collision design, the M5 deterministic-flicker-in-`core` decision (ADR-026) and
  lit-render gate (ADR-027), the M6 deterministic-audio + no-miniaudio-yet
  decision (ADR-028) and WAV-spectrum gate (ADR-030), the M7 low-frequency biome
  field (ADR-031), pillars + validator extension (ADR-032), and stacked-level
  verticality (ADR-033), far-chunk float-precision deferral (camera-relative
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
`--stream` · `--walkbot` · `--topdown` · `--shot` · `--render-wav` · `--footsteps`
· `--audiosoak` · `--biomeat` · `--descend` · `--post` (see `app/MODULE.md`).

## Three real bugs caught (and fixed properly, not tuned around)

1. **Far-chunk float precision** tripped the M4 geometry validator (a 0.3 m wall
   measured 0.312 m at 320 km from origin). Fixed with validator thresholds
   sitting between the 0.3 m wall and 4 m cell; far-chunk camera-relative
   rendering remains a documented deferral.
2. **Em-dash in a Catch2 test name** made `catch_discover_tests` register a name
   that ctest's re-invocation couldn't match (Unicode arg round-trip), so a
   correct connectivity test "failed" only under ctest. Keep test names ASCII.
3. **Audio "underruns" that weren't.** The first real-time mixer model gave each
   block exactly one real-time slot (zero headroom), so ordinary Windows sleep
   jitter under load counted as 110 underruns. The fix was the *correct model*,
   not a looser threshold: a prebuffered producer that renders ahead of a virtual
   read cursor with ~170 ms headroom (how a real device ring works) — 0 underruns,
   and the off-vs-on tick-time comparison still proves the audio thread never
   blocks the sim.

## What's next

- **M12** integration, polish, acceptance: noclip intro sequence, photo mode, a
  settings file, README with one-command build/run, final spec-reconciliation audit,
  and the **12 h unattended acceptance soak with the Director ON** (M10 soak gates +
  the KEEL sidecar). The Director's M12 packaging: vendor a clean release
  `keel-serve.exe` + a tuned `keel.lock` into `third_party/keel/` (config, not a fork).

## How to continue (next session)

1. Read `docs/ARCHITECTURE.md`, the latest `docs/SESSION_LOG.md` entry, and the
   M12 section of `docs/MILESTONES.md`.
2. Produce the M12 change manifest (noclip intro, photo mode, `config.toml`, README
   one-command run, KEEL release-vendor, the 12 h acceptance harness) before code.
3. Run `gate.ps1 -Milestone M12` until exit 0, regression-sweep M0–M11, tag
   `v1.0`, push, write the SESSION_LOG entry.

_The repo is the memory. Last fully-green tag: `m11-green`. Never resume from a
broken state — revert to the last green tag if needed._
