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

#include "contracts/stream_events_v1.h"
#include "contracts/world_view_v1.h"

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

    // Build BLAS-per-chunk + a TLAS from the resident streamed geometry (M9
    // phase 2). Vertices are world-space (no per-instance transform), so the AS
    // holds exactly what the raster renderer draws.
    bool build_scene(const std::vector<contracts::ResidentChunk>& chunks);

    // M25: update a single DYNAMIC creature mesh (the Shoggoth's body) without rebuilding
    // the cached chunk BLASes — write its verts into the reserved tail of the shade buffer,
    // build one creature BLAS, and rebuild only the TLAS. Cheap enough to call every frame
    // so the creature shows + animates in the ray-traced path. Call after build_scene.
    bool update_creature(const contracts::ChunkVertex* verts, uint32_t count);

    // Cast primary rays from the camera through the TLAS; closest-hit shades by
    // distance, miss = background. (gate #1 visualisation; depth-compare next.)
    bool render_scene(const contracts::CameraPose& camera);

    // Path-traced render (M9 phase 3): accumulate `samples` spp into a float
    // buffer via inline RayQuery — emissive fluorescents as area-grid lights,
    // shadow rays for direct visibility, one cosine-weighted diffuse-GI bounce,
    // seeded per-(pixel,sample) RNG — then tonemap to the RGBA output and write
    // NDC depth. Deterministic for a fixed (scene, camera, samples, seed); read
    // the result with readback() / readback_depth(). Equivalent to a single
    // render_pt_frame() with reset = true.
    bool render_pt(const contracts::CameraPose& camera, uint32_t samples, uint32_t seed);

    // Interactive PT (M9 phase 4): accumulate `samples` more spp this frame. When
    // `reset` is true the accumulator is cleared first (call this whenever the
    // camera moves — otherwise stale samples ghost); when false the new samples
    // refine the converging image across frames. Resolves to RGBA + NDC depth each
    // call. The per-sample RNG indexes continue across frames, so accumulation is
    // progressive and deterministic.
    //
    // `denoise` (default off): run an edge-aware multi-scale spatial filter over the
    // accumulated radiance (geometry-guided depth+normal edge-stopping) before tonemap —
    // turns a noisy few-spp frame clean. INTERACTIVE ONLY; leave off for the converged
    // goldens (their seed/output must stay bit-stable). `frame` decorrelates the per-pixel
    // RNG across frames (0 for the deterministic offline path).
    bool render_pt_frame(const contracts::CameraPose& camera, uint32_t samples,
                         uint32_t seed, bool reset, bool denoise = false, uint32_t frame = 0);

    // Interactive flashlight (default OFF): a torch co-located with the eye, aimed along the camera
    // forward, that brightens primary hits inside a soft cone — no 3D model, no shadow rays (a primary
    // hit is eye-visible by construction). OFF (the default, and the only state the offline/golden path
    // ever uses) keeps the PT output bit-identical: the shader's flashlight branch is skipped.
    void set_flashlight(bool on);

    // Samples accumulated into the current (un-reset) image — 0 right after a reset.
    uint32_t accum_samples() const;

    bool readback(std::vector<uint8_t>& rgba);    // size width*height*4, RGBA

    // Read back the per-pixel primary-hit depth from the last render_scene() as
    // NDC depth (DirectX [0,1], left-handed; background/miss = 1.0), row-major
    // width*height. Uses the same hyperbolic projection as render_d3d12's depth
    // buffer at the same pose, so the two are comparable per pixel (M9 gate #1).
    bool readback_depth(std::vector<float>& depth);

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
