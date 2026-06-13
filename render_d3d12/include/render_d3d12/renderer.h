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

#include "contracts/world_view_v1.h"
#include "contracts/stream_events_v1.h"

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

    // Headless: render the test-room geometry from the view's camera (lit), with
    // depth, into the offscreen target. Deterministic for a fixed view/GPU.
    bool render_world_view(const contracts::WorldView& view);

    // Select the seed whose procedural material textures are used by the lit
    // chunk render (M5). Cheap; the upload happens lazily on the next draw.
    void set_texture_seed(uint64_t seed);

    // Enable/configure the VHS post-process pass (M8): seeded film grain,
    // chromatic aberration, barrel distortion, scanlines, vignette. `time` (sim
    // seconds) drives grain/interlace; `hud` composites the overlay. When enabled,
    // headless renders read back the post-processed image. Cheap; the pipeline is
    // built lazily on the next render.
    void set_post(bool enabled, uint32_t seed, float time, bool hud);

    // Upload an RGBA HUD overlay (width*height, must match the render size) that
    // the post pass composites on top (M8). Builds the post pipeline if needed.
    bool upload_hud_overlay(const uint8_t* rgba, uint32_t width, uint32_t height);

    // Headless: draw the resident streamed chunks from the camera (depth-tested,
    // vertex-colored). Uploads up to `upload_budget` new chunk meshes per call
    // and frees meshes that are no longer resident (bounds GPU memory, smooths
    // hitches). Returns the number of chunks actually drawn via out_drawn.
    bool render_chunks(const contracts::CameraPose& camera,
                       const std::vector<contracts::ResidentChunk>& resident,
                       uint32_t upload_budget, uint64_t tick, uint32_t* out_drawn);

    // Headless: orthographic top-down render of the given chunks over the world
    // region [cx-half, cx+half] x [cz-half, cz+half] (a debug-render golden).
    bool render_topdown(const std::vector<contracts::ResidentChunk>& chunks,
                        float cx, float cz, float half);

    // Headless only: copy the rendered target back to CPU as tight RGBA.
    bool readback(FrameImage& out);

    // Headless only: copy the D32_FLOAT depth buffer from the last render back to
    // CPU as NDC depth (DirectX [0,1], left-handed; cleared/background = 1.0),
    // row-major width*height. Comparable to render_dxr's depth at the same pose
    // (M9 gate #1). Leaves the depth buffer in DEPTH_WRITE for the next render.
    bool readback_depth(std::vector<float>& out, uint32_t* width, uint32_t* height);

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
