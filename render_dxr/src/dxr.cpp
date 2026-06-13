#include "render_dxr/dxr.h"

#include "dxc.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
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

}  // namespace br::render_dxr
