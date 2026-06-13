#include "render_dxr/dxr.h"

#include "dxc.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace br::render_dxr {

namespace {

// A minimal but valid DXR shader library (raygen + miss), enough to prove the
// SM 6.3 DXIL toolchain compiles + signs.
const char* kProbeShader = R"(
RWTexture2D<float4> g_out : register(u0);
struct Payload { float4 color; };
[shader("raygeneration")]
void RayGen() { g_out[DispatchRaysIndex().xy] = float4(1, 0, 0, 1); }
[shader("miss")]
void Miss(inout Payload p) { p.color = float4(0, 0, 0, 1); }
)";

std::string wide_to_utf8(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

void append_detail(std::string& d, const std::string& s) {
    if (!d.empty()) d += " | ";
    d += s;
}

}  // namespace

DxrCaps probe_caps() {
    DxrCaps caps;

    // Debug layer (idempotent; matches the raster renderer so the gate is clean).
    ComPtr<ID3D12Debug> debug;
    UINT factoryFlags = 0;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory)))) {
        append_detail(caps.detail, "CreateDXGIFactory2 failed");
    }

    ComPtr<ID3D12Device> device;
    if (factory) {
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapterByGpuPreference(
                 i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
             ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
                caps.adapter = wide_to_utf8(desc.Description);
                break;
            }
            adapter.Reset();
            device.Reset();
        }
    }

    if (device) {
        ComPtr<ID3D12Device5> dev5;
        if (SUCCEEDED(device.As(&dev5))) {
            caps.device5 = true;
            D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5{};
            if (SUCCEEDED(dev5->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5)))) {
                if (opt5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1) caps.raytracing_tier = 11;
                else if (opt5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0) caps.raytracing_tier = 10;
            }
        }
    } else {
        append_detail(caps.detail, "no hardware D3D12 device");
    }

    // DXC toolchain probe (independent of the device).
    DxcCompiler dxc;
    caps.dxc_available = dxc.available();
    if (caps.dxc_available) {
        std::vector<uint8_t> dxil;
        std::string err;
        caps.dxc_compiled = dxc.compile_library(kProbeShader, dxil, err);
        if (caps.dxc_compiled) {
            append_detail(caps.detail, "dxil_bytes=" + std::to_string(dxil.size()) + " via " + dxc.load_path());
        } else {
            append_detail(caps.detail, "dxc: " + err);
        }
    } else {
        append_detail(caps.detail, "dxcompiler.dll not found");
    }

    return caps;
}

// ---------------------------------------------------------------------------
// DxrRenderer (M9 phase 1b): full DispatchRays pipeline proof.
// ---------------------------------------------------------------------------

namespace {

const char* kRaygenShader = R"(
RWTexture2D<float4> g_out : register(u0);
[shader("raygeneration")]
void RayGen() {
    uint2 px = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;
    float2 uv = (float2(px) + 0.5) / float2(dim);
    g_out[px] = float4(uv.x, uv.y, 0.5, 1.0);
}
)";

constexpr DXGI_FORMAT kDxrFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

D3D12_HEAP_PROPERTIES heap_props(D3D12_HEAP_TYPE t) {
    D3D12_HEAP_PROPERTIES p{};
    p.Type = t;
    return p;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from,
                                  D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}

}  // namespace

struct DxrRenderer::Impl {
    ComPtr<ID3D12Device5> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList4> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> infoQueue;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceValue = 0;

    ComPtr<ID3D12Resource> uav;          // raygen output (UNORDERED_ACCESS)
    ComPtr<ID3D12Resource> readbackBuf;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT readbackRows = 0;
    UINT64 readbackRowSize = 0, readbackTotal = 0;
    ComPtr<ID3D12DescriptorHeap> uavHeap;  // shader-visible (1 UAV)

    ComPtr<ID3D12RootSignature> globalRS;
    ComPtr<ID3D12StateObject> pso;
    ComPtr<ID3D12Resource> sbt;            // raygen shader table

    // Scene path (phase 2): BLAS-per-chunk + TLAS + TraceRay pipeline.
    ComPtr<ID3D12RootSignature> sceneRS;
    ComPtr<ID3D12StateObject> scenePso;
    ComPtr<ID3D12Resource> sceneSbt;       // raygen | miss | hitgroup (64 B slots)
    ComPtr<ID3D12Resource> tlas;
    std::vector<ComPtr<ID3D12Resource>> blas;     // kept alive (referenced by TLAS)
    std::vector<ComPtr<ID3D12Resource>> vbs;      // chunk vertex buffers
    bool sceneReady = false;

    uint32_t width = 0, height = 0;

    void wait_idle() {
        const UINT64 v = ++fenceValue;
        queue->Signal(fence.Get(), v);
        if (fence->GetCompletedValue() < v) {
            fence->SetEventOnCompletion(v, fenceEvent);
            WaitForSingleObject(fenceEvent, 5000);
        }
    }
};

DxrRenderer::DxrRenderer() : impl_(std::make_unique<Impl>()) {}
DxrRenderer::~DxrRenderer() {
    if (impl_ && impl_->fenceEvent) CloseHandle(impl_->fenceEvent);
}

uint32_t DxrRenderer::width() const { return impl_ ? impl_->width : 0; }
uint32_t DxrRenderer::height() const { return impl_ ? impl_->height : 0; }

bool DxrRenderer::init(uint32_t w, uint32_t h) {
    Impl& d = *impl_;
    d.width = w;
    d.height = h;

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

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5{};
    if (FAILED(d.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5))) ||
        opt5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0) {
        last_error_ = "device lacks DXR (RaytracingTier < 1.0)";
        return false;
    }
    if (SUCCEEDED(d.device.As(&d.infoQueue))) {
        d.infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
        d.infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
        d.infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(d.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&d.queue)))) { last_error_ = "queue"; return false; }
    if (FAILED(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&d.alloc)))) { last_error_ = "alloc"; return false; }
    ComPtr<ID3D12GraphicsCommandList> list0;
    if (FAILED(d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.alloc.Get(), nullptr, IID_PPV_ARGS(&list0)))) { last_error_ = "list"; return false; }
    list0->Close();
    if (FAILED(list0.As(&d.list))) { last_error_ = "no GraphicsCommandList4"; return false; }
    if (FAILED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.fence)))) { last_error_ = "fence"; return false; }
    d.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // UAV output texture (R8G8B8A8, UNORDERED_ACCESS).
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = kDxrFormat; td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const D3D12_HEAP_PROPERTIES defHeap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(d.device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&d.uav)))) { last_error_ = "uav tex"; return false; }

    d.device->GetCopyableFootprints(&td, 0, 1, 0, &d.footprint, &d.readbackRows, &d.readbackRowSize, &d.readbackTotal);
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = d.readbackTotal; bd.Height = 1;
    bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const D3D12_HEAP_PROPERTIES rbHeap = heap_props(D3D12_HEAP_TYPE_READBACK);
    if (FAILED(d.device->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&d.readbackBuf)))) { last_error_ = "readback"; return false; }

    // Shader-visible UAV heap + view.
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = 1; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(d.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&d.uavHeap)))) { last_error_ = "uav heap"; return false; }
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = kDxrFormat; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    d.device->CreateUnorderedAccessView(d.uav.Get(), nullptr, &ud, d.uavHeap->GetCPUDescriptorHandleForHeapStart());

    // Global root signature: one UAV descriptor table (u0).
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; range.NumDescriptors = 1; range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp.DescriptorTable.NumDescriptorRanges = 1; rp.DescriptorTable.pDescriptorRanges = &range;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 1; rs.pParameters = &rp; rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> sig, sigErr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigErr))) { last_error_ = "global RS serialize"; return false; }
    if (FAILED(d.device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&d.globalRS)))) { last_error_ = "global RS"; return false; }

    // Compile the raygen library to DXIL.
    DxcCompiler dxc;
    std::vector<uint8_t> dxil;
    std::string cerr;
    if (!dxc.available()) { last_error_ = "dxcompiler.dll unavailable"; return false; }
    if (!dxc.compile_library(kRaygenShader, dxil, cerr)) { last_error_ = "raygen compile: " + cerr; return false; }

    // Raytracing state object.
    D3D12_EXPORT_DESC ex{};
    ex.Name = L"RayGen"; ex.Flags = D3D12_EXPORT_FLAG_NONE;
    D3D12_DXIL_LIBRARY_DESC lib{};
    lib.DXILLibrary.pShaderBytecode = dxil.data(); lib.DXILLibrary.BytecodeLength = dxil.size();
    lib.NumExports = 1; lib.pExports = &ex;
    D3D12_RAYTRACING_SHADER_CONFIG sc{}; sc.MaxPayloadSizeInBytes = 16; sc.MaxAttributeSizeInBytes = 8;
    D3D12_RAYTRACING_PIPELINE_CONFIG pc{}; pc.MaxTraceRecursionDepth = 1;
    D3D12_GLOBAL_ROOT_SIGNATURE grs{}; grs.pGlobalRootSignature = d.globalRS.Get();
    D3D12_STATE_SUBOBJECT subs[4];
    subs[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY; subs[0].pDesc = &lib;
    subs[1].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG; subs[1].pDesc = &sc;
    subs[2].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG; subs[2].pDesc = &pc;
    subs[3].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE; subs[3].pDesc = &grs;
    D3D12_STATE_OBJECT_DESC so{};
    so.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE; so.NumSubobjects = 4; so.pSubobjects = subs;
    if (FAILED(d.device->CreateStateObject(&so, IID_PPV_ARGS(&d.pso)))) { last_error_ = "CreateStateObject"; return false; }

    // Shader binding table: one raygen record (32-byte id) in a 64-byte buffer.
    ComPtr<ID3D12StateObjectProperties> props;
    if (FAILED(d.pso.As(&props))) { last_error_ = "state object props"; return false; }
    const void* rayId = props->GetShaderIdentifier(L"RayGen");
    if (!rayId) { last_error_ = "raygen shader id"; return false; }
    const UINT64 sbtSize = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;  // 64
    D3D12_RESOURCE_DESC sd{};
    sd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; sd.Width = sbtSize; sd.Height = 1;
    sd.DepthOrArraySize = 1; sd.MipLevels = 1; sd.Format = DXGI_FORMAT_UNKNOWN; sd.SampleDesc.Count = 1;
    sd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const D3D12_HEAP_PROPERTIES upHeap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
    if (FAILED(d.device->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &sd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&d.sbt)))) { last_error_ = "sbt buffer"; return false; }
    uint8_t* mapped = nullptr;
    const D3D12_RANGE none{ 0, 0 };
    if (FAILED(d.sbt->Map(0, &none, reinterpret_cast<void**>(&mapped)))) { last_error_ = "sbt map"; return false; }
    std::memcpy(mapped, rayId, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    d.sbt->Unmap(0, nullptr);
    return true;
}

bool DxrRenderer::render_gradient() {
    Impl& d = *impl_;
    if (!d.device) { last_error_ = "not initialised"; return false; }
    if (FAILED(d.alloc->Reset())) { last_error_ = "alloc reset"; return false; }
    if (FAILED(d.list->Reset(d.alloc.Get(), nullptr))) { last_error_ = "list reset"; return false; }

    ID3D12DescriptorHeap* heaps[] = { d.uavHeap.Get() };
    d.list->SetDescriptorHeaps(1, heaps);
    d.list->SetComputeRootSignature(d.globalRS.Get());
    d.list->SetComputeRootDescriptorTable(0, d.uavHeap->GetGPUDescriptorHandleForHeapStart());
    d.list->SetPipelineState1(d.pso.Get());

    D3D12_DISPATCH_RAYS_DESC dr{};
    dr.RayGenerationShaderRecord.StartAddress = d.sbt->GetGPUVirtualAddress();
    dr.RayGenerationShaderRecord.SizeInBytes = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
    dr.Width = d.width; dr.Height = d.height; dr.Depth = 1;
    d.list->DispatchRays(&dr);

    const D3D12_RESOURCE_BARRIER toCopy = transition(d.uav.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    d.list->ResourceBarrier(1, &toCopy);
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = d.readbackBuf.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = d.footprint;
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = d.uav.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const D3D12_RESOURCE_BARRIER back = transition(d.uav.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    d.list->ResourceBarrier(1, &back);

    if (FAILED(d.list->Close())) { last_error_ = "list close"; return false; }
    ID3D12CommandList* lists[] = { d.list.Get() };
    d.queue->ExecuteCommandLists(1, lists);
    d.wait_idle();
    return true;
}

bool DxrRenderer::readback(std::vector<uint8_t>& rgba) {
    Impl& d = *impl_;
    if (!d.readbackBuf) { last_error_ = "no readback buffer"; return false; }
    void* mapped = nullptr;
    const D3D12_RANGE rd{ 0, static_cast<SIZE_T>(d.readbackTotal) };
    if (FAILED(d.readbackBuf->Map(0, &rd, &mapped))) { last_error_ = "readback map"; return false; }
    rgba.resize(static_cast<size_t>(d.width) * d.height * 4u);
    const auto* base = static_cast<const uint8_t*>(mapped) + d.footprint.Offset;
    const SIZE_T srcPitch = d.footprint.Footprint.RowPitch;
    for (uint32_t y = 0; y < d.height; ++y)
        std::memcpy(&rgba[static_cast<size_t>(y) * d.width * 4u], base + static_cast<size_t>(y) * srcPitch,
                    static_cast<size_t>(d.width) * 4u);
    const D3D12_RANGE noWrite{ 0, 0 };
    d.readbackBuf->Unmap(0, &noWrite);
    return true;
}

uint32_t DxrRenderer::debug_error_count() {
    Impl& d = *impl_;
    if (!d.infoQueue) return 0;
    uint32_t count = 0;
    const UINT64 n = d.infoQueue->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        d.infoQueue->GetMessage(i, nullptr, &len);
        if (len == 0) continue;
        std::vector<uint8_t> buf(len);
        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (SUCCEEDED(d.infoQueue->GetMessage(i, msg, &len))) {
            if (msg->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
                msg->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                msg->Severity == D3D12_MESSAGE_SEVERITY_WARNING) {
                ++count;
            }
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// Scene path (M9 phase 2): BLAS/TLAS from chunk geometry + TraceRay primary rays.
// ---------------------------------------------------------------------------

namespace {

const char* kSceneShader = R"(
cbuffer Cam : register(b0) {
    float3 uPos;   float uTanY;
    float3 uFwd;   float uAspect;
    float3 uRight; float _p0;
    float3 uUp;    float _p1;
};
RWTexture2D<float4> g_out : register(u0);
RaytracingAccelerationStructure g_scene : register(t0);
struct Payload { float3 color; float t; };

[shader("raygeneration")]
void RayGen() {
    uint2 px = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;
    float2 uv = (float2(px) + 0.5) / float2(dim);
    float sx = (2.0 * uv.x - 1.0) * uTanY * uAspect;
    float sy = (1.0 - 2.0 * uv.y) * uTanY;
    float3 dir = normalize(uFwd + uRight * sx + uUp * sy);
    RayDesc ray; ray.Origin = uPos; ray.Direction = dir; ray.TMin = 0.02; ray.TMax = 500.0;
    Payload p; p.color = float3(0.02, 0.02, 0.06); p.t = -1.0;
    TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, p);
    g_out[px] = float4(p.color, 1.0);
}
[shader("closesthit")]
void CHit(inout Payload p, BuiltInTriangleIntersectionAttributes attr) {
    float d = saturate(RayTCurrent() / 50.0);
    float shade = 1.0 - 0.85 * d;           // near = bright, far = dim
    p.color = float3(shade, shade, shade * 0.85);
    p.t = RayTCurrent();
}
[shader("miss")]
void Miss(inout Payload p) { p.color = float3(0.02, 0.02, 0.06); }
)";

struct V3 { float x, y, z; };
V3 cross3(V3 a, V3 b) { return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x }; }
V3 norm3(V3 a) {
    const float l = std::sqrt(a.x*a.x + a.y*a.y + a.z*a.z);
    const float s = (l > 1e-8f) ? 1.0f / l : 0.0f;
    return { a.x*s, a.y*s, a.z*s };
}

ComPtr<ID3D12Resource> make_buffer(ID3D12Device5* dev, UINT64 size, D3D12_HEAP_TYPE heap,
                                   D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = DXGI_FORMAT_UNKNOWN; d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = flags;
    const D3D12_HEAP_PROPERTIES hp = heap_props(heap);
    ComPtr<ID3D12Resource> r;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r));
    return r;
}

}  // namespace

bool DxrRenderer::build_scene(const std::vector<contracts::ResidentChunk>& chunks) {
    Impl& d = *impl_;
    if (!d.device) { last_error_ = "not initialised"; return false; }
    d.blas.clear(); d.vbs.clear(); d.tlas.Reset(); d.sceneReady = false;

    // Upload each chunk's vertices to an upload-heap buffer + build a BLAS.
    std::vector<ComPtr<ID3D12Resource>> scratches;
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
    if (FAILED(d.alloc->Reset())) { last_error_ = "alloc reset"; return false; }
    if (FAILED(d.list->Reset(d.alloc.Get(), nullptr))) { last_error_ = "list reset"; return false; }

    for (const contracts::ResidentChunk& rc : chunks) {
        if (rc.vertex_count < 3) continue;
        const UINT64 vbytes = static_cast<UINT64>(rc.vertex_count) * sizeof(contracts::ChunkVertex);
        ComPtr<ID3D12Resource> vb = make_buffer(d.device.Get(), vbytes, D3D12_HEAP_TYPE_UPLOAD,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
        if (!vb) { last_error_ = "chunk vb"; return false; }
        void* m = nullptr; const D3D12_RANGE none{ 0, 0 };
        vb->Map(0, &none, &m);
        std::memcpy(m, rc.vertices, static_cast<size_t>(vbytes));
        vb->Unmap(0, nullptr);

        D3D12_RAYTRACING_GEOMETRY_DESC geo{};
        geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geo.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geo.Triangles.VertexBuffer.StartAddress = vb->GetGPUVirtualAddress();
        geo.Triangles.VertexBuffer.StrideInBytes = sizeof(contracts::ChunkVertex);
        geo.Triangles.VertexCount = rc.vertex_count;
        geo.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;  // pos is the first field

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        in.NumDescs = 1; in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY; in.pGeometryDescs = &geo;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pi{};
        d.device->GetRaytracingAccelerationStructurePrebuildInfo(&in, &pi);

        ComPtr<ID3D12Resource> scratch = make_buffer(d.device.Get(), pi.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ComPtr<ID3D12Resource> result = make_buffer(d.device.Get(), pi.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (!scratch || !result) { last_error_ = "blas buffers"; return false; }

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
        bd.Inputs = in;
        bd.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
        bd.DestAccelerationStructureData = result->GetGPUVirtualAddress();
        d.list->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
        D3D12_RESOURCE_BARRIER uavb{}; uavb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; uavb.UAV.pResource = result.Get();
        d.list->ResourceBarrier(1, &uavb);

        D3D12_RAYTRACING_INSTANCE_DESC inst{};
        inst.Transform[0][0] = inst.Transform[1][1] = inst.Transform[2][2] = 1.0f;  // identity (world-space verts)
        inst.InstanceMask = 0xFF;
        inst.AccelerationStructure = result->GetGPUVirtualAddress();
        instances.push_back(inst);
        d.blas.push_back(result);
        d.vbs.push_back(vb);
        scratches.push_back(scratch);
    }
    if (instances.empty()) { last_error_ = "no geometry to build"; return false; }

    // TLAS: upload instance descs, build.
    const UINT64 instBytes = instances.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    ComPtr<ID3D12Resource> instBuf = make_buffer(d.device.Get(), instBytes, D3D12_HEAP_TYPE_UPLOAD,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
    void* im = nullptr; const D3D12_RANGE none{ 0, 0 };
    instBuf->Map(0, &none, &im);
    std::memcpy(im, instances.data(), static_cast<size_t>(instBytes));
    instBuf->Unmap(0, nullptr);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tin{};
    tin.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tin.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    tin.NumDescs = static_cast<UINT>(instances.size());
    tin.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tin.InstanceDescs = instBuf->GetGPUVirtualAddress();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tpi{};
    d.device->GetRaytracingAccelerationStructurePrebuildInfo(&tin, &tpi);
    ComPtr<ID3D12Resource> tscratch = make_buffer(d.device.Get(), tpi.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    d.tlas = make_buffer(d.device.Get(), tpi.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!tscratch || !d.tlas) { last_error_ = "tlas buffers"; return false; }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tbd{};
    tbd.Inputs = tin;
    tbd.ScratchAccelerationStructureData = tscratch->GetGPUVirtualAddress();
    tbd.DestAccelerationStructureData = d.tlas->GetGPUVirtualAddress();
    d.list->BuildRaytracingAccelerationStructure(&tbd, 0, nullptr);

    if (FAILED(d.list->Close())) { last_error_ = "as build close"; return false; }
    ID3D12CommandList* lists[] = { d.list.Get() };
    d.queue->ExecuteCommandLists(1, lists);
    d.wait_idle();  // scratches/instBuf can drop after the build completes

    // Build the scene state object + SBT once (independent of geometry).
    if (!d.scenePso) {
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; range.NumDescriptors = 1; range.BaseShaderRegister = 0;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER rp[3]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[0].Constants.Num32BitValues = 16; rp[0].Constants.ShaderRegister = 0;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable.NumDescriptorRanges = 1; rp[1].DescriptorTable.pDescriptorRanges = &range;
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rp[2].Descriptor.ShaderRegister = 0;  // t0 = TLAS
        D3D12_ROOT_SIGNATURE_DESC rs{}; rs.NumParameters = 3; rs.pParameters = rp; rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> sig, se;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &se))) { last_error_ = "scene RS serialize"; return false; }
        if (FAILED(d.device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&d.sceneRS)))) { last_error_ = "scene RS"; return false; }

        DxcCompiler dxc;
        std::vector<uint8_t> dxil; std::string cerr;
        if (!dxc.compile_library(kSceneShader, dxil, cerr)) { last_error_ = "scene compile: " + cerr; return false; }
        D3D12_EXPORT_DESC ex[3] = {
            { L"RayGen", nullptr, D3D12_EXPORT_FLAG_NONE },
            { L"CHit",   nullptr, D3D12_EXPORT_FLAG_NONE },
            { L"Miss",   nullptr, D3D12_EXPORT_FLAG_NONE },
        };
        D3D12_DXIL_LIBRARY_DESC lib{}; lib.DXILLibrary.pShaderBytecode = dxil.data();
        lib.DXILLibrary.BytecodeLength = dxil.size(); lib.NumExports = 3; lib.pExports = ex;
        D3D12_HIT_GROUP_DESC hg{}; hg.HitGroupExport = L"HitGroup"; hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES; hg.ClosestHitShaderImport = L"CHit";
        D3D12_RAYTRACING_SHADER_CONFIG sc{}; sc.MaxPayloadSizeInBytes = 16; sc.MaxAttributeSizeInBytes = 8;
        D3D12_RAYTRACING_PIPELINE_CONFIG pc{}; pc.MaxTraceRecursionDepth = 1;
        D3D12_GLOBAL_ROOT_SIGNATURE grs{}; grs.pGlobalRootSignature = d.sceneRS.Get();
        D3D12_STATE_SUBOBJECT subs[5];
        subs[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY; subs[0].pDesc = &lib;
        subs[1].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP; subs[1].pDesc = &hg;
        subs[2].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG; subs[2].pDesc = &sc;
        subs[3].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG; subs[3].pDesc = &pc;
        subs[4].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE; subs[4].pDesc = &grs;
        D3D12_STATE_OBJECT_DESC so{}; so.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE; so.NumSubobjects = 5; so.pSubobjects = subs;
        if (FAILED(d.device->CreateStateObject(&so, IID_PPV_ARGS(&d.scenePso)))) { last_error_ = "scene state object"; return false; }

        ComPtr<ID3D12StateObjectProperties> props;
        d.scenePso.As(&props);
        const void* idRay = props->GetShaderIdentifier(L"RayGen");
        const void* idMiss = props->GetShaderIdentifier(L"Miss");
        const void* idHit = props->GetShaderIdentifier(L"HitGroup");
        if (!idRay || !idMiss || !idHit) { last_error_ = "scene shader ids"; return false; }
        const UINT64 slot = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;  // 64
        d.sceneSbt = make_buffer(d.device.Get(), slot * 3, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
        uint8_t* sm = nullptr; const D3D12_RANGE n2{ 0, 0 };
        d.sceneSbt->Map(0, &n2, reinterpret_cast<void**>(&sm));
        std::memcpy(sm + slot * 0, idRay, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        std::memcpy(sm + slot * 1, idMiss, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        std::memcpy(sm + slot * 2, idHit, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        d.sceneSbt->Unmap(0, nullptr);
    }
    d.sceneReady = true;
    return true;
}

bool DxrRenderer::render_scene(const contracts::CameraPose& cam) {
    Impl& d = *impl_;
    if (!d.sceneReady) { last_error_ = "scene not built"; return false; }

    const float cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
    const float sy = std::sin(cam.yaw), cy = std::cos(cam.yaw);
    const V3 fwd = norm3({ cp * sy, sp, cp * cy });
    const V3 right = norm3(cross3({ 0, 1, 0 }, fwd));
    const V3 up = cross3(fwd, right);
    float c[16] = {};
    c[0] = cam.pos[0]; c[1] = cam.pos[1]; c[2] = cam.pos[2]; c[3] = std::tan(cam.fov_y * 0.5f);
    c[4] = fwd.x;   c[5] = fwd.y;   c[6] = fwd.z;   c[7] = cam.aspect;
    c[8] = right.x; c[9] = right.y; c[10] = right.z; c[11] = 0.0f;
    c[12] = up.x;   c[13] = up.y;   c[14] = up.z;   c[15] = 0.0f;

    if (FAILED(d.alloc->Reset())) { last_error_ = "alloc reset"; return false; }
    if (FAILED(d.list->Reset(d.alloc.Get(), nullptr))) { last_error_ = "list reset"; return false; }
    ID3D12DescriptorHeap* heaps[] = { d.uavHeap.Get() };
    d.list->SetDescriptorHeaps(1, heaps);
    d.list->SetComputeRootSignature(d.sceneRS.Get());
    d.list->SetComputeRoot32BitConstants(0, 16, c, 0);
    d.list->SetComputeRootDescriptorTable(1, d.uavHeap->GetGPUDescriptorHandleForHeapStart());
    d.list->SetComputeRootShaderResourceView(2, d.tlas->GetGPUVirtualAddress());
    d.list->SetPipelineState1(d.scenePso.Get());

    const UINT64 slot = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
    const D3D12_GPU_VIRTUAL_ADDRESS base = d.sceneSbt->GetGPUVirtualAddress();
    D3D12_DISPATCH_RAYS_DESC dr{};
    dr.RayGenerationShaderRecord = { base + slot * 0, slot };
    dr.MissShaderTable = { base + slot * 1, slot, slot };
    dr.HitGroupTable = { base + slot * 2, slot, slot };
    dr.Width = d.width; dr.Height = d.height; dr.Depth = 1;
    d.list->DispatchRays(&dr);

    const D3D12_RESOURCE_BARRIER toCopy = transition(d.uav.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    d.list->ResourceBarrier(1, &toCopy);
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = d.readbackBuf.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = d.footprint;
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = d.uav.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const D3D12_RESOURCE_BARRIER back = transition(d.uav.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    d.list->ResourceBarrier(1, &back);
    if (FAILED(d.list->Close())) { last_error_ = "scene list close"; return false; }
    ID3D12CommandList* lists[] = { d.list.Get() };
    d.queue->ExecuteCommandLists(1, lists);
    d.wait_idle();
    return true;
}

}  // namespace br::render_dxr
