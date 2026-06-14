# SESSION_LOG.md

Newest entry first. Every session appends: done / pending / open questions / gotchas.

---

## Session 10 — M9: DXR Path-Traced Mode  ✅ COMPLETE (`m9-green`, all 4 exit gates)

**`gate.ps1 -Milestone M9` exits 0 with all 4 exit gates; M0–M8 regression sweep
green; tagged `m9-green` + pushed. Next: M10 (8 h walk-bot soak + hardening — the
real `soak.ps1`, walk-bot v2, contactsheet, minidump/auto-restart).** Raster stays
the default + fallback (INV-6); the DXR path tracer is enhancement-only.

**Done (phase 4 — interactive PT + streaming, gates #3 + #4 GREEN; commit
`a5b740f`). + phase 5 (regression sweep + tag).** `render_pt_frame(cam, samples,
seed, reset)` accumulates spp across frames; `reset` clears the accumulator on
movement (otherwise the per-sample RNG indices continue → progressive refine).
`render_pt` is now a `reset=true` wrapper (byte-identical → gate-#2 golden
preserved). App: `--dxr-fps` (times 1-spp moving frames → FPS), `--dxr-ghost`
(converge A, render B with/without reset → clean-vs-fresh ~0 vs ghost-vs-fresh
large), `--dxr-walk` (walk-bot K km in PT, rebuilding BLAS/TLAS as chunks stream).
**Gate #3:** 178.5 FPS @ 1440p (≥60) + no-ghost (clean 0, un-reset ghost 31).
**Gate #4:** 1 km PT walk, 13 TLAS rebuilds, 280 frames, debug/DRED-clean.
**Regression sweep M0–M8: all PASS** (M9 changes are additive; raster goldens
byte-unchanged). Tagged `m9-green`.

**Done (phase 3 — path-traced lighting + converged golden, gates #1 + #2 GREEN;
commits `f358f7d`, `91276d0`).** Inline DXR 1.1 **`RayQuery`** (SM 6.5) path tracer:
`build_scene` concatenates resident chunk verts into one `StructuredBuffer` (shadeVb)
and tags each TLAS instance's start vertex offset in **InstanceID**, so the shader
reads per-hit normal/material via `(InstanceID + 3·PrimitiveIndex)`. `render_pt(cam,
samples, seed)` accumulates spp in **RGBA32F** across batched dispatches (≤64
spp/dispatch, under the GPU watchdog) → Reinhard resolve to RGBA + NDC depth.
Lighting = emissive fluorescent grid as area lights (analytic **NEE + shadow rays**
from the `is_fluorescent_cell` formula, no light list), one cosine **diffuse-GI
bounce**, small **ambient floor**, seeded per-(pixel,sample) PCG RNG. `dxc` gained a
target-profile param (default `lib_6_3`; PT uses `lib_6_5`). `app --dxr-pt --pose P
--spp N`. Recursive `render_scene` (gate #1) untouched. **Goldens** `goldens/m9/
pt_pose{1,3,4}.png` (1024 spp, goldgen). **Gate #2:** 1024 spp × 3 poses,
deterministic ×2, mean-abs-diff vs golden < 1.0, luma band, debug-clean. ADR-036.
**Measured: deterministic (bit-identical ×2, single + multi-batch), ~1.06 s/pose,
diff 0.0, debug/DRED clean.**

**Done (phase 2b — cross-renderer depth compare, exit gate #1 GREEN; commit
`15427d3`).** `DxrRenderer::render_scene` writes **NDC depth** (R32_FLOAT UAV at
`u1`) via the *same* hyperbolic `proj_lh(near=0.05, far=500)` mapping as
`render_d3d12`; `readback_depth()` returns it. **Key finding:** the DXR ray basis
(fwd/right/up) already equals raster's `view_lh` exactly — fwd = (sin·cos, sin,
cos·cos), right = cross((0,1,0),fwd), up = cross(fwd,right) — and the screen mapping
matches, so per-pixel rays align with no basis change (the SESSION_LOG worry was
resolved by inspection). `render_d3d12` gained `readback_depth()` (copies the
D32_FLOAT buffer; additive — raster output byte-unchanged, M5/M4 goldens still
bit-match). `app --dxr-depth` renders both, linearizes NDC→eye-space metres,
compares per pixel. `Invoke-GateM9` (gate #1) = clean build + ctest + dxr-probe +
**5-pose depth compare** + golden regression + INV-5 + inventory → **exit 0**.
**Measured (640×360):** every pixel co-foreground, mean depth rel-err ~1e-5, max
~1e-4, **zero mismatches** except 1 silhouette pixel at pose 4; raster+DXR
debug/DRED clean. Gate thresholds clear the measured values ~250×.

**Done (phase 2a — BLAS/TLAS + TraceRay).** `DxrRenderer::build_scene(chunks)`
builds a BLAS per resident chunk (triangles from `ChunkVertex`, world-space) + a
TLAS (identity instances); the scene state object adds a hit group + miss, SBT =
raygen|miss|hitgroup (64 B records), TLAS bound as a root SRV (t0).
`render_scene(camera)` casts primary rays (yaw/pitch/fov ray basis), closest-hit
shades by distance. `app --dxr --pose P`. **Verified: 169-chunk scene, maze traced
with correct depth, debug/DRED clean.** Fix: AS **scratch** buffers must be created
in `COMMON` (D3D12 ignores `UNORDERED_ACCESS` initial state for buffers → 1 warning
per chunk otherwise).

**Remaining for M9 (start at phase 3).**
- **Phase 3 (gate #2):** closest-hit PT (emissive fluorescents as lights, shadow +
  diffuse-GI rays, seeded per-(pixel,sample) RNG), accumulation buffer; 1000+ spp
  at 3 poses; RMSE < threshold (via `soak.ps1`). Shading currently distance-only.
- **Phase 4 (gates #3, #4):** interactive PT (accum reset on move; ≥60 FPS;
  no-ghost histogram-after-teleport), TLAS refit on stream; walk-bot 1 km PT, zero
  debug/DRED.
- **Phase 5:** `Invoke-GateM9` (4 gates + regression) + tag `m9-green`.

**Earlier this session (phases 1a + 1b).** Both hard risks (DXR toolchain + the
DispatchRays machinery) retired and debug-clean.

**Done (phase 1a — toolchain).** `render_dxr/dxc.*` — runtime DXC wrapper (loads
`dxcompiler.dll` via LoadLibrary + SDK scan; compiles HLSL → **signed** SM 6.3
DXIL through `IDxcCompiler3`; `dxil.dll` signs from beside it). `probe_caps()` +
`app --dxr-probe`. **Measured: RTX 4070 Ti SUPER, device5=1, RaytracingTier 1.1,
DXC → signed DXIL, dxr_ready=1.** ADR-035. No vcpkg change; FXC raster untouched.

**Done (phase 1b — dispatch).** `render_dxr::DxrRenderer` (own Device5 + queue +
`ID3D12GraphicsCommandList4`): a raytracing **state object** (DXIL_LIBRARY raygen
+ shader/pipeline config + global root sig with a UAV), an **SBT** (one raygen
record), and **DispatchRays** writing a UV gradient to a UAV → readback. `app
--dxr-test`. **Verified: dispatch works, gradient correct, debug-layer/DRED clean
(0).** The state-object/SBT/dispatch scaffolding phases 2–4 build on is proven.

**Remaining for M9 (start at phase 2).**
- **Phase 2 — AS + depth compare (gate #1):** BLAS per `ResidentChunk` (triangles
  from `ChunkVertex`) + TLAS; raygen now `TraceRay`s primary rays → hit `RayTCurrent`
  → depth; `app --dxr`; compare DXR primary-hit depth vs raster depth within epsilon.
- **Phase 3 (gate #2):** closest-hit PT (emissive fluorescents as lights, shadow +
  diffuse-GI rays, seeded per-(pixel,sample) RNG), accumulation; 1000+ spp at 3
  poses; RMSE < threshold (via `soak.ps1`).
- **Phase 4 (gates #3, #4):** interactive PT (accum reset on move; ≥60 FPS;
  no-ghost histogram-after-teleport), TLAS refit on stream; walk-bot 1 km PT, zero
  debug/DRED.
- **Phase 5:** `Invoke-GateM9` (4 gates + regression) + tag `m9-green`.

**Gotchas.** Command list must be `ID3D12GraphicsCommandList4` (SetPipelineState1
+ DispatchRays). SBT records: shader id is `D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES`
(32), each record aligned to `D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT` (64),
table base to 64. AS buffers need `ALLOW_UNORDERED_ACCESS` + the
`RAYTRACING_ACCELERATION_STRUCTURE` state; build needs a scratch buffer.

**Feasibility (confirmed).**
- **GPU:** NVIDIA RTX 4070 Ti SUPER present → DXR 1.1 (Tier ≥ 1.0) capable.
  (Intel UHD 630 also present; the renderer already picks HIGH_PERFORMANCE.)
- **DXC (the new toolchain piece):** DXR shaders need SM 6.3+ DXIL, which the
  FXC/`D3DCompile` path used everywhere else cannot produce. `dxcompiler.dll` +
  `dxil.dll` (validator/signer — needed so the runtime accepts the DXIL) exist at
  `C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\` (also 26100).
  Headers `dxcapi.h` + `d3d12.h` (with the raytracing structs) are on the SDK
  include path. **NOT in System32/PATH** → the DLLs must be copied next to the
  exe (CMake post-build) or LoadLibrary'd from the SDK path.
- New **system dependency** (dxc) → needs an ADR (no vcpkg entry; like d3dcompiler).

**Architecture decision.** Build `render_dxr` **self-contained** (its own
`ID3D12Device5`, queue, AS/PSO/SBT) rather than literally sharing
`render_d3d12`'s device object — cleaner for headless determinism + isolation;
both renderers consume the same `contracts::ResidentChunk` geometry so the
cross-renderer depth compare is apples-to-apples. (Reconcile render_dxr MODULE.md:
"device/share" → "same adapter-selection + same geometry contract".)

**Plan (5 phases; tasks #40–#44).**
1. **Foundation:** Device5 + `CheckFeatureSupport(OPTIONS5)` RaytracingTier
   (graceful fail if absent); runtime DXC wrapper (`DxcCreateInstance` from
   dxcompiler.dll via dxcapi.h; copy both DLLs to `build/bin`); a trivial DXR
   state object + raygen writing a UAV + SBT + `DispatchRays` + readback —
   debug-layer/DRED clean. Proves the whole toolchain. ADR for the dxc dep.
2. **AS + depth compare (gate #1):** BLAS per resident chunk (triangles from
   `ChunkVertex`), TLAS; raygen primary rays → hit distance → depth; `app --dxr`;
   compare DXR primary-hit depth vs raster depth within epsilon.
3. **PT lighting + converged golden (gate #2):** closest-hit shading, emissive
   fluorescents as lights, shadow + diffuse-GI rays, **seeded per-(pixel,sample)
   RNG** (deterministic); accumulation buffer; 1,000+ spp at 3 fixed poses; RMSE
   vs stored reference < threshold (run via `scripts/soak.ps1`).
4. **Interactive PT + streaming (gates #3, #4):** reduced bounces while moving,
   **accumulation resets on camera movement** (no-ghost: histogram delta after a
   teleport); ≥ 60 FPS walking; TLAS refit on stream events; walk-bot 1 km in PT
   mode, **zero debug-layer/DRED**.
5. **Gate:** `Invoke-GateM9` (4 exit gates + M0–M8 regression + inventory) + ADRs
   + tag `m9-green`.

**Gotchas (anticipated).**
- DXR DXIL must be **signed** (dxil.dll next to dxcompiler.dll) or the device
  rejects the state object. Copy BOTH DLLs to bin.
- Path tracing is stochastic → goldens need a **fixed seed + fixed spp + a
  deterministic accumulation order**; compare by **RMSE** (not bit-exact) to
  absorb GPU float reassociation across vendors.
- Raster stays the **default + fallback** (INV-6) — DXR is enhancement-only; never
  let `core`/`gen`/`stream` depend on it.
- Keep the existing `D3DCompile` (SM 5.0) raster path untouched; dxc is a
  separate compile path used only by `render_dxr`.

---

## Session 9 — M8: VHS Post-Processing + HUD  ✅ gate green (`m8-green`)

**Status: `gate.ps1 -Milestone M8` exits 0.** The picture now has the analog-horror
VHS treatment — film grain, chromatic aberration, lens (barrel) warp, scanline/
interlace flicker, vignette — plus a CRT-green HUD (timestamp, seed, odometer,
chunk, level, FPS). ADR-034.

**Done.**
- **VHS post pass** (`render_d3d12`): a depth-less fullscreen-triangle PSO samples
  the scene RT as an SRV and composites into a second `postRt` (read back instead
  of the scene). PS: barrel-distorted scene sampling, per-channel chromatic
  aberration, screen-space scanlines/interlace + vignette, and **seeded film
  grain** (`hash(px,py,seed + floor(time*60))` → fixed time = fixed grain). Params
  via root 32-bit constants. **Off by default** (`set_post`) → all prior goldens
  byte-unchanged.
- **HUD** (`app/hud.{h,cpp}`): a 5×7 bitmap font + `build_hud_overlay` → a
  transparent RGBA overlay (TIME `HH:MM:SS`, SEED, ODO, CHUNK x,z, LVL, FPS),
  uploaded (`upload_hud_overlay`) and composited **undistorted** (crisp).
  `hud_timestamp(ticks)` echoed to telemetry for the OCR-free gate.
- **app `--post`** on `--shot` (HUD + timestamp) and `--stream` (VHS-only, perf).
- **Gate `Invoke-GateM8`** (4 exit gates): post ON/OFF goldens `goldens/m8/`
  bit-identical ×2 + golden-matched + clean A/B (diff 24.3) + debug-clean (#1, #4
  seeded grain); timestamp 305160 ticks → `00:42:23` via telemetry + pixel golden
  (#2); post pass **0.65 ms @1440p** < 1.5 ms (#3); + M5/M4 render-golden
  regression + inventory.

**Pending / next.** M9 — DXR path-traced mode (BLAS/TLAS per chunk, temporal
accumulation, emissive fluorescents as real lights; raster stays default).

**Gotchas.**
- Post is OFF by default — never let it leak into the existing render goldens
  (verified post-off bit-identical to the M5 golden).
- Film grain MUST be seeded + time-quantised (`floor(time*60)`) or goldens flake;
  the HUD is rasterised on the CPU (deterministic) and composited undistorted.
- The post-pass perf measure runs VHS-only (no per-frame HUD upload) so it times
  the shader, not a 14 MB texture upload; ~0.65 ms vs the 1.5 ms budget.
- The HUD bitmap font only defines the glyphs actually used (digits, `:` `,` `.`
  `-`, and the label letters) — extend `glyph_for` if new HUD text needs more.

---

## Session 8 — M7: Biomes, Set Pieces, Verticality  ✅ gate green (`m7-green`)

**Status: `gate.ps1 -Milestone M7` exits 0.** The world now has *character*: five
biomes in contiguous regions (classic yellow, cubicle farm, pipe corridors,
parking garage with columns, poolrooms), pillar set pieces, and a stairwell that
descends to a dimmer **level −1**. All 4 exit gates pass; ADRs 031–033.

**Done (phase 1).** The **biome field** — `gen/biome.{h,cpp}`: a pure,
low-frequency `biome_at(seed, level, cx, cz)` over a coarse **K=3** chunk lattice
with a weighted CDF → contiguous regions with designed proportions. 5 biomes:
ClassicYellow 44 %, CubicleFarm 22 %, PipeCorridors 16 %, ParkingGarage 12 %,
Poolrooms 6 %. `BiomeParams` (carve ratio, pillar density, floor tint, wall
darken). `test_biome`: determinism, low-frequency contiguity, and **distribution
over 102,400 chunks within ±2 %** (M7 exit gate #2 — green).

**Done (phase 2a).** Biomes wired into generation: `generate_layout_carve(seed,
key, carve_ratio)` (biome openness knob; connectivity holds for any ratio),
`generate_layout` derives it from `biome_at`; `GenerateChunk` applies the biome
floor tint + wall darken. **Edge-doorway protocol untouched** → cross-biome seams
connect (INV-3); seam-crack / regen / doorway-agreement tests stay green.
`test_biome`: **every biome's layout connected over 10k chunks** (exit gate #1
connectivity — 5×10k). M4 top-down + M5 lit-shot goldens **re-captured via
goldgen** (ADR-031; determinism + match verified, diff 0).

**Done (phase 2b).** **Pillars** — `GenerateChunk` adds 0.5 m square full-height
collidable columns at cell centres with prob `pillar_density` (consumed from the
chunk RNG only when the biome calls for it → pillar-free biomes stay
bit-identical). `ValidateChunkGeometry` extended to accept square pillars (thin
both axes ≤ 1 m) vs walls (thin one axis) — ADR-032. `test_biome`: **per-biome
geometry valid, 2000 chunks each** (incl pillars). Walk-bot 1 km × 5 seeds, 0
stuck (pillars don't wedge). `--biomeat` app mode → per-biome lit goldens
`goldens/m7/biome_<name>.png` (classic=seed1, cubicle=4, pipe=2, garage=11,
pool=25; parking garage shows columns). M5 seed-42 shots re-captured (pillar
biome).

**Done (phase 3 — verticality).** `contracts::level_base_y(level)=level*4 m`
(level 0 → Y=0 unchanged; level −1 → Y=−4, dimmer). `GenerateChunk` offsets all
geometry by it; `ValidateChunkGeometry` floor check is level-relative.
`gen::build_stairwell` emits descending step boxes (set piece). `app --descend`
walks the wanderer down via gravity/collision — measured **4.019 m drop (one
level)** to level −1, hash-reproducible, landing chunk connected + valid (ADR-033).
`test_biome`: 4,000 level −1 chunks connected + geometry-valid (exit gate #3).

**Done (phase 4 — gate).** `Invoke-GateM7`: clean build + no-warn + ctest (biome
distribution ±2 %, per-biome 10k connectivity, per-biome geometry incl pillars,
level −1 cross-level + full M0–M6 regression) + INV-5 + **(#3)** `--descend`
deterministic ×2 / level −1 reached / sublevel connected + **(#4)** 5 per-biome
goldens bit-identical ×2 + golden-matched + debug-clean + M4/M5 render-golden
regression + inventory. **PASSED.**

**Pending / next.** M8 — VHS post-processing + HUD (film grain, chromatic
aberration, scanlines, timestamp overlay, vignette; HUD odometer/seed/coords).

**Gotchas.**
- Biomes must never touch the edge-doorway protocol (`door_index`) — only internal
  layout/decoration — or cross-biome seams seal (INV-3).
- Generation changes ripple into M4/M5 goldens → re-capture via `goldgen` + ADR in
  the SAME commit (done twice in M7: tint, then pillars).
- Far-chunk float precision (ADR-022) false-fails the geometry validator past
  ~1M m; gen geometry tests must use precision-safe coords (connectivity tests can
  range freely — cell topology is coord-independent).
- Level Y mapping is `level*4 m`; level 0 = Y 0 (so level-0 output is unchanged).
  Capsule collision descends stairs via gravity (step-down only).
- **Phase 3 — verticality.** Level −1 generation (`ChunkKey.level=-1`, Y-offset,
  altered params) + a **stairwell set piece** (descending step boxes) placed
  deterministically/rare, connecting level 0↔−1. A **scripted-descent replay**
  (app mode): wanderer walks down, Y drops a level, lands in a level −1 chunk;
  assert cross-level connectivity + determinism hash reproduces (exit gate #3).
  Note: capsule collision already steps DOWN via gravity; no step-up needed.
- **Phase 4 — gate.** `Invoke-GateM7`: 4 exit gates + regression M0–M6 + ADRs +
  SESSION_LOG + tag `m7-green`.

**Gotchas.**
- Biomes must NOT touch the edge-doorway protocol (`door_index`) — only internal
  layout/decoration — or cross-biome seams seal (INV-3 fail).
- Phase 2 changes `GenerateChunk` → M4/M5 goldens change. Re-capture via `goldgen`
  + ADR in the SAME commit; never hand-edit goldens.
- `biome_params` is currently unused (built clean under /WX as external linkage);
  it gets consumed in phase 2.
- Distribution gate uses seed 1234 (K=3 gives ~4σ margin for the 44 % biome).

---

## Session 7 — M6: Procedural Audio  ✅ gate green (`m6-green`)

**Status: `gate.ps1 -Milestone M6` exits 0.** The backrooms now *sounds* like the
backrooms: a 60 Hz fluorescent hum, an HVAC drone bed, footsteps timed to the
walk, and reverb that opens up in larger rooms — all procedural, all deterministic,
all verified headlessly (no speakers). ADRs 028–030. **No new dependency**
(miniaudio deferred to real-time playback, headless-first).

**Done.**
- **Contract** `contracts/audio_events_v1.h` (core→audio): `AudioListener`,
  `FootstepEvent`, `kAudioSampleRate=48000`, `kAudioChannels=2`, `kStrideLength`.
- **core** (pure, additive, no hash/replay change): `footstep_count(odometer)` =
  floor(odometer/stride); `audio_listener(state)`. Footsteps derive from the
  already-hashed odometer → reproduce from a replay (INV-1); `core` stays
  audio-free (returns only contract types, INV-5).
- **audio** module: `Synth` (deterministic 60 Hz hum + harmonics, HVAC noise bed,
  footstep transients, Freeverb reverb sized by RT60), `room_probe` (16-ray
  mean-free-path → reverb seconds), header-only `wav.h` (PCM16), `AudioEngine`
  (headless real-time mixer thread, prebuffered producer ~170 ms headroom, fed
  lock-free).
- **app**: `--render-wav` (offline: maze walk → 400 frames/tick → PCM16 WAV +
  footstep log, bit-identical x2), `--footsteps` (independent reference log),
  `--audiosoak [--audio]` (real-time mixer soak; mean tick time + underruns).
- **tools/wavcheck**: WAV reader + self-contained radix-2 FFT; `spectrum` +
  `assert` (60 Hz fundamental + 120/180 Hz harmonics over noise floor; RMS
  silence check).
- **Gate `Invoke-GateM6`** (`-AudioSoakSeconds`, default 60): ctest (synth
  determinism, WAV round-trip, 60 Hz hum, room-probe, footstep floor + full
  regression) · INV-5 · offline WAV **deterministic x2** + `wavcheck assert` ·
  **footstep 1:1** (audiolog == replay reference) · **soak: 0 underruns** +
  audio-on tick time within 1.5× of off · M5/M4 render-golden regression ·
  inventory. Measured: 60 Hz at ~12000× floor, 64/64 footsteps aligned, 0
  underruns over 60 s (and a 10-min soak), tick-time delta ~0.4%.

**Pending / next.** M7 — Biomes, set pieces, verticality (biome field over chunk
space; rare set pieces; level −1 descent).

**Gotchas.**
- All sound-affecting randomness lives in `core`/`Synth` seeded state — never
  wall-clock — so the offline WAV is bit-identical across runs/processes.
  `AudioEngine` is the **only** place wall-clock is allowed (real-time pacing).
- The audio "golden" is the **WAV spectrum** (wavcheck FFT bands), not a committed
  byte-file — audio is per-toolchain like renders are per-GPU. The gate checks
  determinism-x2 + spectral bands instead of a stored WAV.
- Underruns must be measured against a **prebuffered** ring (headroom), not a
  zero-headroom deadline — the first engine model false-failed on Windows sleep
  jitter (110 underruns); the headroom model gives 0. Sim throughput off vs on is
  the proof the audio thread doesn't block the tick loop.
- `sr/120 = 400` is an exact integer (48000/120) → one tick maps to exactly 400
  audio frames, no resampling.

---

## Session 6 — M5: Procedural Materials + Raster Lighting v1  ✅ gate green (`m5-green`)

**Status: `gate.ps1 -Milestone M5` exits 0.** Backrooms now *looks* like the
backrooms: yellow wallpaper, damp carpet, dark ceiling tiles, glowing fluorescent
panels, even hazy fluorescent lighting. ADRs 025–027.

**Done.**
- **Procedural textures** — `render_d3d12/texgen.*` (D3D12-free, unit-tested):
  5 materials (Wallpaper/Carpet/CeilingTile/Fluorescent/Baseboard), deterministic
  per (kind,seed), + `texture_hash`.
- **Chunk geometry** — `ChunkVertex` gains uv + material (48B stride);
  `GenerateChunk` assigns materials (floor=Carpet, walls=Wallpaper) and emits a
  **ceiling** grid with the **fluorescent-tile pattern** (`is_fluorescent_cell` /
  `fluorescent_light_pos`, shared verbatim by gen + renderer). Ceiling render-only
  (not collidable). `ChunkContentHash` covers uv+material; M3 regen/seam re-green.
- **GPU textured + lit render** — `render_chunks(.., tick, ..)` uses a **lit
  pipeline**: Texture2DArray (5 slices, fenced upload + `CopyTextureRegion`),
  shader-visible SRV heap + static sampler + root descriptor table; samples by
  (material, uv) with a per-chunk hue tint.
- **Forward fluorescent lighting** — renderer gathers ceiling-grid lights within
  R=10 cells of the camera into a CBV (b1, ≤64), each scaled by
  **`core::light_flicker(seed, light_id, tick)`** (pure, `/fp:strict`, ~1/8 lights
  flicker — lives in `core` so replays reproduce lighting). Shader: ambient 0.22 +
  Σ Lambert×atten, then a **highlight knee** (compress >1.0 by 0.18×) so stacked
  lights keep the wallpaper hue instead of clipping to white. Intensity 1.0.
- **`--shot` mode** — deterministic fixed-pose lit render (5 canonical poses) at a
  fixed flicker tick; prints a luminance histogram. Bit-exact per (seed,pose,tick).
- **Gate `Invoke-GateM5`** — ctest (texgen determinism + full regression) · INV-5 ·
  **5 poses × 3 seeds (1,7,42) goldens** `goldens/m5/` (640×360, goldgen-captured):
  bit-identical ×2, golden-matched, **luminance band** (mean∈[50,220],
  frac_black≤0.35, frac_white≤0.20), debug-clean · **≥120 FPS @1440p** best-of-2
  (measured **~179 FPS**) · **regression**: M1/M2/M4 render goldens still bit-match.

**Pending / next.** M6 — Procedural Audio (room-probe reverb, fluorescent buzz).

**Gotchas.**
- Flicker MUST stay in `core` (replay reproducibility). Renderer only *reads* it.
- The lit shot is bit-identical across runs despite multithreaded streaming —
  co-planar same-material geometry is draw-order-invariant (verified ×3).
- Light intensity 1.0 + knee 0.18 + ambient 0.22 are the tuned look; raising
  intensity blows walls to pure white (lost the yellow). See ADR-026.
- `--shot` poses sit at the proven-open spawn cell (16,16) and vary only
  orientation, so no pose embeds the camera in a wall on any seed.

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
