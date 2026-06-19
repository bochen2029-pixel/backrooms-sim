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
- `render_chunks` (M3, lit in M5) — headless: draws resident streamed chunks via
  the **lit pipeline** (pos/nrm/color/uv/material; 48-byte stride) with a
  persistently-mapped vertex-buffer pool (allocation-free stream-in) and a
  per-frame upload budget; frees evicted chunk slots. Samples a
  **Texture2DArray** (5 material slices) through a shader-visible SRV heap +
  static sampler, applies a per-chunk hue tint, and accumulates **forward
  fluorescent lighting**: the ceiling-grid lights near the camera (CBV b1), each
  scaled by `core::light_flicker(seed, light_id, tick)` so replays reproduce the
  lighting exactly (INV-2). A highlight knee keeps the wallpaper's hue under
  stacked lights instead of clipping to white (ADR-026). Takes a `tick` for the
  flicker phase.
- `render_chunks_windowed` (M13) — windowed twin of `render_chunks`: the **same**
  geometry + forward fluorescent lighting, but targets the swapchain back buffer
  (PRESENT→RENDER_TARGET→draw→PRESENT, fence-synced + `Present`) instead of an offscreen
  RT. Requires `init_windowed` (which now also builds the depth buffer). The gated headless
  `render_chunks` is byte-for-byte untouched; this is the real-time playable (`app --play`) path.
- `present_overlay_windowed` (M15) — blit a CPU RGBA overlay (the menu) to the swapchain
  back buffer via a fullscreen triangle, then `Present`. Its **own** texture + SRV heap + root
  signature + PSO, fully separate from the VHS post pass (whose `rtvHeap` slot 1 would collide
  with the second back buffer windowed) — so the headless post/readback goldens are untouched.
  The game shell (`app --game`) uses it for menu screens; the reusable HUD-present primitive.
- `resize` (M16) — windowed: waits the GPU idle, releases the back buffers, `ResizeBuffers`,
  rebuilds the RTVs + depth + (lazily) the overlay pipeline, for resolution changes and
  borderless-fullscreen toggles. The HWND style/placement is the app's; this owns the GPU side.
- `render_topdown` (M4) — headless: orthographic top-down render of a chunk set
  (debug golden of the maze layout); discards ceiling/fluorescent material via a
  root-constant toggle so the M4 layout goldens stay stable under M5.
- `set_texture_seed` (M5) — selects the seed for the procedural material
  textures; the Texture2DArray upload happens lazily on the next lit draw.
- `set_post` / `upload_hud_overlay` (M8) — the **VHS post pass**: a fullscreen
  shader (scene RT → SRV → `postRt`) applying seeded film grain, chromatic
  aberration, barrel distortion, scanline/interlace flicker, and vignette, then
  compositing a CPU-rasterised HUD overlay (undistorted). Off by default (so
  prior goldens are byte-unchanged); ~0.6 ms at 1440p (ADR-034).
- `set_dread` (Apparition Phase 2b, ADR-084) — a soft "dread" dim of the **windowed**
  forward lighting (1.0 = off/default; lower darkens the fluorescents while a recent
  apparition verdict lingers). Multiplies the per-light intensity in
  `render_chunks_windowed` **only**; the headless golden + creature-POV path
  (`render_chunks`) is byte-for-byte untouched, so goldens stay bit-identical.
  Presentation-only; driven by `app`'s live atmosphere window.
- `render_d3d12/texgen.h` (M5) — D3D12-free procedural material textures:
  `generate_texture(kind, seed, rgba)` for `TexKind`
  {Wallpaper, Carpet, CeilingTile, Fluorescent, Baseboard} + `texture_hash`.
  Deterministic per `(kind, seed)`; unit-tested and usable off-GPU.

**Links:** `d3d12 dxgi dxguid d3dcompiler Psapi` (PRIVATE — implementation detail).

**Planned.** Path-traced parity via `render_dxr` (M9); in-world rendered
stairwells (M7 follow-up).

**Contracts consumed:** `contracts/world_view_v1.h` (M2+),
`contracts/stream_events_v1.h` (M3), `contracts/chunk_gen_v1.h` (M5, material ids
+ fluorescent-grid helpers shared with `gen`). Lighting math: `core/lighting.h`.

**Status:** M8 — adds the **VHS post pass** (grain/aberration/distortion/scanlines/
vignette + HUD/timestamp overlay), off by default. Goldens `goldens/m8/`
(post on/off, HUD timestamp). Earlier: M5 lit chunk render + fluorescent lighting
(`goldens/m5/`), M4 top-down (`goldens/m4/`), M2 room (`goldens/m2/`), M1 clear
(`goldens/m1/`).
