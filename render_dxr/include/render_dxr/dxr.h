#pragma once
//
// render_dxr/dxr.h — DXR path-traced mode (M9). Enhancement-only (INV-6): the
// raster renderer stays the default + fallback. `render_dxr` is self-contained
// (its own ID3D12Device5) and consumes the same streamed geometry as raster.
//
#include <string>

namespace br::render_dxr {

// End-to-end DXR capability, probed on the actual device + toolchain.
struct DxrCaps {
    bool device5 = false;        // ID3D12Device5 obtained
    int raytracing_tier = 0;     // 0 none, 10 = TIER_1_0, 11 = TIER_1_1
    bool dxc_available = false;  // dxcompiler.dll loaded + DxcCreateInstance ok
    bool dxc_compiled = false;   // a trivial DXR shader library compiled to DXIL
    std::string adapter;         // adapter description (UTF-8)
    std::string detail;          // diagnostic / error detail
};

// Probe device + DXR tier + DXC shader compilation. Pure query; creates and
// tears down a throwaway device. Never throws.
DxrCaps probe_caps();

}  // namespace br::render_dxr
