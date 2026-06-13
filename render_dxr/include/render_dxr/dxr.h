#pragma once
//
// render_dxr/dxr.h — DXR path-traced mode (M9). Enhancement-only (INV-6): the
// raster renderer stays the default + fallback. `render_dxr` is self-contained
// (its own ID3D12Device5) and consumes the same streamed geometry as raster.
//
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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

// Headless DXR renderer (M9). Self-contained: own Device5 + queue + raytracing
// state object + shader binding table; dispatches rays and reads back RGBA.
// Phase 1b proves the full DispatchRays pipeline (a raygen UV gradient, no
// TraceRay yet); BLAS/TLAS + primary rays land in phase 2.
class DxrRenderer {
public:
    DxrRenderer();
    ~DxrRenderer();
    DxrRenderer(const DxrRenderer&) = delete;
    DxrRenderer& operator=(const DxrRenderer&) = delete;

    bool init(uint32_t width, uint32_t height);
    bool render_gradient();                       // raygen writes a UV gradient
    bool readback(std::vector<uint8_t>& rgba);    // size width*height*4, RGBA
    uint32_t width() const;
    uint32_t height() const;
    uint32_t debug_error_count();

    const std::string& last_error() const { return last_error_; }

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    std::string last_error_;
};

}  // namespace br::render_dxr
