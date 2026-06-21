# STAGED CHANGESET — RT perf item B: fewer per-frame GPU stalls (fold denoise + AS fast-build)

> **STATUS: STAGED, NOT APPLIED.** Nothing compiled/run/committed. Apply by **anchor-matching against the CURRENT
> code** (Edit tool / fuzzy patch), NOT file-swap. All hunks are in `render_dxr/src/dxr.cpp` (which the other
> instance is NOT editing — low drift risk). See `_staged_rt_perf_A/CHANGESET.md` for the apply protocol.

## Goal & scope
The in-game RT frame is fully serial (`RT_PERF_PLAN.md` §3). This is the **high-confidence, low-risk** slice of
item B:
- **B1 — fold the denoise dispatch into the accumulate command list.** Today the interactive frame does TWO
  `ExecuteCommandLists` + `wait_idle`s (the accumulate batch, then a separate denoise pass). Fold them → **ONE**.
- **B2 — per-frame acceleration-structure rebuilds use `PREFER_FAST_BUILD`** (not `PREFER_FAST_TRACE`). The creature
  BLAS + the TLAS are rebuilt **every frame** in `update_creature`; for per-frame-rebuilt geometry, fast-BUILD is
  the right flag (NVIDIA RT best practices). *(The STATIC chunk BLASes in `build_scene` stay `PREFER_FAST_TRACE` —
  built once, traced many times. DO NOT change those.)*

**Compatibility with item A:** B1 still does a final `wait_idle` (one instead of two) — `render_pt_frame` still
fully syncs on return, so A's fence-free `present_pt_texture` remains correct. This changeset does **NOT** touch
the per-frame readback path or remove the final sync. (The bigger pipelining levers — removing `update_creature`'s
waits, and N-frames-in-flight — are DEFERRED below; they DO interact with A and need a cross-queue fence.)

**Determinism:** the offline/golden `render_pt` path uses `denoise=false`, so B1 (the denoise fold) never runs for
goldens. B2 only changes a build-perf flag, not output. **M9 goldens stay bit-identical.**

---

## HUNKS (all in `render_dxr/src/dxr.cpp`)

### B1a — `render_pt_frame`: fold the denoise into the resolve batch's command list
**INTENT:** in the final (resolve) accumulate batch, if `denoise`, after the depth copy: UAV-barrier `g_accum` +
`g_guide` (so the accumulate writes are visible to the denoise read), re-set the root constants to the denoise
config (`uResolve=3`), `DispatchRays` once more, then `copy_color()` — all in the SAME list. Root sig / PSO /
heaps / SRVs are still bound from `bind_pt()` earlier in this iteration; only the 28 root constants change.
```
FIND:
            const D3D12_RESOURCE_BARRIER dBack = transition(d.depthTex.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            d.list->ResourceBarrier(1, &dBack);
            if (!denoise) copy_color();   // denoise off: color is final here
        }
REPLACE:
            const D3D12_RESOURCE_BARRIER dBack = transition(d.depthTex.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            d.list->ResourceBarrier(1, &dBack);
            if (denoise) {
                // RT_PERF item B1: fold the denoise dispatch into THIS list -> one ExecuteCommandLists + wait_idle
                // for the frame instead of two. The accumulate pass (uResolve==2) wrote g_accum + g_guide; a UAV
                // barrier makes those writes visible to the denoise pass (uResolve==3) which reads them and writes
                // the resolved color. Root sig/PSO/heaps/SRVs stay bound from bind_pt() above; only constants change.
                D3D12_RESOURCE_BARRIER uavb[2]{};
                uavb[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; uavb[0].UAV.pResource = d.accumTex.Get();
                uavb[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; uavb[1].UAV.pResource = d.guideTex.Get();
                d.list->ResourceBarrier(2, uavb);
                uint32_t cd[28] = {};
                cd[18] = total; cd[20] = 3u; setf(cd, 21, kPtExposure);
                setf(cd, 22, static_cast<float>(d.width)); setf(cd, 23, static_cast<float>(d.height));
                cd[24] = frame;
                d.list->SetComputeRoot32BitConstants(0, 28, cd, 0);
                d.list->DispatchRays(&dr);
            }
            copy_color();   // denoise off: the uResolve==1 tonemap is final; denoise on: the fold above made it final
        }
```
**VALIDATION POINT:** the UAV barrier on `accumTex` + `guideTex` is what makes this correct (without it the
denoise pass could read stale g_accum). `copy_color()`'s own `UAV->COPY_SOURCE` transition synchronizes the
denoise write to `d.uav`. Confirm `d.accumTex` / `d.guideTex` are the Impl resources behind `g_accum`(u2) /
`g_guide`(u3) — they are (dxr.cpp ~L326 / ~L337). `setf` + `dr` are in scope in `render_pt_frame`.

### B1b — `render_pt_frame`: delete the now-redundant separate denoise pass
**INTENT:** the folded dispatch above replaces this whole block.
```
FIND:
    if (denoise) {
        // Edge-aware filter: read the accumulated radiance + the geometry guide, write the resolved color.
        // The accumulate list already waited, so those writes are complete (no cross-list UAV barrier needed).
        uint32_t c[28] = {};
        c[18] = total; c[20] = 3u; setf(c, 21, kPtExposure);
        setf(c, 22, static_cast<float>(d.width)); setf(c, 23, static_cast<float>(d.height));
        c[24] = frame;
        if (FAILED(d.alloc->Reset())) { last_error_ = "pt alloc reset (denoise)"; return false; }
        if (FAILED(d.list->Reset(d.alloc.Get(), nullptr))) { last_error_ = "pt list reset (denoise)"; return false; }
        bind_pt(c);
        d.list->DispatchRays(&dr);
        copy_color();
        if (FAILED(d.list->Close())) { last_error_ = "pt denoise list close"; return false; }
        ID3D12CommandList* lists[] = { d.list.Get() };
        d.queue->ExecuteCommandLists(1, lists);
        d.wait_idle();
    }

    d.accumSamples = total;
REPLACE:
    d.accumSamples = total;
```

### B2a — `update_creature`: the per-frame CREATURE BLAS uses fast-BUILD
**INTENT:** this is the per-frame creature bottom-level rebuild (anchor includes `pGeometryDescs = &geo`, which is
the creature BLAS — NOT the chunk BLASes in `build_scene`).
```
FIND:
    bin.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bin.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    bin.NumDescs = 1; bin.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY; bin.pGeometryDescs = &geo;
REPLACE:
    bin.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bin.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;   // RT_PERF item B2: rebuilt every frame -> fast BUILD
    bin.NumDescs = 1; bin.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY; bin.pGeometryDescs = &geo;
```

### B2b — `update_creature`: the per-frame TLAS uses fast-BUILD
**INTENT:** the per-frame top-level rebuild (anchor includes `tin.NumDescs = static_cast<UINT>(instances.size())`,
unique to `update_creature`).
```
FIND:
    tin.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tin.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    tin.NumDescs = static_cast<UINT>(instances.size());
REPLACE:
    tin.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tin.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;   // RT_PERF item B2: rebuilt every frame -> fast BUILD
    tin.NumDescs = static_cast<UINT>(instances.size());
```
**DO NOT** change the `PREFER_FAST_TRACE` flags in `build_scene` (the static per-chunk BLASes + the initial TLAS,
dxr.cpp ~L893 / ~L962) — those are built once and traced many times; fast-trace is correct for them.

---

## VALIDATION CHECKLIST (untested — validate on apply)
1. Build clean (`/WX`).
2. **`gate.ps1 -Milestone M9`** — PT goldens bit-identical (B1 never runs for goldens since they use
   `denoise=false`; B2 is a build flag only). The denoiser sub-gate ("interactive PT denoiser cuts noise toward
   ground truth") must still pass — i.e. the folded denoise produces the SAME result as the separate pass (it
   should: same dispatch, same inputs; the UAV barrier replaces the old inter-list wait).
3. **`gate.ps1 -Milestone M30`** — live RT smoke: `rt_frames >= 1`, `debug_error_count: 0` (watch for a missing
   UAV barrier → the debug layer flags a read-before-write hazard on g_accum/g_guide).
4. Live `--game --rt`: measurably **higher FPS** (one fewer GPU drain per frame + cheaper AS builds), image
   unchanged.
5. **`audit.ps1`** green.

## DEFERRED — the bigger pipelining levers (separate changesets; they DO interact with A)
- **B3 — remove `update_creature`'s `wait_idle`s** (it waits twice: before touching the shared `shadeVb`/creature
  buffers, and after the build). Removing them needs a fence so the next render doesn't race the build; and the
  shared-buffer write needs the previous frame's trace done. Real win, but fence-careful.
- **B4 — N frames-in-flight** (a ring of allocator/list/fence; record frame N+1 while N is in flight). The biggest
  pipelining win, but: (a) it removes the final `wait_idle` that **item A's present relies on** → A's
  `present_pt_texture` would then need a **cross-queue fence** (raster queue waits on the DXR fence) before
  sampling the PT texture; (b) the readback ring. Do A + B1 + B2 first, measure, then take B4 with the fence.
- **Ray cost (item D — stochastic light sampling):** the dominant *ray* cost in open rooms (the 5×5 NEE grid + flares
  + flashlight, evaluated at the primary hit AND the GI bounce). Pick 1-2 lights by power/solid-angle per sample.
  **NOT golden-safe** (changes the lighting integral) → needs golden regeneration via `goldgen` + an ADR. Bigger lift.

## ROLLBACK
`git revert <commit>` (or `git reset --hard pre-rtperfB` if tagged before applying). B1/B2 are isolated to
`dxr.cpp`; the golden path is untouched.
