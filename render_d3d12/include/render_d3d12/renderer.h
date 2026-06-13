#pragma once
//
// render_d3d12/renderer.h — minimal D3D12 renderer surface for M1.
//
// Opaque PIMPL: no D3D12, DXGI, or <windows.h> types appear here, so the sim
// core stays isolated (INV-5) and `app` can drive rendering without inheriting
// the graphics headers. M1 renders a single deterministic clear-color frame,
// headless (offscreen -> CPU RGBA) or windowed (swapchain -> present).
//
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace br::render_d3d12 {

// Tightly-packed 8-bit RGBA frame read back from the GPU (headless path).
struct FrameImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;  // size == width*height*4, row-major, no padding
};

// The deterministic clear color M1 renders, as 8-bit RGBA. Exposed so goldens
// and gates can reason about expected output without a GPU.
struct ClearColor {
    uint8_t r, g, b, a;
};
ClearColor clear_color() noexcept;

class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Initialize a headless device + offscreen render target (no window).
    bool init_headless(uint32_t width, uint32_t height);

    // Initialize a device + swapchain bound to a native window handle (HWND
    // passed as void* to keep <windows.h> out of this header).
    bool init_windowed(void* native_window_handle, uint32_t width, uint32_t height);

    // Record + submit one clear-color frame; present if windowed. Blocks on the
    // frame fence so the GPU is idle on return.
    bool render_clear_frame();

    // Headless only: copy the rendered target back to CPU as tight RGBA.
    bool readback(FrameImage& out);

    // Count of D3D12 debug-layer messages at CORRUPTION/ERROR/WARNING severity
    // accumulated so far (the M1 debug-layer gate requires this to be 0).
    uint32_t debug_error_count();

    // Current process private-bytes usage (for the memory soak gate).
    uint64_t process_private_bytes();

    const std::string& last_error() const noexcept { return last_error_; }
    void shutdown();

    struct Impl;  // opaque; defined in renderer.cpp (kept accessible for helpers)

private:
    std::unique_ptr<Impl> impl_;
    std::string last_error_;
};

}  // namespace br::render_d3d12
