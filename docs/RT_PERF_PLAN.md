# RT Performance Plan — why the in-game path tracer is slow, and the ranked fixes

> **STATUS 2026-06-18 (Session 40): the ghosting fix + items A + B + C are SHIPPED + gate-M9-green + pushed.** Done:
> the **ghosting** (material-7 history reject, E22 `0c8e0b3`), **A** the cross-device readback eliminated (single-device
> `present_pt_texture`, E23 `0f3da13`), **B** the frame pipelined (denoise folded into the accumulate list + per-frame
> AS → `PREFER_FAST_BUILD`, E24 `e072e8a`, tag `rtperf-green`), **C** the creature BLAS refit in place
> (`ALLOW_UPDATE`+`PERFORM_UPDATE`, the mesh is writhe-stable, E25 `df97807`). Interactive PT ~173 FPS @ 1440p (gate),
> live `--game --rt` ~116–123 fps debug-clean, ghosting gone.
>
> **DECISION: stop here. D / E / SVGF are deferred as measured-optional.** Step 0 diagnosed the slowness as ~80%
> structural stalls (fixed by A+B+C) / ~20% ray cost. The remaining items are all **ray-cost cuts that change the
> converged lighting output** → each needs a goldens regen (against "goldens sacred") OR interactive-only two-path
> gating + an unbiasedness convergence oracle + a live look-A/B — integrator surgery with real regression risk on the
> operator's gameplay view, for an already-playable scene. **E's skip-denoise-when-converged was prototyped and reverted**
> (keys off camera convergence, but the ghost fix makes the creature always 1-spp → a writhing creature in a still view
> would render noisy). **D** needs the shader restructured (`direct_light` is in the *deterministic* term, not
> per-sample). Next lever IF open rooms are still heavy at the operator's real settings: **interactive-only stochastic
> direct lighting (RIS)** with a high-spp convergence test as its oracle. Rollback anchor: tag `pre-rtperf` `0644ef8`.

> Logged 2026-06-18 at the operator's request ("ray tracing is unplayable, too slow — log the plan, focus on
> raster for now"). A diagnosis + ranked optimization plan, grounded in a code scan (`render_dxr/src/dxr.cpp`,
> the `run_game` RT path) + current literature (ReSTIR, NVIDIA RT best practices, SVGF). Builds on GLM doc 01
> (`_brainstorm/GLM/01_RTX_RENDERING_EFFICIENCY.md`); Tier 1 (temporal accumulation) already shipped (E12).

## Key insight: the renderer isn't the bottleneck — the frame *around* it is
The M9 gate measures the PT render+resolve at **178 FPS / 5.6 ms** @ 1440p, 1 spp. If in-game feels like 30-40 FPS,
**~20 ms/frame is everything *except* the rays.** The bottleneck is almost certainly **structural stalls**, not ray
cost. (Profile to confirm — the open-room scene is also ray-heavier than the gate's corridor.)

Three structural stalls, all in `dxr.cpp` / the `run_game` RT path:
1. **A full GPU→CPU→GPU round-trip every frame.** DXR device renders → `readback()` blocking `Map` of a ~5 MB
   buffer → CPU `std::vector` → the **raster** device re-uploads it as a texture → presents. Two separate D3D12
   devices that can only talk through the CPU → total serialization.
2. **~4-5 `wait_idle()` stalls/frame, no frames-in-flight** (`update_creature` waits ×2, accumulate waits, denoise
   waits, present fences). CPU and GPU never overlap.
3. **Full creature-BLAS + full-TLAS rebuild every frame with `PREFER_FAST_TRACE`** (dxr.cpp:1131/1165) — the
   worst-case flag for per-frame-rebuilt geometry.

The **creature "ghosting"** is the GLM "1a" tradeoff from Tier 1: the accumulator blends the *moving* creature's
stale pixels against the static background → the "quantum-superposition" smear. Known + fixable.

## Step 0 — profile first
Instrument render / readback / present with timestamps, or PIX/Nsight one frame in the open room. ~80% confident
it's the stalls; fix the real bottleneck, not a guess.

## The ghosting (targeted, cheap)
The creature is **material 7**. In accumulate/resolve, detect material-7 primary hits and **reject history on those
pixels** (use the fresh sample, don't blend) — kills the creature smear without touching background accumulation.
~30 LOC, low risk. *Proper fix = SVGF (per-object motion vectors + neighborhood-clamp) — a milestone.*

## The slowness, ranked (impact × confidence ÷ effort)
| # | Fix | Why | Effort | Conf |
|---|---|---|---|---|
| **A** | **Kill the cross-device readback** — render DXR into a **shared D3D12 resource** the raster device samples directly (`CreateSharedHandle`), or make the DxrRenderer **reuse the raster `Device5`** (one device does raster+DXR) so the PT output is a UAV sampled in place. No CPU round-trip. | Removes the per-frame GPU→CPU→GPU stall (likely #1 in-game cost) | Med-High | High |
| **B** | **Pipeline the frame (GLM Tier 2)** — fold denoise into the accumulate command list (one `ExecuteCommandLists`); drop redundant `update_creature` waits; 2 frames-in-flight. | Removes 3-4 of ~5 `wait_idle` stalls | Medium | High |
| **C** | **AS refit, not rebuild** (NVIDIA-confirmed) — creature BLAS → `ALLOW_UPDATE` + **refit** (the writhe is "bending not breaking" = the ideal refit case); `PREFER_FAST_BUILD` where you still rebuild; refit the TLAS when only the creature moved. | Cuts per-frame build cost a lot | Low | High |
| **D** | **Stochastic light sampling (ReSTIR-lite)** — today direct lighting loops a 5×5 NEE grid + every flare + the flashlight at **both** the primary hit *and* the GI bounce (~12-18 shadow rays/sample). Pick **1-2 lights** by power/solid-angle per sample; temporal accumulation absorbs the variance. | ~6-10× fewer shadow rays — the dominant *ray* cost in open rooms | Medium | Med-High |
| **E** | **No full NEE at the GI bounce** (single light / emitter-hit only) + **skip the denoiser when converged** (`accum_samples` high). | Halves shadow rays + frees the à-trous pass on static views | Low | Med |

**Future / milestone:** SVGF (proper temporal denoiser — fixes ghosting *and* clean 1-spp during motion); full
ReSTIR DI (the right answer if the flare/light count grows — 6-60× equal-error faster for many lights).

## Already optimal (don't re-touch)
Inline RayQuery; `ACCEPT_FIRST_HIT_AND_END_SEARCH` on shadow rays (dxr.cpp:607); 2/3 internal res; minimal payload;
Tier-1 temporal accumulation. The *per-ray* path is fine — the problem is *how many* rays + the *stalls around* it.

## Recommended first increment (~½ day, all M9-gate-safe / interactive-only)
profile (0) → ghosting fix (material-7 history reject) → AS refit/FAST_BUILD (C) → GI-NEE cut + skip-denoise-when-
converged (E). Then measure and decide on the structural rewrites (A cross-device + B pipelining) — that's where the
real win is.

## Sources
- ReSTIR (Bitterli 2020): https://benedikt-bitterli.me/restir/ · https://en.wikipedia.org/wiki/Spatiotemporal_reservoir_resampling
- NVIDIA Ray Tracing Best Practices: https://developer.nvidia.com/blog/rtx-best-practices/
- NVIDIA AS refit vs rebuild: https://developer.nvidia.com/blog/practical-real-time-ray-tracing-rtx/
- SVGF (KIT 2017): https://cg.ivd.kit.edu/publications/2017/svgf/svgf_preprint.pdf
