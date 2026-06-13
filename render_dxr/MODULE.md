# MODULE: render_dxr (L2)

**Purpose.** DXR 1.1 path-traced mode: BLAS per chunk, TLAS refit on stream
events, temporal accumulation, emissive fluorescents as true light sources.
Enhancement only — never a dependency (INV-6).

**Depends on:** `render_d3d12` (device/share), `stream`.

**Public surface (M0).** `render_dxr/render_dxr.h` — identity stub.

**Planned.** BLAS/TLAS + path tracer + accumulation (M9).

**Status:** M0 stub.
