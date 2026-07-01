# RT @ 4K — the Carmack pass: ranked optimizations from a full code read (report only, no code changed)

> **Session 45 (2026-07-01).** Operator: *"at 4K, ray tracing is horribly slow — even without DLSS this can't be
> this slow; find clever optimizations, Quake-III-fast-inverse-sqrt spirit."* This report is grounded in a fresh
> read of `render_dxr/src/dxr.cpp` (the whole PT shader + host), the `run_game` RT path in `app/src/main.cpp`, and
> `render_d3d12/src/renderer.cpp::present_pt_texture`. It extends `RT_PERF_PLAN.md` (sessions 40/43/44) — nothing
> here re-litigates what shipped; everything here is *new* headroom found in the current code.
>
> **The one-line diagnosis: while you WALK, the path tracer never accumulates — every frame is a from-scratch
> 4-spp reset frame (~10 rays/px), and on top of that every frame pays ~30 MB of dead PCIe readback copies and a
> single-buffered triple CPU⇄GPU sync. The still-view optimizations shipped so far never engage during gameplay.**

---

## 0. TL;DR — ranked levers

| # | Lever | Est. win @ 4K-Quality (walking) | Image change? | Size | Risk |
|---|---|---|---|---|---|
| 1 | Kill the **dead per-frame readback copies** (color+depth → readback heaps nobody reads in-game) | ~1–2 ms/frame | none | ~20 LOC | none |
| 2 | **One-submit frame + 1 frame in flight** (fold creature-AS build into the PT list; stop `wait_idle`-ing 3–4× per frame; persistent buffers instead of per-frame `CreateCommittedResource`) | ~2–4 ms/frame + CPU/GPU overlap | none | ~150–250 LOC | low (hazards need care) |
| 3 | **Camera-only depth reprojection** of `g_accum` (the session-44 keystone — this report adds the evidence it's the #1 lever: walking = permanent reset) | walking drops 10→4 rays/px ≈ **2.5×** on the dominant cost, AND looks better | interactive-only, better | ~300 LOC + oracle | medium |
| 4 | **Denoiser diet** (fp16 filter input, packed guide, per-pixel adaptive taps by `g_accum.a`, LDS-tiled compute) | ~1–3 ms/frame | interactive-only, ≈none | ~150 LOC | low |
| 5 | **GI ray `TMax` clamp** to ~24–32 m (interactive bit) | ~5–15% of ray time, most in open rooms/shafts — the operator's pain case | interactive-only, tiny bias | ~10 LOC + oracle | low |
| 6 | **Low-discrepancy sampling** (R2 / blue-noise instead of white `rndf`) | same rays, visibly less noise → enables 1-spp everywhere | interactive-only | ~60 LOC | low |
| 7 | **Irradiance lattice** (DDGI-lite probes on the 4 m cell grid; kills the GI bounce ray + its shadow ray) | remaining 4→2 rays/px ≈ **2×** on top of #3 | interactive-only, *visible* (needs A/B) | milestone-sized | medium-high |
| 8 | **Fetch diet** (per-primitive 4 B normal-code+mat instead of 3×48 B vertex fetch; static normals are only ±X/±Y/±Z) | ~5–10% (latency/cache on incoherent hits) | none (bit-care needed) | ~80 LOC | low-med |
| 9 | **Catmull-Rom + RCAS present upscale** | quality enabler: makes Balanced/Performance look near-Quality at 4K → user-side 1.5–3× | present-only | ~60 LOC shader | low |
| 10 | SER (Shader Execution Reordering, Ada) — needs the GI bounce restructured to `TraceRay`/HitObject | 15–40% on incoherent rays (vendor-claimed) | none | large + vendor/SDK gate | high (ADR, Agility ≥1.716 preview or NVAPI) |

Estimated compound (rough, ±40%): **~17 fps → ~60–80 fps at 4K native-window, Quality (2/3) internal scale**, without
touching the offline/golden path. Items 1–6 + 9 alone should land ~45–60 fps; item 7 is the second doubling.

**Step 0 before any of it: add GPU timestamp queries** (one `ID3D12QueryHeap`, ~40 LOC) around creature-AS / PT
dispatch / denoise dispatch / copies / present. Every number below is an estimate; the repo's own culture (oracles
over assertions) applies to perf too. `--dxr-bench` exists but times whole `render_pt_frame` calls CPU-side
([main.cpp:5190-5202](../app/src/main.cpp)) — it can't split rays vs denoise vs copies.

---

## 1. Where a 4K frame actually goes (current code, walking)

Geometry of the problem (all confirmed in code):

- Internal PT res at 4K **Quality = 2/3 → 2560×1440 = 3.69 M px** ([main.cpp:1983](../app/src/main.cpp), `kRtScales`).
- **Walking/turning = accumulator reset EVERY frame.** `pt_view_moved()` uses ε = 1e-8 m² position / 1e-5 rad angle
  ([main.cpp:906-910](../app/src/main.cpp)); head-bob alone (M18, applied to the camera each frame,
  [main.cpp:1977](../app/src/main.cpp)) exceeds it whenever you move. The comment at
  [main.cpp:902-905](../app/src/main.cpp) says this is by design ("head-bob/mouse-look move the pose past it, which
  correctly resets"). On reset, `run_game` asks for **4 spp** instead of 1
  ([main.cpp:2019-2022](../app/src/main.cpp)).
- Per-pixel rays on a reset frame ([dxr.cpp:801-895](../render_dxr/src/dxr.cpp)): 1 primary + 1 RIS shadow
  (`direct_light_stochastic`, [dxr.cpp:648-669](../render_dxr/src/dxr.cpp)) + 4 GI bounce traces + up to 4 bounce
  RIS shadows = **~10 rays/px** → **~37 M rays/frame**. A still (accumulating) frame is 4 rays/px — **walking costs
  2.5× per frame and never converges**. The Tier-1 accumulation (E12) and everything built on it only pays off
  standing still. This asymmetry — the gameplay case being the worst case — is the core finding.
- **Denoise filter**: single dispatch, 4 à-trous levels × 24 taps = **96 taps/px**, each tap reading
  `g_accum` (RGBA32F, 16 B) + `g_guide` (RGBA32F, 16 B) ([dxr.cpp:762-799](../render_dxr/src/dxr.cpp)) → ~3 KB/px
  raw texel traffic at internal res, every frame, converged or not.
- **Dead copies**: the resolve batch copies the full **depth** texture to a READBACK (system-memory) buffer every
  frame ([dxr.cpp:1401-1407](../render_dxr/src/dxr.cpp)) and `copy_color()` copies the full **color** UAV to another
  readback buffer every frame ([dxr.cpp:1350-1358, 1425](../render_dxr/src/dxr.cpp)). In-game, depth readback has
  **no consumer at all**, and color is consumed only when the Director VLM / voice-chat grabs a POV (~every 28 s) or
  `--out` is set ([main.cpp:2026-2037](../app/src/main.cpp)). At Quality-internal that is **29.5 MB/frame over PCIe,
  serialized on the direct queue** (~1.3–1.9 ms at PCIe-4 rates) — negligible in the session-44 720p smoke
  (~0.8 MB), which is why it never showed up; at 4K it's real money. This is also why 4K feels *disproportionately*
  worse than the resolution ratio alone predicts.
- **Sync structure**: the DxrRenderer is single-buffered — one allocator, one list, `wait_idle()` after every
  submit. Per frame: `update_creature` waits **twice** ([dxr.cpp:1170, 1270](../render_dxr/src/dxr.cpp)), the PT
  batch waits ([dxr.cpp:1431](../render_dxr/src/dxr.cpp)), present waits its fence
  ([renderer.cpp:1947-1954](../render_d3d12/src/renderer.cpp)). Three-to-four full CPU⇄GPU round trips; zero
  overlap; all CPU work (sim, brain polls, caption builds) adds serially to the frame.
- **Per-frame allocation churn**: `update_creature` calls `make_buffer` (= `CreateCommittedResource`) for the
  creature VB, BLAS scratch, instance buffer, TLAS scratch, **and re-creates the TLAS resource itself** every frame
  ([dxr.cpp:1186, 1215, 1243, 1256-1259](../render_dxr/src/dxr.cpp)) — 4–6 committed-resource create/destroys per
  frame (~0.2–1 ms CPU + allocator churn). The refit (item C) fixed the *build* cost; the *resource lifecycle*
  around it still pays full price.

Back-of-envelope at 4K-Quality walking, anchored to session 44's measurements (PT ≈ 5.7 ms at 0.41 M px internal →
×9 px → ~51 ms; fixed ≈ 5.5 ms of which the copies grow ×9): **~55–60 ms ≈ 17 fps. That's the "horribly slow."**

What is *already good* and needs no touch: inline RayQuery everywhere; `ACCEPT_FIRST_HIT_AND_END_SEARCH` +
tight `TMax` on shadow rays ([dxr.cpp:613-620](../render_dxr/src/dxr.cpp)); RIS single-light NEE (E31); opaque
geometry flags; chunk BLAS `PREFER_FAST_TRACE` / creature refit-in-place (E25); flat-albedo materials (no
procedural-noise tax in the PT — verified: `albedo_of` is branchy constants, [dxr.cpp:604-611](../render_dxr/src/dxr.cpp)).

---

## 2. Found money — zero image change, no goldens risk

### 2.1 Delete the dead readback copies (~1–2 ms @ 4K)
Make both per-frame copies conditional on an explicit request:

- `render_pt_frame(..., bool want_readback=false)` — only record the color copy when true. The game loop passes
  true only on the sparse vision/chat/`--out` frames it already knows about ([main.cpp:2026-2037](../app/src/main.cpp)).
- Depth copy: only gates/tests read `readback_depth()` — gate/offline entry points request it; `run_game` never does.

Offline/golden paths (`render_pt`, gate harnesses, `--game-shot`) always request readback → **bit-identical gates by
construction**. This is the highest certainty-per-LOC item in the report.

### 2.2 One-submit frame + persistent scratch (~2–4 ms + overlap)
`update_creature` and `render_pt_frame` execute back-to-back on the same queue every frame, each with its own
submit+drain. Fold them:

1. Record creature BLAS refit + TLAS build + UAV barrier + PT accumulate + (denoise) + present blit into **one**
   command list / one `ExecuteCommandLists` (the denoise fold of E24 proved the pattern).
2. Replace `wait_idle()`-after-submit with **one fence waited at the top of the *next* frame** (1 frame in flight).
   Hazards to double-buffer: the creature tail of `shadeVb` (a second 4096-vert region, ping-pong via
   `InstanceID`), the instance/scratch buffers, and the 28-DWORD root constants (re-set per list anyway).
3. Allocate creature VB / scratches / instance buffer / TLAS **once at max size** and reuse
   (`kMaxCreatureVerts` already bounds them; TLAS instance count is bounded by the streaming ring + 1).

`update_creature`'s two waits, the PT wait, and the per-frame `CreateCommittedResource` churn all disappear. The
raster present fence stays (it paces the swapchain). Debug layer stays on (ADR-077) and will catch any hazard we
miss — that's its job.

### 2.3 Incremental `build_scene` (kills the chunk-crossing hitch)
Every 32 m crossing rebuilds **all** resident chunk BLASes + waits ([dxr.cpp:924-1053](../render_dxr/src/dxr.cpp)),
though typically only the leading edge of the ring changed. Cache BLAS + vertex range per `ChunkKey` (generation is
deterministic per key), build only new chunks, drop departed ones, rebuild just the TLAS. Turns a periodic
many-ms hitch into a small steady cost. (Same trick the creature-vision warm window used for meshes, E11.)

---

## 3. The keystone, now with evidence: camera-only depth reprojection

Session 44 already scoped this; the code read upgrades it from "elegant frontier" to **the single biggest lever**,
because walking is a permanent reset today (§1). What makes it unusually cheap *here*:

- **The world is static and the camera pose is exact.** Reprojection needs no motion vectors: for each pixel,
  reconstruct the world position from `g_depth` + the *previous* camera, project into the *current* camera, and
  fetch history. One matrix transform per pixel, no G-buffer changes — `g_depth` and `g_guide` (normal + view-Z)
  already exist ([dxr.cpp:881-884](../render_dxr/src/dxr.cpp)).
- **The only moving object already self-identifies.** The mat-7 history reject (E22) composes perfectly: creature
  pixels reject reprojected history exactly as they reject accumulated history today — the ghost fix carries over
  for free. Disocclusion test = |reprojected depth − current primary depth| + normal agreement from `g_guide`.
- **The payoff is double**: walking frames stop resetting → 1 spp moving (10→4 rays/px, ≈2.5× on the dominant
  cost) **and** the walking image inherits converged history instead of 4-spp noise + boil — this directly
  addresses "noisy AND slow," same as the operator's original complaint pair.

Sketch (interactive-only, new `uFrame` bit, offline path untouched → gate M9 bit-identical):
ping-pong `g_accum`/`g_guide` (A/B), pass `prevView` in the cbuffer (root-constant budget: currently 28 DWORDs —
move to a small CBV if needed); in the accumulate pass, when `uSampleStart==0 && reproject`, seed
`prevSum/prevCount` from the reprojected fetch (bilinear on accum, count clamped to e.g. ≤32 so stale radiance
decays) instead of zero. Validation-plan sketch: (a) still-view A/B must be bit-comparable to today's accumulation
after N frames (an oracle can assert MAD ≈ noise floor); (b) the moving case is the hard-to-automate one — walk-bot
A/B captures + the operator's eyes (this is why session 44 deferred it; it remains the one item needing a careful
pass, but it's worth it).

Once history transport exists, two follow-ons get cheap:
- **ReSTIR-lite temporal light reuse**: persist last frame's RIS winner (packed light cell + weight, one
  R32G32_UINT texture) as an extra reservoir candidate → big variance cut in penumbrae for ~zero rays. The E31
  unbiasedness oracle (`--dxr-stoch`, [main.cpp:5144-5155](../app/src/main.cpp)) extends to cover it.
- **Per-pixel adaptive spp**: converged pixels (`g_accum.a ≥ N`, not mat-7, via last frame's guide) early-out in
  raygen before tracing. Convergence is spatially coherent → whole warps retire; a still view progressively stops
  tracing (GPU cools to the present cost). This is E-done-right — per-pixel, so the writhing creature keeps its
  full rate (the regression that killed E's whole-frame version can't happen).

---

## 4. Quality-per-ray: the magic constant is 1.32471795724474602596

The Quake trick wasn't "compute faster," it was "compute *less* for the same perceived result." Today every sample
uses white noise (`rndf`, a PCG-hash chain, [dxr.cpp:601](../render_dxr/src/dxr.cpp)) for AA jitter, hemisphere
direction, and the RIS pick. White noise error ∝ N^-0.5; stratified/low-discrepancy sequences approach N^-1 —
**at 1–4 spp this is the difference between "speckle" and "smooth"** for the same ray count.

- **R2 sequence** (Roberts): the 2-D generalization of the golden ratio, built from the *plastic constant*
  ρ = 1.3247179572… → α = (1/ρ, 1/ρ²) ≈ (0.7548777, 0.5698403). `frac(n·α + cranley_patterson(px))` — two fused
  multiply-adds, **no texture, no state**, deterministic. Use sample index `n = pixCount` so the sequence continues
  across accumulation, per-pixel decorrelated by a hash offset.
- Or a **procedural blue-noise tile** (void-and-cluster, ~150 LOC, generated once at startup from the world seed —
  no asset files, per repo rules) for the AA jitter + RIS pick; R2 for the hemisphere.
- Interactive-only via the existing bit pattern; offline keeps `rndf` → goldens bit-identical. Oracle: converged
  A/B (both must converge to the same image — same harness as E31); the *win* shows as lower MAD at fixed low spp,
  which is directly assertable (`--dxr-denoise`-style: MAD(LDS 4spp, ref) < MAD(white 4spp, ref)).

This is deliberately sequenced *before* the integrator surgery: better samples make 1-spp-everywhere (post-
reprojection) visually acceptable, which is what lets the ray count stay down.

---

## 5. Denoiser diet (~1–3 ms @ 4K-Quality internal)

The filter is 96 taps × 32 B/px of RGBA32F traffic every frame ([dxr.cpp:777-793](../render_dxr/src/dxr.cpp)).
Three independent cuts, all interactive-only (the filter never runs in the golden path):

1. **fp16 filter input**: after accumulate, write `c = accum.rgb/count` once into an R16G16B16A16F (or R11G11B10 +
   separate count) texture; filter reads that instead of dividing 32-bit `g_accum` at every tap. Halves tap bytes
   and removes 96 divides/px. `g_accum` itself stays RGBA32F — accumulation precision is untouched (goldens safe).
2. **Pack the guide**: oct-encode the normal into 2×fp16 + view-Z fp16 → 8 B instead of 16 B. Normals here are
   axis-aligned or creature-organic; oct encoding is lossless enough for edge-stopping.
3. **Adaptive taps by convergence**: à-trous exists to hide low sample counts. Scale levels by `g_accum.a`:
   count ≥ 64 → 1 level (or skip, keeping tonemap-only resolve); count < 8 (fresh reset / creature) → all 4.
   Per-pixel, so the creature keeps full filtering — again dodging the reverted-E trap.
4. (Optional) Move the filter from a raygen dispatch to a **compute PSO with 16×16 LDS tiles** — dilation-1/2 taps
   come from groupshared instead of L2. Standard; do it last, measure first.

---

## 6. Integrator surgery — interactive-only, oracle-gated

### 6.1 GI ray TMax clamp (~10 LOC)
`trace()` uses `TMax = uFar = 500 m` for GI bounce rays ([dxr.cpp:740](../render_dxr/src/dxr.cpp), kSceneFar
[dxr.cpp:512](../render_dxr/src/dxr.cpp)). A cosine-weighted bounce that travels >24 m in the Backrooms
contributes ≈ nothing (the NEE falloff `1/(1+0.35·d²)` at the far end is already dust), but it *traverses the
whole TLAS diagonally* — worst exactly in open rooms and down shafts, the operator's heavy scenes. Clamp bounce
`TMax` to ~24–32 m behind a new `uFrame` bit; miss-within-clamp contributes 0 exactly as miss-at-500 does today.
Bias is a slight GI darkening in vistas; assert with the E31-style convergence oracle (err_excess vs noise floor)
plus a shaft look-A/B. Primary rays keep 500 m (vistas must render); light/flare shadow rays are already
distance-bounded.

### 6.2 Compact the light loops (~free ALU)
`direct_light`/`direct_light_stochastic` iterate the full 5×5 cell window and reject odd cells inside the loop
([dxr.cpp:627-630, 653-656](../render_dxr/src/dxr.cpp)) — 25 iterations for ≤9 candidates, and this runs up to 5×
per pixel per reset frame (primary + 4 bounces). Iterate the even lattice directly (`gi0 = (ci-2+1)&~1; gi += 2`)
→ 9 iterations. Same math, same order ⇒ can be made bit-identical (verify with the gate; if DXC reorders FP, fall
back to the interactive bit).

### 6.3 The centerpiece: an irradiance lattice on the 4 m grid (DDGI-lite)
The scene's whole personality is its regularity: cells are 4 m ([dxr.cpp:594](../render_dxr/src/dxr.cpp)), lights
sit on the even-even cell lattice ([dxr.cpp:630-631](../render_dxr/src/dxr.cpp)), materials are flat albedos, and
**indirect light in such a scene is a smooth, low-frequency, *static* field**. We currently re-derive it from
scratch with 1–4 stochastic bounce rays per pixel per frame, forever. The Carmack move is to compute it **once per
place, not once per pixel**:

- **Probe topology is trivial** (this is what the grid buys us — placement heuristics are DDGI's hard part and we
  don't have one): probes at cell centers, 2 heights (~0.8 m / ~2.2 m), per resident level. Streaming ring ≈ a few
  thousand probes; an SH-1 or ambient-cube RGB probe is 36–72 B → the whole lattice is a few hundred KB.
- **Progressive GPU bake, zero extra machinery**: each frame, update K probes (round-robin) with ~32 hemisphere
  rays each from a tiny side dispatch — ~2K rays/frame (0.005% of today's budget). Static scene ⇒ converges once,
  stays valid; new chunks bake as they stream in (same rhythm as BLAS builds). Flares/flashlight stay analytic
  per-pixel exactly as today (they're the *dynamic* lights; the lattice is the *static* GI).
- **Runtime**: diffuse GI = trilinear blend of 8 probes (weighted by normal) × albedo — replaces the GI bounce ray
  *and* its bounce-NEE shadow ray. Per-pixel PT drops to primary + 1 RIS shadow = **2 rays/px**.
- **Leak control** — the classic DDGI failure is light bleeding through the thin walls the Backrooms is made of.
  Full DDGI answers with depth-moment visibility textures per probe. We have a cheaper, scene-shaped answer: the
  wall layout is a 2-D map of axis-aligned segments (gen's collision boxes,
  [chunk_gen_v1.h:89](../contracts/chunk_gen_v1.h)) — do a **2-D DDA on a per-cell wall/lintel bitfield** for each
  of the ≤8 probe→point interpolants (≤3 steps each, pure ALU, no rays). This is the Wolfenstein raycast reborn —
  not to replace the RT cores (they'd win, see §10) but to zero-out probe weights through walls, which RT cores
  would charge rays for.
- **Honesty**: this is the one *visibly different* item (1-bounce stochastic GI → cached multi-bounce-capable GI;
  it can actually look *better* — kAmbient's flat floor, [dxr.cpp:599](../render_dxr/src/dxr.cpp), exists to paper
  over 1-bounce underestimation and could shrink). Interactive-only bit, settings-menu toggle, look-A/B with the
  operator, convergence-band oracle vs converged PT for the corridor case. Milestone-sized (the only item here
  that is); do it after items 1–6 prove insufficient, or because 4K/120 is wanted.

### 6.4 Rejected: emitter-hit-only at the bounce
Killing bounce-NEE (old item E) saves 1 ray/px but the fluorescent panels subtend small solid angles from most of
the floor → variance explodes in exactly the dim areas the flashlight exists for. The lattice (§6.3) obsoletes the
question. Keep bounce-NEE until then.

---

## 7. Fetch diet — your normals are enumerable

`trace()` fetches **3 × 48 B `Vertex` structs** per committed hit and lerps normals
([dxr.cpp:745-751](../render_dxr/src/dxr.cpp)) — for geometry whose normals are one of **six axis directions**
everywhere except the creature. The `cr,cg,cb,u,v` fields (20 B/vert) are never read by the PT at all.

Replace with a per-primitive side buffer: 1 byte normal-code (0–5 ⇒ ±X/±Y/±Z lookup) + 1 byte material → **2–4 B
per triangle instead of 144 B per hit**, no interpolation, no `normalize()`. Creature primitives (InstanceID ≥
`chunkVertCount`) keep the vertex path for organic shading. Incoherent GI-bounce hits are exactly where these
144-B random fetches miss cache; this also shrinks `shadeVb` pressure. Build the side buffer in `build_scene`
where the vertex ranges are already walked ([dxr.cpp:944-997](../render_dxr/src/dxr.cpp)). Bit-identical normals ⇒
gate-provable (the codes are exact axis vectors; today's lerp of three identical normals + normalize is the same
vector up to FP — verify, else interactive-bit it).

---

## 8. Hardware cheats (Ada-specific, scoped not recommended-yet)

- **SER**: the GI bounce + its shading is the divergent part. Inline `RayQuery` can't reorder; SER needs
  `TraceRay`/HitObject form — either DXR 1.2 (`MaybeReorderThread`, Agility SDK ≥ 1.716-preview vs our vendored
  1.619 release, ADR-081) or NVAPI intrinsics (new dependency ⇒ ADR, Iron Rule 8). Vendor-claimed 15–40% on
  incoherent workloads. Park until items 1–7 land; re-evaluate if the bounce survives §6.3.
- **fp16 shading math**: skip — the shader is memory/latency-bound around traversal, not ALU-bound; the fp16 that
  matters is the denoiser I/O (§5.1).
- **Opacity micromaps / intersection shaders**: n/a — everything is opaque triangles already.

---

## 9. The present path — spend 0.5 ms to *earn* a resolution tier

`present_pt_texture` is a plain bilinear fullscreen blit ([renderer.cpp:1870-1956](../render_d3d12/src/renderer.cpp)).
At 4K the kindest thing money can buy is making **Balanced (1/2) look like Quality (2/3)**:

- **Catmull-Rom 9-tap upscale + RCAS-style sharpen** in the blit PS (~60 LOC, hand-rolled, no SDK — consistent
  with the no-DLSS stance). Present-only ⇒ zero goldens exposure; A/B screenshot pair as the oracle.
- Pairs with §4: low-discrepancy AA jitter + accumulation is already a hand-rolled temporal AA; the sharpener is
  its display-side complement.
- (Cheeky, optional) **Peripheral spp falloff**: the eye reads the screen center; halving GI spp outside a center
  disc is ~free perceptually — *if* the RT path ever gains the raster path's VHS vignette to motivate it
  aesthetically. Note: RT currently has **no** VHS post at all (`finalize_to_readback` is raster-only,
  [renderer.cpp:1485](../render_d3d12/src/renderer.cpp)) — worth a separate look purely for art consistency.

---

## 10. Sidebar: the literal Wolfenstein option (and why not)

Shadow rays to ceiling lights are geometrically 2-D queries: walls are axis-aligned extrusions of a cell map with
door/lintel heights. A per-cell 4-edge height bitfield (256 B/chunk) + 2-D DDA (≤6 steps) answers "can P see light
L" in pure ALU — Wolfenstein/Doom sector logic, 1992 calling. **Verdict: don't** — on Ada the RT cores traverse
concurrently with ALU and `ACCEPT_FIRST_HIT` shadow rays are their best case; the DDA would trade dedicated-silicon
work for occupancy-competing ALU. It earns its keep only as the **probe-visibility test in §6.3** (where the
alternative is spending real rays) — that's the honest home for the trick. Revisit only for a hypothetical
non-RTX port.

---

## 11. What NOT to do

- **No DLSS/FSR/XeSS SDKs** — operator's explicit constraint; §9 is the hand-rolled substitute.
- **No VXGI** — settled in session 44 (leaks through thin walls; the modern successors are the §6.3 family).
- **No full SVGF yet** — variance estimation + history moments is a milestone; reprojection (§3) + adaptive à-trous
  (§5.3) captures most of its value at a fraction of the machinery. Re-scope only if creature-motion noise still
  bothers after §3+§4.
- **No goldens regen, no threshold relaxing** (Iron Rule 6). Every image-affecting item above is interactive-only
  behind `uFrame` bits with the offline path byte-identical — the E31/E32 pattern, which the gate already proves.
- **Don't chase the RIS loop with micro-ALU** beyond §6.2 — 25 candidate weights are ~free next to one ray.

---

## 12. Suggested attack order (each step independently shippable, audit-green, revertable)

| Step | Items | Gate story | Est. cumulative @ 4K-Quality, walking |
|---|---|---|---|
| 0 | GPU timestamps (`ID3D12QueryHeap`) + log split: AS / trace / denoise / copies / present | none (instrumentation) | baseline ~17 fps, now *measured* |
| 1 | §2.1 dead copies + §6.2 light-loop compaction | gate M9 bit-identical | ~19–20 fps |
| 2 | §2.2 one-submit + persistent buffers (+§2.3 incremental build_scene) | gate M9 + debug layer clean + 1 km TLAS walk | ~21–23 fps, hitchless crossings |
| 3 | §3 reprojection (the keystone) | still-A/B oracle + walk-bot A/B + operator eyes | **~35–45 fps** |
| 4 | §4 R2/blue-noise + §5 denoiser diet | E31-style convergence oracle + MAD-at-4spp assert | ~45–55 fps |
| 5 | §6.1 GI TMax clamp + §7 fetch diet + §9 upscaler | convergence oracle / bit-compare / screenshot A/B | ~50–65 fps (Balanced+sharpen ≈ Quality look ⇒ effectively more) |
| 6 | §6.3 irradiance lattice (milestone) | new convergence-band oracle + look A/B + settings toggle | **~70–100+ fps** |

All estimates ±40% until step 0 exists. Rollback discipline per house rules: tag before each step
(`pre-rt4k-s<N>`), `_staged_*` backup for anything structural, ledger entries E34+.

---

## Sources / grounding

- This repo: `render_dxr/src/dxr.cpp` (PT shader + host, read in full), `app/src/main.cpp` run_game RT path,
  `render_d3d12/src/renderer.cpp` present path, `docs/RT_PERF_PLAN.md`, `docs/SESSION_LOG.md` §40/43/44.
- R2 / plastic-constant sequence: M. Roberts, "The Unreasonable Effectiveness of Quasirandom Sequences" (2018).
- DDGI: Majercik et al., "Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields" (JCGT 2019).
- ReSTIR: Bitterli et al. 2020 (already cited in RT_PERF_PLAN.md).
- SER: NVIDIA Ada whitepaper + DXR 1.2 announcement (Agility SDK preview ≥ 1.716).
- Blue noise: Ulichney, void-and-cluster (1993); Cranley-Patterson rotation for per-pixel decorrelation.
