# MODULE: render_d3d12 (L2)

**Purpose.** Raster renderer: D3D12 device/swapchain, procedural textures,
fluorescent forward/clustered lighting, VHS post stack, HUD. Default + fallback
renderer (INV-6).

**Depends on:** `core` (read-only WorldView), `stream` (residency events).

**Public surface.**
- `render_d3d12/render_d3d12.h` — identity stub.
- `render_d3d12/renderer.h` (M1) — opaque PIMPL `Renderer`: headless device +
  offscreen RT + CPU readback (`FrameImage`), or windowed swapchain. Renders one
  deterministic clear-color frame. `debug_error_count()` surfaces the D3D12
  debug-layer/DRED state; `process_private_bytes()` feeds the memory soak. No
  D3D12/DXGI/`<windows.h>` types leak through the header (INV-5 stays intact).

- `render_world_view` (M2) — headless: draws the test-room geometry from a
  `contracts::WorldView` camera, depth-tested and lit (root-constant MVP + a
  fixed light), via a runtime-compiled (D3DCompile) PSO. Single source of room
  geometry comes from `core::test_room`.
- `render_chunks` (M3) — headless: draws resident streamed chunks (pos/nrm/color
  pipeline) with a persistently-mapped vertex-buffer pool (allocation-free
  stream-in) and a per-frame upload budget. Frees evicted chunk slots.

**Links:** `d3d12 dxgi dxguid d3dcompiler Psapi` (PRIVATE — implementation detail).

**Planned.** Procedural materials + lighting v1 (M5), VHS post + HUD (M8),
real geometry from streamed chunks (M3+).

**Contracts consumed:** `contracts/world_view_v1.h` (M2+), `contracts/stream_events_v1.h` (M3).

**Status:** M1 — device, debug layer + DRED, clear-color frame (headless PNG +
windowed swapchain). Clear color RGBA (46,43,33,255); golden `goldens/m1/`.
