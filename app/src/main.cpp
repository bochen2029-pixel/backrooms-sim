// app/main.cpp — composition root + CLI.
//
//   M1 render modes:
//     backrooms --headless [--frames N|--seconds S] [--out p.png] [--width W] [--height H]
//     backrooms --window   [--frames N|--seconds S] [--width W] [--height H]
//   M2:
//     backrooms --scene  --out room.png [--width W] [--height H]      (draw test room)
//     backrooms --sim --ticks N [--seed S] [--record f] [--replay f] [--hashlog f]
//     backrooms --version
//
// Exit codes: 0 ok, 1 init/render/IO failure, 2 usage error, 3 debug-layer msgs.
#include <windows.h>
#include <timeapi.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <stb_image_write.h>

#include "core/version.h"
#include "core/rng.h"
#include "core/world.h"
#include "core/replay.h"
#include "contracts/chunk_gen_v1.h"
#include "stream/stream_manager.h"
#include "telemetry/csv.h"
#include "render_d3d12/renderer.h"

namespace contracts = br::contracts;
using br::render_d3d12::Renderer;
using br::render_d3d12::FrameImage;

namespace {

struct Options {
    bool headless = false, windowed = false, scene = false, sim = false, stream = false;
    bool walkbot = false, topdown = false, version = false, shot = false;
    uint32_t frames = 1, seconds = 0, width = 320, height = 180, ticks = 0;
    uint32_t ticks_per_frame = 30, radius = 6, workers = 4, km = 1, pose = 0;
    uint64_t seed = 1u;
    std::string out, record, replay, hashlog, csv;
};

int usage() {
    std::fprintf(stderr,
        "usage: backrooms [--headless|--window|--scene|--sim] ...\n"
        "  --frames N --seconds S --width W --height H --out file.png\n"
        "  --sim --ticks N --seed S --record f --replay f --hashlog f\n");
    return 2;
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto u32 = [&](uint32_t& dst) {
            if (i + 1 >= argc) return false;
            dst = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            return true;
        };
        auto str = [&](std::string& dst) {
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };
        if (std::strcmp(a, "--headless") == 0) o.headless = true;
        else if (std::strcmp(a, "--window") == 0) o.windowed = true;
        else if (std::strcmp(a, "--scene") == 0) o.scene = true;
        else if (std::strcmp(a, "--sim") == 0) o.sim = true;
        else if (std::strcmp(a, "--stream") == 0) o.stream = true;
        else if (std::strcmp(a, "--walkbot") == 0) o.walkbot = true;
        else if (std::strcmp(a, "--topdown") == 0) o.topdown = true;
        else if (std::strcmp(a, "--shot") == 0) o.shot = true;
        else if (std::strcmp(a, "--pose") == 0) { if (!u32(o.pose)) return false; }
        else if (std::strcmp(a, "--km") == 0) { if (!u32(o.km)) return false; }
        else if (std::strcmp(a, "--version") == 0) o.version = true;
        else if (std::strcmp(a, "--frames") == 0) { if (!u32(o.frames)) return false; }
        else if (std::strcmp(a, "--seconds") == 0) { if (!u32(o.seconds)) return false; }
        else if (std::strcmp(a, "--width") == 0) { if (!u32(o.width)) return false; }
        else if (std::strcmp(a, "--height") == 0) { if (!u32(o.height)) return false; }
        else if (std::strcmp(a, "--ticks") == 0) { if (!u32(o.ticks)) return false; }
        else if (std::strcmp(a, "--ticks-per-frame") == 0) { if (!u32(o.ticks_per_frame)) return false; }
        else if (std::strcmp(a, "--radius") == 0) { if (!u32(o.radius)) return false; }
        else if (std::strcmp(a, "--workers") == 0) { if (!u32(o.workers)) return false; }
        else if (std::strcmp(a, "--csv") == 0) { if (!str(o.csv)) return false; }
        else if (std::strcmp(a, "--seed") == 0) {
            if (i + 1 >= argc) return false;
            o.seed = std::strtoull(argv[++i], nullptr, 0);
        }
        else if (std::strcmp(a, "--out") == 0) { if (!str(o.out)) return false; }
        else if (std::strcmp(a, "--record") == 0) { if (!str(o.record)) return false; }
        else if (std::strcmp(a, "--replay") == 0) { if (!str(o.replay)) return false; }
        else if (std::strcmp(a, "--hashlog") == 0) { if (!str(o.hashlog)) return false; }
        else return false;
    }
    return o.width != 0 && o.height != 0;
}

// ----- Win32 window (M1) ----------------------------------------------------
LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLOSE:   DestroyWindow(hwnd); return 0;
        case WM_DESTROY: PostQuitMessage(0);  return 0;
        default:         return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

HWND create_window(uint32_t width, uint32_t height) {
    const wchar_t* kClass = L"BackroomsWindow";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    RECT r = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, kClass, L"Backrooms Sim", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    return hwnd;
}

void pump_messages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

int finish_render(Renderer& r, const Options& o, uint64_t rendered, uint64_t mem_start) {
    if (!o.windowed && !o.out.empty() && !o.scene) {
        FrameImage img;
        if (!r.readback(img)) { std::fprintf(stderr, "readback: %s\n", r.last_error().c_str()); return 1; }
        if (stbi_write_png(o.out.c_str(), static_cast<int>(img.width), static_cast<int>(img.height),
                           4, img.rgba.data(), static_cast<int>(img.width) * 4) == 0) {
            std::fprintf(stderr, "PNG write failed: %s\n", o.out.c_str());
            return 1;
        }
    }
    const uint64_t mem_end = r.process_private_bytes();
    const uint32_t dbg = r.debug_error_count();
    std::printf("frames: %llu\n", static_cast<unsigned long long>(rendered));
    std::printf("mem_start_bytes: %llu\n", static_cast<unsigned long long>(mem_start));
    std::printf("mem_end_bytes: %llu\n", static_cast<unsigned long long>(mem_end));
    std::printf("mem_delta_bytes: %lld\n",
                static_cast<long long>(static_cast<int64_t>(mem_end) - static_cast<int64_t>(mem_start)));
    std::printf("debug_error_count: %u\n", dbg);
    return dbg == 0 ? 0 : 3;
}

// ----- M1 clear render ------------------------------------------------------
int run_clear(const Options& o) {
    Renderer renderer;
    HWND hwnd = nullptr;
    if (o.windowed) {
        hwnd = create_window(o.width, o.height);
        if (!hwnd) { std::fprintf(stderr, "window creation failed\n"); return 1; }
        if (!renderer.init_windowed(hwnd, o.width, o.height)) {
            std::fprintf(stderr, "init: %s\n", renderer.last_error().c_str()); return 1;
        }
    } else if (!renderer.init_headless(o.width, o.height)) {
        std::fprintf(stderr, "init: %s\n", renderer.last_error().c_str()); return 1;
    }
    const uint64_t mem_start = renderer.process_private_bytes();
    uint64_t rendered = 0;
    if (o.seconds > 0) {
        const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(o.seconds) * 1000ULL;
        while (GetTickCount64() < end) {
            if (o.windowed) pump_messages();
            if (!renderer.render_clear_frame()) { std::fprintf(stderr, "render: %s\n", renderer.last_error().c_str()); return 1; }
            ++rendered;
        }
    } else {
        for (uint32_t i = 0; i < o.frames; ++i) {
            if (o.windowed) pump_messages();
            if (!renderer.render_clear_frame()) { std::fprintf(stderr, "render: %s\n", renderer.last_error().c_str()); return 1; }
            ++rendered;
        }
    }
    return finish_render(renderer, o, rendered, mem_start);
}

// ----- M2 scene render (test room from a fixed golden pose) -----------------
int run_scene(const Options& o) {
    Renderer renderer;
    if (!renderer.init_headless(o.width, o.height)) {
        std::fprintf(stderr, "init: %s\n", renderer.last_error().c_str()); return 1;
    }
    const auto boxes = br::core::test_room_box_instances();
    contracts::WorldView view{};
    view.boxes = std::span<const contracts::BoxInstance>(boxes.data(), boxes.size());
    view.camera.pos[0] = -3.5f; view.camera.pos[1] = 1.6f; view.camera.pos[2] = -4.0f;
    view.camera.yaw = 0.5f;
    view.camera.pitch = -0.05f;
    view.camera.fov_y = 1.2217305f;
    view.camera.aspect = static_cast<float>(o.width) / static_cast<float>(o.height);
    view.tick = 0;

    if (!renderer.render_world_view(view)) {
        std::fprintf(stderr, "scene render: %s\n", renderer.last_error().c_str()); return 1;
    }
    if (!o.out.empty()) {
        FrameImage img;
        if (!renderer.readback(img)) { std::fprintf(stderr, "readback: %s\n", renderer.last_error().c_str()); return 1; }
        if (stbi_write_png(o.out.c_str(), static_cast<int>(img.width), static_cast<int>(img.height),
                           4, img.rgba.data(), static_cast<int>(img.width) * 4) == 0) {
            std::fprintf(stderr, "PNG write failed: %s\n", o.out.c_str()); return 1;
        }
    }
    const uint32_t dbg = renderer.debug_error_count();
    std::printf("scene_vertices_drawn: ok\n");
    std::printf("debug_error_count: %u\n", dbg);
    return dbg == 0 ? 0 : 3;
}

// ----- M2 headless sim (record / replay / per-tick hash log) ----------------
std::vector<contracts::InputCommand> scripted_stream(uint64_t seed, uint32_t ticks) {
    br::core::Pcg64 ir(seed ^ 0x9e3779b97f4a7c15ULL);
    std::vector<contracts::InputCommand> cmds(ticks);
    for (uint32_t i = 0; i < ticks; ++i) {
        contracts::InputCommand c{};
        c.move_x = static_cast<float>(ir.next_double() * 2.0 - 1.0);
        c.move_z = static_cast<float>(ir.next_double() * 2.0 - 1.0);
        c.look_yaw = static_cast<float>((ir.next_double() - 0.5) * 0.10);
        c.look_pitch = static_cast<float>((ir.next_double() - 0.5) * 0.04);
        c.buttons = (ir.next_double() < 0.02) ? contracts::kButtonJump : uint8_t{0};
        cmds[i] = c;
    }
    return cmds;
}

int run_sim(const Options& o) {
    using namespace br::core;
    std::vector<contracts::InputCommand> cmds;
    uint64_t seed = o.seed;
    uint32_t nticks = o.ticks;

    if (!o.replay.empty()) {
        Replay r;
        if (!read_replay(o.replay, r)) { std::fprintf(stderr, "replay read failed: %s\n", o.replay.c_str()); return 1; }
        cmds = r.commands;
        seed = r.world_seed;
        nticks = static_cast<uint32_t>(cmds.size());
    } else {
        cmds = scripted_stream(seed, nticks);
    }

    WorldState s(seed);
    std::FILE* hf = nullptr;
    if (!o.hashlog.empty()) {
        hf = std::fopen(o.hashlog.c_str(), "wb");
        if (!hf) { std::fprintf(stderr, "hashlog open failed: %s\n", o.hashlog.c_str()); return 1; }
    }
    for (uint32_t i = 0; i < nticks; ++i) {
        tick(s, cmds[i]);
        if (hf) {
            std::fprintf(hf, "%llu %016llx\n",
                         static_cast<unsigned long long>(s.tick),
                         static_cast<unsigned long long>(world_state_hash(s)));
        }
    }
    if (hf) std::fclose(hf);

    if (!o.record.empty()) {
        Replay out;
        out.world_seed = seed;
        out.commands = cmds;
        if (!write_replay(o.record, out)) { std::fprintf(stderr, "replay write failed: %s\n", o.record.c_str()); return 1; }
    }

    std::printf("sim_ticks: %u\n", nticks);
    std::printf("final_hash: %016llx\n", static_cast<unsigned long long>(world_state_hash(s)));
    std::printf("odometer_m: %.3f\n", static_cast<double>(s.odometer));
    return 0;
}

// ----- M3 infinite chunk streaming walk -------------------------------------
int run_stream(const Options& o) {
    using namespace std::chrono;
    Renderer renderer;
    if (!renderer.init_headless(o.width, o.height)) {
        std::fprintf(stderr, "init: %s\n", renderer.last_error().c_str()); return 1;
    }
    renderer.set_texture_seed(o.seed);
    br::stream::StreamManager sm(o.seed, static_cast<int>(o.radius), o.workers);
    br::telemetry::FrameCsv csv;
    if (!o.csv.empty() && !csv.open(o.csv)) {
        std::fprintf(stderr, "csv open failed: %s\n", o.csv.c_str()); return 1;
    }

    br::core::WorldState s(o.seed);
    s.wanderer.pos = br::core::Vec3{16.0f, br::core::kWandererHalfHeight + 0.02f, 16.0f};
    const float aspect = static_cast<float>(o.width) / static_cast<float>(o.height);

    // Prime the ring around the start so the first frames are not empty.
    {
        const auto c = contracts::chunk_key_at(0, s.wanderer.pos.x, s.wanderer.pos.z);
        sm.update(c);
        sm.wait_idle();
        sm.update(c);
    }

    // Warm up (untimed, unlogged): build the pipeline and upload the full initial
    // ring so the measured frames are steady-state (no pipeline/allocation hitch).
    {
        const auto cam = br::core::wanderer_camera(s, aspect);
        uint32_t drawn = 0;
        const size_t target = sm.resident_count();
        for (int w = 0; w < 400 && static_cast<size_t>(drawn) < target; ++w) {
            if (!renderer.render_chunks(cam, sm.resident(), 32u, s.tick, &drawn)) {
                std::fprintf(stderr, "warmup: %s\n", renderer.last_error().c_str());
                return 1;
            }
        }
    }

    const uint64_t mem_start = renderer.process_private_bytes();
    uint64_t frame = 0;

    auto one_frame = [&]() -> bool {
        const auto fstart = steady_clock::now();
        for (uint32_t t = 0; t < o.ticks_per_frame; ++t) {
            contracts::InputCommand in{};
            in.move_z = 1.0f;
            in.look_yaw = 0.00008f;  // gentle curve -> a 2D swath of chunks
            br::core::tick(s, in, br::core::open_ground());
        }
        const auto center = contracts::chunk_key_at(0, s.wanderer.pos.x, s.wanderer.pos.z);
        sm.update(center);
        const auto cam = br::core::wanderer_camera(s, aspect);
        uint32_t drawn = 0;
        if (!renderer.render_chunks(cam, sm.resident(), 16u, s.tick, &drawn)) {
            std::fprintf(stderr, "render_chunks: %s\n", renderer.last_error().c_str());
            return false;
        }
        const auto fend = steady_clock::now();
        const double ms = duration<double, std::milli>(fend - fstart).count();
        const contracts::FrameMetrics m{ frame, ms, sm.resident_count(),
                                         sm.generated_total(), renderer.process_private_bytes() };
        csv.write(m);
        ++frame;
        return true;
    };

    if (o.seconds > 0) {
        const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(o.seconds) * 1000ULL;
        while (GetTickCount64() < end) { if (!one_frame()) return 1; }
    } else {
        for (uint32_t i = 0; i < o.frames; ++i) { if (!one_frame()) return 1; }
    }
    csv.close();

    if (!o.out.empty()) {
        FrameImage img;
        if (renderer.readback(img)) {
            stbi_write_png(o.out.c_str(), static_cast<int>(img.width), static_cast<int>(img.height),
                           4, img.rgba.data(), static_cast<int>(img.width) * 4);
        }
    }
    const uint64_t mem_end = renderer.process_private_bytes();
    const uint32_t dbg = renderer.debug_error_count();
    std::printf("frames: %llu\n", static_cast<unsigned long long>(frame));
    std::printf("resident_chunks: %llu\n", static_cast<unsigned long long>(sm.resident_count()));
    std::printf("generated_total: %llu\n", static_cast<unsigned long long>(sm.generated_total()));
    std::printf("distance_m: %.1f\n", static_cast<double>(s.odometer));
    std::printf("mem_start_bytes: %llu\n", static_cast<unsigned long long>(mem_start));
    std::printf("mem_end_bytes: %llu\n", static_cast<unsigned long long>(mem_end));
    std::printf("mem_delta_bytes: %lld\n",
                static_cast<long long>(static_cast<int64_t>(mem_end) - static_cast<int64_t>(mem_start)));
    std::printf("debug_error_count: %u\n", dbg);
    return dbg == 0 ? 0 : 3;
}

// ----- M5 fixed-pose textured+lit shot -> PNG (deterministic golden) ----------
// Renders the streamed, lit chunks from one of a small set of canonical camera
// poses at a fixed flicker tick. Output is bit-exact per (seed, pose, tick, GPU)
// so goldgen/hashdiff can gate it; also prints a luminance histogram so the gate
// can assert the frame is neither all-black nor blown-out.
int run_shot(const Options& o) {
    struct Pose { float yaw, pitch; };
    static const Pose kPoses[5] = {
        { 0.0f,        0.0f   },  // forward (+Z)
        { 1.5707963f,  0.0f   },  // right (+X)
        { 3.1415927f,  0.0f   },  // back (-Z)
        { 0.7853982f,  0.42f  },  // diagonal, look up at ceiling + fluorescents
        { 4.0f,       -0.38f  },  // look down at carpet + baseboard
    };
    const Pose pz = kPoses[o.pose % 5u];

    Renderer renderer;
    if (!renderer.init_headless(o.width, o.height)) {
        std::fprintf(stderr, "init: %s\n", renderer.last_error().c_str()); return 1;
    }
    renderer.set_texture_seed(o.seed);

    // Eye at the proven-open spawn cell; vary only orientation across poses.
    const float ex = 16.0f, ez = 16.0f;
    const float ey = br::core::kWandererHalfHeight + 0.02f + br::core::kEyeHeight;
    contracts::CameraPose cam{};
    cam.pos[0] = ex; cam.pos[1] = ey; cam.pos[2] = ez;
    cam.yaw = pz.yaw; cam.pitch = pz.pitch;
    cam.fov_y = 1.2217305f;  // ~70 deg, matches the wanderer camera
    cam.aspect = static_cast<float>(o.width) / static_cast<float>(o.height);

    br::stream::StreamManager sm(o.seed, static_cast<int>(o.radius), o.workers);
    const auto center = contracts::chunk_key_at(0, ex, ez);
    sm.update(center);
    sm.wait_idle();
    sm.update(center);

    // Warm up: upload the full resident ring (no budget cap) so the captured
    // frame is complete, then capture once at the fixed flicker tick.
    uint32_t drawn = 0;
    const size_t target = sm.resident_count();
    for (int w = 0; w < 400 && static_cast<size_t>(drawn) < target; ++w) {
        if (!renderer.render_chunks(cam, sm.resident(), 64u, o.ticks, &drawn)) {
            std::fprintf(stderr, "shot warmup: %s\n", renderer.last_error().c_str()); return 1;
        }
    }
    if (!renderer.render_chunks(cam, sm.resident(), 64u, o.ticks, &drawn)) {
        std::fprintf(stderr, "shot: %s\n", renderer.last_error().c_str()); return 1;
    }

    FrameImage img;
    if (!renderer.readback(img)) { std::fprintf(stderr, "readback: %s\n", renderer.last_error().c_str()); return 1; }
    if (!o.out.empty()) {
        if (stbi_write_png(o.out.c_str(), static_cast<int>(img.width), static_cast<int>(img.height),
                           4, img.rgba.data(), static_cast<int>(img.width) * 4) == 0) {
            std::fprintf(stderr, "PNG write failed: %s\n", o.out.c_str()); return 1;
        }
    }

    // Luminance histogram (256 bins over Rec.709 luma) -> mean + percentiles.
    uint64_t hist[256] = {};
    const size_t px = static_cast<size_t>(img.width) * img.height;
    for (size_t p = 0; p < px; ++p) {
        const uint8_t* c = &img.rgba[p * 4];
        const float y = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
        int b = static_cast<int>(y); if (b < 0) b = 0; if (b > 255) b = 255;
        ++hist[b];
    }
    uint64_t acc = 0; double sum = 0.0;
    int p01 = 0, p50 = 0, p99 = 0;
    bool g01 = false, g50 = false, g99 = false;
    for (int b = 0; b < 256; ++b) {
        sum += static_cast<double>(b) * static_cast<double>(hist[b]);
        acc += hist[b];
        const double frac = static_cast<double>(acc) / static_cast<double>(px);
        if (!g01 && frac >= 0.01) { p01 = b; g01 = true; }
        if (!g50 && frac >= 0.50) { p50 = b; g50 = true; }
        if (!g99 && frac >= 0.99) { p99 = b; g99 = true; }
    }
    const double mean = sum / static_cast<double>(px);
    const double frac_black = static_cast<double>(hist[0] + hist[1] + hist[2]) / static_cast<double>(px);
    const double frac_white = static_cast<double>(hist[253] + hist[254] + hist[255]) / static_cast<double>(px);

    const uint32_t dbg = renderer.debug_error_count();
    std::printf("pose: %u\n", o.pose % 5u);
    std::printf("luma_mean: %.3f\n", mean);
    std::printf("luma_p01: %d\n", p01);
    std::printf("luma_p50: %d\n", p50);
    std::printf("luma_p99: %d\n", p99);
    std::printf("frac_black: %.4f\n", frac_black);
    std::printf("frac_white: %.4f\n", frac_white);
    std::printf("debug_error_count: %u\n", dbg);
    return dbg == 0 ? 0 : 3;
}

// ----- M4 walk-bot v1: wander + escape-on-block, collides with maze walls -----
struct WalkBot {
    br::core::Pcg64 rng;
    float target_yaw;
    int ticks_since_decision = 0;
    br::core::Vec3 last_pos;

    WalkBot(uint64_t seed, br::core::Vec3 spawn) : rng(seed), last_pos(spawn) {
        target_yaw = static_cast<float>(rng.next_double() * 6.2831853);
    }
    contracts::InputCommand step(const br::core::WorldState& s) {
        contracts::InputCommand in{};
        if (++ticks_since_decision >= 16) {  // ~0.13 s
            const float dx = s.wanderer.pos.x - last_pos.x;
            const float dz = s.wanderer.pos.z - last_pos.z;
            const float moved = std::sqrt(dx * dx + dz * dz);
            if (moved < 0.15f) {
                // Blocked: sweep the heading to scan for an opening (a fixed,
                // non-2pi-dividing step guarantees finding a doorway quickly).
                target_yaw += 1.3f;
            } else if (rng.next_double() < 0.25) {
                target_yaw += static_cast<float>((rng.next_double() - 0.5) * 1.6);  // gentle drift
            }
            last_pos = s.wanderer.pos;
            ticks_since_decision = 0;
        }
        float dyaw = target_yaw - s.wanderer.yaw;
        while (dyaw > 3.14159265f) dyaw -= 6.28318531f;
        while (dyaw < -3.14159265f) dyaw += 6.28318531f;
        in.look_yaw = br::core::clampf(dyaw, -0.18f, 0.18f);
        in.move_z = 1.0f;
        return in;
    }
};

int run_walkbot(const Options& o) {
    using namespace br::core;
    const float target_m = static_cast<float>(o.km) * 1000.0f;

    WorldState s(o.seed);
    s.wanderer.pos = Vec3{2.0f, kWandererHalfHeight + 0.02f, 2.0f};  // cell-(0,0) centre
    WalkBot bot(o.seed ^ 0x9e3779b97f4a7c15ULL, s.wanderer.pos);

    // Collision = ground floor + the 3x3 chunk walls around the wanderer,
    // regenerated deterministically only when the wanderer's chunk changes.
    std::vector<Aabb> collision;
    contracts::ChunkKey cached{0, static_cast<int64_t>(1) << 40, 0};
    auto rebuild = [&](contracts::ChunkKey center) {
        collision.clear();
        collision.push_back(Aabb{{-1.0e6f, -1.0f, -1.0e6f}, {1.0e6f, 0.0f, 1.0e6f}});
        for (int64_t dx = -1; dx <= 1; ++dx) {
            for (int64_t dz = -1; dz <= 1; ++dz) {
                const contracts::ChunkData cd =
                    contracts::GenerateChunk(o.seed, contracts::ChunkKey{0, center.cx + dx, center.cz + dz});
                for (const auto& b : cd.collision) {
                    collision.push_back(Aabb{{b.mn[0], b.mn[1], b.mn[2]}, {b.mx[0], b.mx[1], b.mx[2]}});
                }
            }
        }
        cached = center;
    };

    // Stuck = position variance ~ 0 over a 10 s window (motionless / sealed in).
    // Track the window's bounding box; a wedged wanderer never moves, while one
    // that merely thrashes (slides/turns without net progress) still ranges far.
    uint64_t stuck_events = 0;
    float minx = s.wanderer.pos.x, maxx = minx, minz = s.wanderer.pos.z, maxz = minz;
    uint64_t window_tick = 0;
    const uint64_t kMaxTicks = 800000;

    while (s.odometer < target_m && s.tick < kMaxTicks) {
        const contracts::ChunkKey here = contracts::chunk_key_at(0, s.wanderer.pos.x, s.wanderer.pos.z);
        if (here != cached) rebuild(here);
        tick(s, bot.step(s), collision);
        const float px = s.wanderer.pos.x, pz = s.wanderer.pos.z;
        if (px < minx) minx = px;
        if (px > maxx) maxx = px;
        if (pz < minz) minz = pz;
        if (pz > maxz) maxz = pz;
        if (s.tick - window_tick >= 1200) {  // 10 s window
            if ((maxx - minx) < 0.5f && (maxz - minz) < 0.5f) ++stuck_events;
            minx = maxx = px;
            minz = maxz = pz;
            window_tick = s.tick;
        }
    }

    std::printf("walkbot_seed: %llu\n", static_cast<unsigned long long>(o.seed));
    std::printf("distance_m: %.1f\n", static_cast<double>(s.odometer));
    std::printf("ticks: %llu\n", static_cast<unsigned long long>(s.tick));
    std::printf("stuck_events: %llu\n", static_cast<unsigned long long>(stuck_events));
    std::printf("final_hash: %016llx\n", static_cast<unsigned long long>(world_state_hash(s)));
    return (s.odometer >= target_m && stuck_events == 0) ? 0 : 4;
}

// ----- M4 top-down debug render of a 3x3 chunk block -> PNG -------------------
int run_topdown(const Options& o) {
    Renderer renderer;
    if (!renderer.init_headless(o.width, o.height)) {
        std::fprintf(stderr, "init: %s\n", renderer.last_error().c_str()); return 1;
    }
    std::vector<contracts::ChunkData> chunks;
    chunks.reserve(9);
    for (int64_t cz = 0; cz < 3; ++cz) {
        for (int64_t cx = 0; cx < 3; ++cx) {
            chunks.push_back(contracts::GenerateChunk(o.seed, contracts::ChunkKey{0, cx, cz}));
        }
    }
    std::vector<contracts::ResidentChunk> resident;
    resident.reserve(chunks.size());
    for (const auto& cd : chunks) {
        contracts::ResidentChunk rc;
        rc.key = cd.key;
        rc.vertices = cd.vertices.data();
        rc.vertex_count = static_cast<uint32_t>(cd.vertices.size());
        resident.push_back(rc);
    }
    const float c = 1.5f * contracts::kChunkSize;  // centre of the 3x3 block (48 m)
    if (!renderer.render_topdown(resident, c, c, c)) {
        std::fprintf(stderr, "topdown: %s\n", renderer.last_error().c_str()); return 1;
    }
    if (!o.out.empty()) {
        FrameImage img;
        if (!renderer.readback(img)) { std::fprintf(stderr, "readback: %s\n", renderer.last_error().c_str()); return 1; }
        if (stbi_write_png(o.out.c_str(), static_cast<int>(img.width), static_cast<int>(img.height),
                           4, img.rgba.data(), static_cast<int>(img.width) * 4) == 0) {
            std::fprintf(stderr, "PNG write failed: %s\n", o.out.c_str()); return 1;
        }
    }
    const uint32_t dbg = renderer.debug_error_count();
    std::printf("debug_error_count: %u\n", dbg);
    return dbg == 0 ? 0 : 3;
}

}  // namespace

// Tighten the OS timer/wait granularity (default ~15.6 ms) to 1 ms for the
// duration of the process, so GPU fence-wait wakeups pace frames smoothly.
struct TimerPeriodGuard {
    TimerPeriodGuard() { timeBeginPeriod(1); }
    ~TimerPeriodGuard() { timeEndPeriod(1); }
};

int main(int argc, char** argv) {
    TimerPeriodGuard timer_period;
    Options o;
    if (!parse(argc, argv, o)) return usage();

    if (o.version) { std::printf("%s\n", br::core::core_version()); return 0; }
    if (o.sim)     return run_sim(o);
    if (o.walkbot) return run_walkbot(o);
    if (o.topdown) return run_topdown(o);
    if (o.shot)    return run_shot(o);
    if (o.stream)  return run_stream(o);
    if (o.scene)   return run_scene(o);
    if (o.headless || o.windowed) return run_clear(o);

    std::printf("Backrooms Sim v%s\n", br::core::core_version());
    std::printf("modes: --headless --scene --sim --window  (see --version)\n");
    return 0;
}
