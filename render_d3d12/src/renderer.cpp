// render_d3d12/renderer.cpp — M1 D3D12 renderer: device, debug layer + DRED,
// deterministic clear-color frame, headless offscreen readback, windowed
// swapchain present. No CD3DX12 helpers (avoids a new dependency); resource
// descriptors are filled by hand.
#include "render_d3d12/renderer.h"
#include "render_d3d12/texgen.h"
#include "core/lighting.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <psapi.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace br::render_d3d12 {

namespace {

constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr UINT kBackBufferCount = 2;

// Canonical clear color (8-bit). A dim warm "backrooms" tone; deterministic.
constexpr uint8_t kClearR = 46, kClearG = 43, kClearB = 33, kClearA = 255;
const float kClearFloat[4] = {
    kClearR / 255.0f, kClearG / 255.0f, kClearB / 255.0f, kClearA / 255.0f
};

D3D12_HEAP_PROPERTIES heap_props(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES p = {};
    p.Type = type;
    p.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    p.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    p.CreationNodeMask = 1;
    p.VisibleNodeMask = 1;
    return p;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r,
                                  D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource = r;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}

}  // namespace

ClearColor clear_color() noexcept {
    return ClearColor{ kClearR, kClearG, kClearB, kClearA };
}

// One slot in the chunk vertex-buffer pool: a persistently-mapped upload buffer
// of fixed capacity, reused across chunks so steady-state uploads are a memcpy
// (no per-frame allocation -> no streaming hitches).
struct ChunkSlot {
    ComPtr<ID3D12Resource> vb;
    void* mapped = nullptr;
    D3D12_VERTEX_BUFFER_VIEW view = {};
    UINT vertex_count = 0;
};
constexpr size_t kChunkSlotCapacityBytes = 6144 * 48;  // ~3600 maze verts/chunk @ 48B stride + headroom

// Forward-lighting constant buffer (matches register(b1) in the lit shader).
constexpr int kMaxLights = 64;
struct LightsCB {
    float ambientCount[4];           // rgb ambient, w = count
    float lights[kMaxLights][4];     // xyz world pos, w intensity
};
constexpr size_t kLightCbBytes = (sizeof(LightsCB) + 255u) & ~size_t(255u);

// VHS post-process parameters (M8), passed as root 32-bit constants (b0).
// Packed as 12 DWORDs: resolution, time, 5 effect intensities, grain seed, pads.
struct PostParams {
    float resX = 0.0f, resY = 0.0f;  // render resolution (pixels)
    float time = 0.0f;               // sim time (s) — drives grain/interlace
    float grain = 0.08f;             // film-grain intensity
    float aberration = 0.0025f;      // chromatic-aberration radial offset
    float distortion = 0.06f;        // barrel-distortion strength
    float scanline = 0.18f;          // scanline darkening depth
    float vignette = 0.35f;          // vignette strength
    uint32_t seed = 1u;              // grain seed (deterministic)
    uint32_t hud = 0u;               // 1 = composite the HUD overlay (M8 p2)
    float pad0 = 0.0f, pad1 = 0.0f;
};

struct Renderer::Impl {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12InfoQueue> infoQueue;

    // Headless offscreen target + readback staging.
    ComPtr<ID3D12Resource> rt;
    ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT readbackRows = 0;
    UINT64 readbackRowSize = 0;
    UINT64 readbackTotal = 0;

    // Windowed swapchain.
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12Resource> backbuffers[kBackBufferCount];

    // Depth + scene pipeline (M2: draw the test room).
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    ComPtr<ID3D12Resource> depth;
    ComPtr<ID3D12RootSignature> sceneRoot;
    ComPtr<ID3D12PipelineState> scenePso;
    ComPtr<ID3D12Resource> vb;
    D3D12_VERTEX_BUFFER_VIEW vbView = {};
    UINT vertexCount = 0;
    bool sceneReady = false;

    // Streamed-chunk pipeline + a reused vertex-buffer pool (M3).
    ComPtr<ID3D12RootSignature> chunkRoot;
    ComPtr<ID3D12PipelineState> chunkPso;
    bool chunkReady = false;
    std::vector<ChunkSlot> chunkPool;
    std::vector<uint32_t> chunkFree;                 // free pool slot indices
    std::map<contracts::ChunkKey, uint32_t> chunkSlotOf;

    // Textured + lit pipeline (M5): material texture-array + sampler.
    ComPtr<ID3D12RootSignature> litRoot;
    ComPtr<ID3D12PipelineState> litPso;
    bool litReady = false;
    ComPtr<ID3D12Resource> texArray;                 // Texture2DArray of materials
    ComPtr<ID3D12DescriptorHeap> srvHeap;            // shader-visible (1 SRV)
    uint64_t texSeed = 0;
    uint64_t pendingTexSeed = 0;                     // seed the app wants textured
    bool texUploaded = false;
    ComPtr<ID3D12Resource> lightCb;                  // per-frame forward-light CBV
    void* lightCbMapped = nullptr;

    // VHS post-process pass (M8): scene RT -> fullscreen effects -> post RT.
    ComPtr<ID3D12Resource> postRt;                   // final composited target
    ComPtr<ID3D12RootSignature> postRoot;
    ComPtr<ID3D12PipelineState> postPso;
    ComPtr<ID3D12DescriptorHeap> postSrvHeap;        // shader-visible: [0]=scene [1]=HUD
    ComPtr<ID3D12Resource> hudTex;                   // CPU-rasterised HUD overlay (M8 p2)
    ComPtr<ID3D12Resource> hudUpload;
    UINT hudW = 0, hudH = 0;
    bool postReady = false;
    bool postEnabled = false;
    PostParams postParams;

    // Windowed overlay present (M15 menus): a CPU RGBA overlay blitted to the back
    // buffer by a fullscreen triangle. Fully separate from the post/readback path so
    // the headless goldens are untouched (and it never collides with the RTV heap).
    ComPtr<ID3D12Resource> ovlTex;
    ComPtr<ID3D12Resource> ovlUpload;
    ComPtr<ID3D12DescriptorHeap> ovlSrvHeap;          // shader-visible (1 SRV = overlay)
    ComPtr<ID3D12RootSignature> ovlRoot;
    ComPtr<ID3D12PipelineState> ovlPso;
    ComPtr<ID3D12PipelineState> ovlBlendPso;   // alpha-blended variant: paint a transparent overlay OVER the world
    UINT ovlW = 0, ovlH = 0;
    bool ovlReady = false;

    HANDLE fenceEvent = nullptr;
    UINT64 fenceValue = 0;
    UINT rtvDescSize = 0;
    UINT width = 0;
    UINT height = 0;
    bool windowed = false;

    ~Impl() {
        if (fenceEvent) { CloseHandle(fenceEvent); fenceEvent = nullptr; }
    }
};

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() { shutdown(); }

void Renderer::shutdown() {
    if (impl_) {
        // Ensure the GPU is idle before tearing down (best-effort).
        if (impl_->queue && impl_->fence && impl_->fenceEvent) {
            const UINT64 v = ++impl_->fenceValue;
            if (SUCCEEDED(impl_->queue->Signal(impl_->fence.Get(), v))) {
                if (impl_->fence->GetCompletedValue() < v) {
                    impl_->fence->SetEventOnCompletion(v, impl_->fenceEvent);
                    WaitForSingleObject(impl_->fenceEvent, 2000);
                }
            }
        }
    }
}

namespace {

// A freshly-created device can be "born removed" if the GPU is mid-reset (e.g. a TDR after another
// process crashed/was-killed on the driver): D3D12CreateDevice SUCCEEDS but the device is already dead,
// so the first real resource (the texture array) fails with DXGI_ERROR_DEVICE_REMOVED. Probe the device
// with a trivial allocation so we reject a dead one here and fall through (next adapter, then WARP) --
// graceful degradation instead of a hard crash for a user whose driver hiccups. Cheap; runs once at init.
bool device_usable(ID3D12Device* dev) {
    if (!dev) return false;
    // Mirror the real texture array (a DEFAULT-heap VRAM texture): on a dead/TDR'd device the small
    // buffer alloc still succeeds but the texture alloc fails, so probe with the actual resource shape.
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = static_cast<UINT64>(kTexSize); rd.Height = static_cast<UINT>(kTexSize);
    rd.DepthOrArraySize = static_cast<UINT16>(kTexCount); rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    Microsoft::WRL::ComPtr<ID3D12Resource> probe;
    return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&probe)));
}

// Shared device/queue/fence/list bring-up used by both headless and windowed.
bool create_device_core(Renderer::Impl& d, std::string& err) {
    UINT factoryFlags = 0;
#ifndef BR_RELEASE
    // Debug layer + DRED (dev/gate builds). Errors here are non-fatal. Compiled OUT
    // of the packaged release (BR_RELEASE, M17) so an end user with the Windows SDK
    // installed never trips its debug layer / breakpoints on a shipped build.
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dred;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
        dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    }
#endif

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory)))) {
        err = "CreateDXGIFactory2 failed";
        return false;
    }

    // Prefer a high-performance hardware adapter (the RTX); fall back to WARP.
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0;
         factory->EnumAdapterByGpuPreference(
             i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
             IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                        IID_PPV_ARGS(&d.device))) &&
            device_usable(d.device.Get())) {   // reject a born-removed device (post-TDR) -> next adapter, then WARP
            break;
        }
        adapter.Reset();
        d.device.Reset();
    }
    if (!d.device) {
        ComPtr<IDXGIAdapter> warp;
        if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
            D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_12_0,
                              IID_PPV_ARGS(&d.device));
        }
    }
    if (!d.device) {
        err = "no D3D12 device (hardware or WARP) could be created";
        return false;
    }

    // Collect debug messages rather than breaking, so the gate can read them.
    if (SUCCEEDED(d.device.As(&d.infoQueue))) {
        d.infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
        d.infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
        d.infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
    }

    D3D12_COMMAND_QUEUE_DESC qDesc = {};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (FAILED(d.device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&d.queue)))) {
        err = "CreateCommandQueue failed";
        return false;
    }
    if (FAILED(d.device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&d.alloc)))) {
        err = "CreateCommandAllocator failed";
        return false;
    }
    if (FAILED(d.device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.alloc.Get(), nullptr,
            IID_PPV_ARGS(&d.list)))) {
        err = "CreateCommandList failed";
        return false;
    }
    d.list->Close();  // start closed; render loop resets it

    if (FAILED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                     IID_PPV_ARGS(&d.fence)))) {
        err = "CreateFence failed";
        return false;
    }
    d.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!d.fenceEvent) { err = "CreateEvent failed"; return false; }

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.NumDescriptors = kBackBufferCount;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(d.device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&d.rtvHeap)))) {
        err = "CreateDescriptorHeap(RTV) failed";
        return false;
    }
    d.rtvDescSize =
        d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return true;
}

// Depth-stencil target + DSV (D32_FLOAT), sized to the render dimensions.
bool create_depth(Renderer::Impl& d, std::string& err) {
    D3D12_DESCRIPTOR_HEAP_DESC dh = {};
    dh.NumDescriptors = 1;
    dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(d.device->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&d.dsvHeap)))) {
        err = "CreateDescriptorHeap(DSV) failed";
        return false;
    }
    D3D12_RESOURCE_DESC dd = {};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = d.width;
    dd.Height = d.height;
    dd.DepthOrArraySize = 1;
    dd.MipLevels = 1;
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.SampleDesc.Count = 1;
    dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv = {};
    cv.Format = DXGI_FORMAT_D32_FLOAT;
    cv.DepthStencil.Depth = 1.0f;
    const D3D12_HEAP_PROPERTIES def = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(d.device->CreateCommittedResource(
            &def, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &cv, IID_PPV_ARGS(&d.depth)))) {
        err = "CreateCommittedResource(depth) failed";
        return false;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    d.device->CreateDepthStencilView(d.depth.Get(), &dsv,
                                     d.dsvHeap->GetCPUDescriptorHandleForHeapStart());
    return true;
}

}  // namespace

bool Renderer::init_headless(uint32_t width, uint32_t height) {
    Impl& d = *impl_;
    d.width = width;
    d.height = height;
    d.windowed = false;
    if (!create_device_core(d, last_error_)) return false;

    // Offscreen render target (starts in RENDER_TARGET; clear value must match
    // the clear we issue, or the debug layer warns).
    D3D12_RESOURCE_DESC rtDesc = {};
    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtDesc.Width = width;
    rtDesc.Height = height;
    rtDesc.DepthOrArraySize = 1;
    rtDesc.MipLevels = 1;
    rtDesc.Format = kFormat;
    rtDesc.SampleDesc.Count = 1;
    rtDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE cv = {};
    cv.Format = kFormat;
    std::memcpy(cv.Color, kClearFloat, sizeof(kClearFloat));

    const D3D12_HEAP_PROPERTIES defHeap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(d.device->CreateCommittedResource(
            &defHeap, D3D12_HEAP_FLAG_NONE, &rtDesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&d.rt)))) {
        last_error_ = "CreateCommittedResource(RT) failed";
        return false;
    }
    d.device->CreateRenderTargetView(
        d.rt.Get(), nullptr, d.rtvHeap->GetCPUDescriptorHandleForHeapStart());

    // Readback buffer sized from the copyable footprint (row pitch is aligned).
    d.device->GetCopyableFootprints(&rtDesc, 0, 1, 0, &d.footprint,
                                    &d.readbackRows, &d.readbackRowSize,
                                    &d.readbackTotal);
    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = d.readbackTotal;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const D3D12_HEAP_PROPERTIES rbHeap = heap_props(D3D12_HEAP_TYPE_READBACK);
    if (FAILED(d.device->CreateCommittedResource(
            &rbHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&d.readback)))) {
        last_error_ = "CreateCommittedResource(readback) failed";
        return false;
    }
    if (!create_depth(d, last_error_)) return false;
    return true;
}

bool Renderer::init_windowed(void* native_window_handle, uint32_t width, uint32_t height) {
    Impl& d = *impl_;
    d.width = width;
    d.height = height;
    d.windowed = true;
    if (!create_device_core(d, last_error_)) return false;

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        last_error_ = "CreateDXGIFactory2 (swapchain) failed";
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 sc = {};
    sc.Width = width;
    sc.Height = height;
    sc.Format = kFormat;
    sc.SampleDesc.Count = 1;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = kBackBufferCount;
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    HWND hwnd = static_cast<HWND>(native_window_handle);
    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(factory->CreateSwapChainForHwnd(d.queue.Get(), hwnd, &sc, nullptr,
                                               nullptr, &sc1))) {
        last_error_ = "CreateSwapChainForHwnd failed";
        return false;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(sc1.As(&d.swapchain))) {
        last_error_ = "IDXGISwapChain3 query failed";
        return false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kBackBufferCount; ++i) {
        if (FAILED(d.swapchain->GetBuffer(i, IID_PPV_ARGS(&d.backbuffers[i])))) {
            last_error_ = "swapchain GetBuffer failed";
            return false;
        }
        d.device->CreateRenderTargetView(d.backbuffers[i].Get(), nullptr, rtv);
        rtv.ptr += d.rtvDescSize;
    }
    if (!create_depth(d, last_error_)) return false;  // M13: depth for the windowed maze render
    return true;
}

bool Renderer::resize(uint32_t width, uint32_t height) {
    Impl& d = *impl_;
    if (!d.windowed || !d.swapchain) { last_error_ = "resize needs a window"; return false; }
    if (width == 0 || height == 0) return true;                 // ignore minimize
    if (width == d.width && height == d.height) return true;    // no-op

    // GPU must be idle before the back buffers are released.
    const UINT64 v = ++d.fenceValue;
    if (FAILED(d.queue->Signal(d.fence.Get(), v))) { last_error_ = "resize fence signal failed"; return false; }
    if (d.fence->GetCompletedValue() < v) {
        if (FAILED(d.fence->SetEventOnCompletion(v, d.fenceEvent))) { last_error_ = "resize fence event failed"; return false; }
        WaitForSingleObject(d.fenceEvent, 5000);
    }
    for (UINT i = 0; i < kBackBufferCount; ++i) d.backbuffers[i].Reset();

    if (FAILED(d.swapchain->ResizeBuffers(kBackBufferCount, width, height, kFormat, 0))) {
        last_error_ = "ResizeBuffers failed"; return false;
    }
    d.width = width; d.height = height;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kBackBufferCount; ++i) {
        if (FAILED(d.swapchain->GetBuffer(i, IID_PPV_ARGS(&d.backbuffers[i])))) {
            last_error_ = "GetBuffer (resize) failed"; return false;
        }
        d.device->CreateRenderTargetView(d.backbuffers[i].Get(), nullptr, rtv);
        rtv.ptr += d.rtvDescSize;
    }
    if (!create_depth(d, last_error_)) return false;  // depth at the new size

    // The overlay pipeline's texture is sized to the old resolution — drop it so it
    // rebuilds lazily at the new size on the next present (post path is unused windowed).
    d.ovlReady = false;
    d.ovlTex.Reset(); d.ovlUpload.Reset(); d.ovlSrvHeap.Reset(); d.ovlRoot.Reset(); d.ovlPso.Reset(); d.ovlBlendPso.Reset();
    return true;
}

bool Renderer::render_clear_frame() {
    Impl& d = *impl_;
    if (FAILED(d.alloc->Reset())) { last_error_ = "allocator reset failed"; return false; }
    if (FAILED(d.list->Reset(d.alloc.Get(), nullptr))) {
        last_error_ = "command list reset failed";
        return false;
    }

    UINT bbIndex = 0;
    ID3D12Resource* target = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();

    if (d.windowed) {
        bbIndex = d.swapchain->GetCurrentBackBufferIndex();
        target = d.backbuffers[bbIndex].Get();
        rtv.ptr += static_cast<SIZE_T>(bbIndex) * d.rtvDescSize;
        const D3D12_RESOURCE_BARRIER toRT = transition(
            target, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        d.list->ResourceBarrier(1, &toRT);
    } else {
        target = d.rt.Get();  // already in RENDER_TARGET
    }

    d.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    d.list->ClearRenderTargetView(rtv, kClearFloat, 0, nullptr);

    if (d.windowed) {
        const D3D12_RESOURCE_BARRIER toPresent = transition(
            target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        d.list->ResourceBarrier(1, &toPresent);
    } else {
        // Copy offscreen target -> readback buffer.
        const D3D12_RESOURCE_BARRIER toCopy = transition(
            target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        d.list->ResourceBarrier(1, &toCopy);

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = d.readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = d.footprint;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = target;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        const D3D12_RESOURCE_BARRIER back = transition(
            target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        d.list->ResourceBarrier(1, &back);
    }

    if (FAILED(d.list->Close())) { last_error_ = "command list close failed"; return false; }
    ID3D12CommandList* lists[] = { d.list.Get() };
    d.queue->ExecuteCommandLists(1, lists);

    if (d.windowed) {
        if (FAILED(d.swapchain->Present(1, 0))) {
            last_error_ = "Present failed";
            return false;
        }
    }

    // Block on the frame fence (GPU idle on return).
    const UINT64 v = ++d.fenceValue;
    if (FAILED(d.queue->Signal(d.fence.Get(), v))) {
        last_error_ = "fence Signal failed";
        return false;
    }
    if (d.fence->GetCompletedValue() < v) {
        if (FAILED(d.fence->SetEventOnCompletion(v, d.fenceEvent))) {
            last_error_ = "SetEventOnCompletion failed";
            return false;
        }
        if (WaitForSingleObject(d.fenceEvent, 5000) != WAIT_OBJECT_0) {
            last_error_ = "fence wait timed out";
            return false;
        }
    }
    return true;
}

bool Renderer::readback(FrameImage& out) {
    Impl& d = *impl_;
    if (d.windowed || !d.readback) {
        last_error_ = "readback is only available in headless mode";
        return false;
    }
    void* mapped = nullptr;
    D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(d.readbackTotal) };
    if (FAILED(d.readback->Map(0, &readRange, &mapped))) {
        last_error_ = "readback Map failed";
        return false;
    }
    out.width = d.width;
    out.height = d.height;
    out.rgba.assign(static_cast<size_t>(d.width) * d.height * 4u, 0u);

    const auto* base = static_cast<const uint8_t*>(mapped) +
                       static_cast<size_t>(d.footprint.Offset);
    const SIZE_T srcPitch = d.footprint.Footprint.RowPitch;
    const size_t tightPitch = static_cast<size_t>(d.width) * 4u;
    for (UINT row = 0; row < d.height; ++row) {
        std::memcpy(out.rgba.data() + static_cast<size_t>(row) * tightPitch,
                    base + static_cast<size_t>(row) * srcPitch, tightPitch);
    }
    const D3D12_RANGE noWrite = { 0, 0 };
    d.readback->Unmap(0, &noWrite);
    return true;
}

bool Renderer::readback_depth(std::vector<float>& out, uint32_t* width, uint32_t* height) {
    Impl& d = *impl_;
    if (d.windowed || !d.depth) {
        last_error_ = "readback_depth is only available in headless mode";
        return false;
    }
    const D3D12_RESOURCE_DESC dd = d.depth->GetDesc();  // D32_FLOAT
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT rows = 0; UINT64 rowSize = 0, total = 0;
    d.device->GetCopyableFootprints(&dd, 0, 1, 0, &fp, &rows, &rowSize, &total);

    // Transient readback buffer (gate-only path; not perf-critical).
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1;
    bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const D3D12_HEAP_PROPERTIES rbHeap = heap_props(D3D12_HEAP_TYPE_READBACK);
    ComPtr<ID3D12Resource> rb;
    if (FAILED(d.device->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))) {
        last_error_ = "depth readback buffer alloc failed"; return false;
    }

    if (FAILED(d.alloc->Reset())) { last_error_ = "allocator reset failed"; return false; }
    if (FAILED(d.list->Reset(d.alloc.Get(), nullptr))) { last_error_ = "command list reset failed"; return false; }
    const D3D12_RESOURCE_BARRIER toCopy = transition(d.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    d.list->ResourceBarrier(1, &toCopy);
    D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = rb.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = d.depth.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const D3D12_RESOURCE_BARRIER back = transition(d.depth.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    d.list->ResourceBarrier(1, &back);
    if (FAILED(d.list->Close())) { last_error_ = "command list close failed"; return false; }
    ID3D12CommandList* lists[] = { d.list.Get() };
    d.queue->ExecuteCommandLists(1, lists);
    const UINT64 v = ++d.fenceValue;
    if (FAILED(d.queue->Signal(d.fence.Get(), v))) { last_error_ = "fence Signal failed"; return false; }
    if (d.fence->GetCompletedValue() < v) {
        if (FAILED(d.fence->SetEventOnCompletion(v, d.fenceEvent))) { last_error_ = "SetEventOnCompletion failed"; return false; }
        if (WaitForSingleObject(d.fenceEvent, 5000) != WAIT_OBJECT_0) { last_error_ = "fence wait timed out"; return false; }
    }

    void* mapped = nullptr;
    const D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(total) };
    if (FAILED(rb->Map(0, &readRange, &mapped))) { last_error_ = "depth readback Map failed"; return false; }
    out.assign(static_cast<size_t>(d.width) * d.height, 1.0f);
    const auto* base = static_cast<const uint8_t*>(mapped) + static_cast<size_t>(fp.Offset);
    const SIZE_T srcPitch = fp.Footprint.RowPitch;
    for (UINT row = 0; row < d.height; ++row) {
        std::memcpy(out.data() + static_cast<size_t>(row) * d.width,
                    base + static_cast<size_t>(row) * srcPitch,
                    static_cast<size_t>(d.width) * sizeof(float));
    }
    const D3D12_RANGE noWrite = { 0, 0 };
    rb->Unmap(0, &noWrite);
    if (width) *width = d.width;
    if (height) *height = d.height;
    return true;
}

uint32_t Renderer::debug_error_count() {
    Impl& d = *impl_;
    if (!d.infoQueue) return 0;
    const UINT64 n = d.infoQueue->GetNumStoredMessages();
    uint32_t count = 0;
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        d.infoQueue->GetMessage(i, nullptr, &len);
        if (len == 0) continue;
        std::vector<char> buf(len);
        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (FAILED(d.infoQueue->GetMessage(i, msg, &len))) continue;
        if (msg->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
            msg->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
            msg->Severity == D3D12_MESSAGE_SEVERITY_WARNING) {
            ++count;
            std::fprintf(stderr, "[d3d12-debug] %.*s\n",
                         static_cast<int>(msg->DescriptionByteLength),
                         msg->pDescription);
        }
    }
    return count;
}

uint64_t Renderer::process_private_bytes() {
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                             sizeof(pmc))) {
        return static_cast<uint64_t>(pmc.PrivateUsage);
    }
    return 0;
}

// ===========================================================================
// M2 scene rendering: lit, depth-tested test-room geometry.
// ===========================================================================
namespace {

const char* kSceneHlsl = R"(
cbuffer Constants : register(b0) {
    row_major float4x4 uMVP;
    float3 uLightDir;
    float  uPad;
};
struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; };
struct VSOut { float4 clip : SV_POSITION; float3 nrm : NORMAL; };
VSOut VSMain(VSIn i) {
    VSOut o;
    o.clip = mul(float4(i.pos, 1.0), uMVP);
    o.nrm = i.nrm;
    return o;
}
float4 PSMain(VSOut i) : SV_TARGET {
    float3 n = normalize(i.nrm);
    float ndl = saturate(dot(n, -normalize(uLightDir)));
    float3 base = float3(0.85, 0.80, 0.55);   // backrooms wallpaper tone
    float3 col = base * (0.25 + 0.75 * ndl);
    return float4(col, 1.0);
}
)";

struct Mat4 { float m[16]; };  // row-major; clip = v * M

Mat4 mat_mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    }
    return r;
}

Mat4 view_lh(const float eye[3], float yaw, float pitch) {
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    float z[3] = { std::sin(yaw) * cp, sp, std::cos(yaw) * cp };
    const float zl = std::sqrt(z[0]*z[0] + z[1]*z[1] + z[2]*z[2]);
    for (float& c : z) c /= zl;
    // x = normalize(cross(up=(0,1,0), z)) = normalize(z.z, 0, -z.x)
    float x[3] = { z[2], 0.0f, -z[0] };
    const float xl = std::sqrt(x[0]*x[0] + x[2]*x[2]);
    x[0] /= xl; x[2] /= xl;
    // y = cross(z, x)
    float y[3] = { z[1]*x[2] - z[2]*x[1], z[2]*x[0] - z[0]*x[2], z[0]*x[1] - z[1]*x[0] };
    auto d3 = [](const float* a, const float* b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; };
    Mat4 v{};
    v.m[0]=x[0]; v.m[1]=y[0]; v.m[2]=z[0]; v.m[3]=0.0f;
    v.m[4]=x[1]; v.m[5]=y[1]; v.m[6]=z[1]; v.m[7]=0.0f;
    v.m[8]=x[2]; v.m[9]=y[2]; v.m[10]=z[2]; v.m[11]=0.0f;
    v.m[12]=-d3(x,eye); v.m[13]=-d3(y,eye); v.m[14]=-d3(z,eye); v.m[15]=1.0f;
    return v;
}

Mat4 proj_lh(float fov_y, float aspect, float n, float f) {
    const float ys = 1.0f / std::tan(fov_y * 0.5f);
    const float xs = ys / aspect;
    const float q = f / (f - n);
    Mat4 p{};
    p.m[0]=xs; p.m[5]=ys; p.m[10]=q; p.m[11]=1.0f; p.m[14]=-n*q*1.0f; p.m[15]=0.0f;
    return p;
}

void push_quad(std::vector<float>& v, const float a[3], const float b[3],
               const float c[3], const float dd[3], const float nrm[3]) {
    const float* tri[6] = { a, b, c, a, c, dd };
    for (const float* p : tri) {
        v.push_back(p[0]); v.push_back(p[1]); v.push_back(p[2]);
        v.push_back(nrm[0]); v.push_back(nrm[1]); v.push_back(nrm[2]);
    }
}

void push_box(std::vector<float>& v, const contracts::BoxInstance& box) {
    const float x0=box.mn[0], y0=box.mn[1], z0=box.mn[2];
    const float x1=box.mx[0], y1=box.mx[1], z1=box.mx[2];
    const float c000[3]={x0,y0,z0}, c001[3]={x0,y0,z1}, c010[3]={x0,y1,z0}, c011[3]={x0,y1,z1};
    const float c100[3]={x1,y0,z0}, c101[3]={x1,y0,z1}, c110[3]={x1,y1,z0}, c111[3]={x1,y1,z1};
    const float nxn[3]={-1,0,0}, nxp[3]={1,0,0}, nyn[3]={0,-1,0}, nyp[3]={0,1,0}, nzn[3]={0,0,-1}, nzp[3]={0,0,1};
    push_quad(v, c000, c001, c011, c010, nxn);  // -X
    push_quad(v, c100, c110, c111, c101, nxp);  // +X
    push_quad(v, c000, c100, c101, c001, nyn);  // -Y
    push_quad(v, c010, c011, c111, c110, nyp);  // +Y
    push_quad(v, c000, c010, c110, c100, nzn);  // -Z
    push_quad(v, c001, c101, c111, c011, nzp);  // +Z
}

bool compile_shader(const char* src, const char* entry, const char* target,
                    ComPtr<ID3DBlob>& out, std::string& err) {
    ComPtr<ID3DBlob> errBlob;
    const HRESULT hr = D3DCompile(src, std::strlen(src), "shader", nullptr,
                                  nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &errBlob);
    if (FAILED(hr)) {
        err = "shader compile failed";
        if (errBlob) err += std::string(": ") + static_cast<const char*>(errBlob->GetBufferPointer());
        return false;
    }
    return true;
}

bool ensure_scene_pipeline(Renderer::Impl& d, std::string& err) {
    if (d.sceneReady) return true;

    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.ShaderRegister = 0;
    param.Constants.RegisterSpace = 0;
    param.Constants.Num32BitValues = 20;  // 16 (mvp) + 3 (light) + 1 (pad)
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 1;
    rs.pParameters = &param;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig, sigErr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigErr))) {
        err = "SerializeRootSignature failed";
        return false;
    }
    if (FAILED(d.device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                             IID_PPV_ARGS(&d.sceneRoot)))) {
        err = "CreateRootSignature failed";
        return false;
    }

    ComPtr<ID3DBlob> vs, ps;
    if (!compile_shader(kSceneHlsl, "VSMain", "vs_5_0", vs, err)) return false;
    if (!compile_shader(kSceneHlsl, "PSMain", "ps_5_0", ps, err)) return false;

    D3D12_INPUT_ELEMENT_DESC layout[2] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = d.sceneRoot.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.SampleMask = UINT_MAX;
    pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.InputLayout = { layout, 2 };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kFormat;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    if (FAILED(d.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&d.scenePso)))) {
        err = "CreateGraphicsPipelineState failed";
        return false;
    }
    d.sceneReady = true;
    return true;
}

bool build_vertex_buffer(Renderer::Impl& d, const contracts::WorldView& view, std::string& err) {
    std::vector<float> verts;
    for (const contracts::BoxInstance& b : view.boxes) push_box(verts, b);
    if (verts.empty()) { err = "world view has no geometry"; return false; }

    const size_t bytes = verts.size() * sizeof(float);
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const D3D12_HEAP_PROPERTIES up = heap_props(D3D12_HEAP_TYPE_UPLOAD);
    if (FAILED(d.device->CreateCommittedResource(
            &up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&d.vb)))) {
        err = "CreateCommittedResource(VB) failed";
        return false;
    }
    void* mapped = nullptr;
    const D3D12_RANGE none = { 0, 0 };
    if (FAILED(d.vb->Map(0, &none, &mapped))) { err = "VB Map failed"; return false; }
    std::memcpy(mapped, verts.data(), bytes);
    d.vb->Unmap(0, nullptr);

    d.vbView.BufferLocation = d.vb->GetGPUVirtualAddress();
    d.vbView.SizeInBytes = static_cast<UINT>(bytes);
    d.vbView.StrideInBytes = 6 * sizeof(float);
    d.vertexCount = static_cast<UINT>(verts.size() / 6);
    return true;
}

const char* kChunkHlsl = R"(
cbuffer Constants : register(b0) {
    row_major float4x4 uMVP;
    float3 uLightDir;
    float  uTopDownDiscard;   // >0.5: hide ceiling/fluorescent (top-down debug)
};
struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; float3 col : COLOR; float2 uv : TEXCOORD0; float mat : TEXCOORD1; };
struct VSOut { float4 clip : SV_POSITION; float3 nrm : NORMAL; float3 col : COLOR; float mat : TEXCOORD1; };
VSOut VSMain(VSIn i) {
    VSOut o;
    o.clip = mul(float4(i.pos, 1.0), uMVP);
    o.nrm = i.nrm;
    o.col = i.col;
    o.mat = i.mat;
    return o;
}
float4 PSMain(VSOut i) : SV_TARGET {
    if (uTopDownDiscard > 0.5 && i.mat >= 1.5) discard;
    float ndl = saturate(dot(normalize(i.nrm), -normalize(uLightDir)));
    return float4(i.col * (0.30 + 0.70 * ndl), 1.0);
}
)";

void fill_opaque_pso_states(D3D12_GRAPHICS_PIPELINE_STATE_DESC& pso) {
    pso.SampleMask = UINT_MAX;
    pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kFormat;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
}

bool ensure_chunk_pipeline(Renderer::Impl& d, std::string& err) {
    if (d.chunkReady) return true;

    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.Num32BitValues = 20;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 1;
    rs.pParameters = &param;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> sig, sigErr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigErr))) {
        err = "chunk SerializeRootSignature failed";
        return false;
    }
    if (FAILED(d.device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                             IID_PPV_ARGS(&d.chunkRoot)))) {
        err = "chunk CreateRootSignature failed";
        return false;
    }
    ComPtr<ID3DBlob> vs, ps;
    if (!compile_shader(kChunkHlsl, "VSMain", "vs_5_0", vs, err)) return false;
    if (!compile_shader(kChunkHlsl, "PSMain", "ps_5_0", ps, err)) return false;

    D3D12_INPUT_ELEMENT_DESC layout[5] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT,       0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = d.chunkRoot.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.InputLayout = { layout, 5 };
    fill_opaque_pso_states(pso);
    if (FAILED(d.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&d.chunkPso)))) {
        err = "chunk CreateGraphicsPipelineState failed";
        return false;
    }
    d.chunkReady = true;
    return true;
}

bool upload_chunk_mesh(Renderer::Impl& d, const contracts::ResidentChunk& rc, std::string& err) {
    const size_t bytes = static_cast<size_t>(rc.vertex_count) * sizeof(contracts::ChunkVertex);
    if (bytes == 0) return true;
    if (bytes > kChunkSlotCapacityBytes) { err = "chunk exceeds pool slot capacity"; return false; }

    uint32_t slot;
    if (!d.chunkFree.empty()) {
        slot = d.chunkFree.back();
        d.chunkFree.pop_back();
    } else {
        // Grow the pool: allocate + persistently map a fresh upload buffer.
        ChunkSlot fresh;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = kChunkSlotCapacityBytes;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const D3D12_HEAP_PROPERTIES up = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        if (FAILED(d.device->CreateCommittedResource(
                &up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr, IID_PPV_ARGS(&fresh.vb)))) {
            err = "chunk pool VB create failed";
            return false;
        }
        const D3D12_RANGE none = { 0, 0 };
        if (FAILED(fresh.vb->Map(0, &none, &fresh.mapped))) { err = "chunk pool VB map failed"; return false; }
        fresh.view.StrideInBytes = sizeof(contracts::ChunkVertex);
        d.chunkPool.push_back(std::move(fresh));
        slot = static_cast<uint32_t>(d.chunkPool.size() - 1);
    }

    ChunkSlot& cs = d.chunkPool[slot];
    std::memcpy(cs.mapped, rc.vertices, bytes);              // steady-state: memcpy only
    cs.view.BufferLocation = cs.vb->GetGPUVirtualAddress();
    cs.view.SizeInBytes = static_cast<UINT>(bytes);
    cs.view.StrideInBytes = sizeof(contracts::ChunkVertex);
    cs.vertex_count = rc.vertex_count;
    d.chunkSlotOf[rc.key] = slot;
    return true;
}

const char* kLitHlsl = R"(
cbuffer Constants : register(b0) {
    row_major float4x4 uMVP;
    float4 uPad0;
};
cbuffer Lights : register(b1) {
    float4 uAmbientCount;   // rgb = flat ambient, w = active light count
    float4 uLights[64];     // xyz = world pos, w = intensity (flicker-scaled)
};
Texture2DArray gTex : register(t0);
SamplerState   gSamp : register(s0);
struct VSIn  { float3 pos:POSITION; float3 nrm:NORMAL; float3 col:COLOR; float2 uv:TEXCOORD0; float mat:TEXCOORD1; };
struct VSOut { float4 clip:SV_POSITION; float3 wpos:TEXCOORD2; float3 wnrm:NORMAL; float3 col:COLOR; float2 uv:TEXCOORD0; float mat:TEXCOORD1; };
VSOut VSMain(VSIn i) {
    VSOut o;
    o.clip = mul(float4(i.pos, 1.0), uMVP);
    o.wpos = i.pos;                 // chunk geometry is world-space
    o.wnrm = i.nrm; o.col = i.col; o.uv = i.uv; o.mat = i.mat;
    return o;
}
float4 PSMain(VSOut i) : SV_TARGET {
    float3 tex = gTex.Sample(gSamp, float3(i.uv, i.mat)).rgb;
    float m = max(i.col.r, max(i.col.g, i.col.b));
    float3 tint = (m > 0.001) ? (i.col / m) : float3(1,1,1);
    float3 albedo = tex * lerp(float3(1,1,1), tint, 0.5);
    if (i.mat >= 2.5 && i.mat < 3.5) return float4(albedo, 1.0);   // fluorescent: emissive

    float3 N = normalize(i.wnrm);
    float sum = 0.0;
    int count = (int)uAmbientCount.w;
    [loop] for (int k = 0; k < count; ++k) {
        float3 d = uLights[k].xyz - i.wpos;
        float dist = length(d);
        float ndl = saturate(dot(N, d / max(dist, 0.001)));
        float atten = uLights[k].w / (1.0 + 0.09 * dist + 0.05 * dist * dist);
        sum += ndl * atten;
    }
    // Highlight knee: keep the wallpaper's hue under stacked lights instead of
    // clipping to pure white (compress everything above unity exposure).
    float lit = uAmbientCount.r + sum;
    if (lit > 1.0) lit = 1.0 + (lit - 1.0) * 0.18;
    return float4(albedo * lit, 1.0);
}
)";

bool upload_textures(Renderer::Impl& d, uint64_t seed, std::string& err) {
    const UINT slices = kTexCount;
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = static_cast<UINT64>(kTexSize);
    td.Height = static_cast<UINT>(kTexSize);
    td.DepthOrArraySize = static_cast<UINT16>(slices);
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    const D3D12_HEAP_PROPERTIES def = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    const HRESULT thr = d.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&d.texArray));
    if (FAILED(thr)) {
        const HRESULT rr = d.device ? d.device->GetDeviceRemovedReason() : 0;
        char hb[96]; std::snprintf(hb, sizeof(hb), "texture array create failed (hr=0x%08lX removed=0x%08lX)",
                                   static_cast<unsigned long>(thr), static_cast<unsigned long>(rr));
        err = hb; return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[kTexCount] = {};
    UINT rows[kTexCount] = {};
    UINT64 rowBytes[kTexCount] = {};
    UINT64 total = 0;
    d.device->GetCopyableFootprints(&td, 0, slices, 0, fp, rows, rowBytes, &total);

    ComPtr<ID3D12Resource> upload;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const D3D12_HEAP_PROPERTIES up = heap_props(D3D12_HEAP_TYPE_UPLOAD);
    if (FAILED(d.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))) {
        err = "texture upload buffer failed"; return false;
    }
    uint8_t* mapped = nullptr;
    const D3D12_RANGE none = { 0, 0 };
    if (FAILED(upload->Map(0, &none, reinterpret_cast<void**>(&mapped)))) { err = "texture map failed"; return false; }
    std::vector<uint8_t> rgba;
    for (UINT s = 0; s < slices; ++s) {
        generate_texture(static_cast<TexKind>(s), seed, rgba);
        const UINT64 rp = fp[s].Footprint.RowPitch;
        for (UINT y = 0; y < static_cast<UINT>(kTexSize); ++y) {
            std::memcpy(mapped + fp[s].Offset + static_cast<size_t>(y) * rp,
                        rgba.data() + static_cast<size_t>(y) * kTexSize * 4u,
                        static_cast<size_t>(kTexSize) * 4u);
        }
    }
    upload->Unmap(0, nullptr);

    if (FAILED(d.alloc->Reset())) { err = "tex alloc reset"; return false; }
    if (FAILED(d.list->Reset(d.alloc.Get(), nullptr))) { err = "tex list reset"; return false; }
    for (UINT s = 0; s < slices; ++s) {
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = d.texArray.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = s;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp[s];
        d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    const D3D12_RESOURCE_BARRIER toSrv = transition(
        d.texArray.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    d.list->ResourceBarrier(1, &toSrv);
    if (FAILED(d.list->Close())) { err = "tex list close"; return false; }
    ID3D12CommandList* lists[] = { d.list.Get() };
    d.queue->ExecuteCommandLists(1, lists);
    const UINT64 v = ++d.fenceValue;
    d.queue->Signal(d.fence.Get(), v);
    if (d.fence->GetCompletedValue() < v) {
        d.fence->SetEventOnCompletion(v, d.fenceEvent);
        WaitForSingleObject(d.fenceEvent, 5000);
    }

    if (!d.srvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.NumDescriptors = 1;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(d.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&d.srvHeap)))) { err = "srv heap"; return false; }
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2DArray.MipLevels = 1;
    sd.Texture2DArray.ArraySize = slices;
    d.device->CreateShaderResourceView(d.texArray.Get(), &sd,
                                       d.srvHeap->GetCPUDescriptorHandleForHeapStart());
    d.texSeed = seed;
    d.texUploaded = true;
    return true;
}

bool ensure_lit_pipeline(Renderer::Impl& d, uint64_t seed, std::string& err) {
    if (!d.texUploaded || d.texSeed != seed) {
        if (!upload_textures(d, seed, err)) return false;
    }
    if (d.litReady) return true;

    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 20;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[2].Descriptor.ShaderRegister = 1;  // b1 (lights)
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 3;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &samp;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> sig, sigErr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigErr))) { err = "lit SerializeRootSignature"; return false; }
    if (FAILED(d.device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&d.litRoot)))) { err = "lit CreateRootSignature"; return false; }

    ComPtr<ID3DBlob> vs, ps;
    if (!compile_shader(kLitHlsl, "VSMain", "vs_5_0", vs, err)) return false;
    if (!compile_shader(kLitHlsl, "PSMain", "ps_5_0", ps, err)) return false;
    D3D12_INPUT_ELEMENT_DESC layout[5] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT,       0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = d.litRoot.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.InputLayout = { layout, 5 };
    fill_opaque_pso_states(pso);
    if (FAILED(d.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&d.litPso)))) { err = "lit pso"; return false; }
    d.litReady = true;
    return true;
}

// ----- VHS post-process pass (M8) -------------------------------------------
const char* kPostHlsl = R"(
cbuffer P : register(b0) {
    float2 uRes; float uTime;
    float uGrain; float uAberr; float uDistort; float uScan; float uVignette;
    uint uSeed; uint uHudOn; float2 uPad;
};
Texture2D    gScene : register(t0);
Texture2D    gHud   : register(t1);
SamplerState gSamp  : register(s0);

struct VOut { float4 pos:SV_POSITION; float2 uv:TEXCOORD0; };
VOut VSMain(uint id : SV_VertexID) {
    VOut o;
    float2 t = float2((id << 1) & 2, id & 2);   // fullscreen triangle: (0,0)(2,0)(0,2)
    o.uv = t;
    o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
float h12(uint x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return float(x & 0x00ffffffu) / 16777216.0;
}
float4 PSMain(VOut i) : SV_TARGET {
    float2 uv = i.uv;
    float2 c = uv * 2.0 - 1.0;
    float r2 = dot(c, c);
    // Barrel distortion of the *scene* sample coords.
    float2 duv = (c * (1.0 + uDistort * r2)) * 0.5 + 0.5;
    float2 dir = duv - 0.5;
    float3 col;
    col.r = gScene.Sample(gSamp, duv + dir * uAberr).r;  // chromatic aberration
    col.g = gScene.Sample(gSamp, duv).g;
    col.b = gScene.Sample(gSamp, duv - dir * uAberr).b;
    if (duv.x < 0.0 || duv.x > 1.0 || duv.y < 0.0 || duv.y > 1.0) col = float3(0,0,0);
    // Screen-space CRT effects (use undistorted output coords).
    float row = uv.y * uRes.y;
    float scan = 1.0 - uScan * (0.5 + 0.5 * cos(row * 6.2831853));
    float inter = 1.0 - uScan * 0.25 * step(0.5, frac(row * 0.5 + floor(uTime * 30.0) * 0.5));
    col *= scan * inter;
    // Seeded film grain (per pixel, stepped ~60 Hz so a fixed time -> fixed grain).
    uint px = (uint)(uv.x * uRes.x);
    uint py = (uint)(uv.y * uRes.y);
    uint tt = (uint)(uTime * 60.0);
    float g = h12(px * 73856093u ^ py * 19349663u ^ (uSeed + tt) * 83492791u);
    col += (g - 0.5) * uGrain;
    // Vignette.
    col *= 1.0 - uVignette * r2 * 0.5;
    col = saturate(col);
    // HUD overlay (screen-space, undistorted, alpha-over) — kept crisp.
    if (uHudOn > 0u) {
        float4 hud = gHud.Sample(gSamp, uv);
        col = lerp(col, hud.rgb, hud.a);
    }
    return float4(col, 1.0);
}
)";

// Upload `rgba` (width*height*4) into the HUD overlay texture, leaving it in
// PIXEL_SHADER_RESOURCE. Reused for the 1x1-ish zero placeholder and the real
// per-frame HUD (M8 p2). Runs its own fenced command list (setup-time, rare).
bool upload_hud(Renderer::Impl& d, const uint8_t* rgba, std::string& err) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT rows = 0; UINT64 rowSize = 0, total = 0;
    D3D12_RESOURCE_DESC td = d.hudTex->GetDesc();
    d.device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &rowSize, &total);
    if (!d.hudUpload) {
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const D3D12_HEAP_PROPERTIES up = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        if (FAILED(d.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&d.hudUpload)))) {
            err = "hud upload buffer"; return false;
        }
    }
    uint8_t* mapped = nullptr;
    const D3D12_RANGE none = { 0, 0 };
    if (FAILED(d.hudUpload->Map(0, &none, reinterpret_cast<void**>(&mapped)))) { err = "hud map"; return false; }
    for (UINT y = 0; y < d.hudH; ++y)
        std::memcpy(mapped + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch,
                    rgba + static_cast<size_t>(y) * d.hudW * 4u, static_cast<size_t>(d.hudW) * 4u);
    d.hudUpload->Unmap(0, nullptr);

    if (FAILED(d.alloc->Reset())) { err = "hud alloc reset"; return false; }
    if (FAILED(d.list->Reset(d.alloc.Get(), nullptr))) { err = "hud list reset"; return false; }
    const D3D12_RESOURCE_BARRIER toCopy = transition(
        d.hudTex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    d.list->ResourceBarrier(1, &toCopy);
    D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = d.hudTex.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = d.hudUpload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
    d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const D3D12_RESOURCE_BARRIER toSrv = transition(
        d.hudTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    d.list->ResourceBarrier(1, &toSrv);
    if (FAILED(d.list->Close())) { err = "hud list close"; return false; }
    ID3D12CommandList* lists[] = { d.list.Get() };
    d.queue->ExecuteCommandLists(1, lists);
    const UINT64 v = ++d.fenceValue;
    d.queue->Signal(d.fence.Get(), v);
    if (d.fence->GetCompletedValue() < v) {
        d.fence->SetEventOnCompletion(v, d.fenceEvent);
        WaitForSingleObject(d.fenceEvent, 5000);
    }
    return true;
}

bool ensure_post_pipeline(Renderer::Impl& d, std::string& err) {
    if (d.postReady) return true;

    // Post render target (same size/format as the scene), RTV at rtvHeap slot 1.
    D3D12_RESOURCE_DESC rtd = {};
    rtd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtd.Width = d.width; rtd.Height = d.height; rtd.DepthOrArraySize = 1; rtd.MipLevels = 1;
    rtd.Format = kFormat; rtd.SampleDesc.Count = 1; rtd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rtd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = {}; cv.Format = kFormat; std::memcpy(cv.Color, kClearFloat, sizeof(kClearFloat));
    const D3D12_HEAP_PROPERTIES defHeap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(d.device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &rtd,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&d.postRt)))) {
        err = "post RT create"; return false;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE postRtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    postRtv.ptr += static_cast<SIZE_T>(d.rtvDescSize);  // slot 1
    d.device->CreateRenderTargetView(d.postRt.Get(), nullptr, postRtv);

    // HUD overlay texture (full-res), created in PSR; zero-initialised (transparent).
    d.hudW = d.width; d.hudH = d.height;
    D3D12_RESOURCE_DESC hd = {};
    hd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    hd.Width = d.hudW; hd.Height = d.hudH; hd.DepthOrArraySize = 1; hd.MipLevels = 1;
    hd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; hd.SampleDesc.Count = 1; hd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (FAILED(d.device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &hd,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&d.hudTex)))) {
        err = "hud tex create"; return false;
    }
    {
        std::vector<uint8_t> zero(static_cast<size_t>(d.hudW) * d.hudH * 4u, 0u);
        if (!upload_hud(d, zero.data(), err)) return false;
    }

    // Shader-visible SRV heap: [0] = scene, [1] = HUD.
    D3D12_DESCRIPTOR_HEAP_DESC sh = {};
    sh.NumDescriptors = 2; sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(d.device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&d.postSrvHeap)))) { err = "post srv heap"; return false; }
    const UINT inc = d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = kFormat; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE sc0 = d.postSrvHeap->GetCPUDescriptorHandleForHeapStart();
    d.device->CreateShaderResourceView(d.rt.Get(), &sd, sc0);          // scene
    D3D12_CPU_DESCRIPTOR_HANDLE sc1 = sc0; sc1.ptr += inc;
    d.device->CreateShaderResourceView(d.hudTex.Get(), &sd, sc1);      // HUD

    // Root signature: b0 32-bit constants (12) + SRV table (t0,t1) + static sampler.
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 2;
    range.BaseShaderRegister = 0; range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 12; params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1; params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ShaderRegister = 0; samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL; samp.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 2; rs.pParameters = params; rs.NumStaticSamplers = 1; rs.pStaticSamplers = &samp;
    ComPtr<ID3DBlob> sig, sigErr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigErr))) { err = "post root serialize"; return false; }
    if (FAILED(d.device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&d.postRoot)))) { err = "post root create"; return false; }

    ComPtr<ID3DBlob> vs, ps;
    if (!compile_shader(kPostHlsl, "VSMain", "vs_5_0", vs, err)) return false;
    if (!compile_shader(kPostHlsl, "PSMain", "ps_5_0", ps, err)) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = d.postRoot.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.SampleMask = UINT_MAX;
    pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.DepthStencilState.DepthEnable = FALSE;       // fullscreen pass, no depth
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1; pso.RTVFormats[0] = kFormat;
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN; pso.SampleDesc.Count = 1;
    if (FAILED(d.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&d.postPso)))) { err = "post pso"; return false; }
    d.postReady = true;
    return true;
}

// Final stage of a headless render: either a straight copy of the scene RT to the
// readback buffer (post off), or the VHS post pass scene -> postRt -> readback
// (post on). Records into the already-open command list `d.list`.
void finalize_to_readback(Renderer::Impl& d, bool post) {
    ID3D12Resource* copySrc = d.rt.Get();
    if (post && d.postReady) {
        const D3D12_RESOURCE_BARRIER toSrv = transition(
            d.rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        d.list->ResourceBarrier(1, &toSrv);
        D3D12_CPU_DESCRIPTOR_HANDLE postRtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        postRtv.ptr += static_cast<SIZE_T>(d.rtvDescSize);
        d.list->OMSetRenderTargets(1, &postRtv, FALSE, nullptr);
        D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(d.width), static_cast<float>(d.height), 0.0f, 1.0f };
        D3D12_RECT sc = { 0, 0, static_cast<LONG>(d.width), static_cast<LONG>(d.height) };
        d.list->RSSetViewports(1, &vp);
        d.list->RSSetScissorRects(1, &sc);
        ID3D12DescriptorHeap* heaps[] = { d.postSrvHeap.Get() };
        d.list->SetDescriptorHeaps(1, heaps);
        d.list->SetGraphicsRootSignature(d.postRoot.Get());
        d.list->SetPipelineState(d.postPso.Get());
        d.list->SetGraphicsRoot32BitConstants(0, 12, &d.postParams, 0);
        d.list->SetGraphicsRootDescriptorTable(1, d.postSrvHeap->GetGPUDescriptorHandleForHeapStart());
        d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d.list->DrawInstanced(3, 1, 0, 0);
        const D3D12_RESOURCE_BARRIER postToCopy = transition(
            d.postRt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        d.list->ResourceBarrier(1, &postToCopy);
        copySrc = d.postRt.Get();
    } else {
        const D3D12_RESOURCE_BARRIER toCopy = transition(
            d.rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        d.list->ResourceBarrier(1, &toCopy);
    }

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = d.readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = d.footprint;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = copySrc; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    if (post && d.postReady) {
        const D3D12_RESOURCE_BARRIER postBack = transition(
            d.postRt.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        d.list->ResourceBarrier(1, &postBack);
        const D3D12_RESOURCE_BARRIER sceneBack = transition(
            d.rt.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        d.list->ResourceBarrier(1, &sceneBack);
    } else {
        const D3D12_RESOURCE_BARRIER backToRt = transition(
            d.rt.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        d.list->ResourceBarrier(1, &backToRt);
    }
}

}  // namespace

bool Renderer::render_world_view(const contracts::WorldView& view) {
    Impl& d = *impl_;
    if (d.windowed || !d.rt) {
        last_error_ = "render_world_view is headless-only (M2)";
        return false;
    }
    if (!ensure_scene_pipeline(d, last_error_)) return false;
    if (!build_vertex_buffer(d, view, last_error_)) return false;

    // MVP from the camera; fixed light direction.
    const auto& cam = view.camera;
    const Mat4 mvp = mat_mul(view_lh(cam.pos, cam.yaw, cam.pitch),
                             proj_lh(cam.fov_y, cam.aspect, 0.05f, 100.0f));
    float constants[20];
    std::memcpy(constants, mvp.m, sizeof(mvp.m));
    float ld[3] = { -0.3f, -1.0f, -0.2f };
    const float ll = std::sqrt(ld[0]*ld[0] + ld[1]*ld[1] + ld[2]*ld[2]);
    constants[16] = ld[0]/ll; constants[17] = ld[1]/ll; constants[18] = ld[2]/ll;
    constants[19] = 0.0f;

    if (FAILED(d.alloc->Reset())) { last_error_ = "allocator reset failed"; return false; }
    if (FAILED(d.list->Reset(d.alloc.Get(), nullptr))) {
        last_error_ = "command list reset failed";
        return false;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = d.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    d.list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    d.list->ClearRenderTargetView(rtv, kClearFloat, 0, nullptr);
    d.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(d.width), static_cast<float>(d.height), 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, static_cast<LONG>(d.width), static_cast<LONG>(d.height) };
    d.list->RSSetViewports(1, &vp);
    d.list->RSSetScissorRects(1, &sc);

    d.list->SetGraphicsRootSignature(d.sceneRoot.Get());
    d.list->SetPipelineState(d.scenePso.Get());
    d.list->SetGraphicsRoot32BitConstants(0, 20, constants, 0);
    d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d.list->IASetVertexBuffers(0, 1, &d.vbView);
    d.list->DrawInstanced(d.vertexCount, 1, 0, 0);

    // Offscreen target -> readback (same as the clear path).
    const D3D12_RESOURCE_BARRIER toCopy = transition(
        d.rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    d.list->ResourceBarrier(1, &toCopy);
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = d.readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = d.footprint;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = d.rt.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const D3D12_RESOURCE_BARRIER back = transition(
        d.rt.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    d.list->ResourceBarrier(1, &back);

    if (FAILED(d.list->Close())) { last_error_ = "command list close failed"; return false; }
    ID3D12CommandList* lists[] = { d.list.Get() };
    d.queue->ExecuteCommandLists(1, lists);

    const UINT64 v = ++d.fenceValue;
    if (FAILED(d.queue->Signal(d.fence.Get(), v))) { last_error_ = "fence Signal failed"; return false; }
    if (d.fence->GetCompletedValue() < v) {
        if (FAILED(d.fence->SetEventOnCompletion(v, d.fenceEvent))) {
            last_error_ = "SetEventOnCompletion failed";
            return false;
        }
        if (WaitForSingleObject(d.fenceEvent, 5000) != WAIT_OBJECT_0) {
            last_error_ = "fence wait timed out";
            return false;
        }
    }
    return true;
}

void Renderer::set_texture_seed(uint64_t seed) {
    if (impl_) impl_->pendingTexSeed = seed;
}

void Renderer::set_post(bool enabled, uint32_t seed, float time, bool hud, bool clean) {
    if (!impl_) return;
    impl_->postEnabled = enabled;
    impl_->postParams.seed = seed;
    impl_->postParams.time = time;
    impl_->postParams.hud = hud ? 1u : 0u;
    if (clean) {  // HUD-only: zero the VHS effects -> a crisp overlay over the untouched scene (subtitles)
        impl_->postParams.grain = 0.0f; impl_->postParams.aberration = 0.0f; impl_->postParams.distortion = 0.0f;
        impl_->postParams.scanline = 0.0f; impl_->postParams.vignette = 0.0f;
    } else {      // the full VHS look (M8 defaults)
        impl_->postParams.grain = 0.08f; impl_->postParams.aberration = 0.0025f; impl_->postParams.distortion = 0.06f;
        impl_->postParams.scanline = 0.18f; impl_->postParams.vignette = 0.35f;
    }
}

bool Renderer::upload_hud_overlay(const uint8_t* rgba, uint32_t width, uint32_t height) {
    if (!impl_) return false;
    Impl& d = *impl_;
    if (!ensure_post_pipeline(d, last_error_)) return false;
    if (width != d.hudW || height != d.hudH) { last_error_ = "hud overlay size mismatch"; return false; }
    return upload_hud(d, rgba, last_error_);
}

// --- Windowed overlay present (M15 menus) ------------------------------------
// A CPU-rasterised RGBA overlay (the menu) blitted to the back buffer by a single
// fullscreen triangle. Its own texture/heap/root-sig/PSO — independent of the post
// pass (which would collide with the windowed RTV heap), so headless goldens are
// untouched. The menu overlay fills the frame, so the PS just writes its RGB opaque.
static const char* kOverlayHlsl = R"(
Texture2D    gOvl  : register(t0);
SamplerState gSamp : register(s0);
struct VOut { float4 pos:SV_POSITION; float2 uv:TEXCOORD0; };
VOut VSMain(uint id : SV_VertexID) {
    VOut o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.uv = t;
    o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
float4 PSMain(VOut i) : SV_TARGET {
    float4 c = gOvl.Sample(gSamp, i.uv);
    return float4(c.rgb, 1.0);
}
float4 PSBlend(VOut i) : SV_TARGET {   // keep the sampled alpha -> alpha-over the world (transparent = world shows)
    return gOvl.Sample(gSamp, i.uv);
}
)";

bool upload_overlay(Renderer::Impl& d, const uint8_t* rgba, std::string& err) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT rows = 0; UINT64 rowSize = 0, total = 0;
    D3D12_RESOURCE_DESC td = d.ovlTex->GetDesc();
    d.device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &rowSize, &total);
    if (!d.ovlUpload) {
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const D3D12_HEAP_PROPERTIES up = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        if (FAILED(d.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&d.ovlUpload)))) {
            err = "overlay upload buffer"; return false;
        }
    }
    uint8_t* mapped = nullptr;
    const D3D12_RANGE none = { 0, 0 };
    if (FAILED(d.ovlUpload->Map(0, &none, reinterpret_cast<void**>(&mapped)))) { err = "overlay map"; return false; }
    for (UINT y = 0; y < d.ovlH; ++y)
        std::memcpy(mapped + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch,
                    rgba + static_cast<size_t>(y) * d.ovlW * 4u, static_cast<size_t>(d.ovlW) * 4u);
    d.ovlUpload->Unmap(0, nullptr);

    if (FAILED(d.alloc->Reset())) { err = "overlay alloc reset"; return false; }
    if (FAILED(d.list->Reset(d.alloc.Get(), nullptr))) { err = "overlay list reset"; return false; }
    const D3D12_RESOURCE_BARRIER toCopy = transition(
        d.ovlTex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    d.list->ResourceBarrier(1, &toCopy);
    D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = d.ovlTex.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = d.ovlUpload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
    d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const D3D12_RESOURCE_BARRIER toSrv = transition(
        d.ovlTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    d.list->ResourceBarrier(1, &toSrv);
    if (FAILED(d.list->Close())) { err = "overlay list close"; return false; }
    ID3D12CommandList* lists[] = { d.list.Get() };
    d.queue->ExecuteCommandLists(1, lists);
    const UINT64 v = ++d.fenceValue;
    d.queue->Signal(d.fence.Get(), v);
    if (d.fence->GetCompletedValue() < v) {
        d.fence->SetEventOnCompletion(v, d.fenceEvent);
        WaitForSingleObject(d.fenceEvent, 5000);
    }
    return true;
}

bool ensure_overlay_pipeline(Renderer::Impl& d, uint32_t srcW, uint32_t srcH, std::string& err) {
    // The overlay texture is the SOURCE size (M19: may be < window for upscaled RT);
    // rebuild it if the requested source size changed. The PSO/root sig are reusable.
    if (d.ovlReady && d.ovlW == srcW && d.ovlH == srcH) return true;
    if (d.ovlReady && (d.ovlW != srcW || d.ovlH != srcH)) {
        d.ovlTex.Reset(); d.ovlUpload.Reset(); d.ovlSrvHeap.Reset();  // resize the source texture
    }
    const bool first = !d.ovlReady;
    d.ovlW = srcW; d.ovlH = srcH;
    const D3D12_HEAP_PROPERTIES defHeap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC hd = {};
    hd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    hd.Width = d.ovlW; hd.Height = d.ovlH; hd.DepthOrArraySize = 1; hd.MipLevels = 1;
    hd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; hd.SampleDesc.Count = 1; hd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (FAILED(d.device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &hd,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&d.ovlTex)))) {
        err = "overlay tex create"; return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC sh = {};
    sh.NumDescriptors = 1; sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(d.device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&d.ovlSrvHeap)))) { err = "overlay srv heap"; return false; }
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    d.device->CreateShaderResourceView(d.ovlTex.Get(), &sd, d.ovlSrvHeap->GetCPUDescriptorHandleForHeapStart());

    if (!first) { d.ovlReady = true; return true; }  // resize only rebuilds the texture + SRV

    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 1;
    range.BaseShaderRegister = 0; range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER params[1] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1; params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ShaderRegister = 0; samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL; samp.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 1; rs.pParameters = params; rs.NumStaticSamplers = 1; rs.pStaticSamplers = &samp;
    ComPtr<ID3DBlob> sig, sigErr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigErr))) { err = "overlay root serialize"; return false; }
    if (FAILED(d.device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&d.ovlRoot)))) { err = "overlay root create"; return false; }

    ComPtr<ID3DBlob> vs, ps;
    if (!compile_shader(kOverlayHlsl, "VSMain", "vs_5_0", vs, err)) return false;
    if (!compile_shader(kOverlayHlsl, "PSMain", "ps_5_0", ps, err)) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = d.ovlRoot.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.SampleMask = UINT_MAX;
    pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1; pso.RTVFormats[0] = kFormat;
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN; pso.SampleDesc.Count = 1;
    if (FAILED(d.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&d.ovlPso)))) { err = "overlay pso"; return false; }

    // Alpha-blended variant: same fullscreen triangle, but PSBlend keeps the texture's alpha and the blend
    // state does src-alpha-over -> a transparent overlay (a caption) painted directly OVER the world, no clear.
    ComPtr<ID3DBlob> psb;
    if (!compile_shader(kOverlayHlsl, "PSBlend", "ps_5_0", psb, err)) return false;
    pso.PS = { psb->GetBufferPointer(), psb->GetBufferSize() };
    pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
    pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    if (FAILED(d.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&d.ovlBlendPso)))) { err = "overlay blend pso"; return false; }

    d.ovlReady = true;
    return true;
}

bool Renderer::present_overlay_windowed(const uint8_t* rgba, uint32_t width, uint32_t height) {
    Impl& d = *impl_;
    if (!d.windowed || !d.swapchain) { last_error_ = "present_overlay_windowed needs a window"; return false; }
    // M19: the overlay may be a smaller source (e.g. an upscaled ray-traced frame);
    // the fullscreen-triangle sampler upscales it to the window. width/height = source.
    if (!ensure_overlay_pipeline(d, width, height, last_error_)) return false;
    if (!upload_overlay(d, rgba, last_error_)) return false;

    if (FAILED(d.alloc->Reset())) { last_error_ = "allocator reset failed"; return false; }
    if (FAILED(d.list->Reset(d.alloc.Get(), nullptr))) { last_error_ = "command list reset failed"; return false; }

    const UINT bbIndex = d.swapchain->GetCurrentBackBufferIndex();
    ID3D12Resource* bb = d.backbuffers[bbIndex].Get();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(bbIndex) * d.rtvDescSize;

    const D3D12_RESOURCE_BARRIER toRT = transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    d.list->ResourceBarrier(1, &toRT);
    d.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    d.list->ClearRenderTargetView(rtv, black, 0, nullptr);

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(d.width), static_cast<float>(d.height), 0.0f, 1.0f };
    D3D12_RECT scissor = { 0, 0, static_cast<LONG>(d.width), static_cast<LONG>(d.height) };
    d.list->RSSetViewports(1, &vp);
    d.list->RSSetScissorRects(1, &scissor);
    ID3D12DescriptorHeap* heaps[] = { d.ovlSrvHeap.Get() };
    d.list->SetDescriptorHeaps(1, heaps);
    d.list->SetGraphicsRootSignature(d.ovlRoot.Get());
    d.list->SetPipelineState(d.ovlPso.Get());
    d.list->SetGraphicsRootDescriptorTable(0, d.ovlSrvHeap->GetGPUDescriptorHandleForHeapStart());
    d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d.list->DrawInstanced(3, 1, 0, 0);

    const D3D12_RESOURCE_BARRIER toPresent = transition(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    d.list->ResourceBarrier(1, &toPresent);
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

bool Renderer::upload_caption_overlay(const uint8_t* rgba, uint32_t width, uint32_t height) {
    if (!impl_) return false;
    Impl& d = *impl_;
    if (!ensure_overlay_pipeline(d, width, height, last_error_)) return false;
    return upload_overlay(d, rgba, last_error_);
}

bool Renderer::render_chunks_windowed(const contracts::CameraPose& camera,
                                      const std::vector<contracts::ResidentChunk>& resident,
                                      uint32_t upload_budget, uint64_t tick, uint32_t* out_drawn,
                                      bool draw_overlay) {
    Impl& d = *impl_;
    if (!d.windowed || !d.swapchain) { last_error_ = "render_chunks_windowed needs a window"; return false; }
    if (!ensure_lit_pipeline(d, d.pendingTexSeed, last_error_)) return false;

    // Forward fluorescent lighting (identical to render_chunks): grid lights -> CBV.
    if (!d.lightCb) {
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = kLightCbBytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const D3D12_HEAP_PROPERTIES up = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        if (FAILED(d.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&d.lightCb)))) {
            last_error_ = "light CBV create failed"; return false;
        }
        const D3D12_RANGE none = { 0, 0 };
        if (FAILED(d.lightCb->Map(0, &none, &d.lightCbMapped))) { last_error_ = "light CBV map failed"; return false; }
    }
    {
        LightsCB lc = {};
        lc.ambientCount[0] = lc.ambientCount[1] = lc.ambientCount[2] = 0.22f;
        int n = 0;
        const float cell = contracts::kCellSize;
        const int64_t gi0 = static_cast<int64_t>(std::floor(camera.pos[0] / cell));
        const int64_t gj0 = static_cast<int64_t>(std::floor(camera.pos[2] / cell));
        const int R = 10;
        for (int64_t dgi = -R; dgi <= R && n < kMaxLights; ++dgi)
            for (int64_t dgj = -R; dgj <= R && n < kMaxLights; ++dgj) {
                const int64_t gi = gi0 + dgi, gj = gj0 + dgj;
                if (!contracts::is_fluorescent_cell(gi, gj)) continue;
                float p[3]; contracts::fluorescent_light_pos(gi, gj, p); p[1] += contracts::level_base_y(contracts::level_from_y(camera.pos[1]));  // M26: lights at the wanderer's current floor
                const uint64_t lid = static_cast<uint64_t>(gi) * 0x9e3779b97f4a7c15ULL ^ static_cast<uint64_t>(gj) * 0xc2b2ae3d27d4eb4fULL;
                const float fl = br::core::light_flicker(d.pendingTexSeed, lid, tick);
                lc.lights[n][0] = p[0]; lc.lights[n][1] = p[1]; lc.lights[n][2] = p[2]; lc.lights[n][3] = fl;
                ++n;
            }
        lc.ambientCount[3] = static_cast<float>(n);
        std::memcpy(d.lightCbMapped, &lc, sizeof(lc));
    }

    // Pool sync (identical): evict non-resident, upload new (budgeted).
    std::set<contracts::ChunkKey> live;
    for (const auto& rc : resident) live.insert(rc.key);
    for (auto it = d.chunkSlotOf.begin(); it != d.chunkSlotOf.end();) {
        if (live.find(it->first) == live.end()) { d.chunkFree.push_back(it->second); it = d.chunkSlotOf.erase(it); }
        else ++it;
    }
    uint32_t uploaded = 0;
    for (const auto& rc : resident) {
        if (d.chunkSlotOf.find(rc.key) == d.chunkSlotOf.end()) {
            if (uploaded >= upload_budget) continue;
            if (!upload_chunk_mesh(d, rc, last_error_)) return false;
            ++uploaded;
        }
    }

    const Mat4 mvp = mat_mul(view_lh(camera.pos, camera.yaw, camera.pitch),
                             proj_lh(camera.fov_y, camera.aspect, 0.05f, 500.0f));
    float constants[20];
    std::memcpy(constants, mvp.m, sizeof(mvp.m));
    float ld[3] = { -0.3f, -1.0f, -0.2f };
    const float ll = std::sqrt(ld[0]*ld[0] + ld[1]*ld[1] + ld[2]*ld[2]);
    constants[16] = ld[0]/ll; constants[17] = ld[1]/ll; constants[18] = ld[2]/ll; constants[19] = 0.0f;

    if (FAILED(d.alloc->Reset())) { last_error_ = "allocator reset failed"; return false; }
    if (FAILED(d.list->Reset(d.alloc.Get(), nullptr))) { last_error_ = "command list reset failed"; return false; }

    const UINT bbIndex = d.swapchain->GetCurrentBackBufferIndex();
    ID3D12Resource* bb = d.backbuffers[bbIndex].Get();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(bbIndex) * d.rtvDescSize;
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = d.dsvHeap->GetCPUDescriptorHandleForHeapStart();

    const D3D12_RESOURCE_BARRIER toRT = transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    d.list->ResourceBarrier(1, &toRT);
    d.list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    d.list->ClearRenderTargetView(rtv, kClearFloat, 0, nullptr);
    d.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(d.width), static_cast<float>(d.height), 0.0f, 1.0f };
    D3D12_RECT scissor = { 0, 0, static_cast<LONG>(d.width), static_cast<LONG>(d.height) };
    d.list->RSSetViewports(1, &vp);
    d.list->RSSetScissorRects(1, &scissor);
    ID3D12DescriptorHeap* heaps[] = { d.srvHeap.Get() };
    d.list->SetDescriptorHeaps(1, heaps);
    d.list->SetGraphicsRootSignature(d.litRoot.Get());
    d.list->SetPipelineState(d.litPso.Get());
    d.list->SetGraphicsRoot32BitConstants(0, 20, constants, 0);
    d.list->SetGraphicsRootDescriptorTable(1, d.srvHeap->GetGPUDescriptorHandleForHeapStart());
    d.list->SetGraphicsRootConstantBufferView(2, d.lightCb->GetGPUVirtualAddress());
    d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    uint32_t drawn = 0;
    for (const auto& rc : resident) {
        const auto it = d.chunkSlotOf.find(rc.key);
        if (it == d.chunkSlotOf.end()) continue;
        const ChunkSlot& slot = d.chunkPool[it->second];
        d.list->IASetVertexBuffers(0, 1, &slot.view);
        d.list->DrawInstanced(slot.vertex_count, 1, 0, 0);
        ++drawn;
    }
    if (out_drawn) *out_drawn = drawn;

    // Director subtitle: alpha-blend the caption texture (uploaded via upload_caption_overlay) OVER the world,
    // same RTV, same command list. ovlBlendPso keeps the sampled alpha so transparent pixels show the scene;
    // depth is off so it draws on top. No clear, no post pass -- just words painted on screen.
    if (draw_overlay && d.ovlBlendPso && d.ovlSrvHeap) {
        ID3D12DescriptorHeap* ovlHeaps[] = { d.ovlSrvHeap.Get() };
        d.list->SetDescriptorHeaps(1, ovlHeaps);
        d.list->SetGraphicsRootSignature(d.ovlRoot.Get());
        d.list->SetPipelineState(d.ovlBlendPso.Get());
        d.list->SetGraphicsRootDescriptorTable(0, d.ovlSrvHeap->GetGPUDescriptorHandleForHeapStart());
        d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d.list->DrawInstanced(3, 1, 0, 0);
    }

    const D3D12_RESOURCE_BARRIER toPresent = transition(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    d.list->ResourceBarrier(1, &toPresent);
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

bool Renderer::render_chunks(const contracts::CameraPose& camera,
                             const std::vector<contracts::ResidentChunk>& resident,
                             uint32_t upload_budget, uint64_t tick, uint32_t* out_drawn) {
    Impl& d = *impl_;
    if (d.windowed || !d.rt) {
        last_error_ = "render_chunks is headless-only (M3)";
        return false;
    }
    if (!ensure_lit_pipeline(d, d.pendingTexSeed, last_error_)) return false;
    if (d.postEnabled) {
        if (!ensure_post_pipeline(d, last_error_)) return false;  // submits its own setup list
        d.postParams.resX = static_cast<float>(d.width);
        d.postParams.resY = static_cast<float>(d.height);
    }

    // Forward fluorescent lighting: gather the lights in the ceiling grid near
    // the camera, scale each by its deterministic flicker (core), into the CBV.
    if (!d.lightCb) {
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = kLightCbBytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const D3D12_HEAP_PROPERTIES up = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        if (FAILED(d.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&d.lightCb)))) {
            last_error_ = "light CBV create failed"; return false;
        }
        const D3D12_RANGE none = { 0, 0 };
        if (FAILED(d.lightCb->Map(0, &none, &d.lightCbMapped))) { last_error_ = "light CBV map failed"; return false; }
    }
    {
        LightsCB lc = {};
        lc.ambientCount[0] = lc.ambientCount[1] = lc.ambientCount[2] = 0.22f;  // flat ambient
        int n = 0;
        const float cs = contracts::kCellSize;
        const int64_t gi0 = static_cast<int64_t>(std::floor(camera.pos[0] / cs));
        const int64_t gj0 = static_cast<int64_t>(std::floor(camera.pos[2] / cs));
        const int R = 10;
        for (int64_t dgi = -R; dgi <= R && n < kMaxLights; ++dgi) {
            for (int64_t dgj = -R; dgj <= R && n < kMaxLights; ++dgj) {
                const int64_t gi = gi0 + dgi, gj = gj0 + dgj;
                if (!contracts::is_fluorescent_cell(gi, gj)) continue;
                float p[3];
                contracts::fluorescent_light_pos(gi, gj, p); p[1] += contracts::level_base_y(contracts::level_from_y(camera.pos[1]));  // M26: lights at the wanderer's current floor
                const uint64_t lid = static_cast<uint64_t>(gi) * 0x9e3779b97f4a7c15ULL ^
                                     static_cast<uint64_t>(gj) * 0xc2b2ae3d27d4eb4fULL;
                const float fl = br::core::light_flicker(d.pendingTexSeed, lid, tick);
                lc.lights[n][0] = p[0]; lc.lights[n][1] = p[1]; lc.lights[n][2] = p[2];
                lc.lights[n][3] = 1.0f * fl;
                ++n;
            }
        }
        lc.ambientCount[3] = static_cast<float>(n);
        std::memcpy(d.lightCbMapped, &lc, sizeof(lc));
    }

    // Return slots whose chunk is no longer resident; upload new ones (budgeted).
    std::set<contracts::ChunkKey> live;
    for (const auto& rc : resident) live.insert(rc.key);
    for (auto it = d.chunkSlotOf.begin(); it != d.chunkSlotOf.end();) {
        if (live.find(it->first) == live.end()) {
            d.chunkFree.push_back(it->second);
            it = d.chunkSlotOf.erase(it);
        } else {
            ++it;
        }
    }
    uint32_t uploaded = 0;
    for (const auto& rc : resident) {
        if (d.chunkSlotOf.find(rc.key) == d.chunkSlotOf.end()) {
            if (uploaded >= upload_budget) continue;
            if (!upload_chunk_mesh(d, rc, last_error_)) return false;
            ++uploaded;
        }
    }

    const Mat4 mvp = mat_mul(view_lh(camera.pos, camera.yaw, camera.pitch),
                             proj_lh(camera.fov_y, camera.aspect, 0.05f, 500.0f));
    float constants[20];
    std::memcpy(constants, mvp.m, sizeof(mvp.m));
    float ld[3] = { -0.3f, -1.0f, -0.2f };
    const float ll = std::sqrt(ld[0]*ld[0] + ld[1]*ld[1] + ld[2]*ld[2]);
    constants[16] = ld[0]/ll; constants[17] = ld[1]/ll; constants[18] = ld[2]/ll;
    constants[19] = 0.0f;

    if (FAILED(d.alloc->Reset())) { last_error_ = "allocator reset failed"; return false; }
    if (FAILED(d.list->Reset(d.alloc.Get(), nullptr))) {
        last_error_ = "command list reset failed";
        return false;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = d.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    d.list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    d.list->ClearRenderTargetView(rtv, kClearFloat, 0, nullptr);
    d.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(d.width), static_cast<float>(d.height), 0.0f, 1.0f };
    D3D12_RECT scissor = { 0, 0, static_cast<LONG>(d.width), static_cast<LONG>(d.height) };
    d.list->RSSetViewports(1, &vp);
    d.list->RSSetScissorRects(1, &scissor);
    ID3D12DescriptorHeap* heaps[] = { d.srvHeap.Get() };
    d.list->SetDescriptorHeaps(1, heaps);
    d.list->SetGraphicsRootSignature(d.litRoot.Get());
    d.list->SetPipelineState(d.litPso.Get());
    d.list->SetGraphicsRoot32BitConstants(0, 20, constants, 0);
    d.list->SetGraphicsRootDescriptorTable(1, d.srvHeap->GetGPUDescriptorHandleForHeapStart());
    d.list->SetGraphicsRootConstantBufferView(2, d.lightCb->GetGPUVirtualAddress());
    d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    uint32_t drawn = 0;
    for (const auto& rc : resident) {
        const auto it = d.chunkSlotOf.find(rc.key);
        if (it == d.chunkSlotOf.end()) continue;  // not uploaded yet (budgeted)
        const ChunkSlot& cs = d.chunkPool[it->second];
        d.list->IASetVertexBuffers(0, 1, &cs.view);
        d.list->DrawInstanced(cs.vertex_count, 1, 0, 0);
        ++drawn;
    }
    if (out_drawn) *out_drawn = drawn;

    finalize_to_readback(d, d.postEnabled);  // VHS post pass (M8) or a straight copy

    if (FAILED(d.list->Close())) { last_error_ = "command list close failed"; return false; }
    ID3D12CommandList* lists[] = { d.list.Get() };
    d.queue->ExecuteCommandLists(1, lists);
    const UINT64 v = ++d.fenceValue;
    if (FAILED(d.queue->Signal(d.fence.Get(), v))) { last_error_ = "fence Signal failed"; return false; }
    if (d.fence->GetCompletedValue() < v) {
        if (FAILED(d.fence->SetEventOnCompletion(v, d.fenceEvent))) {
            last_error_ = "SetEventOnCompletion failed";
            return false;
        }
        if (WaitForSingleObject(d.fenceEvent, 5000) != WAIT_OBJECT_0) {
            last_error_ = "fence wait timed out";
            return false;
        }
    }
    return true;
}

bool Renderer::render_topdown(const std::vector<contracts::ResidentChunk>& chunks,
                              float cx, float cz, float half) {
    Impl& d = *impl_;
    if (d.windowed || !d.rt) { last_error_ = "render_topdown is headless-only"; return false; }
    if (!ensure_chunk_pipeline(d, last_error_)) return false;

    // Sync the pool to exactly the given chunks (upload all, no budget).
    std::set<contracts::ChunkKey> live;
    for (const auto& rc : chunks) live.insert(rc.key);
    for (auto it = d.chunkSlotOf.begin(); it != d.chunkSlotOf.end();) {
        if (live.find(it->first) == live.end()) { d.chunkFree.push_back(it->second); it = d.chunkSlotOf.erase(it); }
        else ++it;
    }
    for (const auto& rc : chunks) {
        if (d.chunkSlotOf.find(rc.key) == d.chunkSlotOf.end()) {
            if (!upload_chunk_mesh(d, rc, last_error_)) return false;
        }
    }

    // Orthographic top-down MVP (row-major; world XZ -> screen, world Y -> depth).
    const float minY = -1.0f, maxY = 4.0f, range = maxY - minY;
    float constants[20] = {};
    constants[0] = 1.0f / half;       // wx -> clip.x
    constants[9] = 1.0f / half;       // wz -> clip.y
    constants[6] = -1.0f / range;     // wy -> clip.z
    constants[12] = -cx / half;
    constants[13] = -cz / half;
    constants[14] = maxY / range;
    constants[15] = 1.0f;
    constants[17] = -1.0f;            // light straight down
    constants[19] = 1.0f;            // hide ceiling/fluorescent (see the maze)

    if (FAILED(d.alloc->Reset())) { last_error_ = "allocator reset failed"; return false; }
    if (FAILED(d.list->Reset(d.alloc.Get(), nullptr))) { last_error_ = "command list reset failed"; return false; }

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = d.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    d.list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    d.list->ClearRenderTargetView(rtv, kClearFloat, 0, nullptr);
    d.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(d.width), static_cast<float>(d.height), 0.0f, 1.0f };
    D3D12_RECT scissor = { 0, 0, static_cast<LONG>(d.width), static_cast<LONG>(d.height) };
    d.list->RSSetViewports(1, &vp);
    d.list->RSSetScissorRects(1, &scissor);
    d.list->SetGraphicsRootSignature(d.chunkRoot.Get());
    d.list->SetPipelineState(d.chunkPso.Get());
    d.list->SetGraphicsRoot32BitConstants(0, 20, constants, 0);
    d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (const auto& rc : chunks) {
        const auto it = d.chunkSlotOf.find(rc.key);
        if (it == d.chunkSlotOf.end()) continue;
        const ChunkSlot& cs = d.chunkPool[it->second];
        d.list->IASetVertexBuffers(0, 1, &cs.view);
        d.list->DrawInstanced(cs.vertex_count, 1, 0, 0);
    }

    const D3D12_RESOURCE_BARRIER toCopy = transition(
        d.rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    d.list->ResourceBarrier(1, &toCopy);
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = d.readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = d.footprint;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = d.rt.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const D3D12_RESOURCE_BARRIER backToRt = transition(
        d.rt.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    d.list->ResourceBarrier(1, &backToRt);

    if (FAILED(d.list->Close())) { last_error_ = "command list close failed"; return false; }
    ID3D12CommandList* lists[] = { d.list.Get() };
    d.queue->ExecuteCommandLists(1, lists);
    const UINT64 v = ++d.fenceValue;
    if (FAILED(d.queue->Signal(d.fence.Get(), v))) { last_error_ = "fence Signal failed"; return false; }
    if (d.fence->GetCompletedValue() < v) {
        if (FAILED(d.fence->SetEventOnCompletion(v, d.fenceEvent))) { last_error_ = "SetEventOnCompletion failed"; return false; }
        if (WaitForSingleObject(d.fenceEvent, 5000) != WAIT_OBJECT_0) { last_error_ = "fence wait timed out"; return false; }
    }
    return true;
}

}  // namespace br::render_d3d12
