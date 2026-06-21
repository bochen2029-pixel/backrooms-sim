# STAGED CHANGESET — RT "ghosting" fix: the creature stops smearing (material-7 history reject)

> **STATUS: STAGED, NOT APPLIED.** Nothing compiled/run/committed. Apply by **anchor-matching against the
> CURRENT code** (Edit tool / fuzzy patch), NOT by file-swap — another instance is editing in parallel (it's on
> `shoggoth.*` / `main.cpp`, NOT `render_dxr/`, so these `dxr.cpp` shader anchors are low drift risk). See
> `_staged_rt_perf_A/CHANGESET.md` for the apply protocol; this one is **independent of A** and can go in any
> order (it touches only the PT *shader*, not the present path or the device).

## Goal
Kill the "ghostly fuzzy quantum-superposition" smear on the Shoggoth in RT mode. **Cause:** the Tier-1 temporal
accumulator (shipped E12) blends each pixel's history across frames; for the *moving* creature, last frame's
position lingers in the accumulator → a smear. **Fix (GLM "1b"):** give the accumulator a **per-pixel sample
count** (in the unused `g_accum.a`), and make the dynamic creature (**material 7**) **reject its history every
frame** — its pixels show only the current sample (un-ghosted), while the static background keeps accumulating
clean. ~4 small shader edits, all in the `kPtShader` string in `render_dxr/src/dxr.cpp`.

## Why it's golden-safe (M9 stays bit-identical — verify, but it's true by construction)
The offline/golden path (`render_pt` → `render_pt_frame(reset=true, denoise=false)`) renders scenes with **no
material 7**, and `uSampleStart==0` already resets the pixel. So the new per-pixel count `pixCount` equals the old
global `uTotal` for every pixel (`64×N` batches sum to `uTotal` exactly in float), and `total/pixCount ==
total/uTotal` → **bit-identical**. The denoiser edits (G3) only run on the interactive path (`denoise=true`),
never for goldens. **Determinism untouched.**

---

## HUNKS (all in `render_dxr/src/dxr.cpp`, inside the `kPtShader` HLSL string)

### G1 — accumulate: per-pixel count + material-7 history reject
```
FIND:
    float3 local = deterministic * float(uSampleCount) + indirectSum;
    float3 total = (uSampleStart == 0u) ? local : (g_accum[px].rgb + local);
    g_accum[px] = float4(total, 1.0);
REPLACE:
    float3 local = deterministic * float(uSampleCount) + indirectSum;
    // Per-pixel accumulated sample count lives in g_accum.a (previously an unused 1.0). The DYNAMIC creature
    // (material 7) rejects its history every frame -> moving creature pixels show only the current sample (no
    // ghost smear), while the static background keeps accumulating. Golden-safe: golden scenes have no material
    // 7 and uSampleStart==0 already resets, so pixCount == uTotal there -> total/pixCount is bit-identical.
    bool  resetPix  = (uSampleStart == 0u) || (h.hit && h.mat > 6.5 && h.mat < 7.5);
    float prevCount = resetPix ? 0.0 : g_accum[px].a;
    float3 prevSum  = resetPix ? float3(0.0, 0.0, 0.0) : g_accum[px].rgb;
    float3 total    = prevSum + local;
    float pixCount  = prevCount + float(uSampleCount);
    g_accum[px] = float4(total, pixCount);
```
**INTENT / DRIFT:** if the other instance changed the material id of the creature, match it (today main.cpp sets
`v.material = 7.0f` for the creature → the shader test `h.mat in (6.5,7.5)`). If `g_accum` packing changed, keep
the principle: store the per-pixel count in a free channel and reset it for creature pixels.

### G2 — final-batch tonemap (denoise OFF, the golden path): divide by the per-pixel count
```
FIND:
            float3 c = total / float(max(uTotal, 1u));
REPLACE:
            float3 c = total / max(pixCount, 1.0);   // per-pixel count (== uTotal in the golden path -> bit-identical)
```

### G3a — denoiser resolve: center pixel divides by its own count
```
FIND:
    float invT = 1.0 / float(max(uTotal, 1u));
    float3 cC = g_accum[px].rgb * invT;
REPLACE:
    float4 aC = g_accum[px];
    float3 cC = aC.rgb / max(aC.a, 1.0);   // per-pixel count: creature pixels = this frame only -> no ghost
```
**DRIFT:** this is inside `denoise_resolve(...)`. After this edit, the local `invT` no longer exists — G3b removes
its other use. If `denoise_resolve` gained other `invT` uses, replace each `g_accum[...].rgb * invT` with
`g_accum[...].rgb / max(g_accum[...].a, 1.0)` (read the float4 once per sample as below).

### G3b — denoiser resolve: each neighbour divides by ITS own count
```
FIND:
            float3 cq = g_accum[uint2(q)].rgb * invT;
REPLACE:
            float4 aq = g_accum[uint2(q)]; float3 cq = aq.rgb / max(aq.a, 1.0);
```

---

## VALIDATION CHECKLIST (untested — validate on apply)
1. Build clean (`/WX`).
2. **`gate.ps1 -Milestone M9` — PT goldens MUST stay bit-identical** (this is the determinism proof; if they
   diverge, the per-pixel count != uTotal somewhere — recheck G1/G2).
3. **`audit.ps1`** green (ctest + record==replay + inventory + isolation).
4. Live `--game --rt` near the creature while standing still: the creature should be **crisp, no smear** as it
   writhes; the background stays clean. While moving, it was never ghosted (camera motion already reset) — still fine.
5. (If available) compare an A/B: before = creature smears when you hold still; after = sharp.

## RISK / NOTES
- **Pure shader change**, interactive-only effect, golden-bit-identical, no device/present/host change → very low
  blast radius. Independent of items A and B.
- The creature now shows at the **current frame's spp** (noisier than the accumulated background while you hold
  still). That's the correct trade (sharp-but-slightly-noisier beats smeared); the denoiser still cleans it. The
  *full* fix (creature stays low-noise AND un-ghosted) is **SVGF** with per-object motion vectors — a later milestone.
- **Rollback:** `git revert <commit>` (or `git reset --hard pre-rtperf-ghost` if tagged before applying).
