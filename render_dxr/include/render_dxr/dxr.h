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

    // `external_device5` (optional, void* = an ID3D12Device5*, INV-5): when non-null AND DXR-capable, reuse it
    // instead of creating an own device — so the PT output lives on the caller's device and can be presented
    // without a CPU readback (RT_PERF item A). Null = create+own a device (the headless/offline path).
    bool init(uint32_t width, uint32_t height, void* external_device5 = nullptr);
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
    //
    // `aa` (default off): jitter the primary ray a sub-pixel amount per frame so temporal
    // accumulation resolves anti-aliased edges (free temporal AA). INTERACTIVE ONLY — the
    // offline/golden + gate paths leave it off so their output stays bit-identical.
    //
    // `stochastic_lights` (default off): sample ONE ceiling light per shading point via a
    // weighted reservoir (RIS) + a single shadow ray, instead of shadow-raying the whole 5x5
    // grid — far fewer shadow rays in light-dense rooms. Unbiased (converges to the full-NEE
    // image via temporal accumulation; proven by run_dxr_stoch). INTERACTIVE ONLY — offline/
    // golden uses full NEE so goldens stay bit-identical.
    //
    // `want_readback` (default ON): at the resolve, copy the color + depth into the CPU-readback
    // staging buffers that readback()/readback_depth() map. The interactive game passes false on
    // every frame nothing reads back (the two copies are ~30 MB/frame of dead PCIe traffic at a
    // 4K-Quality internal res) and true only on the sparse POV-grab frames (Director vision /
    // voice chat / --out). Offline/golden/gate paths keep the default, so every readback()-based
    // oracle is untouched. NOTE: readback()/readback_depth() after a want_readback=false frame
    // map the LAST copied frame (stale) — request the copy on the frame you read.
    bool render_pt_frame(const contracts::CameraPose& camera, uint32_t samples,
                         uint32_t seed, bool reset, bool denoise = false, uint32_t frame = 0,
                         bool aa = false, bool stochastic_lights = false, bool want_readback = true);

    // Interactive flashlight (default OFF): a torch co-located with the eye, aimed along the camera
    // forward, that brightens primary hits inside a soft cone — no 3D model, no shadow rays (a primary
    // hit is eye-visible by construction). OFF (the default, and the only state the offline/golden path
    // ever uses) keeps the PT output bit-identical: the shader's flashlight branch is skipped.
    void set_flashlight(bool on);

    // Interactive green flares (default NONE): up to `count` analytic point lights, packed as float4
    // {x,y,z,intensity}, that the PT shader adds as emissive green "chemlight" breadcrumbs — they light
    // nearby surfaces (shadow-rayed) and show as glowing points. Presentation only; `count==0` (the default,
    // and the only state the offline/golden path ever uses) keeps the PT output bit-identical (the shader's
    // flare branches are skipped). The caller pre-culls to the nearest few; this uploads them for the next render.
    void set_flares(const float* xyzi, uint32_t count);

    // Apparition Phase 2b.2 (default 1.0 = OFF): a soft "dread" dim of the path-traced output while a recent
    // apparition verdict lingers. Applied POST-accumulation (a display multiply on the final tonemapped color),
    // so it never pollutes the radiance accumulator and can decay per-frame with no reset / no re-noising.
    // 1.0 (the default, and the only state the offline/golden path ever uses) keeps the PT output bit-identical:
    // the shader's `[branch] if (uDread < 1.0)` is skipped. Clamped to [0,1].
    void set_dread(float dim01);

    // Samples accumulated into the current (un-reset) image — 0 right after a reset.
    uint32_t accum_samples() const;

    bool readback(std::vector<uint8_t>& rgba);    // size width*height*4, RGBA

    // The path-traced color output (an ID3D12Resource*, returned as void* per INV-5). Valid after a
    // render_pt_frame(); left in UNORDERED_ACCESS state. When DxrRenderer was init'd on an external device,
    // the raster renderer can sample this directly (present_pt_texture) — no CPU readback. Null if not init'd.
    void* pt_output() const;

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
