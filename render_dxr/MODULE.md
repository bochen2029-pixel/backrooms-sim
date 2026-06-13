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
- `render_dxr/render_dxr.h` — identity stub (module banner).
- `dxc.*` (internal) — runtime DXC wrapper: loads `dxcompiler.dll`/`dxil.dll`,
  compiles HLSL → signed SM 6.3 DXIL.

**Planned.** BLAS per chunk + TLAS + path tracer + temporal accumulation (M9
phases 2–4); raster stays default + fallback (INV-6).

**Status:** M9 (phase 1) — DXR toolchain proven on the dev box (RTX 4070 Ti SUPER,
Device5, RaytracingTier 1.1, DXC → signed DXIL). AS/PSO/SBT next.
