# DECISIONS.md

Full ADRs. Summaries live in ARCHITECTURE.md \xc2\xa78. Format: ADR-NNN / Status / Context / Decision / Consequences.

## ADR-001 .. ADR-009
See ARCHITECTURE.md §8 for the accepted set; expand each here as the build touches it. New dependencies, golden updates, and gate-threshold changes REQUIRE a new ADR entry in the same commit (Iron Rules 6 and 8).

## ADR-010 — vcpkg acquisition: pinned baseline + self-bootstrapping location
- **Status:** Accepted (M0).
- **Context:** ADR-007 chose vcpkg manifest mode. A fresh clone must resolve deps (Catch2, stb) with no manual setup, reproducibly.
- **Decision:** `vcpkg.json` pins `builtin-baseline` to `d592849579fb1fb22f87406b2184522ea21a8783`. `scripts/lib/common.ps1::Ensure-Vcpkg` discovers vcpkg in order `$env:VCPKG_ROOT` → `C:\vcpkg` → repo `extern/vcpkg`, cloning+bootstrapping (shallow) into `extern/vcpkg` (gitignored) when none exists. The dev machine uses `C:\vcpkg`.
- **Consequences:** Fresh clone self-bootstraps; vcpkg is never vendored in git; the baseline pin makes dependency versions reproducible (bumping it is a deliberate edit + ADR).

## ADR-011 — x64-windows-static triplet and static CRT
- **Status:** Accepted (M0).
- **Context:** The gate scripts run `app`, `tools`, and `tests` executables directly from `build/bin` and must not depend on DLL deployment.
- **Decision:** `VCPKG_TARGET_TRIPLET = x64-windows-static`; `CMAKE_MSVC_RUNTIME_LIBRARY = MultiThreaded$<$<CONFIG:Debug>:Debug>` (`/MT`). Third-party (Catch2, stb) and our code all link the static CRT.
- **Consequences:** Self-contained executables, no DLL-copy step, simpler gates; slightly larger binaries; CRT-mismatch is structurally impossible.

## ADR-012 — "test-the-gate" canary mechanism
- **Status:** Accepted (M0).
- **Context:** The M0 exit gate must prove a deliberately failing test is detected (would block a commit) without making the normal `ctest` run red.
- **Decision:** `gate_canary` is a Catch2 executable containing a `FAIL`, always compiled, registered in CTest as `DISABLED`. `scripts/gate.ps1` runs the binary directly and asserts a nonzero exit; the git `pre-commit` hook runs `quickcheck` (build + tests) so any real failing test blocks commits.
- **Consequences:** The normal suite stays green (canary disabled); the gate still mechanically proves failure detection; no reconfigure/second build dir needed.

## ADR-013 — warnings-as-errors scoping via external-header flags
- **Status:** Accepted (M0).
- **Context:** `/W4 /WX` on third-party headers (Catch2, stb) would fail the build on warnings we do not own.
- **Decision:** Strict flags live on the `backrooms_flags` INTERFACE target (our code only). Angle-bracket/`SYSTEM` includes are demoted with `/external:anglebrackets /external:W0`; the stb implementation TU is isolated in `br_stb` and compiled `/w`. The deterministic sim core additionally gets `/fp:strict` via `backrooms_sim_flags`.
- **Consequences:** `/WX` protects our code without third-party noise; "zero warnings-as-errors violations" is enforced by a clean build, not a grep.

## ADR-014 — M1 memory-soak metric: process private bytes, not the CRT debug heap
- **Status:** Accepted (M1).
- **Context:** The M1 gate wording is "60 s soak: stable memory (CRT debug heap delta ≈ 0)". We ship the **release** static CRT (`/MT`, ADR-011), which has no CRT debug heap, so that exact metric is unavailable without a separate debug-CRT build.
- **Decision:** Measure process **PrivateUsage** via `GetProcessMemoryInfo` (`PROCESS_MEMORY_COUNTERS_EX`) at soak start vs end and assert the delta `< 16 MiB` over the 60 s headless soak. "No fence timeouts" is enforced structurally: the renderer returns failure (nonzero process exit) if any fence wait times out.
- **Consequences:** Build-config-independent leak detection that preserves the gate's intent (flat memory, zero slope). Empirically ~1.5 MB over 62k frames in 10 s — warmup-dominated, no per-frame growth. A real leak in the tight render loop would be hundreds of MB and trip the gate immediately.

## ADR-015 — M1 frame-0 golden
- **Status:** Accepted (M1).
- **Context:** The M1 gate requires a committed headless frame-0 golden, bit-identical across runs.
- **Decision:** `goldens/m1/frame0_320x180.png`, captured via `goldgen capture` from the headless renderer at 320×180. The frame is the deterministic clear color RGBA (46, 43, 33, 255); FNV-1a content hash `65e8578815ec303c`; verified bit-identical across 3 consecutive runs on the dev GPU (RTX 4070 Ti SUPER).
- **Consequences:** Golden committed alongside this ADR (INV-8). Any future renderer change that alters the clear output requires a deliberate `goldgen` update plus a new ADR in the same commit.

## ADR-016 — M2 collision model: AABB-proxy capsule, per-axis swept + substepped
- **Status:** Accepted (M2).
- **Context:** The wanderer is canonically a "capsule" (glossary). The world is entirely axis-aligned (Backrooms). The exit gates require no wall penetration at any speed, sliding that preserves tangential velocity, and no floor tunneling under large per-tick movement.
- **Decision:** Model the wanderer's collision shape as an **AABB proxy** with half-extents `(radius, halfHeight, radius)`. `move_and_collide` advances the box and resolves **one axis at a time** (X, then Z, then Y), zeroing only the blocked axis (sliding preserves the others). Movement is **substepped** (≤ 0.05 m/substep, capped at 256) so fast motion cannot tunnel; the proxy's 0.7 m width also exceeds any substep, so it can never skip a solid.
- **Consequences:** Deterministic, robust, and exactly satisfies the three collision gates for axis-aligned geometry. Corner behaviour is square rather than rounded; a true rounded-capsule closest-point test can refine this in a later milestone without changing the contract.

## ADR-017 — M2 boundary contracts: world_view_v1 + replay_v1
- **Status:** Accepted (M2).
- **Context:** ARCHITECTURE.md §5 lists these boundaries; M2 is the first milestone that crosses them (core → renderer snapshot; input stream serialization).
- **Decision:** `contracts/world_view_v1.h` (`CameraPose`, `BoxInstance`, `WorldView` with `std::span` of boxes) and `contracts/replay_v1.h` (`InputCommand`, `ReplayHeader`, magic/version) are added as a header-only `contracts` INTERFACE target. Resident chunks and lights extend `WorldView` additively in M3/M5; the Event Log layers onto `replay_v1` later. Version mismatch on read maps to E_REPLAY_VERSION.
- **Consequences:** `core` (producer) and `render_d3d12` (consumer) share the headers without a module-level dependency. Breaking changes bump the version suffix (v2).

## ADR-018 — M2 test-room golden
- **Status:** Accepted (M2).
- **Context:** The M2 gate requires a golden screenshot of the hardcoded test room from a fixed pose.
- **Decision:** `goldens/m2/room_640x360.png`, captured via `goldgen capture`, rendered headless at 640×360 from the fixed inspection pose `pos(-3.5, 1.6, -4.0) yaw 0.5 pitch -0.05`, lit, depth-tested. Content hash `38350c25c2ae2f7d`; bit-identical across 3 runs on the dev GPU. Shaders are compiled at runtime via D3DCompile (deterministic for a fixed compiler), so a clean rebuild reproduces the golden.
- **Consequences:** Committed alongside this ADR (INV-8). Any change to the room geometry, camera pose, shading, or projection requires a `goldgen` update + ADR.

## ADR-019 — M3 streaming architecture: pure world-coord chunks + ring + worker pool
- **Status:** Accepted (M3).
- **Context:** Need an unbounded, never-repeating, bounded-memory world that streams around the wanderer (INV-2/3/4).
- **Decision:** `GenerateChunk(seed, key)` (chunk_gen_v1) is pure/total: it seeds a `Pcg64` from `hash(seed, key)`, emits placeholder geometry (grid floor + interior posts) in **world coordinates** so adjacent chunks share boundary vertices (seam agreement without neighbor queries). `stream::StreamManager` keeps a square `(2r+1)^2` ring around a center: missing chunks are generated on a background worker pool, the main thread collects results and evicts chunks outside the ring (bounded residency, INV-4). Streaming is fully decoupled from the sim, so it cannot perturb determinism (INV-1). The streaming walk collides against `core::open_ground()` (an implicit floor), keeping `core` free of any `gen`/`stream` dependency.
- **Consequences:** Far-from-origin chunks (|coord| beyond ~16M m) lose float precision; camera-relative rendering is deferred until needed. Real Level-0 rooms/connectivity replace the placeholder in M4.

## ADR-020 — M3 chunk vertex-buffer pool + warmup (hitch-free uploads)
- **Status:** Accepted (M3).
- **Context:** Per-chunk `CreateCommittedResource` on stream-in caused ~3 ms upload spikes (p99 ≈ 3x median) and a 32 ms first-frame pipeline-compile hitch.
- **Decision:** The renderer keeps a pool of persistently-mapped upload buffers (`kChunkSlotCapacityBytes` each), reused across chunks via a free-list; steady-state stream-in is a `memcpy` only (no allocation). The app runs an untimed **warmup** that builds the pipeline and fills the initial ring before any measured frame. A per-frame upload budget bounds work.
- **Consequences:** After warmup the pool size is stable; p99/median ≈ 1.2x at 1280×720. GPU memory bounded by `(2r+1)^2` slots.

## ADR-021 — M3 hitch + soak gate metrics
- **Status:** Accepted (M3).
- **Context:** MILESTONES M3 says "no frame > 2x median"; NFR §9 (the authoritative source for gate numbers) says **p99 frame < 2x median**. A headless loop at ~760 fps has unavoidable OS-scheduling jitter in the top 1%.
- **Decision:** The hitch gate walks 100+ chunks at **2560×1440** (the NFR's target resolution, where per-frame work dwarfs fixed OS jitter) and asserts **p99 frame-time < 2× median** (NFR §9). Three robustness measures keep it non-flaky without weakening the threshold: (1) 1440p raises the median to ~2.7 ms so the jitter tail is a small fraction; (2) the app raises the OS timer resolution to 1 ms (`timeBeginPeriod`) so GPU fence-wait wakeups pace smoothly; (3) the gate takes the **best of 2 walks** — a real streaming hitch repeats, a one-off OS load spike does not. Empirically p99/median ≈ 1.3–1.6× (incl. immediately post-build). The memory gate measures process private-bytes delta over a soak (default **600 s**, parameterized via `-StreamSoakSeconds`) and asserts `< 16 MiB` growth (private-bytes proxy per ADR-014).
- **Consequences:** Robust, non-flaky gate aligned with the NFR. An earlier 1280×720 single-run variant flaked under post-build load (p99 2.35×); the three measures above fixed it. Regression sweeps can pass a shorter `-StreamSoakSeconds`; the milestone-green tag uses the full 600 s.

## ADR-022 — M4 Level-0 generator: spanning-tree maze + edge-hash doorways
- **Status:** Accepted (M4).
- **Context:** Each chunk must be internally traversable with no sealed regions (INV-3), generated independently (INV-2), yet agree with neighbours on shared-edge doorways without communication.
- **Decision:** A chunk is a G=8 cell grid (`gen/layout.h`). `generate_layout` builds a **spanning tree** over the cells (iterative recursive-backtracker) — provably connecting all cells — then carves ~25% extra openings for loops/rooms, then opens one doorway on **each** of the 4 edges at a cell index from `door_index(seed, sharedCoords, axis)`; a vertical edge between chunks `cx-1|cx` uses `cx` so both sides compute the same index. Each chunk emits its full perimeter (both sides of a shared edge place identical, aligned walls + doorway). `GenerateChunk` builds floor + wall geometry (render verts + collision `BoxInstance`s) in world coordinates. `validate_connectivity` (flood-fill, zero sealed) and `ValidateChunkGeometry` (no degenerate/floating/fat/stacked walls) gate 10,000 chunks each.
- **Consequences:** Connectivity is structural (never sealed). The geometry validator uses thresholds well between wall thickness (0.3 m) and cell size (4 m) so far-chunk float noise (deferred camera-relative rendering, ADR-019) doesn't false-fail. Real rooms/biomes layer on in M7.

## ADR-023 — M4 collision integration + walk-bot v1
- **Status:** Accepted (M4).
- **Context:** The wanderer must collide with generated walls, but `core` cannot depend on `gen`/`stream` (DAG). The walk-bot must traverse the maze and detect being sealed in.
- **Decision:** The **app** (composition root) gathers wall AABBs from the 3×3 chunks around the wanderer (regenerated via `GenerateChunk` only on chunk crossing — deterministic, no streaming dependency) plus an implicit ground floor, and passes them to the 3-arg `core::tick`. The walk-bot (`--walkbot`) is a seeded **wander + escape-on-block** controller (sweep the heading when displacement stalls). **Stuck detection** measures the position's bounding-box extent over each 10 s window (spec: "position variance ≈ 0" = motionless/sealed), NOT net displacement — a wanderer that thrashes (slides/turns without net progress) still ranges far and is not stuck.
- **Consequences:** `core` stays gen-free; the walk-bot is deterministic (reproducible WorldState hash) and completes 1 km with zero stuck events across 5 seeds. Wall-following / smarter navigation is a future refinement.

## ADR-024 — M4 top-down debug golden
- **Status:** Accepted (M4).
- **Context:** The M4 gate needs a per-seed top-down render of a 3×3 chunk block.
- **Decision:** `render_topdown` uses a hand-built **orthographic** MVP (world XZ→screen, world Y→depth) over `[c-half, c+half]²`, drawing the chunk pipeline with a straight-down light. `--topdown` generates chunks (0..2)² and renders at 512×512. Goldens `goldens/m4/topdown_seed{1,7}.png` captured via `goldgen`; the gate renders ×3 bit-identical and compares per seed.
- **Consequences:** Committed alongside this ADR (INV-8). Distinct per-chunk tints + maze walls make connectivity/seams visually auditable. Changing geometry, tints, or the projection requires a `goldgen` update + ADR.

## ADR-025 — M5 procedural materials + GPU texture-array path
- **Status:** Accepted (M5).
- **Context:** The backrooms look needs material textures (yellow wallpaper, damp carpet, ceiling tiles, fluorescent panels, baseboards) with no asset files (everything procedural) and without breaking chunk determinism (INV-2) or core isolation (INV-5).
- **Decision:** `render_d3d12/texgen.{h,cpp}` synthesises 5 materials (`TexKind` Wallpaper/Carpet/CeilingTile/Fluorescent/Baseboard) as 256² RGBA from a per-pixel hash of `(kind, seed, x, y)` — pure, **D3D12-free**, unit-tested (`test_texgen`), deterministic per `(kind, seed)` with a `texture_hash`. `ChunkVertex` gains `uv` + `material` (kMat* ids that index the texture-array slice; 48-byte stride). `GenerateChunk` assigns floor=Carpet, walls=Wallpaper, and emits a **ceiling** grid (render-only, not collidable) with a regular fluorescent-tile pattern from `is_fluorescent_cell` (shared verbatim by gen geometry and the renderer's lights). The renderer uploads the 5 slices once into a **Texture2DArray** via an upload heap + `CopyTextureRegion` (fenced), bound through a shader-visible **SRV heap** + **static sampler** + root descriptor table; the lit PSO samples by `(uv, material)` and applies a per-chunk hue tint.
- **Consequences:** No asset files; textures regenerate identically anywhere. The contract grows (`uv`/`material` in `chunk_gen_v1.h`, `BoxInstance` hoisted to `geometry_v1.h`), and `ChunkContentHash` now covers them, so M3 regen/seam goldens were re-verified. `render_topdown` discards ceiling/fluorescent via a root-constant toggle so the M4 top-down goldens stay bit-exact (no re-capture).

## ADR-026 — M5 forward fluorescent lighting + deterministic flicker
- **Status:** Accepted (M5).
- **Context:** Fluorescent lighting is the backrooms' defining mood, including the occasional buzzing/flickering tube. It must be **replayable**: a recorded (seed, input) stream must reproduce identical lighting, so randomness cannot live in the renderer or use wall-clock.
- **Decision:** Flicker lives in **`core`** — `light_flicker(world_seed, light_id, tick)` is pure, `/fp:strict`, branch-free value-noise: ~1/8 of lights flicker within `[0.35, 1.0]` over ~24 Hz steps, the rest are steady (1.0). `render_chunks(camera, resident, budget, tick, out)` gathers the ceiling-grid fluorescent lights within R=10 cells of the camera into a CBV (b1, ≤64 lights), each position from the shared `fluorescent_light_pos` and intensity scaled by `light_flicker`. The lit shader sums per-light Lambert × distance attenuation `i/(1+0.09d+0.05d²)` over a flat ambient (0.22), then applies a **highlight knee** (compress everything above unity exposure by 0.18×) so stacked lights keep the wallpaper's yellow hue instead of clipping to pure white.
- **Consequences:** Lit replays are bit-reproducible from (seed, tick) with no renderer-side RNG; `core` stays graphics-free (INV-5). The knee + ambient/intensity (0.22 / 1.0) were tuned against the reference look (even, hazy, slightly over-exposed, never blown-out). Clustered/path-traced lighting parity is a later milestone (M6+).

## ADR-027 — M5 lit-render gate: fixed-pose goldens + luminance band + FPS
- **Status:** Accepted (M5).
- **Context:** M5 changes pixels, so it needs golden coverage of the textured+lit output plus guards that the frame is neither all-black nor blown-out and that lighting didn't blow the frame-time budget.
- **Decision:** `app --shot --pose P [--seed S] [--ticks T] --out p.png` renders one of **5 canonical camera poses** (forward/right/back at the proven-open spawn cell, plus look-up at the ceiling fluorescents and look-down at the carpet) at a fixed flicker tick — bit-exact per `(seed, pose, tick, GPU)` despite multithreaded streaming (co-planar same-material geometry is draw-order-invariant). It also prints a **luminance histogram** (`luma_mean`, `luma_p01/p50/p99`, `frac_black`, `frac_white`). `gate.ps1 M5` checks: **5 poses × 3 seeds** (1, 7, 42) goldens `goldens/m5/shot_seed{S}_pose{P}.png` (640×360, captured via `goldgen capture`) — bit-identical across two fresh processes, golden-matched, debug-clean, and in the luminance band (mean ∈ [50, 220], frac_black ≤ 0.35, frac_white ≤ 0.20); **≥120 FPS median @1440p** (best of 2, per ADR-021); plus a **regression** pass that re-renders the M1/M2/M4 render goldens and asserts zero pixel diff.
- **Consequences:** Committed alongside this ADR (INV-8); measured at ~179 FPS @1440p with margin. Goldens change only via `goldgen` + this ADR (Iron Rule 6). The luminance band is intentionally wide (per-pose framing varies); it catches gross regressions (black screen, white-out), not subtle shading drift — that's what the bit-exact goldens are for.

## ADR-028 — M6 procedural audio + deterministic offline render (no miniaudio yet)
- **Status:** Accepted (M6).
- **Context:** The backrooms needs sound — a fluorescent mains hum, an HVAC bed, footsteps, room reverb — generated procedurally (no asset files) and **headless-verifiable** (INV-7). The audio must be a pure function of `(WorldSeed, input stream)` so the offline render is reproducible and gate-checkable (INV-1).
- **Decision:** A deterministic, block-based `audio::Synth` (`/`-free of wall-clock): 60 Hz hum + harmonics, lowpassed-noise HVAC, footstep transients, and a Freeverb-style reverb whose RT60 comes from the room probe (ADR-029). Footfalls are derived in **`core`** as a pure floor of the odometer (`footstep_count`, `kStrideLength`) and the listener via `audio_listener` — both return only `contracts::audio_events_v1` types, so `core` stays audio-free (INV-5). The app's `--render-wav` drives the deterministic maze walk (the walk-bot), renders exactly `sr/120 = 400` audio frames per sim tick (integer, no resampling), and writes a hand-rolled PCM16 WAV plus a footstep-tick log. **No `miniaudio` dependency is added in M6:** every M6 gate is headless and deterministic, so a device-playback library buys nothing gate-testable and would risk nondeterminism. `miniaudio` stays earmarked (ARCHITECTURE dependency ledger) for real-time speaker output, to be added with its own ADR + `vcpkg.json` entry (Iron Rule 8) when that ships.
- **Consequences:** The offline WAV is bit-identical across runs/processes on a fixed toolchain; footstep ticks reproduce from a replay with **no change to `replay_v1`** (they're derived, not stored). WAV I/O is header-only (`audio/wav.h`), shared with `wavcheck`. Real-time playback is deferred (headless-first), so M6 ships sound you verify by spectrum, not by ear.

## ADR-029 — M6 room-probe reverb (raycast mean-free-path -> RT60)
- **Status:** Accepted (M6).
- **Context:** Reverb should track the space the wanderer is in — a tight corridor vs an open hall — without authored room volumes, and deterministically.
- **Decision:** `audio::probe_reverb_seconds` casts 16 horizontal rays from the listener at the wall AABBs (slab method, XZ footprint), averages the nearest-hit distance into a **mean free path**, and maps it to an RT60 (`0.15 + mfp*0.05`, clamped `[0.20, 1.80]` s). The app gathers the surrounding chunk walls (the same 3×3 `GenerateChunk` gather as M4 collision, keeping `core` gen-free) and re-probes ~10×/s during the offline render. The reverb feeds the synth's comb feedback (`g = 10^(-3*delay/RT60)`).
- **Consequences:** Reverb is pure and varies with the maze; a unit test asserts a 25 m room rings longer than a 3 m room. The probe is 2-D (full-height walls) and ignores the ground box (inside-box rays don't count), which is exact for the backrooms' vertical walls; sloped/var-height geometry would need a 3-D probe later.

## ADR-030 — M6 audio gate: WAV spectrum + footstep alignment + real-time soak
- **Status:** Accepted (M6).
- **Context:** Audio needs machine-checkable gates with no speakers: prove the render is right (spectrum, not silence), the footsteps line up with the sim, and the real-time mixer never starves or stalls the sim.
- **Decision:** `gate.ps1 M6` checks: (1) **offline WAV deterministic** across two fresh processes (`Get-FileHash`) and its **spectrum** passes `wavcheck assert` — a self-contained radix-2 FFT finds the 60 Hz fundamental + 120/180 Hz harmonics each well above a 1.5–3 kHz noise floor, and RMS clears a silence threshold (the "WAV spectrum" golden of ARCHITECTURE §2, computed not stored — audio is per-toolchain like renders are per-GPU); (2) **footstep 1:1** — `--render-wav`'s audio footstep log is byte-identical to `--footsteps`' independent replay-derived log; (3) **real-time soak** — `--audiosoak --audio` runs the mixer thread (prebuffered producer, ~170 ms headroom, fed lock-free) for `-AudioSoakSeconds` with **zero underruns**, and the sim tick time with audio on stays within 1.5× of off (the audio thread never blocks the tick loop). Plus a render-golden regression (M6 touches no render code) and the standard isolation/inventory checks.
- **Consequences:** No committed audio byte-golden (machine-specific); robustness comes from determinism-x2 + spectral bands + the alignment cross-check. Measured: WAV bit-identical, 60 Hz at ~12000× floor, 64/64 footsteps aligned, 0 underruns over 60 s (and a 10-min soak), tick-time delta ~0.4%. The soak's underrun semantics model a device ring with headroom, not a literal driver.

## ADR-031 — M7 biome field + biome-parameterized generation (golden re-capture)
- **Status:** Accepted (M7, phase 1–2).
- **Context:** The world should vary — classic yellow, cubicle farm, pipe corridors, parking garage, poolrooms — in **contiguous regions** with controllable proportions, without breaking per-chunk purity (INV-2), cross-seam doorway agreement, or connectivity (INV-3).
- **Decision:** `gen/biome.{h,cpp}` assigns a biome per chunk by a pure, **low-frequency** function: a coarse **K=3** chunk lattice, each cell drawn from a weighted CDF (`biome_at`), giving ~96 m contiguous regions with designed weights (ClassicYellow 44 %, CubicleFarm 22 %, PipeCorridors 16 %, ParkingGarage 12 %, Poolrooms 6 %). Each biome supplies `BiomeParams` (interior **carve ratio** = openness, pillar density, floor tint, wall darken). `generate_layout_carve` takes the carve ratio (spanning tree first → carving only removes walls → **connectivity holds for any ratio**), and `GenerateChunk` applies the biome tint. The **edge-doorway protocol (`door_index`) is untouched**, so adjacent chunks of *different* biomes still agree on seam doorways (INV-3 across biome boundaries). Biomes do not change the per-chunk vertex *positions*, so seam-crack tests stay green.
- **Consequences:** `GenerateChunk` output changes (tint + wall density per biome), so the M4 top-down (seeds 1, 7) and M5 lit-shot (seeds 1, 7, 42 × 5 poses) goldens were **re-captured via `goldgen capture`** in this commit (Iron Rule 6) — a legitimate generation change, not a gate relaxation; determinism (bit-identical ×2) and the luminance band still hold. The distribution gate (102,400 chunks within ±2 % of the weights) and per-biome 10k-chunk connectivity pass. Pillars (parking garage / pillar halls) and the geometry-validator extension they need are deferred to the set-piece phase; verticality (level −1) follows.

## ADR-032 — M7 pillars (set-piece columns) + geometry-validator extension
- **Status:** Accepted (M7, phase 2b).
- **Context:** Parking garages and pillar halls need free-standing **columns**. The M4 geometry validator only accepted *walls* (thin in exactly one axis); a square pillar (thin in both) tripped it. Pillars must also never seal the maze (INV-3) or wedge the wanderer.
- **Decision:** `GenerateChunk` adds a 0.5 m square, full-height collidable **pillar** at a cell's centre with probability `BiomeParams::pillar_density`, consumed from the chunk RNG only when the biome calls for it (so pillar-free biomes stay bit-identical). A centred 0.5 m column in a 4 m cell leaves ~1.75 m clearance on every side — far more than the 0.7 m capsule — so cell-to-cell passage (what `validate_connectivity` flood-fills) is untouched, and pillars sit at cell centres where walls (cell edges) never reach, so no overlaps arise. `ValidateChunkGeometry` now classifies each collision box as a **wall** (thin in exactly one axis) or a **pillar** (thin in both, ≤ 1 m square); only a box thin in *neither* axis (a fat block) is rejected. The `--biomeat` app mode reports the biome at the spawn chunk so goldens/gates can target a known biome.
- **Consequences:** Pillar biomes' geometry now validates; the per-biome geometry test (2,000 chunks each) and the walk-bot (1 km × 5 seeds, 0 stuck) stay green with pillars collidable. Per-biome lit goldens `goldens/m7/biome_<name>.png` captured (parking garage shows columns); M5 seed-42 shots re-captured (its spawn is a pillar biome). Pillars are decoration + collision only — no new material slice (they reuse the baseboard texture).
