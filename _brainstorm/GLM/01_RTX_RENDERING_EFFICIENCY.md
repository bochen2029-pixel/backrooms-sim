# 01 — RTX Ray-Tracing: Noisy & Slow (Efficiency Investigation)

> Thread: first pivot of the session. Read-only investigation — no edits made.
> Repo state: HEAD = 90410c3 (ADR-076).
>
> Note: this thread's "it's a TDR-degraded GPU" hypothesis was later **ruled out** by the
> operator's hard-reboot report (see 02). The *efficiency* diagnosis here (temporal
> accumulation disabled, sync GPU stalls, CPU readback, per-frame AS rebuild) is still
> valid and independent of the crash. The crash is a separate bug — see 02.

## The complaint

The RTX/ray-traced mode is very noisy and slow. Raster mode is fine.

## Diagnosis: why it's noisy and slow

The two complaints have a **single shared root cause**, plus three structural cost multipliers.

### Root cause — temporal accumulation is disabled in the interactive path

`app/src/main.cpp:1024` and `:1779` call:
```cpp
dxr->render_pt_frame(cam, 4u, ..., reset=true, denoise=true, frame);
```
`reset=true` **every frame** wipes the RGBA32F accumulator, so each frame is **4 spp from
scratch**. The comment says *"creature animates → fresh frame."* That decision throws away the
one thing that makes real-time path tracing viable: progressive accumulation across frames.
The plumbing for `reset=false` (continue accumulating) already exists and is even exercised by
the `--dxr-ghost` test — it's just never used interactively. So the image is noisy *because*
it's 4-spp-or-nothing, and it's slow *because* shooting enough spp to look clean in one frame
is expensive.

ADR-068 (`docs/DECISIONS.md:384`) already names this: *"the interactive path passed
`reset=true` every frame (so the accumulator was wiped to 4-spp-from-scratch — temporal
integration plumbed but disabled)."* The denoiser commit (`2c5642c`) patched the symptom
(frozen grain + spatial blur) but **left `reset=true` in place**. The "future levers now
unlocked" line in ADR-068 explicitly lists *"full SVGF temporal reprojection"* as deferred.

### Cost multiplier 1 — fully synchronous GPU, 3 round-trips/frame

Every frame does **three separate `ExecuteCommandLists` + `wait_idle()`** cycles with CPU stalls:
1. `render_pt_frame` accumulate dispatch → `wait_idle` (`dxr.cpp:1242`)
2. `render_pt_frame` denoise dispatch → `wait_idle` (`dxr.cpp:1260`)
3. `present_overlay_windowed` overlay draw + `Present` → fence wait (`renderer.cpp:1844`)

`wait_idle` does `WaitForSingleObject` (`dxr.cpp:214`) — the CPU is idle while the GPU works
and vice versa. No frame pipelining, no overlap.

### Cost multiplier 2 — CPU round-trip every frame (by design, but un-pipelined)

DXR owns its own `ID3D12Device5` (no cross-device sharing — ADR-019), so each frame: GPU writes
UAV → blocking `readback()` (`Map` with a read range, `dxr.cpp:440`) → CPU
`std::vector<uint8_t>` → re-upload as the raster overlay texture → draw. The readback `Map`
with a non-null range **stalls until the copy completes**. No readback ring, so the CPU can't
get ahead of the GPU.

### Cost multiplier 3 — ray budget is high at 4 spp

The PT shader (`dxr.cpp:602-731`) per pixel does: 1 primary `RayQuery` + a 5×5 NEE grid of
shadow rays (~6–9 surviving lights), then per sample: 1 GI bounce trace + another full
`direct_light` (6–9 more shadow rays) at the GI hit. At 4 spp that's **~40–50 rays/pixel** —
then thrown away next frame.

### Minor — per-frame AS rebuild for the creature

`update_creature` (`dxr.cpp:1010`) rebuilds the creature BLAS + the **entire TLAS** every
frame with `PREFER_FAST_TRACE` (`:1048`, `:1082`). For a ~1248-vert mesh that updates per
frame, `PREFER_FAST_BUILD` is the right flag (tiny mesh; fast-trace buys nothing and costs
build time).

---

## Proposed plan (prioritized tiers)

### Tier 1 — Re-enable temporal accumulation (the fix that was always intended)

Goal: converge to a clean image at ~1 spp/frame instead of 4 spp from scratch, cut rays/frame ~4×.

1. **`main.cpp:1024` / `:1779`** — pass `reset=false` except when the camera moved or the chunk
   center changed. Track previous camera pose (pos+yaw+pitch within epsilon) and previous
   `dxrCenter`; `reset=true` only on change.
2. **Drop `samples` 4 → 1** once accumulating.
3. **Creature handling** — three options:
   - 1a (cheapest): accept creature pixels ghost slightly while still.
   - 1b (medium): mark creature pixels (material 7) in guide alpha, reset only those pixels.
   - 1c (best): per-object motion vectors → temporal reprojection (full SVGF). See Tier 3.
4. **`update_creature` AS flags** — switch creature BLAS + TLAS rebuild to `PREFER_FAST_BUILD`.

Determinism: safe. `render_pt` (the golden path) is separate, keeps `reset=true, denoise=false,
uFrame=0`. Tier 1 only changes the interactive call sites.

### Tier 2 — De-sync the frame pipeline (remove the CPU stalls)

1. **N frames-in-flight** in `DxrRenderer` — a ring of (allocator, list, fence value) instead
   of one. Record frame N+1 while frame N's readback is in flight. Cap at 2.
2. **Readback ring** — ring of readback buffers + fence-checked `Map`. Removes the blocking
   stall in `readback()` (`dxr.cpp:440`).
3. **Fold the denoise dispatch into the same command list** as accumulate. One
   `ExecuteCommandLists` instead of two.
4. `present_overlay_windowed` stays on the raster device; no longer waits on the DXR GPU.

Determinism: safe — only timing changes. Goldens use `render_pt` which can stay single-flush.

### Tier 3 — Temporal denoiser (SVGF), replacing the spatial-only pass

1. **History buffers** — prev-frame radiance, depth, normal, view matrix.
2. **Motion vectors** — camera reproject (prev/cur view matrices); per-object for the creature.
3. **Reproject + blend** — `prevRadiance(reproject(px)) * α + newSample * (1-α)`, disocclusion
   fallback to the noisy new sample.
4. **Variance-guided a-trous** — drive the spatial filter's edge weights by per-pixel variance.
5. Deprecate `reset=true` for camera motion — reprojection handles it.

Determinism: safe — interactive-only. Keep `uResolve==1` (no-denoise) bit-identical for goldens.

### Tier 4 — Lower-cost lighting (optional, after Tier 1)

1. **Stochastic light selection** — pick 1–2 lights by (solid-angle · power) per sample instead
   of looping all ~6–9. Unbiased; temporal accumulation absorbs the variance.
2. **Russian-roulette the GI bounce** at low spp with compensation.

---

## Recommended sequence

**Tier 1 first.** Smallest diff (~30–50 LOC), the fix the original design intended, directly
attacks both "noisy" and "slow" by doing 1/4 the rays and letting them accumulate. Then measure.
If still slow → Tier 2 (pipelining, pure throughput). If still noisy in motion → Tier 3 (SVGF).
Tier 4 only if lighting is then the bottleneck.

## Constraints respected

- No new dependencies / no asset files (Iron Rule 8; ADR-068) — Tiers 1–4 are from-scratch
  shader/C++ work. ✓
- Determinism sacred (Iron Rule 4) — all changes gated to the interactive `render_pt_frame`
  path; `render_pt` / goldens untouched. ✓
- Diff budget ≤ 400 LOC (Iron Rule 5) — Tier 1 well under; Tier 2 ~150–200; Tier 3 is the big
  one, warrants its own milestone. Each tier independently shippable.
- Raster stays default + fallback (INV-6) — untouched.

## Key file:line references

- Interactive RT call sites: `app/src/main.cpp:1024`, `:1779`
- `wait_idle` (the stall): `render_dxr/src/dxr.cpp:209-216`, called at `:431,900,1016,1102,1160,1242,1260`
- Blocking readback: `render_dxr/src/dxr.cpp:440`
- Per-frame AS rebuild: `render_dxr/src/dxr.cpp:1010` (`update_creature`)
- PT shader ray budget: `render_dxr/src/dxr.cpp:602-731`
- ADR-068 (the named root cause): `docs/DECISIONS.md:384`
- Denoiser commit (patched the symptom): `2c5642c`
