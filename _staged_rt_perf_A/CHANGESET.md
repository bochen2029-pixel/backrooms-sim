# STAGED CHANGESET — RT perf item A: kill the per-frame cross-device GPU→CPU→GPU readback

> **STATUS: STAGED, NOT APPLIED. Do not apply until (a) the operator approves and (b) the other
> instance is done editing.** Nothing here has been compiled, run, or committed. This file is a
> *specification* to be applied later against whatever the codebase has become.
>
> **HOW TO APPLY (read this first):** apply each hunk below by **matching its `FIND:` anchor against the
> CURRENT code and replacing it** (i.e. via the Edit tool / a fuzzy patch), **NOT** by overwriting whole
> files — another instance is editing in parallel (it's on `DYNAMIC_DIRECTOR`/recolor, which touches
> `main.cpp`). Each hunk carries an **INTENT** so that if its anchor has drifted, you can re-derive the edit
> from intent rather than abandoning it. Apply renderer-side hunks (low drift risk) first; the `main.cpp`
> `run_game` hunk is the highest drift risk — treat it as intent-first.
>
> After applying: build, then run `gate.ps1 -Milestone M9` + `-Milestone M30` + a live `--game --rt` smoke
> (see the Validation Checklist at the end). This is **interactive-only** (the offline/golden `render_pt`
> path is untouched), so M9 goldens must stay bit-identical.

---

## 1. Goal & design

**Problem (from `docs/RT_PERF_PLAN.md` item A):** in the windowed RT path, every frame does a full
**GPU→CPU→GPU round-trip** — the DXR device renders the PT frame, `readback()` does a **blocking `Map`** of the
~5 MB output, the CPU copies it into a `std::vector`, and the **raster** device re-uploads it as a texture and
presents (`present_overlay_windowed`). Two separate D3D12 devices that can only talk through the CPU → the GPU
and CPU never overlap. The M9 gate shows the PT render itself is ~5.6 ms / 178 FPS; the in-game slowness is this
round-trip (+ the wait_idles, which are item B, NOT in this changeset).

**Fix (single-device, the cleaner of the two RT_PERF_PLAN A options):** make `DxrRenderer` **reuse the raster
`Renderer`'s `ID3D12Device5`** instead of creating its own. Then the PT output texture lives on the **same
device** as the swapchain, so the raster present can **sample it directly** (a fullscreen blit) — no CPU
readback, no re-upload. `readback()` stays available but is now called only when the Director's VLM / voice-chat
needs the player POV (~every 28 s), off the hot path.

**Why single-device over a cross-device shared resource:** it avoids the error-prone `CreateSharedHandle` /
shared-fence / `COMMON`-state machinery (which can't be validated without running), and it keeps ADR-077's forced
validation-layer fix exactly where it is (on the raster device — unchanged). The DXR device is simply no longer
created separately; it borrows the already-validated raster device.

**Key correctness insight (why this is self-contained + needs no cross-queue fence):** `render_pt_frame` still
`wait_idle`s (removing those is item B, out of scope here). So by the time `present_pt_texture` runs, the DXR
GPU work is **fully complete and the GPU is idle** — the raster queue can transition + sample the PT texture with
a plain resource barrier; no GPU-GPU fence is required. (If item B later removes the wait_idle, a cross-queue
fence becomes necessary — note that for the future.)

**INV-5:** `renderer.h` and `dxr.h` must stay free of D3D12/DXGI types. The device + texture cross the module
boundary as **`void*`** (exactly the existing `void* native_window_handle` pattern). No new includes in the headers.

**Determinism / goldens:** untouched. `render_pt` (the offline/golden path) and `render_pt_frame` are unchanged;
only *how the windowed game presents the result* changes. M9 PT goldens stay bit-identical.

---

## 2. Files touched (6 files, ~8 hunks) — ordered by ascending drift risk

| Order | File | Hunks | Drift risk | What |
|---|---|---|---|---|
| 1 | `render_dxr/include/render_dxr/dxr.h` | 2 | low | `init` gains optional `void* external_device5`; add `pt_output()` |
| 2 | `render_dxr/src/dxr.cpp` | 3 | low | `init` reuses the external device if given; `pt_output()` impl |
| 3 | `render_d3d12/include/render_d3d12/renderer.h` | 1 | low | add `native_device5()` + `present_pt_texture()` decls |
| 4 | `render_d3d12/src/renderer.cpp` | 3 | low | `Impl::ptSrvHeap`; `native_device5()`; `present_pt_texture()` (the big new method) |
| 5 | `app/src/main.cpp` — `run_play` RT block | 2 | medium | pass device into `dxr->init`; present via texture |
| 6 | `app/src/main.cpp` — `run_game` RT block | 2 | **HIGH** | pass device into `dxr->init`; present via texture + gate the readback to VLM-only |

---

## 3. HUNKS

### Hunk 1a — `render_dxr/include/render_dxr/dxr.h` — `init` signature
**INTENT:** `init` accepts an optional external `ID3D12Device5*` (as `void*`, INV-5). Default `nullptr` =
current behaviour (create own device).
```
FIND:
    bool init(uint32_t width, uint32_t height);
REPLACE:
    // `external_device5` (optional, void* = an ID3D12Device5*, INV-5): when non-null AND DXR-capable, reuse it
    // instead of creating an own device — so the PT output lives on the caller's device and can be presented
    // without a CPU readback (RT_PERF_PLAN item A). Null = create+own a device (the headless/offline path).
    bool init(uint32_t width, uint32_t height, void* external_device5 = nullptr);
```

### Hunk 1b — `dxr.h` — expose the PT output texture
**INTENT:** let the (same-device) raster renderer sample the PT color output. `void*` = `ID3D12Resource*`.
```
FIND:
    bool readback(std::vector<uint8_t>& rgba);    // size width*height*4, RGBA
REPLACE:
    bool readback(std::vector<uint8_t>& rgba);    // size width*height*4, RGBA

    // The path-traced color output (an ID3D12Resource*, returned as void* per INV-5). Valid after a
    // render_pt_frame(); left in UNORDERED_ACCESS state. When DxrRenderer was init'd on an external device,
    // the raster renderer can sample this directly (present_pt_texture) — no CPU readback. Null if not init'd.
    void* pt_output() const;
```

### Hunk 2a — `dxr.cpp` — `init` signature
```
FIND:
bool DxrRenderer::init(uint32_t w, uint32_t h) {
    Impl& d = *impl_;
    d.width = w;
    d.height = h;
REPLACE:
bool DxrRenderer::init(uint32_t w, uint32_t h, void* external_device5) {
    Impl& d = *impl_;
    d.width = w;
    d.height = h;
```

### Hunk 2b — `dxr.cpp` — reuse the external device (the core of item A)
**INTENT:** if `external_device5` is non-null, adopt it as `d.device` (ComPtr AddRefs; the caller/raster
renderer owns the original ref and outlives us) and **skip** the factory/adapter/CreateDevice. Then fall through
to the existing `CheckFeatureSupport(OPTIONS5)` which validates DXR for *either* path — if the external device
isn't DXR-capable, init fails and the caller falls back (see Hunk 5a/6a, which only pass the device, and the RT
toggle already disables RT on init failure). **Anchor = the whole device-acquisition block.**
```
FIND:
    ComPtr<ID3D12Debug> debug;
    UINT factoryFlags = 0;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory)))) { last_error_ = "factory"; return false; }
    ComPtr<ID3D12Device> dev0;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapterByGpuPreference(
             i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev0)))) break;
        adapter.Reset(); dev0.Reset();
    }
    if (!dev0 || FAILED(dev0.As(&d.device))) { last_error_ = "no ID3D12Device5"; return false; }
REPLACE:
    // RT_PERF_PLAN item A: reuse the caller's device (the raster renderer's Device5) when provided, so the PT
    // output is presentable without a CPU readback. ComPtr assignment AddRefs; the caller owns the original ref
    // and must outlive this DxrRenderer (it does in run_game: `renderer` is declared before `dxr`).
    if (external_device5) {
        d.device = static_cast<ID3D12Device5*>(external_device5);
    } else {
        ComPtr<ID3D12Debug> debug;
        UINT factoryFlags = 0;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
        ComPtr<IDXGIFactory6> factory;
        if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory)))) { last_error_ = "factory"; return false; }
        ComPtr<ID3D12Device> dev0;
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapterByGpuPreference(
                 i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev0)))) break;
            adapter.Reset(); dev0.Reset();
        }
        if (!dev0 || FAILED(dev0.As(&d.device))) { last_error_ = "no ID3D12Device5"; return false; }
    }
```
**NOTE:** the existing `CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5 ...)` block immediately after this stays
as-is and now also validates the *external* device → a non-DXR raster device (e.g. WARP fallback) makes init
return false, and the caller keeps RT off. The DXR renderer keeps creating its own queue/allocator/list/fence/
resources on `d.device` (whether borrowed or owned) — **no other change in `init`.**

### Hunk 2c — `dxr.cpp` — `pt_output()` impl
**INTENT:** return the PT color UAV resource. Place near the other trivial accessors (e.g. right after
`DxrRenderer::accum_samples()` / `width()` — anchor on whichever exists).
```
FIND:
uint32_t DxrRenderer::accum_samples() const { return impl_ ? impl_->accumSamples : 0; }
REPLACE:
uint32_t DxrRenderer::accum_samples() const { return impl_ ? impl_->accumSamples : 0; }
void* DxrRenderer::pt_output() const { return impl_ ? impl_->uav.Get() : nullptr; }   // RT_PERF_PLAN item A
```
**DRIFT NOTE:** if `accum_samples` moved/changed, place `pt_output()` next to any other one-line accessor impl in
dxr.cpp (e.g. `width()`/`height()`). The body is just `impl_->uav.Get()` (the PT color output; `uav` is the Impl
member at ~dxr.cpp:165).

---

### Hunk 3 — `render_d3d12/include/render_d3d12/renderer.h` — two new decls
**INTENT:** expose the raster device (as `void*`, for DxrRenderer to borrow) + the new GPU-texture present.
Place right after `present_overlay_windowed`.
```
FIND:
    bool present_overlay_windowed(const uint8_t* rgba, uint32_t width, uint32_t height);
REPLACE:
    bool present_overlay_windowed(const uint8_t* rgba, uint32_t width, uint32_t height);

    // RT_PERF_PLAN item A. The raster Device5 (as void* = ID3D12Device5*, INV-5) for the DXR renderer to reuse,
    // so the path-traced output lives on THIS device and can be presented without a CPU readback. Null if the
    // device isn't DXR-capable (caller then lets DxrRenderer create its own). Cached after first call.
    void* native_device5();

    // Present a path-traced frame that is ALREADY a GPU texture on THIS device (pt_texture = void* =
    // ID3D12Resource*, from DxrRenderer::pt_output(), in UNORDERED_ACCESS state). Fullscreen-blits it to the
    // swapchain (the linear sampler upscales the 2/3-res source), optionally alpha-blends the caption last
    // uploaded via upload_caption_overlay() OVER it, then Presents. NO CPU readback / re-upload. Requires
    // init_windowed() + that DxrRenderer was init'd on native_device5(). Blocks on the frame fence.
    bool present_pt_texture(void* pt_texture, bool draw_caption);
```

### Hunk 4a — `renderer.cpp` — `Impl` member for the PT-present SRV heap
**INTENT:** a 1-slot shader-visible heap for the external PT texture's SRV. Add beside the overlay-present
members (`ovlSrvHeap` etc.).
```
FIND:
    ComPtr<ID3D12DescriptorHeap> ovlSrvHeap;          // shader-visible (1 SRV = overlay)
REPLACE:
    ComPtr<ID3D12DescriptorHeap> ovlSrvHeap;          // shader-visible (1 SRV = overlay)
    ComPtr<ID3D12DescriptorHeap> ptSrvHeap;           // RT_PERF_PLAN item A: 1 SRV over the DXR PT output texture
    ComPtr<ID3D12Device5> device5;                    // cached QI of `device` for native_device5() (may be null)
```
**DRIFT NOTE:** if the exact `ovlSrvHeap` comment changed, just add the two `ComPtr` members anywhere in the
`Renderer::Impl` struct (renderer.cpp ~line 102-187). Requires `ID3D12Device5` — `<d3d12.h>` is already included
by renderer.cpp.

### Hunk 4b — `renderer.cpp` — `native_device5()` impl
**INTENT:** QI the raster `device` to `ID3D12Device5`, cache it, return as void*. Null if unsupported (WARP).
Place right before `Renderer::present_overlay_windowed`.
```
FIND:
bool Renderer::present_overlay_windowed(const uint8_t* rgba, uint32_t width, uint32_t height) {
REPLACE:
void* Renderer::native_device5() {
    Impl& d = *impl_;
    if (!d.device5 && d.device) { d.device.As(&d.device5); }   // QI; leaves null if the device isn't a Device5
    return d.device5.Get();
}

bool Renderer::present_overlay_windowed(const uint8_t* rgba, uint32_t width, uint32_t height) {
```

### Hunk 4c — `renderer.cpp` — `present_pt_texture()` (the big new method)
**INTENT:** the same-device GPU-texture present. **Add it immediately AFTER `present_overlay_windowed`'s closing
`}`** (so it can reuse the `ensure_overlay_pipeline` / `transition` helpers + the `kFormat`/`kDxrFormat` constants
already in scope). Modeled line-for-line on `present_overlay_windowed`; the differences are: (1) it samples an
**external** texture via a dedicated `ptSrvHeap` SRV instead of uploading CPU bytes, (2) it transitions that
texture `UNORDERED_ACCESS→PIXEL_SHADER_RESOURCE` and back, (3) an optional second draw alpha-blends the caption.
```
INSERT AFTER the closing brace of `bool Renderer::present_overlay_windowed(...) { ... }`:

bool Renderer::present_pt_texture(void* pt_texture, bool draw_caption) {
    Impl& d = *impl_;
    if (!d.windowed || !d.swapchain) { last_error_ = "present_pt_texture needs a window"; return false; }
    ID3D12Resource* pt = static_cast<ID3D12Resource*>(pt_texture);
    if (!pt) { last_error_ = "present_pt_texture: null texture"; return false; }

    // Reuse the overlay blit pipeline (fullscreen triangle + linear-sampler upscale + the caption-blend PSO),
    // sized to the WINDOW so the caption texture (ovlTex, uploaded at window size by upload_caption_overlay)
    // stays consistent. This does NOT touch the PT texture — that is sampled via ptSrvHeap below.
    if (!ensure_overlay_pipeline(d, d.width, d.height, last_error_)) return false;

    // 1-slot shader-visible SRV over the EXTERNAL PT texture. Re-created each call (cheap CPU descriptor write;
    // the PT resource is recreated when the DXR renderer resizes). Format MUST match DxrRenderer's PT output
    // (kDxrFormat = R8G8B8A8_UNORM).
    if (!d.ptSrvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(d.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&d.ptSrvHeap)))) { last_error_ = "pt srv heap"; return false; }
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;   // == kDxrFormat (DxrRenderer PT output); update if that changes
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    d.device->CreateShaderResourceView(pt, &srv, d.ptSrvHeap->GetCPUDescriptorHandleForHeapStart());

    if (FAILED(d.alloc->Reset())) { last_error_ = "allocator reset failed"; return false; }
    if (FAILED(d.list->Reset(d.alloc.Get(), nullptr))) { last_error_ = "command list reset failed"; return false; }

    // The PT output arrives in UNORDERED_ACCESS (render_pt_frame leaves it there + wait_idle'd, so the GPU is
    // idle -> a plain barrier is safe across the DXR queue / this queue; no cross-queue fence needed while
    // render_pt_frame still syncs). Sample it as a pixel SRV.
    const D3D12_RESOURCE_BARRIER ptToSrv = transition(pt, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    d.list->ResourceBarrier(1, &ptToSrv);

    const UINT bbIndex = d.swapchain->GetCurrentBackBufferIndex();
    ID3D12Resource* bb = d.backbuffers[bbIndex].Get();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(bbIndex) * d.rtvDescSize;
    const D3D12_RESOURCE_BARRIER toRT = transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    d.list->ResourceBarrier(1, &toRT);
    d.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(d.width), static_cast<float>(d.height), 0.0f, 1.0f };
    D3D12_RECT scissor = { 0, 0, static_cast<LONG>(d.width), static_cast<LONG>(d.height) };
    d.list->RSSetViewports(1, &vp);
    d.list->RSSetScissorRects(1, &scissor);
    d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // (1) Opaque fullscreen blit of the PT texture (ovlPso; the linear sampler upscales 2/3-res -> window).
    {
        ID3D12DescriptorHeap* heaps[] = { d.ptSrvHeap.Get() };
        d.list->SetDescriptorHeaps(1, heaps);
        d.list->SetGraphicsRootSignature(d.ovlRoot.Get());
        d.list->SetPipelineState(d.ovlPso.Get());
        d.list->SetGraphicsRootDescriptorTable(0, d.ptSrvHeap->GetGPUDescriptorHandleForHeapStart());
        d.list->DrawInstanced(3, 1, 0, 0);
    }
    // (2) Optional alpha-blended caption OVER the frame (ovlBlendPso + the caption already in ovlTex, window-sized).
    if (draw_caption && d.ovlReady) {
        ID3D12DescriptorHeap* heaps[] = { d.ovlSrvHeap.Get() };
        d.list->SetDescriptorHeaps(1, heaps);
        d.list->SetGraphicsRootSignature(d.ovlRoot.Get());
        d.list->SetPipelineState(d.ovlBlendPso.Get());
        d.list->SetGraphicsRootDescriptorTable(0, d.ovlSrvHeap->GetGPUDescriptorHandleForHeapStart());
        d.list->DrawInstanced(3, 1, 0, 0);
    }

    const D3D12_RESOURCE_BARRIER toPresent = transition(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    d.list->ResourceBarrier(1, &toPresent);
    // Restore the PT texture to UNORDERED_ACCESS for the next DxrRenderer render.
    const D3D12_RESOURCE_BARRIER ptToUav = transition(pt, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    d.list->ResourceBarrier(1, &ptToUav);

    if (FAILED(d.list->Close())) { last_error_ = "command list close failed"; return false; }
    ID3D12CommandList* wlists[] = { d.list.Get() };
    d.queue->ExecuteCommandLists(1, wlists);
    if (FAILED(d.swapchain->Present(1, 0))) { last_error_ = "Present failed"; return false; }
    const UINT64 wv = ++d.fenceValue;
    if (FAILED(d.queue->Signal(d.fence.Get(), wv))) { last_error_ = "fence Signal failed"; return false; }
    if (d.fence->GetCompletedValue() < wv) {
        if (FAILED(d.fence->SetEventOnCompletion(wv, d.fenceEvent))) { last_error_ = "SetEventOnCompletion failed"; return false; }
        if (WaitForSingleObject(d.fenceEvent, 5000) != WAIT_OBJECT_0) { last_error_ = "fence wait timed out"; return false; }
    }
    return true;
}
```
**DRIFT NOTE:** this method depends only on stable Impl members (`device`, `queue`, `alloc`, `list`, `fence`,
`fenceEvent`, `fenceValue`, `swapchain`, `backbuffers`, `rtvHeap`, `rtvDescSize`, `width`, `height`, `ovlRoot`,
`ovlPso`, `ovlBlendPso`, `ovlSrvHeap`, `ovlReady`) + the file-scope `transition()` helper + `ensure_overlay_pipeline`.
If `transition()`'s signature differs, match the existing barrier-helper usage in `present_overlay_windowed`.

---

### Hunk 5a — `app/src/main.cpp` `run_play` — pass the raster device into `dxr->init`
**INTENT:** share the raster device so the PT output is presentable in place.
```
FIND  (in run_play; ~main.cpp:1051-1052):
                dxr = std::make_unique<br::render_dxr::DxrRenderer>();
                if (dxr->init(rw, rh)) { dxrW = rw; dxrH = rh; dxrCenter = contracts::ChunkKey{0, static_cast<int64_t>(1) << 40, 0}; }
REPLACE:
                dxr = std::make_unique<br::render_dxr::DxrRenderer>();
                if (dxr->init(rw, rh, renderer.native_device5())) { dxrW = rw; dxrH = rh; dxrCenter = contracts::ChunkKey{0, static_cast<int64_t>(1) << 40, 0}; }
```
**DRIFT NOTE:** if `native_device5()` returns null on this machine (non-DXR raster device), `init` falls back to
creating its own device (current behaviour) — so passing it is always safe.

### Hunk 5b — `run_play` — present via the GPU texture instead of readback
```
FIND  (~main.cpp:1071):
                if (dxr->readback(rt) && renderer.present_overlay_windowed(rt.data(), dxrW, dxrH)) {
REPLACE:
                if (renderer.present_pt_texture(dxr->pt_output(), /*draw_caption=*/false)) {
```
**DRIFT NOTE:** `run_play` is a dev path with no caption/Director-vision, so the `rt` vector + the readback are no
longer needed here. If the surrounding lines (`std::vector<uint8_t> rt;`, the `++rtFrames; lastRt = rt;` body) cause
an unused-variable warning (`/WX`), drop the now-dead `rt`/`lastRt` lines in this block. Intent: present the PT
output texture directly; no readback in run_play.

---

### Hunk 6a — `app/src/main.cpp` `run_game` — pass the raster device into `dxr->init`
**INTENT:** same as 5a, for the playable path.
```
FIND  (in run_game; ~main.cpp:1900-1901):
                    dxr = std::make_unique<br::render_dxr::DxrRenderer>();
                    if (dxr->init(rw, rh)) { dxrW = rw; dxrH = rh; dxrCenter = contracts::ChunkKey{0, static_cast<int64_t>(1) << 40, 0}; }
REPLACE:
                    dxr = std::make_unique<br::render_dxr::DxrRenderer>();
                    if (dxr->init(rw, rh, renderer.native_device5())) { dxrW = rw; dxrH = rh; dxrCenter = contracts::ChunkKey{0, static_cast<int64_t>(1) << 40, 0}; }
```

### Hunk 6b — `run_game` — present via texture; readback only when the VLM needs it ⚠ HIGHEST DRIFT RISK
**INTENT (apply by intent, the anchor may have moved — the other instance is editing this Director block):**
Today the RT block does `dxr->readback(rt)` **every frame** and, inside that `if`, (i) submits the player POV to
the Director VLM every ~`vision_interval`, (ii) grabs a POV for a pending voice-chat turn, (iii) CPU-composites
the caption into `rt`, then (iv) `present_overlay_windowed(rt.data(), ...)`.

**Change it to:**
1. Keep `dxr->render_pt_frame(...)` as-is.
2. **Do the CPU `readback(rt)` ONLY when a consumer needs the player frame** — i.e. *inside* the existing
   "Director vision due" branch (`now - last_vision >= vision_interval`) and the "pending chat POV" branch
   (`wantChatPov`). Each does its own `std::vector<uint8_t> rt; if (dxr->readback(rt)) { ... encode_pov_b64(rt, dxrW, dxrH) ... }`.
   These already run rarely (~28 s), so the blocking readback leaves the hot path.
3. **Replace the per-frame present** with `renderer.present_pt_texture(dxr->pt_output(), showCap);` then
   `++rtFrames;`. The caption is now composited on the GPU (the `draw_caption=showCap` arg), so **delete the CPU
   caption-composite block** (the `if (showCap) { ... build_caption_overlay into rt ... }` loop) — the caption
   overlay is still uploaded via `upload_caption_overlay` exactly as today; `present_pt_texture` blends it.

**Reference anchors (today's lines; expect drift):**
```
FIND (~main.cpp:1929):
                    if (dxr->readback(rt)) {
... (this whole block currently wraps the vision-submit, chat-POV, caption composite, and present) ...
FIND (~main.cpp:1952):
                        renderer.present_overlay_windowed(rt.data(), dxrW, dxrH);
```
**REPLACE the block's STRUCTURE with (illustrative — adapt to the current variable names):**
```cpp
                    // RT_PERF_PLAN item A: present the PT output as a same-device GPU texture (no per-frame
                    // readback). The CPU readback now happens ONLY for the Director VLM / chat POV (rare).
                    std::vector<uint8_t> rt;   // populated only when a VLM consumer needs it this frame
                    if (model.settings.director && visionDir && now - last_vision >= vision_interval) {
                        last_vision = now;
                        if (dxr->readback(rt)) {
                            std::string b64 = encode_pov_b64(rt, dxrW, dxrH);
                            if (!b64.empty()) visionDir->submit(std::move(b64), vision_entity_context(shog, s.wanderer.pos));
                        }
                    }
                    if (model.settings.director && chat && wantChatPov) {
                        if (rt.empty()) dxr->readback(rt);   // reuse this frame's readback if vision already did it
                        if (!rt.empty()) { wantChatPov = false; chat->submit(std::move(pendingChatWav), encode_pov_b64(rt, dxrW, dxrH), std::move(pendingChatCtx)); }
                    }
                    if (renderer.present_pt_texture(dxr->pt_output(), showCap)) ++rtFrames;
```
**DRIFT NOTE:** preserve whatever the current Director-vision / chat submit logic is (the other instance may have
changed it for `DYNAMIC_DIRECTOR`/recolor) — the ONLY structural changes you must make are: (a) the readback is
no longer unconditional/every-frame (gate it to the vision/chat branches), (b) the present is
`present_pt_texture(dxr->pt_output(), showCap)` not `present_overlay_windowed(rt...)`, (c) the per-frame CPU
caption composite is removed (the caption is GPU-blended in present_pt_texture). If the recolor feature also reads
back the frame, fold its readback into the same rare `rt` grab.

---

## 4. VALIDATION CHECKLIST (this changeset is UNTESTED — validate on apply)
1. **Build** clean (`/WX`), debug + release. Watch for unused `rt`/`lastRt` (run_play) → drop if warned.
2. **`gate.ps1 -Milestone M9`** — PT goldens **bit-identical** (the offline `render_pt` path is untouched; this
   only changes windowed presentation). MUST stay green.
3. **`gate.ps1 -Milestone M30`** — the live dual→**single**-device RT smoke: `--game --auto-play --rt` exits 0,
   `rt_frames >= 1`, `debug_error_count: 0`. (This is the path the change most affects — watch the D3D12 debug
   layer for state/lifetime errors on the borrowed device + the PT texture barriers.)
4. **Live `--game --rt`**: visibly **faster** than before (the win); the frame is correct (no swapped R/B → if
   colors are off, the SRV format vs swapchain `kFormat` mismatch; both should be R8G8B8A8_UNORM); the **caption**
   renders over the RT frame; the **Director vision** still narrates (the rare readback still feeds the VLM).
5. **`audit.ps1`** green (determinism/inventory/isolation unaffected — it's interactive-only + INV-5-clean).
6. **Profile** before/after (PIX or the in-game timing) to confirm the readback round-trip is gone and quantify
   the win (the whole point — RT_PERF_PLAN Step 0).

## 5. RISK / VALIDATION POINTS (things I could not verify without running)
- **Raster device DXR-capability:** `native_device5()` may return null on a WARP/non-DXR raster device → `init`
  falls back to its own device (current behaviour) → present still works via the OLD path only if you ALSO keep a
  fallback. **NOTE:** if `init` created its OWN device (fallback), `pt_output()` is on a DIFFERENT device than the
  swapchain → `present_pt_texture` would be invalid. **Mitigation:** in run_game, if `native_device5()` is null,
  keep the OLD readback+`present_overlay_windowed` path. (Cheap guard; document the branch. On the target RTX
  machines `native_device5()` is non-null, so the fast path is used.)
- **PT texture format** must equal the swapchain sample format (both R8G8B8A8_UNORM today). If `kDxrFormat`
  changes, update the SRV format in Hunk 4c.
- **Resource state across queues:** correct ONLY while `render_pt_frame` still `wait_idle`s (it does). If item B
  (pipelining) later removes that, add a cross-queue fence (raster queue waits on the DXR fence) before sampling.
- **Lifetime:** the raster `Renderer` must outlive `DxrRenderer` (true in run_play/run_game — `renderer` declared
  first, destroyed last). The borrowed-device ComPtr balances its own AddRef/Release.
- **`update_creature` / `build_scene`** run on the borrowed device's DXR queue exactly as before — unaffected.

## 6. ROLLBACK
Additive + isolated: a `git revert` of the applied commit restores the two-device readback present. Or, before
applying, `git tag pre-rtperfA`. The offline/golden path is never touched, so no golden regeneration is involved.

## 7. WHAT THIS DOES NOT DO (explicitly out of scope — separate changesets)
- Item B (remove the `wait_idle` stalls / frames-in-flight). **A deliberately keeps the wait_idle** — that's what
  makes it a self-contained, fence-free change. B is the natural follow-up and would compound the win.
- Item C (AS refit), D (stochastic lights), E (GI-NEE cut), the creature-ghosting fix. All independent.
