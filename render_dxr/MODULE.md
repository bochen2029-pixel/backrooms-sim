# MODULE: render_dxr (L2)

**Purpose.** DXR 1.1 path-traced mode: BLAS per chunk, TLAS refit on stream
events, temporal accumulation, emissive fluorescents as true light sources.
Enhancement only — never a dependency (INV-6).

**Depends on:** `render_d3d12` (same adapter-selection + the shared `ResidentChunk`
geometry contract — **not** literally the same device object; `render_dxr` is
self-contained, ADR-035), `stream`.

**Public surface.**
- `render_dxr/dxr.h` (M9) — `probe_caps()` → `DxrCaps` (Device5, raytracing tier,
  DXC availability + a trial DXR-library compile). Exposed via `app --dxr-probe`.
- `render_dxr/dxr.h` — `DxrRenderer`: `init(w,h)`, `build_scene(chunks)` (BLAS per
  resident chunk + TLAS), `render_scene(camera)` (primary `TraceRay`), `readback`
  (RGBA) + `readback_depth` (per-pixel NDC depth, same hyperbolic mapping as
  render_d3d12's depth buffer — the cross-renderer depth gate). Exposed via
  `app --dxr` / `--dxr-test` / `--dxr-depth`.
- `render_dxr/dxr.h` — interactive presentation hooks, each default-OFF so the offline/golden path stays
  bit-identical (the shader call sites are `[branch]`-guarded): `set_flashlight` (eye-torch), `set_flares`
  (green chemlight analytic point lights), `set_dread` (Apparition Phase 2b.2: a soft **post-accumulation** dim
  of the PT output while a verdict lingers — applied at the tonemap, so it never pollutes the radiance accumulator
  and needs no reset; `1.0`=off → `uDread` branch skipped → goldens exact).
- `render_pt_frame(..., want_readback)` (default **ON**, E35) — the resolve's color+depth staging copies into the
  CPU-readback buffers are per-frame skippable: the interactive game passes false except on the sparse POV-grab
  frames (Director vision / voice chat / `--out`); at a 4K-Quality internal res the copies are ~30 MB/frame of dead
  PCIe traffic otherwise. Offline/golden/gate callers keep the default → `readback()`-based oracles untouched.
- `render_dxr/render_dxr.h` — identity stub (module banner).
- `dxc.*` (internal) — runtime DXC wrapper: loads `dxcompiler.dll`/`dxil.dll`,
  compiles HLSL → signed SM 6.3 DXIL.

**Planned.** Path-traced lighting (emissive fluorescents) + temporal accumulation
+ TLAS refit on stream events (M9 phases 3–4); raster stays default + fallback
(INV-6).

**Status:** M9 (phase 2b) — DXR toolchain + DispatchRays proven; BLAS/TLAS built
from streamed chunks; primary rays trace the maze. Cross-renderer depth compare
(raster vs DXR primary hits, NDC depth within epsilon) is **green** at 5 poses
(mean depth rel-err ~1e-5, debug/DRED clean). PT lighting + converged golden next.
