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
    ComPtr<ID3D12DescriptorHeap> uavHeap;  // shader-visible (u0 color, u1 depth)

    // Primary-hit NDC depth output (M9 phase 2b, the cross-renderer depth gate).
    ComPtr<ID3D12Resource> depthTex;       // R32_FLOAT, UNORDERED_ACCESS (u1)
    ComPtr<ID3D12Resource> depthReadback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT depthFootprint{};
    UINT64 depthTotal = 0;

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

    // Path-traced lighting (M9 phase 3): inline RayQuery + radiance accumulation.
    ComPtr<ID3D12Resource> accumTex;       // RGBA32F radiance accumulator (u2)
    ComPtr<ID3D12Resource> guideTex;       // RGBA32F denoiser guide: world normal.xyz + view-Z in .w (u3)
    ComPtr<ID3D12Resource> shadeVb;        // concatenated chunk verts (StructuredBuffer t1)
    uint32_t shadeVertCount = 0;
    // M25: a single DYNAMIC creature (the Shoggoth body) updated per-frame without
    // rebuilding the cached chunk BLASes -- the chunk TLAS instances + the creature's
    // start vertex in shadeVb are cached, and shadeVb reserves a tail for the creature.
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> chunkInstances;
    uint32_t chunkVertCount = 0;
    ComPtr<ID3D12Resource> creatureBlas, creatureVb;
    uint32_t creatureBlasVerts = 0;   // RT_PERF item C: last creature BLAS vert count -> refit when it matches
    ComPtr<ID3D12RootSignature> ptRS;
    ComPtr<ID3D12StateObject> ptPso;
    ComPtr<ID3D12Resource> ptSbt;          // raygen-only table
    uint32_t accumSamples = 0;             // spp accumulated since the last reset
    float flashIntensity = 0.0f;           // interactive flashlight (0 = off -> goldens bit-identical)
    ComPtr<ID3D12Resource> flareBuf;       // up to kFlareGpuMax green flares (StructuredBuffer t2); upload heap
    uint32_t flareCount = 0;               // active flares this frame (0 = none -> shader branches skipped)

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

bool DxrRenderer::init(uint32_t w, uint32_t h, void* external_device5) {
    Impl& d = *impl_;
    d.width = w;
    d.height = h;

    // RT_PERF item A: reuse the caller's device (the raster renderer's Device5) when provided, so the PT
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

    // Shader-visible UAV heap: slot 0 = color (u0), slot 1 = depth (u1),
    // slot 2 = PT radiance accumulator (u2), slot 3 = denoiser guide (u3).
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = 4; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(d.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&d.uavHeap)))) { last_error_ = "uav heap"; return false; }
    const UINT uavInc = d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = kDxrFormat; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    d.device->CreateUnorderedAccessView(d.uav.Get(), nullptr, &ud, d.uavHeap->GetCPUDescriptorHandleForHeapStart());

    // Primary-hit NDC depth output (R32_FLOAT) + its readback buffer + UAV (u1).
    D3D12_RESOURCE_DESC dt = td;
    dt.Format = DXGI_FORMAT_R32_FLOAT;
    if (FAILED(d.device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &dt,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&d.depthTex)))) { last_error_ = "depth tex"; return false; }
    d.device->GetCopyableFootprints(&dt, 0, 1, 0, &d.depthFootprint, nullptr, nullptr, &d.depthTotal);
    D3D12_RESOURCE_DESC dbd = bd;
    dbd.Width = d.depthTotal;
    if (FAILED(d.device->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &dbd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&d.depthReadback)))) { last_error_ = "depth readback"; return false; }
    D3D12_UNORDERED_ACCESS_VIEW_DESC dud{};
    dud.Format = DXGI_FORMAT_R32_FLOAT; dud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE depthUavCpu = d.uavHeap->GetCPUDescriptorHandleForHeapStart();
    depthUavCpu.ptr += uavInc;  // slot 1
    d.device->CreateUnorderedAccessView(d.depthTex.Get(), nullptr, &dud, depthUavCpu);

    // PT radiance accumulator (RGBA32F) + UAV (u2, heap slot 2).
    D3D12_RESOURCE_DESC at = td;
    at.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    if (FAILED(d.device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &at,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&d.accumTex)))) { last_error_ = "accum tex"; return false; }
    D3D12_UNORDERED_ACCESS_VIEW_DESC aud{};
    aud.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; aud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE accumUavCpu = d.uavHeap->GetCPUDescriptorHandleForHeapStart();
    accumUavCpu.ptr += static_cast<SIZE_T>(uavInc) * 2;  // slot 2
    d.device->CreateUnorderedAccessView(d.accumTex.Get(), nullptr, &aud, accumUavCpu);

    // Denoiser guide (RGBA32F): world normal.xyz + view-space Z in .w, written by the PT resolve and read
    // by the edge-aware filter pass (u3, heap slot 3). Geometry-only -> stops the blur at depth/normal edges.
    D3D12_RESOURCE_DESC gt = at;  // same R32G32B32A32_FLOAT 2D UAV
    if (FAILED(d.device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &gt,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&d.guideTex)))) { last_error_ = "guide tex"; return false; }
    D3D12_CPU_DESCRIPTOR_HANDLE guideUavCpu = d.uavHeap->GetCPUDescriptorHandleForHeapStart();
    guideUavCpu.ptr += static_cast<SIZE_T>(uavInc) * 3;  // slot 3
    d.device->CreateUnorderedAccessView(d.guideTex.Get(), nullptr, &aud, guideUavCpu);

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

bool DxrRenderer::readback_depth(std::vector<float>& depth) {
    Impl& d = *impl_;
    if (!d.depthReadback) { last_error_ = "no depth readback buffer"; return false; }
    void* mapped = nullptr;
    const D3D12_RANGE rd{ 0, static_cast<SIZE_T>(d.depthTotal) };
    if (FAILED(d.depthReadback->Map(0, &rd, &mapped))) { last_error_ = "depth readback map"; return false; }
    depth.resize(static_cast<size_t>(d.width) * d.height);
    const auto* base = static_cast<const uint8_t*>(mapped) + d.depthFootprint.Offset;
    const SIZE_T srcPitch = d.depthFootprint.Footprint.RowPitch;
    for (uint32_t y = 0; y < d.height; ++y)
        std::memcpy(&depth[static_cast<size_t>(y) * d.width], base + static_cast<size_t>(y) * srcPitch,
                    static_cast<size_t>(d.width) * sizeof(float));
    const D3D12_RANGE noWrite{ 0, 0 };
    d.depthReadback->Unmap(0, &noWrite);
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

// Near/far planes — must match render_d3d12's render_chunks proj_lh(0.05, 500)
// so the DXR and raster NDC depth buffers use the same hyperbolic mapping.
constexpr float kSceneNear = 0.05f;
constexpr float kSceneFar = 500.0f;

const char* kSceneShader = R"(
cbuffer Cam : register(b0) {
    float3 uPos;   float uTanY;
    float3 uFwd;   float uAspect;
    float3 uRight; float uNear;
    float3 uUp;    float uFar;
};
RWTexture2D<float4> g_out : register(u0);
RWTexture2D<float>  g_depth : register(u1);
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
    RayDesc ray; ray.Origin = uPos; ray.Direction = dir; ray.TMin = 0.02; ray.TMax = uFar;
    Payload p; p.color = float3(0.02, 0.02, 0.06); p.t = -1.0;
    TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, p);
    g_out[px] = float4(p.color, 1.0);
    // NDC depth matching render_d3d12's proj_lh(uNear,uFar): hyperbolic [0,1],
    // computed from the eye-space z (hit distance projected onto the forward
    // axis), so the DXR and raster depth buffers are comparable per pixel.
    float ndc = 1.0;
    if (p.t > 0.0) {
        float vz = p.t * dot(dir, uFwd);
        float q = uFar / (uFar - uNear);
        ndc = q * (1.0 - uNear / vz);
    }
    g_depth[px] = ndc;
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

// Tonemap exposure for the PT resolve (tuned so the lit scene sits mid-band).
constexpr float kPtExposure = 1.4f;

// Interactive flashlight intensity when ON (radiance scale of the eye torch; 0 = off). Tuned to clearly
// lift dark areas inside the cone without blowing out a normally-lit corridor. Off by default (goldens).
constexpr float kFlashIntensity = 6.0f;

// Max green flares uploaded + rendered per frame (the caller pre-culls to the nearest this many). Caps the
// shader's per-pixel flare work regardless of how many the player has dropped. 64 * float4 = 1 KB.
constexpr uint32_t kFlareGpuMax = 64u;

// M9 phase 3 path tracer — inline RayQuery (SM 6.5). Emissive fluorescent ceiling
// grid as area lights (NEE + shadow rays for direct), one cosine-weighted diffuse
// GI bounce, seeded per-(pixel,sample) RNG, accumulated across dispatches.
const char* kPtShader = R"(
struct Vertex { float px,py,pz, nx,ny,nz, cr,cg,cb, u,v, mat; };
cbuffer PT : register(b0) {
    float3 uPos;   float uTanY;
    float3 uFwd;   float uAspect;
    float3 uRight; float uNear;
    float3 uUp;    float uFar;
    uint uSampleStart; uint uSampleCount; uint uTotal; uint uSeed;
    uint uResolve; float uExposure; float uWidth; float uHeight;  // uResolve: 0 accum, 1 accum+tonemap, 2 accum+guide, 3 denoise
    uint uFrame; float uFlashI; uint uFlareN; uint uPad2;        // uFlashI: flashlight; uFlareN: active green flare count (0=none)
};
RWTexture2D<float4> g_out   : register(u0);
RWTexture2D<float>  g_depth : register(u1);
RWTexture2D<float4> g_accum : register(u2);
RWTexture2D<float4> g_guide : register(u3);   // denoiser guide: world normal.xyz + view-Z in .w
RaytracingAccelerationStructure g_scene : register(t0);
StructuredBuffer<Vertex> g_verts : register(t1);
StructuredBuffer<float4> g_flares : register(t2);   // up to uFlareN green flares: xyz = world pos, w = intensity

static const float PI = 3.14159265;
static const float kCell = 4.0;   // contracts::kCellSize
static const float kCeil = 3.0;   // contracts::kCeilingHeight
static const float3 kLightColor = float3(1.0, 0.95, 0.82);
static const float  kLightPower = 2.4;
static const float3 kEmit = float3(1.7, 1.6, 1.36);
static const float3 kAmbient = float3(0.06, 0.055, 0.045);  // multi-bounce floor (1-bounce GI underestimates an enclosed room)

float rndf(inout uint st){ st = st*747796405u + 2891336453u; uint w = ((st >> ((st >> 28u) + 4u)) ^ st) * 277803737u; return float((w >> 22u) ^ w) * (1.0/4294967296.0); }

bool is_emitter(float m){ return m > 2.5 && m < 3.5; }
float3 albedo_of(float m){
    if (m < 0.5) return float3(0.80, 0.72, 0.40);   // wallpaper
    if (m < 1.5) return float3(0.42, 0.30, 0.19);   // carpet
    if (m < 2.5) return float3(0.16, 0.16, 0.15);   // ceiling tile
    if (m < 3.5) return float3(0.0, 0.0, 0.0);       // fluorescent (emitter)
    if (m > 6.5) return float3(0.90, 0.42, 0.34);   // M25: the Shoggoth (warm salmon, R>G>B)
    return float3(0.28, 0.26, 0.22);                 // baseboard
}

bool occluded(float3 p, float3 target){
    float3 d = target - p; float dist = length(d);
    RayDesc r; r.Origin = p; r.Direction = d / dist; r.TMin = 1e-3; r.TMax = dist - 5e-3;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(g_scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, r);
    q.Proceed();
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// Direct lighting from the nearby fluorescent ceiling grid (NEE + shadow rays).
float3 direct_light(float3 P, float3 N){
    float3 sum = 0;
    int ci = (int)floor(P.x / kCell);
    int cj = (int)floor(P.z / kCell);
    [loop] for (int di = -2; di <= 2; ++di)
    [loop] for (int dj = -2; dj <= 2; ++dj){
        int gi = ci + di, gj = cj + dj;
        if (((gi & 1) != 0) || ((gj & 1) != 0)) continue;   // fluorescent cell = both even
        float3 L = float3((gi + 0.5) * kCell, kCeil, (gj + 0.5) * kCell);
        float3 toL = L - P; float dist2 = dot(toL, toL); float dist = sqrt(dist2);
        float3 wl = toL / dist;
        float ndl = max(dot(N, wl), 0.0);
        if (ndl <= 0.0) continue;
        if (occluded(P + N * 2e-3, L)) continue;
        sum += ndl / (1.0 + 0.35 * dist2);
    }
    return sum * kLightColor * kLightPower;
}

// Interactive eye-torch (uFlashI > 0): a soft cone of light from the camera (uPos) along uFwd. Only ever
// called for PRIMARY hits, which are visible from the eye by construction -> no shadow ray needed. The
// call site is [branch]-guarded on uFlashI, so when off (every golden) this is never reached.
static const float3 kFlashColor  = float3(1.0, 0.98, 0.92);  // crisp near-white torch (vs the warm ceiling)
static const float  kFlashCosIn  = 0.94;   // ~20 deg: full-bright inner cone
static const float  kFlashCosOut = 0.84;   // ~33 deg: soft outer edge
static const float  kFlashFall   = 0.035;  // distance falloff (gentle, for corridor reach)
float3 flashlight(float3 P, float3 N){
    float3 toL = uPos - P;
    float dist2 = dot(toL, toL);
    float dist = sqrt(max(dist2, 1e-8));
    float3 wl = toL / dist;
    float ndl = max(dot(N, wl), 0.0);
    float cs = dot(-wl, uFwd);                          // cosine of the angle off the torch axis
    float cone = smoothstep(kFlashCosOut, kFlashCosIn, cs);
    float atten = 1.0 / (1.0 + kFlashFall * dist2);
    return (uFlashI * cone * ndl * atten) * kFlashColor;
}

// Green "chemlight" flares (option A: analytic point lights, NO scene geometry). uFlareN == 0 -> never called
// (the call sites are [branch]-guarded), so every golden render is bit-identical. The host pre-culls to the
// nearest uFlareN (<= kFlareGpuMax), so these loops are short.
static const float3 kFlareColor = float3(0.16, 1.0, 0.28);   // chemlight green
static const float  kFlareReach = 7.0;     // metres a flare meaningfully lights a surface
static const float  kFlareGlowR = 0.40;    // radius of the visible glowing point along a view ray

// Diffuse green cast onto a surface from nearby flares (shadow-rayed, like direct_light -- no light through walls).
float3 flare_light(float3 P, float3 N){
    float3 sum = 0;
    const float reach2 = kFlareReach * kFlareReach;
    [loop] for (uint i = 0u; i < uFlareN; ++i){
        float4 f = g_flares[i];
        float3 toF = f.xyz - P; float d2 = dot(toF, toF);
        if (d2 > reach2) continue;
        float d = sqrt(max(d2, 1e-6));
        float ndl = max(dot(N, toF / d), 0.0);
        if (ndl <= 0.0) continue;
        if (occluded(P + N * 2e-3, f.xyz)) continue;
        sum += f.w * ndl / (1.0 + 0.7 * d2);
    }
    return sum * kFlareColor;
}

// The visible glowing stick: a flare sitting along the primary ray, in FRONT of the surface hit, adds green.
float3 flare_glow(float3 ro, float3 rd, float tHit){
    float3 g = 0;
    [loop] for (uint i = 0u; i < uFlareN; ++i){
        float4 f = g_flares[i];
        float t = dot(f.xyz - ro, rd);
        if (t <= 0.05 || t >= tHit) continue;               // behind the eye, or occluded by the wall we hit
        float3 closest = ro + rd * t;
        float r2 = dot(f.xyz - closest, f.xyz - closest);
        if (r2 > kFlareGlowR * kFlareGlowR) continue;
        float k = 1.0 - sqrt(r2) / kFlareGlowR;
        g += f.w * k * k * 5.0;                              // bright core, soft edge
    }
    return g * kFlareColor;
}

float3 cosine_dir(float3 N, float u1, float u2){
    float r = sqrt(u1); float phi = 2.0 * PI * u2;
    float3 t = normalize(abs(N.y) < 0.99 ? cross(float3(0,1,0), N) : cross(float3(1,0,0), N));
    float3 b = cross(N, t);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + N * sqrt(max(0.0, 1.0 - u1)));
}

struct Hit { bool hit; float3 P; float3 N; float mat; float t; };
Hit trace(float3 origin, float3 dir){
    Hit h; h.hit = false; h.P = 0; h.N = float3(0,1,0); h.mat = 0; h.t = 0;
    RayDesc r; r.Origin = origin; r.Direction = dir; r.TMin = 1e-3; r.TMax = uFar;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(g_scene, RAY_FLAG_NONE, 0xFF, r);
    while (q.Proceed()) {}
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT){
        uint base = q.CommittedInstanceID() + 3u * q.CommittedPrimitiveIndex();
        Vertex a = g_verts[base + 0], b = g_verts[base + 1], c = g_verts[base + 2];
        float2 bc = q.CommittedTriangleBarycentrics();
        float w = 1.0 - bc.x - bc.y;
        h.hit = true; h.t = q.CommittedRayT();
        h.P = origin + dir * h.t;
        h.N = normalize(float3(a.nx,a.ny,a.nz) * w + float3(b.nx,b.ny,b.nz) * bc.x + float3(c.nx,c.ny,c.nz) * bc.y);
        h.mat = a.mat;
    }
    return h;
}

float luma(float3 c){ return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// Edge-aware multi-scale denoise (uResolve==3). One dispatch, no ping-pong: blur the accumulated radiance in
// LINEAR space across several a-trous dilations (1,2,4,8 px), stopping the blur at depth + normal + luminance
// edges (the geometry guide is exact, so flat walls smooth out while corners/edges stay crisp). Then tonemap.
void denoise_resolve(uint2 px, uint2 dim){
    float4 gC = g_guide[px];
    float3 nC = gC.xyz; float zC = gC.w;
    float4 aC = g_accum[px];
    float3 cC = aC.rgb / max(aC.a, 1.0);   // per-pixel count: creature pixels = this frame only -> no ghost
    if (zC <= 0.0){                                   // background / miss: nothing to denoise, tonemap raw
        float3 b = cC * uExposure; b = b / (b + 1.0);
        g_out[px] = float4(saturate(b), 1.0);
        return;
    }
    float lC = luma(cC);
    float3 sum = cC; float wsum = 1.0;                // include the centre
    const float sigZ = 0.6, sigN = 32.0, sigL = 0.35;
    [unroll] for (uint level = 0u; level < 4u; ++level){
        int stp = int(1u << level);                   // 1, 2, 4, 8
        [unroll] for (int dy = -2; dy <= 2; ++dy)
        [unroll] for (int dx = -2; dx <= 2; ++dx){
            if (dx == 0 && dy == 0) continue;
            int2 q = int2(px) + int2(dx, dy) * stp;
            if (q.x < 0 || q.y < 0 || q.x >= int(dim.x) || q.y >= int(dim.y)) continue;
            float4 gq = g_guide[uint2(q)];
            if (gq.w <= 0.0) continue;                 // skip background neighbours
            float4 aq = g_accum[uint2(q)]; float3 cq = aq.rgb / max(aq.a, 1.0);
            float wz = exp(-abs(zC - gq.w) / (sigZ * max(zC, 1.0)));
            float wn = pow(max(dot(nC, gq.xyz), 0.0), sigN);
            float wl = exp(-abs(lC - luma(cq)) / sigL);
            float w = wz * wn * wl;
            sum += cq * w; wsum += w;
        }
    }
    float3 outc = sum / max(wsum, 1e-4);
    float3 c = outc * uExposure; c = c / (c + 1.0);   // Reinhard
    g_out[px] = float4(saturate(c), 1.0);
}

[shader("raygeneration")]
void RayGen(){
    uint2 px = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;
    if (uResolve == 3u){ denoise_resolve(px, dim); return; }   // filter pass: no tracing
    float2 uv = (float2(px) + 0.5) / float2(dim);
    float sx = (2.0 * uv.x - 1.0) * uTanY * uAspect;
    float sy = (1.0 - 2.0 * uv.y) * uTanY;
    float3 dir = normalize(uFwd + uRight * sx + uUp * sy);

    Hit h = trace(uPos, dir);

    float ndc = 1.0;
    if (h.hit){ float vz = h.t * dot(dir, uFwd); float q = uFar / (uFar - uNear); ndc = q * (1.0 - uNear / vz); }

    // Deterministic term (emission + direct) — identical for every sample.
    float3 deterministic = float3(0.012, 0.012, 0.03);   // background
    bool diffuse = false;
    float3 baseAlb = 0;
    if (h.hit){
        if (is_emitter(h.mat)){
            deterministic = kEmit;
        } else {
            baseAlb = albedo_of(h.mat);
            deterministic = baseAlb * (direct_light(h.P, h.N) + kAmbient);
            [branch] if (uFlashI > 0.0) deterministic += baseAlb * flashlight(h.P, h.N);  // eye-torch (off -> skipped; goldens bit-identical)
            [branch] if (uFlareN > 0u) deterministic += baseAlb * flare_light(h.P, h.N);  // green flares cast onto this surface
            diffuse = true;
        }
    }

    // Visible green flare points: emission along the primary ray (in front of the hit). Off (uFlareN==0) -> skipped.
    [branch] if (uFlareN > 0u) deterministic += flare_glow(uPos, dir, h.hit ? h.t : uFar);

    // Stochastic one-bounce indirect over this dispatch's samples.
    float3 indirectSum = 0;
    if (diffuse){
        [loop] for (uint s = 0; s < uSampleCount; ++s){
            uint sidx = uSampleStart + s;
            uint st = px.x * 1973u + px.y * 9277u + (sidx + 1u) * 26699u + uSeed * 68111u + uFrame * 9781u + 1u;
            float u1 = rndf(st), u2 = rndf(st);
            float3 wi = cosine_dir(h.N, u1, u2);
            Hit b = trace(h.P + h.N * 2e-3, wi);
            if (b.hit){
                if (is_emitter(b.mat)) indirectSum += baseAlb * kEmit;
                else indirectSum += baseAlb * albedo_of(b.mat) * direct_light(b.P, b.N);
            }
        }
    }

    float3 local = deterministic * float(uSampleCount) + indirectSum;
    // Per-pixel accumulated sample count lives in g_accum.a (previously an unused 1.0). The DYNAMIC creature
    // (material 7) rejects its history every frame -> moving creature pixels show only the current sample (no
    // ghost smear), while the static background keeps accumulating. Golden-safe: golden scenes have no material
    // 7 and uSampleStart==0 already resets, so pixCount == uTotal there -> total/pixCount is bit-identical.
    bool  resetPix  = (uSampleStart == 0u) || (h.hit && h.mat > 6.5 && h.mat < 7.5);
    float prevCount = resetPix ? 0.0 : g_accum[px].a;
    float3 prevSum  = resetPix ? float3(0.0, 0.0, 0.0) : g_accum[px].rgb;
    float3 total    = prevSum + local;
    float pixCount  = prevCount + float(uSampleCount);
    g_accum[px] = float4(total, pixCount);

    if (uResolve >= 1u){                                  // final batch: depth + denoiser guide
        g_depth[px] = ndc;
        float vz = h.hit ? (h.t * dot(dir, uFwd)) : 0.0;
        g_guide[px] = float4(h.hit ? h.N : float3(0,0,0), vz);
        if (uResolve == 1u){                              // denoise OFF: tonemap now (the M9 golden path)
            float3 c = total / max(pixCount, 1.0);   // per-pixel count (== uTotal in the golden path -> bit-identical)
            c *= uExposure;
            c = c / (c + 1.0);          // Reinhard
            g_out[px] = float4(saturate(c), 1.0);
        }
        // uResolve == 2u: denoise ON -> the separate filter pass writes g_out from g_accum + g_guide
    }
}
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

// M25: shadeVb reserves this many verts past the chunks for the dynamic creature mesh
// (build_shoggoth_mesh emits a fixed ~1248; 4096 is generous headroom).
constexpr uint32_t kMaxCreatureVerts = 4096u;

}  // namespace

bool DxrRenderer::build_scene(const std::vector<contracts::ResidentChunk>& chunks) {
    Impl& d = *impl_;
    if (!d.device) { last_error_ = "not initialised"; return false; }
    d.blas.clear(); d.vbs.clear(); d.tlas.Reset(); d.sceneReady = false;
    d.shadeVb.Reset(); d.shadeVertCount = 0;
    d.creatureBlas.Reset(); d.creatureVb.Reset(); d.chunkInstances.clear(); d.chunkVertCount = 0;
    d.creatureBlasVerts = 0;   // C: a scene rebuild drops the BLAS -> next update_creature does a full build, not a refit

    // Upload each chunk's vertices to an upload-heap buffer + build a BLAS. The
    // same vertices are also concatenated into one global buffer (shadeVb) the PT
    // shader reads for normals/material; each instance's InstanceID is its start
    // offset into that buffer, so (InstanceID + 3*PrimitiveIndex) indexes a vertex.
    std::vector<ComPtr<ID3D12Resource>> scratches;
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
    std::vector<contracts::ChunkVertex> allVerts;
    uint32_t vtxOffset = 0;
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

        // Audit fix: InstanceID is 24-bit; guard the shade-buffer offset rather than silently
        // wrapping (bounded residency keeps this well under 16M, so this only ever trips on a bug).
        if (vtxOffset > 0xFFFFFFu) { last_error_ = "DXR shade buffer exceeds 24-bit InstanceID"; return false; }
        D3D12_RAYTRACING_INSTANCE_DESC inst{};
        inst.Transform[0][0] = inst.Transform[1][1] = inst.Transform[2][2] = 1.0f;  // identity (world-space verts)
        inst.InstanceID = vtxOffset & 0xFFFFFFu;   // start vertex in shadeVb (24-bit)
        inst.InstanceMask = 0xFF;
        inst.AccelerationStructure = result->GetGPUVirtualAddress();
        instances.push_back(inst);
        d.blas.push_back(result);
        d.vbs.push_back(vb);
        scratches.push_back(scratch);
        allVerts.insert(allVerts.end(), rc.vertices, rc.vertices + rc.vertex_count);
        vtxOffset += rc.vertex_count;
    }
    if (instances.empty()) { last_error_ = "no geometry to build"; return false; }

    // Global shading vertex buffer (PT reads normals/material via InstanceID). M25:
    // reserve a tail of kMaxCreatureVerts so update_creature can drop the dynamic creature
    // in after the chunks without reallocating; cache the chunk instances + vert count so
    // the TLAS can be rebuilt around the creature each frame with the chunk BLASes intact.
    const UINT64 shadeBytes = static_cast<UINT64>(allVerts.size() + kMaxCreatureVerts) * sizeof(contracts::ChunkVertex);
    d.shadeVb = make_buffer(d.device.Get(), shadeBytes, D3D12_HEAP_TYPE_UPLOAD,
                            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
    if (!d.shadeVb) { last_error_ = "shade vb"; return false; }
    void* sv = nullptr; const D3D12_RANGE svnone{ 0, 0 };
    d.shadeVb->Map(0, &svnone, &sv);
    std::memcpy(sv, allVerts.data(), static_cast<size_t>(allVerts.size()) * sizeof(contracts::ChunkVertex));
    d.shadeVb->Unmap(0, nullptr);
    d.shadeVertCount = vtxOffset;
    // Flare upload buffer (StructuredBuffer t2): always present so the PT root SRV binds even when no flares
    // are dropped. flareCount stays 0 until set_flares() -> the shader's flare branches are skipped -> the
    // offline/golden render is bit-identical (set_flares is never called on the --dxr-pt / golden path).
    d.flareBuf = make_buffer(d.device.Get(), static_cast<UINT64>(kFlareGpuMax) * 16u, D3D12_HEAP_TYPE_UPLOAD,
                             D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
    if (!d.flareBuf) { last_error_ = "flare buf"; return false; }
    d.chunkInstances = instances;  // M25: cached for per-frame TLAS rebuilds
    d.chunkVertCount = vtxOffset;

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
    tin.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;   // RT_PERF item B2: rebuilt frequently (per-frame by update_creature) -> fast BUILD
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
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; range.NumDescriptors = 2; range.BaseShaderRegister = 0;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;  // u0 = color, u1 = depth
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

    // Build the PT state object + SBT once (inline RayQuery, SM 6.5, raygen-only).
    if (!d.ptPso) {
        D3D12_DESCRIPTOR_RANGE prange{};
        prange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; prange.NumDescriptors = 4; prange.BaseShaderRegister = 0;  // u0,u1,u2,u3
        prange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER prp[5]{};
        prp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        prp[0].Constants.Num32BitValues = 28; prp[0].Constants.ShaderRegister = 0;
        prp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        prp[1].DescriptorTable.NumDescriptorRanges = 1; prp[1].DescriptorTable.pDescriptorRanges = &prange;
        prp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; prp[2].Descriptor.ShaderRegister = 0;  // t0 = TLAS
        prp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; prp[3].Descriptor.ShaderRegister = 1;  // t1 = shadeVb
        prp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; prp[4].Descriptor.ShaderRegister = 2;  // t2 = flares
        D3D12_ROOT_SIGNATURE_DESC prs{}; prs.NumParameters = 5; prs.pParameters = prp; prs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> psig, pse;
        if (FAILED(D3D12SerializeRootSignature(&prs, D3D_ROOT_SIGNATURE_VERSION_1, &psig, &pse))) { last_error_ = "pt RS serialize"; return false; }
        if (FAILED(d.device->CreateRootSignature(0, psig->GetBufferPointer(), psig->GetBufferSize(), IID_PPV_ARGS(&d.ptRS)))) { last_error_ = "pt RS"; return false; }

        DxcCompiler dxc;
        std::vector<uint8_t> dxil; std::string cerr;
        if (!dxc.compile_library(kPtShader, dxil, cerr, "lib_6_5")) { last_error_ = "pt compile: " + cerr; return false; }
        D3D12_EXPORT_DESC pex = { L"RayGen", nullptr, D3D12_EXPORT_FLAG_NONE };
        D3D12_DXIL_LIBRARY_DESC plib{}; plib.DXILLibrary.pShaderBytecode = dxil.data();
        plib.DXILLibrary.BytecodeLength = dxil.size(); plib.NumExports = 1; plib.pExports = &pex;
        D3D12_RAYTRACING_SHADER_CONFIG psc{}; psc.MaxPayloadSizeInBytes = 4; psc.MaxAttributeSizeInBytes = 8;
        D3D12_RAYTRACING_PIPELINE_CONFIG ppc{}; ppc.MaxTraceRecursionDepth = 1;  // inline RayQuery (no TraceRay recursion)
        D3D12_GLOBAL_ROOT_SIGNATURE pgrs{}; pgrs.pGlobalRootSignature = d.ptRS.Get();
        D3D12_STATE_SUBOBJECT psubs[4];
        psubs[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY; psubs[0].pDesc = &plib;
        psubs[1].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG; psubs[1].pDesc = &psc;
        psubs[2].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG; psubs[2].pDesc = &ppc;
        psubs[3].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE; psubs[3].pDesc = &pgrs;
        D3D12_STATE_OBJECT_DESC pso_desc{}; pso_desc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE; pso_desc.NumSubobjects = 4; pso_desc.pSubobjects = psubs;
        if (FAILED(d.device->CreateStateObject(&pso_desc, IID_PPV_ARGS(&d.ptPso)))) { last_error_ = "pt state object"; return false; }

        ComPtr<ID3D12StateObjectProperties> pprops;
        d.ptPso.As(&pprops);
        const void* pid = pprops->GetShaderIdentifier(L"RayGen");
        if (!pid) { last_error_ = "pt raygen id"; return false; }
        const UINT64 pslot = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
        d.ptSbt = make_buffer(d.device.Get(), pslot, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
        uint8_t* psm = nullptr; const D3D12_RANGE pn{ 0, 0 };
        d.ptSbt->Map(0, &pn, reinterpret_cast<void**>(&psm));
        std::memcpy(psm, pid, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        d.ptSbt->Unmap(0, nullptr);
    }

    d.sceneReady = true;
    return true;
}

// M25: drop one dynamic creature mesh into the already-built scene -- write its verts into
// the reserved shadeVb tail, build a single creature BLAS, and rebuild ONLY the TLAS
// (chunk BLASes stay cached). Cheap enough per frame for the creature to writhe in RT.
bool DxrRenderer::update_creature(const contracts::ChunkVertex* verts, uint32_t count) {
    Impl& d = *impl_;
    if (!d.sceneReady) { last_error_ = "scene not built"; return false; }
    if (count < 3) return true;                       // nothing to add
    if (count > kMaxCreatureVerts) count = kMaxCreatureVerts;  // clamp to the reserved tail

    d.wait_idle();  // the previous frame's trace is done before we touch shared buffers
    if (FAILED(d.alloc->Reset())) { last_error_ = "alloc reset"; return false; }
    if (FAILED(d.list->Reset(d.alloc.Get(), nullptr))) { last_error_ = "list reset"; return false; }
    const D3D12_RANGE none{ 0, 0 };

    // 1) The creature's verts go into the shadeVb tail (right after the chunk verts), so the
    //    PT shader reads them via InstanceID = chunkVertCount. The chunk region is untouched.
    void* sv = nullptr;
    if (FAILED(d.shadeVb->Map(0, &none, &sv))) { last_error_ = "shade map"; return false; }
    std::memcpy(static_cast<contracts::ChunkVertex*>(sv) + d.chunkVertCount, verts,
                static_cast<size_t>(count) * sizeof(contracts::ChunkVertex));
    d.shadeVb->Unmap(0, nullptr);
    d.shadeVertCount = d.chunkVertCount + count;

    // 2) Creature vertex buffer + BLAS.
    const UINT64 vbytes = static_cast<UINT64>(count) * sizeof(contracts::ChunkVertex);
    d.creatureVb = make_buffer(d.device.Get(), vbytes, D3D12_HEAP_TYPE_UPLOAD,
                               D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
    if (!d.creatureVb) { last_error_ = "creature vb"; return false; }
    void* cm = nullptr; d.creatureVb->Map(0, &none, &cm);
    std::memcpy(cm, verts, static_cast<size_t>(vbytes));
    d.creatureVb->Unmap(0, nullptr);

    D3D12_RAYTRACING_GEOMETRY_DESC geo{};
    geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geo.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geo.Triangles.VertexBuffer.StartAddress = d.creatureVb->GetGPUVirtualAddress();
    geo.Triangles.VertexBuffer.StrideInBytes = sizeof(contracts::ChunkVertex);
    geo.Triangles.VertexCount = count;
    geo.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    // RT_PERF item C: ALLOW_UPDATE so the writhe REFITS the BLAS in place (the mesh is writhe-stable -- fixed
    // topology, only positions animate, asserted by the [m25][body] test) instead of a full rebuild every frame.
    // Refit when the vert count matches the last build AND a prior BLAS exists; a count change or a scene rebuild
    // (which Resets creatureBlas + creatureBlasVerts) falls back to a full build. Refit uses the smaller
    // UpdateScratch size and SourceAS = the existing BLAS (in-place). The traced result is identical to a rebuild
    // -- this is a build-cost optimization only, so the offline/golden path (one build, never a refit) is unaffected.
    const bool refit = d.creatureBlas && (count == d.creatureBlasVerts);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bin{};
    bin.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bin.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD
              | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;   // ALLOW_UPDATE -> refittable
    bin.NumDescs = 1; bin.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY; bin.pGeometryDescs = &geo;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bpi{};
    d.device->GetRaytracingAccelerationStructurePrebuildInfo(&bin, &bpi);   // query with ALLOW_UPDATE -> UpdateScratch sized
    const UINT64 bscratchBytes = refit ? bpi.UpdateScratchDataSizeInBytes : bpi.ScratchDataSizeInBytes;
    ComPtr<ID3D12Resource> bscratch = make_buffer(d.device.Get(), bscratchBytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!refit) {   // a full build (re)allocates the BLAS; a refit reuses it in place
        d.creatureBlas = make_buffer(d.device.Get(), bpi.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    }
    if (!bscratch || !d.creatureBlas) { last_error_ = "creature blas buffers"; return false; }
    d.creatureBlasVerts = count;
    if (refit) bin.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;   // build flag (not in the prebuild query)
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bbd{};
    bbd.Inputs = bin;
    bbd.ScratchAccelerationStructureData = bscratch->GetGPUVirtualAddress();
    bbd.DestAccelerationStructureData = d.creatureBlas->GetGPUVirtualAddress();
    if (refit) bbd.SourceAccelerationStructureData = d.creatureBlas->GetGPUVirtualAddress();   // in-place refit
    d.list->BuildRaytracingAccelerationStructure(&bbd, 0, nullptr);
    D3D12_RESOURCE_BARRIER uavb{}; uavb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; uavb.UAV.pResource = d.creatureBlas.Get();
    d.list->ResourceBarrier(1, &uavb);

    // 3) TLAS = cached chunk instances ++ the creature instance (InstanceID = chunkVertCount).
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances = d.chunkInstances;
    D3D12_RAYTRACING_INSTANCE_DESC ci{};
    ci.Transform[0][0] = ci.Transform[1][1] = ci.Transform[2][2] = 1.0f;  // identity (world-space verts)
    ci.InstanceID = d.chunkVertCount & 0xFFFFFFu;
    ci.InstanceMask = 0xFF;
    ci.AccelerationStructure = d.creatureBlas->GetGPUVirtualAddress();
    instances.push_back(ci);

    const UINT64 instBytes = instances.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    ComPtr<ID3D12Resource> instBuf = make_buffer(d.device.Get(), instBytes, D3D12_HEAP_TYPE_UPLOAD,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
    void* im = nullptr; instBuf->Map(0, &none, &im);
    std::memcpy(im, instances.data(), static_cast<size_t>(instBytes));
    instBuf->Unmap(0, nullptr);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tin{};
    tin.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tin.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;   // RT_PERF item B2: rebuilt frequently (per-frame by update_creature) -> fast BUILD
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

    if (FAILED(d.list->Close())) { last_error_ = "creature build close"; return false; }
    ID3D12CommandList* lists[] = { d.list.Get() };
    d.queue->ExecuteCommandLists(1, lists);
    d.wait_idle();  // scratch/instBuf/bscratch can drop after the build completes
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
    c[8] = right.x; c[9] = right.y; c[10] = right.z; c[11] = kSceneNear;
    c[12] = up.x;   c[13] = up.y;   c[14] = up.z;   c[15] = kSceneFar;

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

    // Copy the primary-hit NDC depth to its readback buffer (M9 gate #1).
    const D3D12_RESOURCE_BARRIER dToCopy = transition(d.depthTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    d.list->ResourceBarrier(1, &dToCopy);
    D3D12_TEXTURE_COPY_LOCATION ddst{}; ddst.pResource = d.depthReadback.Get(); ddst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; ddst.PlacedFootprint = d.depthFootprint;
    D3D12_TEXTURE_COPY_LOCATION dsrc{}; dsrc.pResource = d.depthTex.Get(); dsrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dsrc.SubresourceIndex = 0;
    d.list->CopyTextureRegion(&ddst, 0, 0, 0, &dsrc, nullptr);
    const D3D12_RESOURCE_BARRIER dBack = transition(d.depthTex.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    d.list->ResourceBarrier(1, &dBack);

    if (FAILED(d.list->Close())) { last_error_ = "scene list close"; return false; }
    ID3D12CommandList* lists[] = { d.list.Get() };
    d.queue->ExecuteCommandLists(1, lists);
    d.wait_idle();
    return true;
}

bool DxrRenderer::render_pt_frame(const contracts::CameraPose& cam, uint32_t samples, uint32_t seed, bool reset,
                                 bool denoise, uint32_t frame) {
    Impl& d = *impl_;
    if (!d.sceneReady || !d.ptPso || !d.shadeVb) { last_error_ = "pt scene not built"; return false; }
    if (samples == 0) samples = 1;
    const uint32_t base = reset ? 0u : d.accumSamples;   // continue the accumulation, or start fresh
    const uint32_t total = base + samples;

    const float cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
    const float syaw = std::sin(cam.yaw), cyaw = std::cos(cam.yaw);
    const V3 fwd = norm3({ cp * syaw, sp, cp * cyaw });
    const V3 right = norm3(cross3({ 0, 1, 0 }, fwd));
    const V3 up = cross3(fwd, right);

    auto setf = [](uint32_t* arr, int i, float f) { std::memcpy(&arr[i], &f, sizeof(float)); };

    // Copy the resolved color (u0) into the readback buffer -- the last step of the resolve batch (denoise
    // off) or of the filter pass (denoise on). Assumes d.list is open + the color is freshly written.
    auto copy_color = [&]() {
        const D3D12_RESOURCE_BARRIER toCopy = transition(d.uav.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        d.list->ResourceBarrier(1, &toCopy);
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = d.readbackBuf.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = d.footprint;
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = d.uav.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
        d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        const D3D12_RESOURCE_BARRIER back = transition(d.uav.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        d.list->ResourceBarrier(1, &back);
    };
    auto bind_pt = [&](const uint32_t* c) {
        ID3D12DescriptorHeap* heaps[] = { d.uavHeap.Get() };
        d.list->SetDescriptorHeaps(1, heaps);
        d.list->SetComputeRootSignature(d.ptRS.Get());
        d.list->SetComputeRoot32BitConstants(0, 28, c, 0);
        d.list->SetComputeRootDescriptorTable(1, d.uavHeap->GetGPUDescriptorHandleForHeapStart());
        d.list->SetComputeRootShaderResourceView(2, d.tlas->GetGPUVirtualAddress());
        d.list->SetComputeRootShaderResourceView(3, d.shadeVb->GetGPUVirtualAddress());
        d.list->SetComputeRootShaderResourceView(4, d.flareBuf->GetGPUVirtualAddress());   // t2: green flares
        d.list->SetPipelineState1(d.ptPso.Get());
    };
    D3D12_DISPATCH_RAYS_DESC dr{};
    dr.RayGenerationShaderRecord = { d.ptSbt->GetGPUVirtualAddress(), D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT };
    dr.Width = d.width; dr.Height = d.height; dr.Depth = 1;

    // Accumulate in batches of samples per dispatch (keeps each DispatchRays well under the GPU watchdog).
    // The final batch resolves: depth + denoiser guide always; tonemap to color now only when denoise is OFF
    // (uResolve 1). With denoise ON (uResolve 2) the separate filter pass below produces the color.
    const uint32_t kBatch = 64u;
    for (uint32_t start = 0; start < samples; start += kBatch) {
        const uint32_t count = (samples - start < kBatch) ? (samples - start) : kBatch;
        const bool resolve = (start + count >= samples);

        uint32_t c[28] = {};
        setf(c, 0, cam.pos[0]); setf(c, 1, cam.pos[1]); setf(c, 2, cam.pos[2]); setf(c, 3, std::tan(cam.fov_y * 0.5f));
        setf(c, 4, fwd.x);   setf(c, 5, fwd.y);   setf(c, 6, fwd.z);   setf(c, 7, cam.aspect);
        setf(c, 8, right.x); setf(c, 9, right.y); setf(c, 10, right.z); setf(c, 11, kSceneNear);
        setf(c, 12, up.x);   setf(c, 13, up.y);   setf(c, 14, up.z);   setf(c, 15, kSceneFar);
        c[16] = base + start; c[17] = count; c[18] = total; c[19] = seed;
        c[20] = resolve ? (denoise ? 2u : 1u) : 0u; setf(c, 21, kPtExposure);
        setf(c, 22, static_cast<float>(d.width)); setf(c, 23, static_cast<float>(d.height));
        c[24] = frame;  // uFrame: per-frame noise decorrelation (0 for the deterministic offline/golden path)
        setf(c, 25, d.flashIntensity);  // uFlashI: interactive flashlight (0 = off -> shader branch skipped, goldens bit-identical)
        c[26] = d.flareCount;           // uFlareN: active green flares (0 = none -> flare branches skipped, goldens bit-identical)

        if (FAILED(d.alloc->Reset())) { last_error_ = "pt alloc reset"; return false; }
        if (FAILED(d.list->Reset(d.alloc.Get(), nullptr))) { last_error_ = "pt list reset"; return false; }
        bind_pt(c);
        d.list->DispatchRays(&dr);

        if (resolve) {
            const D3D12_RESOURCE_BARRIER dToCopy = transition(d.depthTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            d.list->ResourceBarrier(1, &dToCopy);
            D3D12_TEXTURE_COPY_LOCATION ddst{}; ddst.pResource = d.depthReadback.Get(); ddst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; ddst.PlacedFootprint = d.depthFootprint;
            D3D12_TEXTURE_COPY_LOCATION dsrc{}; dsrc.pResource = d.depthTex.Get(); dsrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dsrc.SubresourceIndex = 0;
            d.list->CopyTextureRegion(&ddst, 0, 0, 0, &dsrc, nullptr);
            const D3D12_RESOURCE_BARRIER dBack = transition(d.depthTex.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            d.list->ResourceBarrier(1, &dBack);
            if (denoise) {
                // RT_PERF item B1: fold the denoise dispatch into THIS list -> one ExecuteCommandLists + wait_idle
                // for the frame instead of two. The accumulate pass (uResolve==2) wrote g_accum + g_guide; a UAV
                // barrier makes those writes visible to the denoise pass (uResolve==3) which reads them and writes
                // the resolved color. Root sig/PSO/heaps/SRVs stay bound from bind_pt() above; only constants change.
                D3D12_RESOURCE_BARRIER uavb[2]{};
                uavb[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; uavb[0].UAV.pResource = d.accumTex.Get();
                uavb[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; uavb[1].UAV.pResource = d.guideTex.Get();
                d.list->ResourceBarrier(2, uavb);
                uint32_t cd[28] = {};
                cd[18] = total; cd[20] = 3u; setf(cd, 21, kPtExposure);
                setf(cd, 22, static_cast<float>(d.width)); setf(cd, 23, static_cast<float>(d.height));
                cd[24] = frame;
                d.list->SetComputeRoot32BitConstants(0, 28, cd, 0);
                d.list->DispatchRays(&dr);
            }
            copy_color();   // denoise off: the uResolve==1 tonemap is final; denoise on: the fold above made it final
        }

        if (FAILED(d.list->Close())) { last_error_ = "pt list close"; return false; }
        ID3D12CommandList* lists[] = { d.list.Get() };
        d.queue->ExecuteCommandLists(1, lists);
        d.wait_idle();
    }

    d.accumSamples = total;
    return true;
}

bool DxrRenderer::render_pt(const contracts::CameraPose& cam, uint32_t samples, uint32_t seed) {
    return render_pt_frame(cam, samples, seed, true);
}

void DxrRenderer::set_flashlight(bool on) { if (impl_) impl_->flashIntensity = on ? kFlashIntensity : 0.0f; }

void DxrRenderer::set_flares(const float* xyzi, uint32_t count) {
    if (!impl_) return;
    Impl& d = *impl_;
    if (count > kFlareGpuMax) count = kFlareGpuMax;
    d.flareCount = count;
    if (count == 0u || !d.flareBuf || !xyzi) return;   // nothing to upload (uFlareN stays 0 -> shader skips)
    void* p = nullptr; const D3D12_RANGE none{ 0, 0 };
    if (SUCCEEDED(d.flareBuf->Map(0, &none, &p)) && p) {
        std::memcpy(p, xyzi, static_cast<size_t>(count) * 16u);   // float4 {x,y,z,intensity} per flare
        d.flareBuf->Unmap(0, nullptr);
    }
}

uint32_t DxrRenderer::accum_samples() const { return impl_ ? impl_->accumSamples : 0; }
void* DxrRenderer::pt_output() const { return impl_ ? impl_->uav.Get() : nullptr; }   // RT_PERF item A

}  // namespace br::render_dxr
