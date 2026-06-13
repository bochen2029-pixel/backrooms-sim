// render_d3d12/renderer.cpp — M1 D3D12 renderer: device, debug layer + DRED,
// deterministic clear-color frame, headless offscreen readback, windowed
// swapchain present. No CD3DX12 helpers (avoids a new dependency); resource
// descriptors are filled by hand.
#include "render_d3d12/renderer.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <psapi.h>

#include <cstdio>
#include <cstring>

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

}  // namespace br::render_d3d12
