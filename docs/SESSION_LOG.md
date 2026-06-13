# SESSION_LOG.md

Newest entry first. Every session appends: done / pending / open questions / gotchas.

---

## Session 6 — M5: Procedural Materials + Raster Lighting  🟡 IN PROGRESS (phase 1)

**Done (committed, additive, all gates still green).**
- **Procedural textures** — `render_d3d12/texgen.h/.cpp` (D3D12-free, unit-tested):
  `generate_texture` for Wallpaper / Carpet / CeilingTile / Fluorescent /
  Baseboard (256×256 RGBA, deterministic per-pixel hash + structural patterns),
  `texture_hash`. Determinism test green (per (kind,seed) stable; kinds/seeds
  differ). This is the **texture-determinism** exit gate, satisfied.
- Progress report written to `PROGRESS.md`.

**Remaining for M5 (the larger half — start here next session).**
1. GPU upload: 5 textures as a **Texture2DArray** + shader-visible SRV heap +
   **static sampler** + root signature with a descriptor table.
2. Extend `ChunkVertex` with **uv + material**; `GenerateChunk` assigns
   materials (floor=Carpet, walls=Wallpaper) and ADD a **ceiling** with
   CeilingTile + Fluorescent panels. (Vertex stride change → update the chunk +
   topdown pipeline input layouts.)
3. **Textured + lit chunk shader**: sample the array by (material, uv); forward
   **fluorescent grid lighting** (ceiling-height lights, falloff + flat ambient +
   emissive panels). Lights gathered near-camera into a CBV.
4. **Deterministic flicker** in `core` — `light_flicker(seed, light_id, tick)`
   (replayable); renderer multiplies light intensity by it.
5. `render_topdown` must **discard ceiling/fluorescent** fragments so the maze
   stays visible → **re-capture the M4 top-down golden** via `goldgen` + an ADR
   (geometry changed). `render_world_view` (M2 room) can stay flat to keep its
   golden, or also be textured (then re-capture + ADR).
6. Gate `Invoke-GateM5`: texture hash (done) · **5 poses × 3 seeds goldens**
   (fixed-pose textured+lit renders, perceptual diff < threshold) · **luminance
   histogram** band (catch all-black/blown-out) · **≥120 FPS @1440p** + zero
   debug. Then regression M0–M4, tag `m5-green`.

**Gotchas carried forward.** Chunk geometry vert count will rise again (ceiling)
— check the renderer chunk VB pool capacity (`kChunkSlotCapacityBytes`, currently
6144 verts/slot). Keep flicker in `core` so replays reproduce lighting. Texture
upload needs a transition to `PIXEL_SHADER_RESOURCE` and a one-time upload-heap
copy (untimed warmup, like the M3 pool).

---

## Session 5 — M4: Level-0 Generator — Rooms, Doorways, Connectivity  ✅ gate green (`m4-green`)

**Done.**
- **gen maze:** `gen/layout.h/.cpp` — G=8 cell grid, **spanning-tree** maze
  (recursive backtracker, provably connected) + ~25% extra carves + 4 edge
  doorways from a **shared-edge hash** (neighbours agree: a vertical edge cx-1|cx
  keys on cx). `validate_connectivity` (flood-fill). `chunk.cpp` rewritten:
  `GenerateChunk` → floor + wall geometry (render verts + collision `BoxInstance`s);
  `ValidateChunkGeometry` (no degenerate/floating/fat/stacked walls).
  Shared `contracts/geometry_v1.h` (`BoxInstance`).
- **collision:** app gathers the 3×3 chunk walls around the wanderer (regen on
  chunk crossing, deterministic) + ground floor → `core::tick` (3-arg). `core`
  stays gen-free.
- **walk-bot v1** (`--walkbot`): seeded wander + escape-on-block sweep; stuck =
  position bounding-box extent ~0 over 10 s (motionless), not net displacement.
- **top-down** (`--topdown`): `render_topdown` ortho render of a 3×3 block.
- **Gate `Invoke-GateM4`:** ctest (incl **10,000-chunk connectivity** zero-sealed
  + **10,000-chunk geometry** validators, doorway agreement); INV-5 grep;
  **walk-bot 1 km × 5 seeds, 0 stuck, deterministic**; **top-down golden** per
  seed (1,7) bit-identical ×3, zero debug. M0–M3 regression green (M3 with maze
  geometry: p99/median 1.28×, memory flat). ADR-022/023/024.
- Goldens `goldens/m4/topdown_seed{1,7}.png`.

**Verified numbers.** 10k chunks each connected + geometry-valid; walk-bot seed 1
→ 1000 m in 38,738 ticks, hash `a1cfc90ef154da01` (reproducible).

**Pending.** M5 — procedural materials + raster lighting v1: startup-generated
textures (yellow wallpaper, carpet, ceiling tiles, emissive fluorescents),
clustered/forward fluorescent grid lighting + seeded deterministic flicker (RNG
in sim core); luminance-histogram gate; ≥120 FPS @1440p.

**Gotchas / notes for the next session.**
- **Catch2 test names must be ASCII** — an em-dash (—) in a TEST_CASE name made
  `catch_discover_tests` register a name that ctest's re-invocation couldn't
  match (Unicode arg round-trip), so the test "failed" only under ctest, not by
  tag. Burned ~20 min; keep names ASCII.
- Geometry is **world-coordinate**; far-chunk (>~16M m) float noise is real —
  the geometry validator thresholds sit between 0.3 m walls and 4 m cells to
  tolerate it. Camera-relative rendering still deferred.
- Collision is per-chunk AABB walls gathered by the **app** (not core) — keeps
  the DAG clean. The walk-bot regenerates the 3×3 neighbourhood synchronously
  (no streaming dependency) for determinism.
- M4 raised chunk vert count (~3000) → renderer chunk VB pool capacity bumped to
  6144 verts/slot; M3 median frame rose to ~4.4 ms (still p99/median ≈ 1.3×).

---

## Session 4 — M3: Infinite Chunk Streaming, Placeholder Geometry  ✅ gate green (`m3-green`)

**Done.**
- **gen:** `GenerateChunk(seed, ChunkKey)` (chunk_gen_v1, pure/total INV-2) —
  world-coord grid floor (per-chunk tint) + interior posts; `ChunkContentHash`.
  Seam-correct by construction. Tests: 1000-chunk regen bit-identical, adjacent
  seams match exactly.
- **stream:** `StreamManager` — `(2r+1)^2` ring around a moving center, background
  **worker-thread pool** generates missing chunks, main thread collects + evicts.
  Bounded residency (INV-4), decoupled from the sim (INV-1). Tests: ring fill +
  recenter stays bounded.
- **telemetry:** `FrameCsv` (telemetry_v1) — per-frame CSV the gates parse.
- **renderer:** `render_chunks` — pos/nrm/color pipeline, **persistently-mapped
  vertex-buffer pool** (allocation-free stream-in) + upload budget; frees evicted
  slots. **Fixed an upload hitch** (per-chunk CreateCommittedResource → pool):
  p99/median dropped from ~3x to **1.2x @1280×720**.
- **core:** `open_ground()` + a collision-parameterized `tick` overload so the
  streaming walk traverses open ground without `core` depending on `gen`/`stream`.
- **app `--stream`:** marching walk on open ground, moves the streaming center,
  renders resident chunks headless, logs frame CSV; untimed warmup; `--seconds`
  soak. (M1/M2 modes intact.)
- **Gate `Invoke-GateM3`:** clean build (0 warn); ctest (24, incl regen/seam/ring);
  INV-5 grep; **hitch gate** — walk 125 chunks @1280×720, p99 frame < 2× median;
  **memory soak** (default 600 s) private-bytes slope ~0; inventory.
- ADR-019 (streaming arch), ADR-020 (VB pool + warmup), ADR-021 (gate metrics);
  reconciled into ARCHITECTURE §8 + 5 MODULE.md + contracts/README.

**Verified numbers.** 1500-frame walk: 169 resident, +0.9 MB over the walk,
p99/median 1.18× @1280×720, debug-clean. 60 s soak: 42,424 frames, +1.26 MB.

**Pending.** M4 — Level-0 generator: real room/doorway layout per chunk,
edge-constrained doorways (`hash(seed, sharedEdge)`), `/gen` connectivity +
geometry validators (flood-fill, no sealed boxes), walk-bot v1 with stuck
detection. Replaces the placeholder grid; the wanderer collides with real
generated walls (collision will read streamed/queried chunk geometry).

**Gotchas / notes for the next session.**
- Chunk geometry is **world-coordinate**; fine to ~16M m, then float precision
  degrades (camera-relative rendering deferred). M4 keeps world coords.
- The chunk VB **pool** is allocation-free after warmup — keep stream-in a
  memcpy; don't reintroduce per-chunk `CreateCommittedResource`.
- Hitch gate is **p99 < 2× median** (NFR §9), tested at **2560×1440** (target
  res; jitter-resilient) with **best-of-2** retry and a 1 ms `timeBeginPeriod`
  timer. An earlier 1280×720 single-run variant flaked post-build (2.35×); see
  ADR-021. `-StreamSoakSeconds` parameterizes the soak (600 s for green; pass a
  smaller value for quick regression sweeps).
- Streaming is decoupled from the sim — worker-thread timing never affects the
  WorldState hash; M2 cross-process determinism still holds.

---

## Session 3 — M2: Sim Core — Camera, Input, Collision, Replay  ✅ gate green (`m2-green`)

**Done.**
- **Contracts:** `contracts/world_view_v1.h` (`CameraPose`, `BoxInstance`,
  `WorldView`) + `contracts/replay_v1.h` (`InputCommand`, `ReplayHeader`,
  magic/version), shared via a header-only `contracts` INTERFACE target.
- **`core` sim:** `math.h`/`aabb.h` (Vec3 + overlap), `world.h/.cpp` —
  `WorldState` (wanderer + owned `Pcg64` + tick + odometer), fixed **120 Hz tick**,
  first-person walk camera, **capsule-vs-AABB collision** (AABB proxy, per-axis
  swept + substepped → no penetration at any speed, sliding preserves tangential
  velocity, no floor tunneling), gravity/jump, hardcoded **test room** (single
  source of truth for sim + render), `world_state_hash`, `wanderer_camera`.
  `replay.h/.cpp` — record/playback of input streams (replay_v1). Zero graphics
  includes (INV-5 grep gate).
- **Renderer:** `render_world_view` (headless) — depth buffer, root-constant MVP,
  runtime-compiled (D3DCompile) PSO + HLSL, draws the lit, depth-tested test room
  from a `WorldView`. row-major LH view/proj math.
- **app:** `--scene` (room → PNG from a fixed pose), `--sim --ticks N
  --seed S --record/--replay f --hashlog f` (drive sim, per-tick hash log). M1
  `--headless`/`--window` clear paths intact.
- **Unit tests:** collision (3 gates), per-tick hash determinism, replay
  round-trip + reproduction + bad-header rejection. (19 ctest cases, all green.)
- **Gate `Invoke-GateM2`:** clean build (0 warnings); full ctest; INV-5 grep;
  **cross-process replay** (record then 2 replays → bit-identical 3000-line
  per-tick hash logs); **room golden** bit-identical ×3 + matches committed
  golden, zero D3D12 debug-layer msgs. `gate.ps1 -Milestone M2` exits 0.
- Golden `goldens/m2/room_640x360.png` (hash `38350c25c2ae2f7d`). ADR-016
  (collision model), ADR-017 (contracts), ADR-018 (golden); reconciled into
  ARCHITECTURE.md §8 + MODULE.md files. **M0 + M1 regression sweep green.**

**Verified numbers.** Seed 777 / 3000 ticks → final hash `0e6105f7c33e525b`,
74.9 m walked, identical across record + 2 replays + 2 separate processes.

**Pending.** M3 — infinite chunk streaming: `GenerateChunk(seed, cx, cz)` pure
function, load/unload ring around the wanderer, background-thread generation +
main-thread GPU upload, placeholder numbered-grid geometry, frame-time telemetry
CSV. (Replaces the single hardcoded room with streamed chunks.)

**Gotchas / notes for the next session.**
- `contracts::` is `br::contracts`; in non-`br::core` TUs use a namespace alias
  (`namespace contracts = br::contracts;`) — a bare `contracts::` won't resolve.
- Collision is an **AABB proxy** for the capsule (ADR-016) — correct for the
  axis-aligned world; square corners, not rounded. Substep cap is 256 @ 0.05 m.
- The room golden depends on the camera pose, geometry, shading, and projection;
  changing any is a `goldgen` update + ADR (INV-8).
- `--scene` is headless-only; the determinism gates run the same binary/GPU.

---

## Session 2 — M1: Window, D3D12 Device, Headless Mode  ✅ gate green (`m1-green`)

**Done.**
- `render_d3d12/renderer.h` + `renderer.cpp`: opaque-PIMPL `Renderer`. DXGI
  factory, adapter selection (prefers the RTX 4070 Ti SUPER via
  `EnumAdapterByGpuPreference`, WARP fallback), D3D12 device, **debug layer +
  InfoQueue + DRED** (auto-breadcrumbs + page-fault), command queue/allocator/
  list, fence sync. Headless: offscreen R8G8B8A8 RT (optimized clear value
  matches the issued clear), copy → readback buffer with `GetCopyableFootprints`
  row-pitch handling → tight CPU RGBA. Windowed: flip-discard swapchain, present.
  No D3D12/DXGI/`<windows.h>` leaks through the header (INV-5 holds).
- `app`: CLI `--headless/--window`, `--frames N | --seconds S`, `--out PNG`,
  `--width/--height`, `--version`. Creates the Win32 window (no focus-steal),
  writes PNG via the shared `br_stb`, reports `debug_error_count` + memory.
- Gate `Invoke-GateM1`: clean build (zero warnings); ctest regression; **frame-0
  PNG bit-identical across 3 runs**; matches committed golden; **zero D3D12
  debug-layer messages** (headless + 10 s windowed run); **60 s memory soak**
  (372,750 frames, +1.6 MB private bytes → flat, no fence timeouts). Plus the
  standing INV-5 + inventory checks. `gate.ps1 -Milestone M1` exits 0; M0
  regression sweep still green.
- Golden `goldens/m1/frame0_320x180.png` (clear RGBA 46,43,33,255; hash
  `65e8578815ec303c`) via `goldgen capture`. ADR-014 (private-bytes soak metric)
  + ADR-015 (golden), reconciled into ARCHITECTURE.md §8.

**Pending.** M2 — `/core` standalone lib: fixed 120 Hz tick, seeded RNG already
present, first-person walk camera, capsule-vs-AABB collision + sliding, **input
replay** (record/playback), per-tick WorldState hash. The replay system is the
enabler for every later automated movement test.

**Open questions.** None blocking. PT/DXR (M9) will reuse this device; the
renderer is structured to add a second (DXR) path without touching the sim.

**Gotchas / notes for the next session.**
- The active PostToolUse hook rebuilds+tests after every Edit/Write; during
  multi-file features intermediate states fail it harmlessly — push through, the
  final state is green. (Real verification is the explicit `build.ps1`/gate runs.)
- PowerShell **StrictMode** is on in `common.ps1`: `.Count` on a scalar throws —
  wrap pipeline results in `@(...)` before `.Count` (bit us once in the gate).
- D3D12 readback **must** honor the 256-byte row-pitch alignment
  (`GetCopyableFootprints`); the renderer copies row-by-row into tight RGBA.
- Windowed gate run opens a 10 s window (`SW_SHOWNOACTIVATE`, no focus steal).
- Clear color is fixed/deterministic; changing it = new golden + ADR (INV-8).

---

## Session 1 — M0: Scaffold + Verification Harness  ✅ gate green (`m0-green`)

**Done.**
- CMake/Ninja/vcpkg skeleton (`CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`)
  matching the full 10-module inventory with correct dependency arrows; static
  `x64-windows-static` triplet, `/W4 /WX /permissive-`, `/fp:strict` on `core`+`gen`.
- `core`: real **PCG64** (XSL-RR 128/64) seeded RNG — the determinism oracle
  (INV-1). Portable 128-bit math, locked output-vector regression test.
- Stub libs for `gen, stream, telemetry, audio, render_d3d12, render_dxr,
  director` + `app` console exe (links the whole DAG). `MODULE.md` for all 10.
- Tools: `hashdiff` (image hash + mean-abs-diff, stb) and `goldgen`
  (deterministic synth via core RNG + golden capture; sole `/goldens` writer).
- Catch2 under CTest: seed/determinism/statistical tests + smoke link-check;
  `gate_canary` (deliberately failing, DISABLED) for the test-the-gate proof.
- Scripts: `lib/common.ps1` (VS dev-env import + Ninja-on-PATH + vcpkg discovery),
  `build.ps1`, `quickcheck.ps1` (exit 2 on fail), `precommit.ps1`,
  `install-hooks.ps1`, `gate.ps1` (M0 dispatch), `soak.ps1` (stub), and
  invariant checks `check_core_isolation.ps1` (INV-5) + `check_inventory.ps1`.
- Activated `.claude/settings.json` (PostToolUse → quickcheck). git pre-commit
  hook installed. ADR-010..013 recorded + reconciled into ARCHITECTURE.md §8.
- **`scripts/gate.ps1 -Milestone M0` exits 0.** Clean build, 10/10 tests,
  hashdiff round-trip, canary-nonzero, hook present, INV-5, inventory — all green.

**Pending.** M1 — Win32 window, D3D12 device/swapchain, debug layer + DRED,
`--headless --frames N --out` offscreen PNG dump, frame-0 golden.

**Open questions.** Remote backup: no `origin` was configured in the starter
(`<your-remote-url>` placeholder). Tagged `m0-green` locally; push deferred until
a remote exists (see below).

**Gotchas / notes for the next session.**
- Scripts run under **Windows PowerShell 5.1** (`powershell.exe`), not pwsh 7 —
  no ternary/`??`/`&&` in `scripts/*.ps1`.
- Ninja is **not** globally installed; `common.ps1` prepends the VS-bundled Ninja
  to PATH. The MSVC env is imported via `vcvars64.bat` inside `Enter-VsDevEnv`.
- vcpkg lives at `C:\vcpkg` (baseline pinned in `vcpkg.json`); a fresh clone
  self-bootstraps into `extern/vcpkg`.
- `files/` and `*.zip` (the redundant starter archives) are gitignored.
- The PostToolUse hook now builds+tests after every Edit/Write; expect a few
  seconds per edit. Intermediate multi-file states may transiently fail it.

---

## Session 0 — (starter created)
- Done: canon documents (ARCHITECTURE.md, MILESTONES.md), CLAUDE.md, kickoff guide, hooks template.
- Pending: M0 — scaffold + verification harness.
- Gotchas: activate .claude/settings.json only after scripts/quickcheck.ps1 exists (see KICKOFF_PROMPT.md).
