# MODULE: render_d3d12 (L2)

**Purpose.** Raster renderer: D3D12 device/swapchain, procedural textures,
fluorescent forward/clustered lighting, VHS post stack, HUD. Default + fallback
renderer (INV-6).

**Depends on:** `core` (read-only WorldView), `stream` (residency events).

**Public surface (M0).** `render_d3d12/render_d3d12.h` — identity stub.

**Planned.** Device/swapchain/queue/fence + debug layer + DRED + headless PNG
(M1), procedural materials + lighting v1 (M5), VHS post + HUD (M8).

**Contracts consumed:** `contracts/world_view_v1.h`, `contracts/stream_events_v1.h`.

**Status:** M0 stub. (D3D12 / DXGI link deps introduced M1.)
