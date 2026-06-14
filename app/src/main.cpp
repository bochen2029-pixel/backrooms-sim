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

#include <algorithm>
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
#include "contracts/audio_events_v1.h"
#include "gen/biome.h"
#include "gen/layout.h"
#include "stream/stream_manager.h"
#include "telemetry/csv.h"
#include "telemetry/crash.h"
#include "render_d3d12/renderer.h"
#include "render_dxr/dxr.h"
#include "director/director.h"
#include "director/keel_client.h"
#include "audio/synth.h"
#include "audio/room_probe.h"
#include "audio/wav.h"
#include "audio/engine.h"
#include "hud.h"

namespace contracts = br::contracts;
namespace audio = br::audio;
using br::render_d3d12::Renderer;
using br::render_d3d12::FrameImage;

namespace {

struct Options {
    bool headless = false, windowed = false, scene = false, sim = false, stream = false;
    bool walkbot = false, topdown = false, version = false, shot = false;
    bool render_wav = false, footsteps = false, audiosoak = false, audio = false;
    bool biomeat = false, descend = false, post = false, dxr_probe = false, dxr_test = false, dxr = false;
    bool dxr_depth = false, dxr_pt = false, dxr_fps = false, dxr_ghost = false, dxr_walk = false;
    bool soak = false, crash_test = false, director_probe = false;
    uint32_t frames = 1, seconds = 0, width = 320, height = 180, ticks = 0;
    uint32_t ticks_per_frame = 30, radius = 6, workers = 4, km = 1, pose = 0, spp = 256;
    uint32_t shot_every = 600;   // soak: write a screenshot every N rendered frames
    uint64_t seed = 1u;
    std::string out, record, replay, hashlog, csv, audiolog, crash_dir, director_url;
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
        else if (std::strcmp(a, "--render-wav") == 0) o.render_wav = true;
        else if (std::strcmp(a, "--footsteps") == 0) o.footsteps = true;
        else if (std::strcmp(a, "--audiolog") == 0) { if (!str(o.audiolog)) return false; }
        else if (std::strcmp(a, "--audiosoak") == 0) o.audiosoak = true;
        else if (std::strcmp(a, "--audio") == 0) o.audio = true;
        else if (std::strcmp(a, "--biomeat") == 0) o.biomeat = true;
        else if (std::strcmp(a, "--descend") == 0) o.descend = true;
        else if (std::strcmp(a, "--post") == 0) o.post = true;
        else if (std::strcmp(a, "--dxr-probe") == 0) o.dxr_probe = true;
        else if (std::strcmp(a, "--dxr-test") == 0) o.dxr_test = true;
        else if (std::strcmp(a, "--dxr") == 0) o.dxr = true;
        else if (std::strcmp(a, "--dxr-depth") == 0) o.dxr_depth = true;
        else if (std::strcmp(a, "--dxr-pt") == 0) o.dxr_pt = true;
        else if (std::strcmp(a, "--dxr-fps") == 0) o.dxr_fps = true;
        else if (std::strcmp(a, "--dxr-ghost") == 0) o.dxr_ghost = true;
        else if (std::strcmp(a, "--dxr-walk") == 0) o.dxr_walk = true;
        else if (std::strcmp(a, "--soak") == 0) o.soak = true;
        else if (std::strcmp(a, "--crash-test") == 0) o.crash_test = true;
        else if (std::strcmp(a, "--crash-dir") == 0) { if (!str(o.crash_dir)) return false; }
        else if (std::strcmp(a, "--director-probe") == 0) o.director_probe = true;
        else if (std::strcmp(a, "--director-url") == 0) { if (!str(o.director_url)) return false; }
        else if (std::strcmp(a, "--shot-every") == 0) { if (!u32(o.shot_every)) return false; }
        else if (std::strcmp(a, "--spp") == 0) { if (!u32(o.spp)) return false; }
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
    renderer.set_post(o.post, static_cast<uint32_t>(o.seed), 0.0f, false);  // VHS post (M8), HUD off
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
    // VHS post (M8): seeded by the world seed, time from the (fixed) tick so the
    // golden is deterministic. HUD composited when post is on.
    renderer.set_post(o.post, static_cast<uint32_t>(o.seed),
                      static_cast<float>(o.ticks) / 120.0f, o.post);
    if (o.post) {
        br::app::HudValues hv;
        hv.sim_ticks = o.ticks;
        hv.seed = o.seed;
        hv.odometer_m = 0.0f;
        const contracts::ChunkKey sc = contracts::chunk_key_at(0, 16.0f, 16.0f);
        hv.chunk_x = sc.cx; hv.chunk_z = sc.cz;
        hv.level = 0;
        hv.fps = 60;
        std::vector<uint8_t> hud;
        br::app::build_hud_overlay(hud, o.width, o.height, hv);
        if (!renderer.upload_hud_overlay(hud.data(), o.width, o.height)) {
            std::fprintf(stderr, "hud: %s\n", renderer.last_error().c_str()); return 1;
        }
        std::printf("timestamp: %s\n", br::app::hud_timestamp(o.ticks).c_str());
    }

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

// ----- M11 Director probe: WandererSummary -> KEEL sidecar -> validated Directive -
int run_director_probe(const Options& o) {
    std::string host = "127.0.0.1";
    int port = 7071;
    if (!o.director_url.empty()) {
        const std::string& u = o.director_url;
        const size_t colon = u.rfind(':');
        if (colon != std::string::npos) { host = u.substr(0, colon); port = std::atoi(u.c_str() + colon + 1); }
        else { host = u; }
    }

    // A representative wanderer summary (the real sim-derived summary lands in 11c).
    contracts::WandererSummary sum{};
    sum.tick = (o.ticks > 0) ? o.ticks : 130000u;   // ~18 min @ 120 Hz
    sum.world_seed = o.seed;
    sum.level = 0;
    sum.chunk_cx = 7; sum.chunk_cz = -2;
    sum.biome = static_cast<int32_t>(o.seed % static_cast<uint64_t>(contracts::kDirectorBiomeCount));
    sum.distance_m = 1240.0f;
    sum.dwell_seconds = 95.0f;
    sum.route_loops = 3;
    sum.location_hash = o.seed * 0x9E3779B97F4A7C15ull + 7u;

    const std::string prompt = br::director::render_prompt(sum);
    std::printf("director_url: %s:%d\n", host.c_str(), port);
    const br::director::KeelResponse resp = br::director::keel_complete(host, port, prompt, 15000);
    if (!resp.ok) {
        std::fprintf(stderr, "keel: %s\n", resp.error.c_str());
        std::printf("keel_ok: 0\n");
        return 1;
    }
    std::printf("keel_ok: 1\n");
    std::printf("keel_http: %d\n", resp.http_status);
    std::printf("keel_tier: %s\n", resp.tier.c_str());
    std::printf("keel_cost: %.6f\n", resp.cost);
    std::printf("keel_route: %s\n", resp.route.c_str());
    std::printf("content: %s\n", resp.content.c_str());

    const br::director::DirectiveResult vr = br::director::validate_directive(resp.content);
    std::printf("directive_valid: %d\n", vr.ok ? 1 : 0);
    if (vr.ok) {
        std::printf("directive_kind: %d\n", static_cast<int>(vr.directive.kind));
        if (vr.directive.caption[0] != '\0') std::printf("directive_caption: %s\n", vr.directive.caption);
    } else {
        std::printf("reject_reason: %s\n", vr.reject_reason.c_str());
    }
    return resp.ok ? 0 : 1;
}

// ----- M10 walk-bot soak: long-haul walk + render + telemetry + audits --------
// Deterministic maze walk with the streaming raster renderer (the shipping path).
// Writes a frame-telemetry CSV (frame_ms -> FPS percentiles, mem_bytes -> slope),
// runs periodic connectivity audits, and dumps periodic screenshots for the
// contactsheet tool. Runs for --seconds S (wall clock) or --ticks N.
int run_soak(const Options& o) {
    using namespace std::chrono;
    using namespace br::core;
    Renderer renderer;
    if (!renderer.init_headless(o.width, o.height)) { std::fprintf(stderr, "init: %s\n", renderer.last_error().c_str()); return 1; }
    renderer.set_texture_seed(o.seed);
    renderer.set_post(o.post, static_cast<uint32_t>(o.seed), 0.0f, false);

    br::stream::StreamManager sm(o.seed, static_cast<int>(o.radius), o.workers);
    br::telemetry::FrameCsv csv;
    bool csvOpen = false;
    if (!o.csv.empty()) {
        if (!csv.open(o.csv)) { std::fprintf(stderr, "csv open failed: %s\n", o.csv.c_str()); return 1; }
        csvOpen = true;
    }

    WorldState s(o.seed);
    s.wanderer.pos = Vec3{ 16.0f, kWandererHalfHeight + 0.02f, 16.0f };
    WalkBot bot(o.seed ^ 0x9e3779b97f4a7c15ULL, s.wanderer.pos);
    const float aspect = static_cast<float>(o.width) / static_cast<float>(o.height);

    std::vector<Aabb> collision;
    contracts::ChunkKey cached{ 0, static_cast<int64_t>(1) << 40, 0 };
    auto rebuild_collision = [&](contracts::ChunkKey c) {
        collision.clear();
        collision.push_back(Aabb{ {-1.0e6f, -1.0f, -1.0e6f}, {1.0e6f, 0.0f, 1.0e6f} });
        for (int64_t dx = -1; dx <= 1; ++dx)
            for (int64_t dz = -1; dz <= 1; ++dz) {
                const contracts::ChunkData cd = contracts::GenerateChunk(o.seed, contracts::ChunkKey{ 0, c.cx + dx, c.cz + dz });
                for (const auto& b : cd.collision) collision.push_back(Aabb{ {b.mn[0], b.mn[1], b.mn[2]}, {b.mx[0], b.mx[1], b.mx[2]} });
            }
        cached = c;
    };

    contracts::ChunkKey c0 = contracts::chunk_key_at(0, s.wanderer.pos.x, s.wanderer.pos.z);
    rebuild_collision(c0);
    sm.update(c0); sm.wait_idle(); sm.update(c0);

    const bool wall = (o.seconds > 0);
    const uint64_t tick_target = (o.ticks > 0) ? o.ticks : 600000u;
    const auto start = steady_clock::now();
    const auto wall_end = start + seconds(static_cast<long long>(o.seconds));
    const uint32_t tpf = (o.ticks_per_frame > 0) ? o.ticks_per_frame : 4u;
    const uint64_t kAuditEvery = 200u;
    const uint64_t shotEvery = (o.shot_every > 0) ? o.shot_every : 600u;

    uint64_t frame = 0, audits = 0, audit_fail = 0, shots = 0, stuck = 0, mem_first = 0, mem_last = 0;
    float minx = s.wanderer.pos.x, maxx = minx, minz = s.wanderer.pos.z, maxz = minz;
    uint64_t window_tick = 0;

    for (;;) {
        if (wall) { if ((frame & 63u) == 0 && steady_clock::now() >= wall_end) break; }
        else if (s.tick >= tick_target) break;

        for (uint32_t k = 0; k < tpf; ++k) {
            const contracts::ChunkKey here = contracts::chunk_key_at(0, s.wanderer.pos.x, s.wanderer.pos.z);
            if (here != cached) rebuild_collision(here);
            tick(s, bot.step(s), collision);
        }
        const float px = s.wanderer.pos.x, pz = s.wanderer.pos.z;
        if (px < minx) minx = px; if (px > maxx) maxx = px;
        if (pz < minz) minz = pz; if (pz > maxz) maxz = pz;
        if (s.tick - window_tick >= 1200) {
            if ((maxx - minx) < 0.5f && (maxz - minz) < 0.5f) ++stuck;
            minx = maxx = px; minz = maxz = pz; window_tick = s.tick;
        }

        const contracts::ChunkKey center = contracts::chunk_key_at(0, px, pz);
        sm.update(center);
        const contracts::CameraPose cam = wanderer_camera(s, aspect);
        uint32_t drawn = 0;
        const auto t0 = steady_clock::now();
        if (!renderer.render_chunks(cam, sm.resident(), 16u, s.tick, &drawn)) { std::fprintf(stderr, "render: %s\n", renderer.last_error().c_str()); return 1; }
        const auto t1 = steady_clock::now();
        const double frame_ms = duration<double, std::milli>(t1 - t0).count();
        const uint64_t mem = renderer.process_private_bytes();
        if (frame == 0) mem_first = mem;
        mem_last = mem;
        if (csvOpen) csv.write(contracts::FrameMetrics{ frame, frame_ms, sm.resident_count(), sm.generated_total(), mem });

        if (frame % kAuditEvery == 0) {
            ++audits;
            for (int64_t dx = -1; dx <= 1; ++dx)
                for (int64_t dz = -1; dz <= 1; ++dz) {
                    const contracts::ChunkKey k{ 0, center.cx + dx, center.cz + dz };
                    if (!br::gen::validate_connectivity(br::gen::generate_layout(o.seed, k))) ++audit_fail;
                }
        }
        if (!o.out.empty() && (frame % shotEvery == 0)) {
            FrameImage img;
            if (renderer.readback(img)) {
                char path[600];
                std::snprintf(path, sizeof(path), "%s/shot_%05llu.png", o.out.c_str(), static_cast<unsigned long long>(shots));
                if (stbi_write_png(path, static_cast<int>(img.width), static_cast<int>(img.height), 4, img.rgba.data(), static_cast<int>(img.width) * 4)) ++shots;
            }
        }
        ++frame;
    }
    if (csvOpen) csv.close();

    const uint32_t dbg = renderer.debug_error_count();
    std::printf("soak_seed: %llu\n", static_cast<unsigned long long>(o.seed));
    std::printf("frames: %llu\n", static_cast<unsigned long long>(frame));
    std::printf("ticks: %llu\n", static_cast<unsigned long long>(s.tick));
    std::printf("distance_m: %.1f\n", static_cast<double>(s.odometer));
    std::printf("audits: %llu\n", static_cast<unsigned long long>(audits));
    std::printf("audit_failures: %llu\n", static_cast<unsigned long long>(audit_fail));
    std::printf("stuck_events: %llu\n", static_cast<unsigned long long>(stuck));
    std::printf("screenshots: %llu\n", static_cast<unsigned long long>(shots));
    std::printf("mem_first_bytes: %llu\n", static_cast<unsigned long long>(mem_first));
    std::printf("mem_last_bytes: %llu\n", static_cast<unsigned long long>(mem_last));
    std::printf("debug_error_count: %u\n", dbg);
    const bool ok = (dbg == 0) && (audit_fail == 0) && (stuck == 0);
    return ok ? 0 : 4;
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

// ----- M6 deterministic maze walk shared by --render-wav and --footsteps ------
// The walk-bot navigates the generated maze (same drive as --walkbot), gathering
// the 3x3 chunk walls around the wanderer for both collision and the room probe.
struct MazeWalker {
    br::core::WorldState s;
    WalkBot bot;
    uint64_t seed;
    std::vector<br::core::Aabb> collision;            // ground + walls (for tick)
    std::vector<contracts::BoxInstance> walls;        // chunk walls only (probe)
    contracts::ChunkKey cached{0, static_cast<int64_t>(1) << 40, 0};

    explicit MazeWalker(uint64_t seed_)
        : s(seed_),
          bot(seed_ ^ 0x9e3779b97f4a7c15ULL,
              br::core::Vec3{2.0f, br::core::kWandererHalfHeight + 0.02f, 2.0f}),
          seed(seed_) {
        s.wanderer.pos = br::core::Vec3{2.0f, br::core::kWandererHalfHeight + 0.02f, 2.0f};
    }

    void rebuild(contracts::ChunkKey center) {
        collision.clear();
        walls.clear();
        collision.push_back(br::core::Aabb{{-1.0e6f, -1.0f, -1.0e6f}, {1.0e6f, 0.0f, 1.0e6f}});
        for (int64_t dx = -1; dx <= 1; ++dx) {
            for (int64_t dz = -1; dz <= 1; ++dz) {
                const contracts::ChunkData cd =
                    contracts::GenerateChunk(seed, contracts::ChunkKey{0, center.cx + dx, center.cz + dz});
                for (const auto& b : cd.collision) {
                    collision.push_back(br::core::Aabb{{b.mn[0], b.mn[1], b.mn[2]},
                                                       {b.mx[0], b.mx[1], b.mx[2]}});
                    walls.push_back(b);
                }
            }
        }
        cached = center;
    }

    void step() {
        const contracts::ChunkKey here =
            contracts::chunk_key_at(0, s.wanderer.pos.x, s.wanderer.pos.z);
        if (here != cached) rebuild(here);
        br::core::tick(s, bot.step(s), collision);
    }
};

// ----- M6 offline audio: replay the maze walk, synth the mix, write WAV -------
int run_render_wav(const Options& o) {
    const uint32_t sr = contracts::kAudioSampleRate;
    const uint32_t ticks = (o.ticks > 0) ? o.ticks : 2400u;          // default 20 s
    const uint32_t fpt = sr / 120u;                                  // 400 frames/tick (exact)
    MazeWalker w(o.seed);
    audio::Synth synth(o.seed, sr);

    std::vector<int16_t> pcm;
    pcm.reserve(static_cast<size_t>(ticks) * fpt * 2u);
    std::vector<float> block(static_cast<size_t>(fpt) * 2u);
    std::vector<uint64_t> footstep_ticks;

    uint64_t prev_steps = br::core::footstep_count(w.s);
    int reverb_throttle = 0;
    for (uint32_t t = 0; t < ticks; ++t) {
        w.step();
        const contracts::AudioListener lis = br::core::audio_listener(w.s);
        const uint64_t steps = br::core::footstep_count(w.s);
        for (uint64_t k = prev_steps; k < steps; ++k) {
            float inten = 0.0f;
            if (lis.speed > 0.1f) {
                inten = 0.3f + (lis.speed / br::core::kWalkSpeed) * 0.7f;
                if (inten > 1.0f) inten = 1.0f;
            }
            synth.trigger_footstep(inten);
            footstep_ticks.push_back(w.s.tick);
        }
        prev_steps = steps;

        if (reverb_throttle == 0) {
            synth.set_reverb_seconds(audio::probe_reverb_seconds(lis, w.walls));
        }
        if (++reverb_throttle >= 12) reverb_throttle = 0;  // re-probe ~10x/sec

        synth.render(lis, block.data(), fpt);
        for (uint32_t i = 0; i < fpt * 2u; ++i) pcm.push_back(audio::to_pcm16(block[i] * 0.85f));
    }

    if (!o.out.empty()) {
        std::string err;
        if (!audio::write_wav(o.out, pcm, sr, static_cast<uint16_t>(contracts::kAudioChannels), err)) {
            std::fprintf(stderr, "wav: %s\n", err.c_str());
            return 1;
        }
    }
    if (!o.audiolog.empty()) {
        std::FILE* f = std::fopen(o.audiolog.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "audiolog open failed: %s\n", o.audiolog.c_str()); return 1; }
        for (uint64_t tk : footstep_ticks) std::fprintf(f, "%llu\n", static_cast<unsigned long long>(tk));
        std::fclose(f);
    }

    double sumsq = 0.0;
    for (int16_t v : pcm) { const double x = static_cast<double>(v) / 32768.0; sumsq += x * x; }
    const double rms = pcm.empty() ? 0.0 : std::sqrt(sumsq / static_cast<double>(pcm.size()));
    std::printf("ticks: %u\n", ticks);
    std::printf("samples: %llu\n", static_cast<unsigned long long>(pcm.size()));
    std::printf("footsteps: %llu\n", static_cast<unsigned long long>(footstep_ticks.size()));
    std::printf("rms: %.5f\n", rms);
    std::printf("distance_m: %.1f\n", static_cast<double>(w.s.odometer));
    return 0;
}

// ----- M6 footstep reference: the same walk, footstep ticks only (no audio) ---
int run_footsteps(const Options& o) {
    const uint32_t ticks = (o.ticks > 0) ? o.ticks : 2400u;
    MazeWalker w(o.seed);
    std::vector<uint64_t> footstep_ticks;
    uint64_t prev = br::core::footstep_count(w.s);
    for (uint32_t t = 0; t < ticks; ++t) {
        w.step();
        const uint64_t steps = br::core::footstep_count(w.s);
        for (uint64_t k = prev; k < steps; ++k) footstep_ticks.push_back(w.s.tick);
        prev = steps;
    }
    if (!o.out.empty()) {
        std::FILE* f = std::fopen(o.out.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "footstep log open failed: %s\n", o.out.c_str()); return 1; }
        for (uint64_t tk : footstep_ticks) std::fprintf(f, "%llu\n", static_cast<unsigned long long>(tk));
        std::fclose(f);
    }
    std::printf("ticks: %u\n", ticks);
    std::printf("footsteps: %llu\n", static_cast<unsigned long long>(footstep_ticks.size()));
    std::printf("distance_m: %.1f\n", static_cast<double>(w.s.odometer));
    return 0;
}

// ----- M6 audio soak: real-time mixer thread must not block the sim, 0 underruns
// Drives the sim flat-out on open ground (uniform tick time, no gen spikes) while
// the audio engine runs on its own thread, fed lock-free. Run with --audio on and
// off to compare sim throughput; underruns must be zero. --seconds sets a wall
// floor (real soak duration); else --ticks a fixed count.
int run_audiosoak(const Options& o) {
    using namespace std::chrono;
    using namespace br::core;
    WorldState s(o.seed);
    s.wanderer.pos = Vec3{16.0f, kWandererHalfHeight + 0.02f, 16.0f};
    const std::vector<Aabb>& ground = open_ground();

    audio::AudioEngine eng(o.seed, contracts::kAudioSampleRate);
    if (o.audio) eng.start();

    uint64_t prev_steps = footstep_count(s), footsteps = 0, nticks = 0;
    const bool wall = (o.seconds > 0);
    const uint64_t tick_target = (o.ticks > 0) ? o.ticks : 2000000u;
    const auto start = steady_clock::now();
    const auto wall_end = start + seconds(static_cast<long long>(o.seconds));
    for (;;) {
        if (wall) { if ((nticks & 8191u) == 0 && steady_clock::now() >= wall_end) break; }
        else if (nticks >= tick_target) break;
        contracts::InputCommand in{};
        in.move_z = 1.0f;
        in.look_yaw = 0.00008f;  // gentle loop -> stays local (no far-chunk walk)
        tick(s, in, ground);
        ++nticks;
        const uint64_t steps = footstep_count(s);
        const uint32_t newf = static_cast<uint32_t>(steps - prev_steps);
        prev_steps = steps;
        footsteps += newf;
        if (o.audio) eng.post(audio_listener(s), 1.2f, newf);
    }
    const auto end = steady_clock::now();
    const double total_ns = duration<double, std::nano>(end - start).count();

    uint64_t underruns = 0, blocks = 0;
    if (o.audio) { underruns = eng.underruns(); blocks = eng.blocks_rendered(); eng.stop(); }

    std::printf("audio: %d\n", o.audio ? 1 : 0);
    std::printf("ticks: %llu\n", static_cast<unsigned long long>(nticks));
    std::printf("wall_ns: %.0f\n", total_ns);
    std::printf("mean_tick_ns: %.2f\n", nticks ? total_ns / static_cast<double>(nticks) : 0.0);
    std::printf("ticks_per_sec: %.0f\n", nticks ? static_cast<double>(nticks) / (total_ns / 1e9) : 0.0);
    std::printf("footsteps: %llu\n", static_cast<unsigned long long>(footsteps));
    std::printf("audio_blocks: %llu\n", static_cast<unsigned long long>(blocks));
    std::printf("underruns: %llu\n", static_cast<unsigned long long>(underruns));
    return (underruns == 0) ? 0 : 5;
}

// ----- M7 verticality: scripted descent of a stairwell to level -1 ------------
// Builds a level-0 approach floor, a stairwell set piece, and a level -1 landing,
// then walks the wanderer forward. Gravity + capsule collision carry it down the
// steps. Reports the drop, the level reached, sublevel connectivity/geometry, and
// the determinism hash (the gate runs it twice and compares).
int run_descend(const Options& o) {
    using namespace br::core;
    WorldState s(o.seed);
    s.wanderer.pos = Vec3{-4.0f, kWandererHalfHeight + 0.02f, 4.0f};

    const float botY = contracts::level_base_y(-1);
    std::vector<Aabb> col;
    col.push_back(Aabb{{-10.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 12.0f}});            // level-0 approach floor
    std::vector<contracts::BoxInstance> steps;
    br::gen::build_stairwell(0.0f, 0.0f, 0, steps);                              // stairwell set piece
    for (const auto& b : steps)
        col.push_back(Aabb{{b.mn[0], b.mn[1], b.mn[2]}, {b.mx[0], b.mx[1], b.mx[2]}});
    col.push_back(Aabb{{12.0f, botY - 1.0f, 0.0f}, {26.0f, botY, 12.0f}});      // level -1 landing floor
    col.push_back(Aabb{{26.0f, botY - 1.0f, 0.0f}, {26.5f, 3.0f, 12.0f}});      // end wall
    col.push_back(Aabb{{-10.0f, botY - 1.0f, -0.5f}, {26.5f, 3.0f, 0.0f}});     // z- corridor wall
    col.push_back(Aabb{{-10.0f, botY - 1.0f, 8.0f}, {26.5f, 3.0f, 8.5f}});      // z+ corridor wall

    const float startY = s.wanderer.pos.y;
    contracts::InputCommand in{};
    in.move_x = 1.0f;  // walk +X toward and down the stairwell
    const uint32_t ticks = (o.ticks > 0) ? o.ticks : 1500u;
    for (uint32_t t = 0; t < ticks; ++t) tick(s, in, col);

    const float endY = s.wanderer.pos.y;
    const int32_t level_reached = (endY < botY + 2.0f) ? -1 : 0;
    const contracts::ChunkKey lkey = contracts::chunk_key_at(-1, s.wanderer.pos.x, s.wanderer.pos.z);
    const contracts::ChunkData cd = contracts::GenerateChunk(o.seed, lkey);
    const bool conn = br::gen::validate_connectivity(br::gen::generate_layout(o.seed, lkey));
    const bool geom = contracts::ValidateChunkGeometry(cd);

    std::printf("seed: %llu\n", static_cast<unsigned long long>(o.seed));
    std::printf("start_y: %.3f\n", static_cast<double>(startY));
    std::printf("end_y: %.3f\n", static_cast<double>(endY));
    std::printf("drop_m: %.3f\n", static_cast<double>(startY - endY));
    std::printf("level_reached: %d\n", level_reached);
    std::printf("sublevel_connected: %d\n", conn ? 1 : 0);
    std::printf("sublevel_geom_valid: %d\n", geom ? 1 : 0);
    std::printf("final_hash: %016llx\n", static_cast<unsigned long long>(world_state_hash(s)));
    return (level_reached == -1 && conn && geom) ? 0 : 6;
}

// ----- M9 DXR capability probe: device tier + DXC shader compilation ----------
int run_dxr_probe(const Options&) {
    const br::render_dxr::DxrCaps c = br::render_dxr::probe_caps();
    std::printf("adapter: %s\n", c.adapter.c_str());
    std::printf("device5: %d\n", c.device5 ? 1 : 0);
    std::printf("raytracing_tier: %d\n", c.raytracing_tier);
    std::printf("dxc_available: %d\n", c.dxc_available ? 1 : 0);
    std::printf("dxc_compiled: %d\n", c.dxc_compiled ? 1 : 0);
    std::printf("detail: %s\n", c.detail.c_str());
    const bool ready = c.device5 && c.raytracing_tier >= 10 && c.dxc_compiled;
    std::printf("dxr_ready: %d\n", ready ? 1 : 0);
    return ready ? 0 : 7;
}

// ----- M9 DXR scene: BLAS/TLAS of the resident chunks + primary-ray render -----
int run_dxr(const Options& o) {
    // Same 5 canonical poses + spawn as --shot, so DXR and raster line up.
    struct Pose { float yaw, pitch; };
    static const Pose kPoses[5] = {
        {0.0f, 0.0f}, {1.5707963f, 0.0f}, {3.1415927f, 0.0f}, {0.7853982f, 0.42f}, {4.0f, -0.38f},
    };
    const Pose pz = kPoses[o.pose % 5u];
    const float ex = 16.0f, ez = 16.0f;
    const float ey = br::core::kWandererHalfHeight + 0.02f + br::core::kEyeHeight;
    contracts::CameraPose cam{};
    cam.pos[0] = ex; cam.pos[1] = ey; cam.pos[2] = ez;
    cam.yaw = pz.yaw; cam.pitch = pz.pitch;
    cam.fov_y = 1.2217305f;
    cam.aspect = static_cast<float>(o.width) / static_cast<float>(o.height);

    br::stream::StreamManager sm(o.seed, static_cast<int>(o.radius), o.workers);
    const auto center = contracts::chunk_key_at(0, ex, ez);
    sm.update(center); sm.wait_idle(); sm.update(center);

    br::render_dxr::DxrRenderer r;
    if (!r.init(o.width, o.height)) { std::fprintf(stderr, "dxr init: %s\n", r.last_error().c_str()); return 1; }
    if (!r.build_scene(sm.resident())) { std::fprintf(stderr, "dxr scene: %s\n", r.last_error().c_str()); return 1; }
    if (!r.render_scene(cam)) { std::fprintf(stderr, "dxr render: %s\n", r.last_error().c_str()); return 1; }
    std::vector<uint8_t> rgba;
    if (!r.readback(rgba)) { std::fprintf(stderr, "dxr readback: %s\n", r.last_error().c_str()); return 1; }
    if (!o.out.empty()) {
        if (stbi_write_png(o.out.c_str(), static_cast<int>(r.width()), static_cast<int>(r.height()),
                           4, rgba.data(), static_cast<int>(r.width()) * 4) == 0) {
            std::fprintf(stderr, "PNG write failed: %s\n", o.out.c_str()); return 1;
        }
    }
    const uint32_t dbg = r.debug_error_count();
    std::printf("resident_chunks: %llu\n", static_cast<unsigned long long>(sm.resident_count()));
    std::printf("debug_error_count: %u\n", dbg);
    return dbg == 0 ? 0 : 3;
}

// The 5 canonical DXR poses (orientation only) at the proven-open spawn cell,
// shared by --dxr / --dxr-pt / --dxr-fps / --dxr-ghost so they all line up.
contracts::CameraPose canonical_pose(const Options& o, uint32_t pose) {
    static const float yaws[5]    = { 0.0f, 1.5707963f, 3.1415927f, 0.7853982f, 4.0f };
    static const float pitches[5] = { 0.0f, 0.0f, 0.0f, 0.42f, -0.38f };
    const uint32_t i = pose % 5u;
    contracts::CameraPose cam{};
    cam.pos[0] = 16.0f;
    cam.pos[1] = br::core::kWandererHalfHeight + 0.02f + br::core::kEyeHeight;
    cam.pos[2] = 16.0f;
    cam.yaw = yaws[i]; cam.pitch = pitches[i];
    cam.fov_y = 1.2217305f;
    cam.aspect = static_cast<float>(o.width) / static_cast<float>(o.height);
    return cam;
}

// ----- M9 phase 3: path-traced render (emissive fluorescents + GI, accumulated) -
int run_dxr_pt(const Options& o) {
    struct Pose { float yaw, pitch; };
    static const Pose kPoses[5] = {
        {0.0f, 0.0f}, {1.5707963f, 0.0f}, {3.1415927f, 0.0f}, {0.7853982f, 0.42f}, {4.0f, -0.38f},
    };
    const Pose pz = kPoses[o.pose % 5u];
    const float ex = 16.0f, ez = 16.0f;
    const float ey = br::core::kWandererHalfHeight + 0.02f + br::core::kEyeHeight;
    contracts::CameraPose cam{};
    cam.pos[0] = ex; cam.pos[1] = ey; cam.pos[2] = ez;
    cam.yaw = pz.yaw; cam.pitch = pz.pitch;
    cam.fov_y = 1.2217305f;
    cam.aspect = static_cast<float>(o.width) / static_cast<float>(o.height);

    br::stream::StreamManager sm(o.seed, static_cast<int>(o.radius), o.workers);
    const auto center = contracts::chunk_key_at(0, ex, ez);
    sm.update(center); sm.wait_idle(); sm.update(center);

    br::render_dxr::DxrRenderer r;
    if (!r.init(o.width, o.height)) { std::fprintf(stderr, "dxr init: %s\n", r.last_error().c_str()); return 1; }
    if (!r.build_scene(sm.resident())) { std::fprintf(stderr, "dxr scene: %s\n", r.last_error().c_str()); return 1; }
    if (!r.render_pt(cam, o.spp, static_cast<uint32_t>(o.seed))) { std::fprintf(stderr, "dxr pt: %s\n", r.last_error().c_str()); return 1; }
    std::vector<uint8_t> rgba;
    if (!r.readback(rgba)) { std::fprintf(stderr, "dxr readback: %s\n", r.last_error().c_str()); return 1; }
    if (!o.out.empty()) {
        if (stbi_write_png(o.out.c_str(), static_cast<int>(r.width()), static_cast<int>(r.height()),
                           4, rgba.data(), static_cast<int>(r.width()) * 4) == 0) {
            std::fprintf(stderr, "PNG write failed: %s\n", o.out.c_str()); return 1;
        }
    }

    // Luminance histogram (Rec.709) -> mean + black/white fractions (band gate).
    uint64_t hist[256] = {};
    const size_t px = static_cast<size_t>(r.width()) * r.height();
    for (size_t p = 0; p < px; ++p) {
        const uint8_t* cc = &rgba[p * 4];
        const float y = 0.2126f * cc[0] + 0.7152f * cc[1] + 0.0722f * cc[2];
        int b = static_cast<int>(y); if (b < 0) b = 0; if (b > 255) b = 255;
        ++hist[b];
    }
    double sum = 0.0;
    for (int b = 0; b < 256; ++b) sum += static_cast<double>(b) * static_cast<double>(hist[b]);
    const double mean = sum / static_cast<double>(px);
    const double frac_black = static_cast<double>(hist[0] + hist[1] + hist[2]) / static_cast<double>(px);
    const double frac_white = static_cast<double>(hist[253] + hist[254] + hist[255]) / static_cast<double>(px);

    const uint32_t dbg = r.debug_error_count();
    std::printf("resident_chunks: %llu\n", static_cast<unsigned long long>(sm.resident_count()));
    std::printf("pose: %u\n", o.pose % 5u);
    std::printf("spp: %u\n", o.spp);
    std::printf("luma_mean: %.3f\n", mean);
    std::printf("frac_black: %.4f\n", frac_black);
    std::printf("frac_white: %.4f\n", frac_white);
    std::printf("debug_error_count: %u\n", dbg);
    return dbg == 0 ? 0 : 3;
}

// ----- M9 phase 4: interactive PT frame rate (gate #3, ">= 60 FPS while walking") -
// Times reduced-sample frames (the moving path resets accumulation every frame).
int run_dxr_fps(const Options& o) {
    const contracts::CameraPose cam = canonical_pose(o, o.pose);
    br::stream::StreamManager sm(o.seed, static_cast<int>(o.radius), o.workers);
    const auto center = contracts::chunk_key_at(0, 16.0f, 16.0f);
    sm.update(center); sm.wait_idle(); sm.update(center);

    br::render_dxr::DxrRenderer r;
    if (!r.init(o.width, o.height)) { std::fprintf(stderr, "dxr init: %s\n", r.last_error().c_str()); return 1; }
    if (!r.build_scene(sm.resident())) { std::fprintf(stderr, "dxr scene: %s\n", r.last_error().c_str()); return 1; }

    const uint32_t spf = (o.spp > 0) ? o.spp : 1u;   // samples per moving frame
    const int N = 120;
    if (!r.render_pt_frame(cam, spf, static_cast<uint32_t>(o.seed), true)) { std::fprintf(stderr, "dxr pt: %s\n", r.last_error().c_str()); return 1; }  // warm-up
    std::vector<double> ms; ms.reserve(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        if (!r.render_pt_frame(cam, spf, static_cast<uint32_t>(o.seed) + static_cast<uint32_t>(i), true)) { std::fprintf(stderr, "dxr pt: %s\n", r.last_error().c_str()); return 1; }
        const auto t1 = std::chrono::steady_clock::now();
        ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(ms.begin(), ms.end());
    const double median = ms[static_cast<size_t>(N / 2)];
    const double p99 = ms[static_cast<size_t>(N * 0.99)];
    const double fps = (median > 0.0) ? 1000.0 / median : 0.0;
    const uint32_t dbg = r.debug_error_count();
    std::printf("resident_chunks: %llu\n", static_cast<unsigned long long>(sm.resident_count()));
    std::printf("frames: %d\n", N);
    std::printf("spp_per_frame: %u\n", spf);
    std::printf("median_ms: %.3f\n", median);
    std::printf("p99_ms: %.3f\n", p99);
    std::printf("fps: %.1f\n", fps);
    std::printf("debug_error_count: %u\n", dbg);
    return dbg == 0 ? 0 : 3;
}

// ----- M9 phase 4: accumulation reset on movement (gate #3, "no ghost") --------
// Converge pose A, then move to pose B. Resetting yields a clean B; NOT resetting
// blends A into B (ghost). Proves the renderer supports a clean reset-on-move.
int run_dxr_ghost(const Options& o) {
    const contracts::CameraPose A = canonical_pose(o, 1);   // a brighter view
    const contracts::CameraPose B = canonical_pose(o, 4);   // a darker view (looking down)
    br::stream::StreamManager sm(o.seed, static_cast<int>(o.radius), o.workers);
    const auto center = contracts::chunk_key_at(0, 16.0f, 16.0f);
    sm.update(center); sm.wait_idle(); sm.update(center);

    br::render_dxr::DxrRenderer r;
    if (!r.init(o.width, o.height)) { std::fprintf(stderr, "dxr init: %s\n", r.last_error().c_str()); return 1; }
    if (!r.build_scene(sm.resident())) { std::fprintf(stderr, "dxr scene: %s\n", r.last_error().c_str()); return 1; }

    const uint32_t seed = static_cast<uint32_t>(o.seed);
    const uint32_t spf = 64u;
    std::vector<uint8_t> img_ghost, img_clean, img_fresh;
    r.render_pt_frame(A, spf, seed, true);     // converge A...
    r.render_pt_frame(A, spf, seed, false);    // ...accumulator now holds A (128 spp)
    r.render_pt_frame(B, spf, seed, false);    // ghost: continue with B, no reset -> A+B blend
    if (!r.readback(img_ghost)) { std::fprintf(stderr, "readback: %s\n", r.last_error().c_str()); return 1; }
    r.render_pt_frame(B, spf, seed, true);     // clean: reset to B
    if (!r.readback(img_clean)) { std::fprintf(stderr, "readback: %s\n", r.last_error().c_str()); return 1; }
    r.render_pt(B, spf, seed);                 // independent fresh B reference
    if (!r.readback(img_fresh)) { std::fprintf(stderr, "readback: %s\n", r.last_error().c_str()); return 1; }

    auto mad = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) -> double {
        if (a.size() != b.size() || a.empty()) return 255.0;
        uint64_t acc = 0;
        for (size_t i = 0; i < a.size(); ++i) { const int d = static_cast<int>(a[i]) - static_cast<int>(b[i]); acc += static_cast<uint64_t>(d < 0 ? -d : d); }
        return static_cast<double>(acc) / static_cast<double>(a.size());
    };
    const double clean_vs_fresh = mad(img_clean, img_fresh);   // reset works -> ~0
    const double ghost_vs_fresh = mad(img_ghost, img_fresh);   // no reset -> large
    const uint32_t dbg = r.debug_error_count();
    std::printf("clean_vs_fresh: %.4f\n", clean_vs_fresh);
    std::printf("ghost_vs_fresh: %.4f\n", ghost_vs_fresh);
    std::printf("debug_error_count: %u\n", dbg);
    return dbg == 0 ? 0 : 3;
}

// ----- M9 phase 4: TLAS rebuild under streaming + walk-bot 1 km PT (gate #4) ----
int run_dxr_walk(const Options& o) {
    using namespace br::core;
    const float target_m = static_cast<float>(o.km) * 1000.0f;

    WorldState s(o.seed);
    s.wanderer.pos = Vec3{ 2.0f, kWandererHalfHeight + 0.02f, 2.0f };
    WalkBot bot(o.seed ^ 0x9e3779b97f4a7c15ULL, s.wanderer.pos);

    std::vector<Aabb> collision;
    contracts::ChunkKey cached{ 0, static_cast<int64_t>(1) << 40, 0 };
    auto rebuild_collision = [&](contracts::ChunkKey c) {
        collision.clear();
        collision.push_back(Aabb{ {-1.0e6f, -1.0f, -1.0e6f}, {1.0e6f, 0.0f, 1.0e6f} });
        for (int64_t dx = -1; dx <= 1; ++dx)
            for (int64_t dz = -1; dz <= 1; ++dz) {
                const contracts::ChunkData cd = contracts::GenerateChunk(o.seed, contracts::ChunkKey{ 0, c.cx + dx, c.cz + dz });
                for (const auto& b : cd.collision) collision.push_back(Aabb{ {b.mn[0], b.mn[1], b.mn[2]}, {b.mx[0], b.mx[1], b.mx[2]} });
            }
        cached = c;
    };

    br::stream::StreamManager sm(o.seed, static_cast<int>(o.radius), o.workers);
    br::render_dxr::DxrRenderer r;
    if (!r.init(o.width, o.height)) { std::fprintf(stderr, "dxr init: %s\n", r.last_error().c_str()); return 1; }

    contracts::ChunkKey center = contracts::chunk_key_at(0, s.wanderer.pos.x, s.wanderer.pos.z);
    rebuild_collision(center);
    sm.update(center); sm.wait_idle(); sm.update(center);
    if (!r.build_scene(sm.resident())) { std::fprintf(stderr, "dxr scene: %s\n", r.last_error().c_str()); return 1; }
    contracts::ChunkKey sceneCenter = center;

    uint32_t rebuilds = 0, frames = 0;
    const uint64_t kMaxTicks = 1500000;
    const uint64_t kRenderEvery = 120;  // ~1 s of sim between PT frames
    uint64_t lastRender = 0;

    while (s.odometer < target_m && s.tick < kMaxTicks) {
        const contracts::ChunkKey here = contracts::chunk_key_at(0, s.wanderer.pos.x, s.wanderer.pos.z);
        if (here != cached) rebuild_collision(here);
        tick(s, bot.step(s), collision);

        if (s.tick - lastRender >= kRenderEvery) {
            const contracts::ChunkKey rc = contracts::chunk_key_at(0, s.wanderer.pos.x, s.wanderer.pos.z);
            if (rc != sceneCenter) {                    // resident set shifted -> rebuild the AS
                sm.update(rc); sm.wait_idle(); sm.update(rc);
                if (!r.build_scene(sm.resident())) { std::fprintf(stderr, "dxr rebuild: %s\n", r.last_error().c_str()); return 1; }
                ++rebuilds; sceneCenter = rc;
            }
            contracts::CameraPose cam{};
            cam.pos[0] = s.wanderer.pos.x; cam.pos[1] = s.wanderer.pos.y + kEyeHeight; cam.pos[2] = s.wanderer.pos.z;
            cam.yaw = s.wanderer.yaw; cam.pitch = 0.0f;
            cam.fov_y = 1.2217305f; cam.aspect = static_cast<float>(o.width) / static_cast<float>(o.height);
            if (!r.render_pt_frame(cam, 1u, static_cast<uint32_t>(o.seed) + frames, true)) { std::fprintf(stderr, "dxr pt: %s\n", r.last_error().c_str()); return 1; }
            ++frames; lastRender = s.tick;
        }
    }

    const uint32_t dbg = r.debug_error_count();
    std::printf("walk_seed: %llu\n", static_cast<unsigned long long>(o.seed));
    std::printf("distance_m: %.1f\n", static_cast<double>(s.odometer));
    std::printf("ticks: %llu\n", static_cast<unsigned long long>(s.tick));
    std::printf("tlas_rebuilds: %u\n", rebuilds);
    std::printf("pt_frames: %u\n", frames);
    std::printf("debug_error_count: %u\n", dbg);
    const bool ok = (s.odometer >= target_m) && (dbg == 0) && (rebuilds > 0);
    return ok ? 0 : 4;
}

// ----- M9 phase 2b: cross-renderer primary-hit depth compare (exit gate #1) ---
// Render the same pose with the rasteriser and the DXR path tracer, read back
// both depth buffers (NDC), linearize to eye-space metres, and compare per
// pixel. Agreement proves the DXR acceleration structures hold exactly the
// streamed geometry the rasteriser draws.
int run_dxr_depth(const Options& o) {
    struct Pose { float yaw, pitch; };
    static const Pose kPoses[5] = {
        {0.0f, 0.0f}, {1.5707963f, 0.0f}, {3.1415927f, 0.0f}, {0.7853982f, 0.42f}, {4.0f, -0.38f},
    };
    const Pose pz = kPoses[o.pose % 5u];
    const float ex = 16.0f, ez = 16.0f;
    const float ey = br::core::kWandererHalfHeight + 0.02f + br::core::kEyeHeight;
    contracts::CameraPose cam{};
    cam.pos[0] = ex; cam.pos[1] = ey; cam.pos[2] = ez;
    cam.yaw = pz.yaw; cam.pitch = pz.pitch;
    cam.fov_y = 1.2217305f;  // ~70 deg, matches --shot / --dxr
    cam.aspect = static_cast<float>(o.width) / static_cast<float>(o.height);

    br::stream::StreamManager sm(o.seed, static_cast<int>(o.radius), o.workers);
    const auto center = contracts::chunk_key_at(0, ex, ez);
    sm.update(center); sm.wait_idle(); sm.update(center);

    // Raster depth at the pose (warm up the chunk pool, then read the D32 buffer).
    Renderer raster;
    if (!raster.init_headless(o.width, o.height)) { std::fprintf(stderr, "raster init: %s\n", raster.last_error().c_str()); return 1; }
    raster.set_texture_seed(o.seed);
    uint32_t drawn = 0;
    const size_t target = sm.resident_count();
    for (int w = 0; w < 400 && static_cast<size_t>(drawn) < target; ++w) {
        if (!raster.render_chunks(cam, sm.resident(), 64u, o.ticks, &drawn)) {
            std::fprintf(stderr, "raster warmup: %s\n", raster.last_error().c_str()); return 1;
        }
    }
    if (!raster.render_chunks(cam, sm.resident(), 64u, o.ticks, &drawn)) {
        std::fprintf(stderr, "raster render: %s\n", raster.last_error().c_str()); return 1;
    }
    std::vector<float> draster; uint32_t rw = 0, rh = 0;
    if (!raster.readback_depth(draster, &rw, &rh)) { std::fprintf(stderr, "raster depth: %s\n", raster.last_error().c_str()); return 1; }
    const uint32_t rasterDbg = raster.debug_error_count();

    // DXR depth at the same pose from the same resident geometry.
    br::render_dxr::DxrRenderer dr;
    if (!dr.init(o.width, o.height)) { std::fprintf(stderr, "dxr init: %s\n", dr.last_error().c_str()); return 1; }
    if (!dr.build_scene(sm.resident())) { std::fprintf(stderr, "dxr scene: %s\n", dr.last_error().c_str()); return 1; }
    if (!dr.render_scene(cam)) { std::fprintf(stderr, "dxr render: %s\n", dr.last_error().c_str()); return 1; }
    std::vector<float> ddxr;
    if (!dr.readback_depth(ddxr)) { std::fprintf(stderr, "dxr depth: %s\n", dr.last_error().c_str()); return 1; }
    const uint32_t dxrDbg = dr.debug_error_count();

    // NDC depth is hyperbolic; linearize to eye-space z (metres) with the shared
    // near/far before applying a relative epsilon.
    const float kNear = 0.05f, kFar = 500.0f;
    const float q = kFar / (kFar - kNear);
    const float kBg = 0.99999f;   // NDC >= this -> background (clear / miss)
    const float kRelTol = 0.02f;  // 2% linear-depth agreement
    auto linz = [&](float dv) -> float {
        const float denom = 1.0f - dv / q;
        if (denom <= 1e-6f) return kFar;
        return kNear / denom;
    };

    const size_t n = static_cast<size_t>(o.width) * o.height;
    uint64_t both_fg = 0, mismatch = 0, edge = 0, both_bg = 0;
    double sum_relerr = 0.0, max_relerr = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const float dvr = draster[i], dvd = ddxr[i];
        const bool bgR = dvr >= kBg, bgD = dvd >= kBg;
        if (bgR && bgD) { ++both_bg; continue; }
        if (bgR != bgD) { ++edge; continue; }   // silhouette / AA
        ++both_fg;
        const float zr = linz(dvr), zd = linz(dvd);
        float diff = zr - zd; if (diff < 0.0f) diff = -diff;
        float den = zr > zd ? zr : zd; if (den < 1.0f) den = 1.0f;
        const double rel = static_cast<double>(diff / den);
        sum_relerr += rel;
        if (rel > max_relerr) max_relerr = rel;
        if (rel > kRelTol) ++mismatch;
    }
    const double fgD = both_fg > 0 ? static_cast<double>(both_fg) : 1.0;
    const double mismatch_frac = static_cast<double>(mismatch) / fgD;
    const double mean_relerr = sum_relerr / fgD;
    const double edge_frac = static_cast<double>(edge) / static_cast<double>(n);

    std::printf("resident_chunks: %llu\n", static_cast<unsigned long long>(sm.resident_count()));
    std::printf("pose: %u\n", o.pose % 5u);
    std::printf("both_fg_pixels: %llu\n", static_cast<unsigned long long>(both_fg));
    std::printf("both_bg_pixels: %llu\n", static_cast<unsigned long long>(both_bg));
    std::printf("edge_pixels: %llu\n", static_cast<unsigned long long>(edge));
    std::printf("both_fg_mismatch_frac: %.6f\n", mismatch_frac);
    std::printf("mean_fg_depth_relerr: %.6f\n", mean_relerr);
    std::printf("max_fg_depth_relerr: %.6f\n", max_relerr);
    std::printf("edge_frac: %.6f\n", edge_frac);
    std::printf("raster_debug: %u\n", rasterDbg);
    std::printf("dxr_debug: %u\n", dxrDbg);
    return (rasterDbg == 0 && dxrDbg == 0) ? 0 : 3;
}

// ----- M9 DXR dispatch test: raygen writes a UV gradient via DispatchRays ------
int run_dxr_test(const Options& o) {
    br::render_dxr::DxrRenderer r;
    if (!r.init(o.width, o.height)) { std::fprintf(stderr, "dxr init: %s\n", r.last_error().c_str()); return 1; }
    if (!r.render_gradient()) { std::fprintf(stderr, "dxr render: %s\n", r.last_error().c_str()); return 1; }
    std::vector<uint8_t> rgba;
    if (!r.readback(rgba)) { std::fprintf(stderr, "dxr readback: %s\n", r.last_error().c_str()); return 1; }
    if (!o.out.empty()) {
        if (stbi_write_png(o.out.c_str(), static_cast<int>(r.width()), static_cast<int>(r.height()),
                           4, rgba.data(), static_cast<int>(r.width()) * 4) == 0) {
            std::fprintf(stderr, "PNG write failed: %s\n", o.out.c_str()); return 1;
        }
    }
    const uint32_t dbg = r.debug_error_count();
    std::printf("dxr_width: %u\n", r.width());
    std::printf("dxr_height: %u\n", r.height());
    std::printf("debug_error_count: %u\n", dbg);
    return dbg == 0 ? 0 : 3;
}

// ----- M7 biome inspection: the biome at the --shot spawn chunk (0,0) ---------
int run_biomeat(const Options& o) {
    const br::gen::Biome b = br::gen::biome_at(o.seed, 0, 0, 0);
    std::printf("seed: %llu\n", static_cast<unsigned long long>(o.seed));
    std::printf("biome_id: %d\n", static_cast<int>(b));
    std::printf("biome: %s\n", br::gen::biome_name(b));
    return 0;
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

    // M10: any fatal fault leaves a post-mortem minidump the soak harness detects.
    br::telemetry::install_crash_handler(o.crash_dir);
    if (o.crash_test) {
        std::fprintf(stderr, "crash-test: forcing a fault (minidump -> %s)\n",
                     o.crash_dir.empty() ? "runs\\crash" : o.crash_dir.c_str());
        std::fflush(stderr);
        br::telemetry::force_crash();  // [[noreturn]]
    }

    if (o.version) { std::printf("%s\n", br::core::core_version()); return 0; }
    if (o.render_wav) return run_render_wav(o);
    if (o.footsteps)  return run_footsteps(o);
    if (o.audiosoak)  return run_audiosoak(o);
    if (o.biomeat)    return run_biomeat(o);
    if (o.descend)    return run_descend(o);
    if (o.dxr_probe)  return run_dxr_probe(o);
    if (o.dxr_test)   return run_dxr_test(o);
    if (o.dxr_depth)  return run_dxr_depth(o);
    if (o.dxr_pt)     return run_dxr_pt(o);
    if (o.dxr_fps)    return run_dxr_fps(o);
    if (o.dxr_ghost)  return run_dxr_ghost(o);
    if (o.dxr_walk)   return run_dxr_walk(o);
    if (o.dxr)        return run_dxr(o);
    if (o.director_probe) return run_director_probe(o);
    if (o.soak)       return run_soak(o);
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
