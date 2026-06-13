// render_d3d12/renderer.cpp — M1 D3D12 renderer: device, debug layer + DRED,
// deterministic clear-color frame, headless offscreen readback, windowed
// swapchain present. No CD3DX12 helpers (avoids a new dependency); resource
// descriptors are filled by hand.
#include "render_d3d12/renderer.h"

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

// Shared device/queue/fence/list bring-up used by both headless and windowed.
bool create_device_core(Renderer::Impl& d, std::string& err) {
    // Debug layer + DRED (dev builds). Errors here are non-fatal.
    ComPtr<ID3D12Debug> debug;
    UINT factoryFlags = 0;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dred;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
        dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    }

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
                                        IID_PPV_ARGS(&d.device)))) {
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

bool compile_shader(const char* entry, const char* target, ComPtr<ID3DBlob>& out, std::string& err) {
    ComPtr<ID3DBlob> errBlob;
    const HRESULT hr = D3DCompile(kSceneHlsl, std::strlen(kSceneHlsl), "scene", nullptr,
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
    if (!compile_shader("VSMain", "vs_5_0", vs, err)) return false;
    if (!compile_shader("PSMain", "ps_5_0", ps, err)) return false;

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

}  // namespace br::render_d3d12
