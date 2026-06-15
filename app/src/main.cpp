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
#include <xinput.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
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
#include "director/host.h"
#include "audio/synth.h"
#include "audio/room_probe.h"
#include "audio/wav.h"
#include "audio/engine.h"
#include "hud.h"
#include "config.h"
#include "gamepad.h"
#include "head_bob.h"
#include "shoggoth.h"
#include "shoggoth_body.h"
#include "shoggoth_brain.h"
#include "shoggoth_brain_host.h"

namespace contracts = br::contracts;
namespace audio = br::audio;
namespace app = br::app;
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
    bool director_record = false, director_replay = false;
    bool director = false, no_director = false;   // --director enables; --no-director forces off (INV-6 kill switch)
    bool director_eval = false, intro = false, play = false;
    bool audiodev = false, null_backend = false, no_audio = false;  // M14 real-time audio output
    float master = 1.0f, sfx = 1.0f;    // M14 playback mix (master + SFX volume, 0..1)
    bool game = false, menu_shot = false, menu_smoke = false;  // M15 menus + game-state shell
    std::string screen;                 // --menu-shot: which screen to render
    uint32_t sel = 0;                   // --menu-shot: selected item index
    bool config_check = false, resize_smoke = false, fullscreen = false;  // M16
    std::string config;                 // --config path (load/save persisted settings)
    bool credits = false;               // M17 credits screen (text)
    bool rt = false;                    // M19 force ray tracing on in --game/--play
    bool shoggoth = false;              // M20 headless shoggoth chase (determinism + nav)
    bool shoggoth_shot = false;         // M20b render the shoggoth body to a PNG
    bool shoggoth_record = false, shoggoth_replay = false;  // M21 brain record/replay
    bool no_shoggoth_brain = false;     // M21b: kill switch for the live async brain in --play/--game
    uint32_t eval_count = 100;          // --director-eval: scenario count
    uint32_t director_interval_s = 15;  // --soak --director: ambient seconds between summaries (wall clock)
    uint32_t frames = 1, seconds = 0, width = 320, height = 180, ticks = 0;
    uint32_t ticks_per_frame = 30, radius = 6, workers = 4, km = 1, pose = 0, spp = 256;
    uint32_t shot_every = 600;   // soak: write a screenshot every N rendered frames
    uint64_t seed = 1u;
    std::string out, record, replay, hashlog, csv, audiolog, crash_dir, director_url, director_log;
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
        auto flt = [&](float& dst) {
            if (i + 1 >= argc) return false;
            dst = std::strtof(argv[++i], nullptr);
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
        else if (std::strcmp(a, "--director-record") == 0) o.director_record = true;
        else if (std::strcmp(a, "--director-replay") == 0) o.director_replay = true;
        else if (std::strcmp(a, "--director-url") == 0) { if (!str(o.director_url)) return false; }
        else if (std::strcmp(a, "--director-log") == 0) { if (!str(o.director_log)) return false; }
        else if (std::strcmp(a, "--director") == 0) o.director = true;
        else if (std::strcmp(a, "--no-director") == 0) o.no_director = true;
        else if (std::strcmp(a, "--director-eval") == 0) o.director_eval = true;
        else if (std::strcmp(a, "--eval-count") == 0) { if (!u32(o.eval_count)) return false; }
        else if (std::strcmp(a, "--director-interval") == 0) { if (!u32(o.director_interval_s)) return false; }
        else if (std::strcmp(a, "--intro") == 0) o.intro = true;
        else if (std::strcmp(a, "--play") == 0) o.play = true;
        else if (std::strcmp(a, "--audiodev") == 0) o.audiodev = true;
        else if (std::strcmp(a, "--null") == 0) o.null_backend = true;
        else if (std::strcmp(a, "--no-audio") == 0) o.no_audio = true;
        else if (std::strcmp(a, "--master") == 0) { if (!flt(o.master)) return false; }
        else if (std::strcmp(a, "--sfx") == 0) { if (!flt(o.sfx)) return false; }
        else if (std::strcmp(a, "--game") == 0) o.game = true;
        else if (std::strcmp(a, "--menu-shot") == 0) o.menu_shot = true;
        else if (std::strcmp(a, "--menu-smoke") == 0) o.menu_smoke = true;
        else if (std::strcmp(a, "--screen") == 0) { if (!str(o.screen)) return false; }
        else if (std::strcmp(a, "--sel") == 0) { if (!u32(o.sel)) return false; }
        else if (std::strcmp(a, "--config") == 0) { if (!str(o.config)) return false; }
        else if (std::strcmp(a, "--config-check") == 0) o.config_check = true;
        else if (std::strcmp(a, "--resize-smoke") == 0) o.resize_smoke = true;
        else if (std::strcmp(a, "--fullscreen") == 0) o.fullscreen = true;
        else if (std::strcmp(a, "--credits") == 0) o.credits = true;
        else if (std::strcmp(a, "--rt") == 0) o.rt = true;
        else if (std::strcmp(a, "--shoggoth") == 0) o.shoggoth = true;
        else if (std::strcmp(a, "--shoggoth-shot") == 0) o.shoggoth_shot = true;
        else if (std::strcmp(a, "--shoggoth-record") == 0) o.shoggoth_record = true;
        else if (std::strcmp(a, "--shoggoth-replay") == 0) o.shoggoth_replay = true;
        else if (std::strcmp(a, "--no-shoggoth-brain") == 0) o.no_shoggoth_brain = true;
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

// ----- M13 playable: real-time windowed walk (WASD + mouse-look) -------------
// The interactive game loop: a fixed 120 Hz sim tick decoupled from render, real
// keyboard/mouse -> the same InputCommand the walk-bot/replay feed -> core::tick ->
// the streamed maze drawn to the window. --seconds N auto-exits (gate-runnable); 0
// runs until the window closes or Esc. The sim path is unchanged (replay-determinism
// holds); only input gathering + windowed present are new.
// M18: offset the render camera by the humanlike head-bob (view-only — never touches
// WorldState, so determinism + goldens are unaffected; the bob is driven by the
// deterministic odometer + speed, so it's reproducible).
void apply_head_bob(contracts::CameraPose& cam, const br::core::WorldState& s) {
    const float hspeed = std::sqrt(s.wanderer.vel.x * s.wanderer.vel.x + s.wanderer.vel.z * s.wanderer.vel.z);
    const app::BobOffset bob = app::head_bob(s.odometer, hspeed, br::core::kWalkSpeed, br::core::kRunSpeed);
    cam.pos[1] += bob.dy;
    const float rx = std::cos(s.wanderer.yaw), rz = -std::sin(s.wanderer.yaw);  // camera right vector
    cam.pos[0] += rx * bob.dx;
    cam.pos[2] += rz * bob.dx;
}

// Defined below in the Director-helpers anonymous namespace; forward-declared here in
// that SAME anonymous namespace (multiple `namespace {}` blocks in a TU are one and the
// same) so the interactive --play / --game loops can resolve the KEEL host:port for the
// M21b live brain without duplicating the scheme/path-stripping logic.
namespace { void parse_host_port(const std::string& url, std::string& host, int& port); }

int run_play(const Options& o) {
    using namespace br::core;
    using namespace std::chrono;
    HWND hwnd = create_window(o.width, o.height);
    if (!hwnd) { std::fprintf(stderr, "window creation failed\n"); return 1; }
    SetForegroundWindow(hwnd); SetFocus(hwnd);
    Renderer renderer;
    if (!renderer.init_windowed(hwnd, o.width, o.height)) { std::fprintf(stderr, "init: %s\n", renderer.last_error().c_str()); return 1; }
    renderer.set_texture_seed(o.seed);

    br::stream::StreamManager sm(o.seed, static_cast<int>(o.radius), o.workers);
    WorldState s(o.seed);
    s.wanderer.pos = Vec3{ 2.0f, kWandererHalfHeight + 0.02f, 2.0f };

    std::vector<Aabb> collision;
    contracts::ChunkKey cached{ 0, static_cast<int64_t>(1) << 40, 0 };
    auto rebuild = [&](contracts::ChunkKey c) {
        collision.clear();
        collision.push_back(Aabb{ {-1.0e6f, -1.0f, -1.0e6f}, {1.0e6f, 0.0f, 1.0e6f} });
        for (int64_t dx = -1; dx <= 1; ++dx)
            for (int64_t dz = -1; dz <= 1; ++dz) {
                const contracts::ChunkData cd = contracts::GenerateChunk(o.seed, contracts::ChunkKey{ 0, c.cx + dx, c.cz + dz });
                for (const auto& b : cd.collision) collision.push_back(Aabb{ {b.mn[0], b.mn[1], b.mn[2]}, {b.mx[0], b.mx[1], b.mx[2]} });
            }
        cached = c;
    };
    // M19: lazy DXR for --play --rt (ray tracing at 2/3 internal res, upscaled to the window).
    std::unique_ptr<br::render_dxr::DxrRenderer> dxr;
    uint32_t dxrW = 0, dxrH = 0;
    contracts::ChunkKey dxrCenter{0, static_cast<int64_t>(1) << 40, 0};
    float lastCamX = 1e9f, lastCamY = 1e9f, lastCamZ = 1e9f, lastYaw = 1e9f, lastPitch = 1e9f;
    bool rtOn = o.rt;
    uint64_t rtFrames = 0;
    std::vector<uint8_t> lastRt;
    // M20b: a Shoggoth hunts the wanderer, its procedural body rendered in-world.
    app::Shoggoth shog;
    shog.pos = s.wanderer.pos; shog.pos.x += 22.0f; shog.pos.z += 6.0f;  // spawn a few cells away
    std::vector<contracts::ChunkVertex> shogBody;
    contracts::ChunkKey c0 = contracts::chunk_key_at(0, s.wanderer.pos.x, s.wanderer.pos.z);
    rebuild(c0);
    sm.update(c0); sm.wait_idle(); sm.update(c0);

    const float aspect = static_cast<float>(o.width) / static_cast<float>(o.height);
    const float kSens = 0.0022f;  // mouse radians/pixel
    ShowCursor(FALSE);
    POINT ctr{ static_cast<LONG>(o.width / 2), static_cast<LONG>(o.height / 2) };
    auto recenter = [&]() -> POINT { POINT p = ctr; ClientToScreen(hwnd, &p); SetCursorPos(p.x, p.y); return p; };
    POINT anchor = recenter();

    br::telemetry::FrameCsv csv;
    const bool csvOpen = !o.csv.empty() && csv.open(o.csv);

    // M14: real-time audio output. The mixer runs on its own thread + device
    // callback, fed lock-free via post(); a failed device open just means silence
    // (never blocks the walk). Determinism is untouched — this is presentation only.
    audio::AudioEngine eng(o.seed, contracts::kAudioSampleRate);
    bool audioOn = false;
    if (!o.no_audio) {
        eng.set_master_volume(o.master);
        eng.set_sfx_volume(o.sfx);
        audioOn = eng.start_device(false);
    }
    uint64_t prevSteps = footstep_count(s);

    const float tickDt = 1.0f / 120.0f;
    const auto t_start = steady_clock::now();
    auto prev = t_start;
    float accum = 0.0f;
    const bool timed = (o.seconds > 0);
    uint64_t frames = 0;
    bool running = true;

    // M21b: the LIVE async brain. KEEL inference runs on its own thread so the creature
    // thinks WHILE you play without ever hitching the loop (mirrors the Director's async
    // host). On by default; --no-shoggoth-brain kills it; a graceful no-op if KEEL is down.
    std::unique_ptr<app::ShoggothBrainHost> brain;
    if (!o.no_shoggoth_brain) {
        std::string bh; int bp; parse_host_port(o.director_url, bh, bp);
        brain = std::make_unique<app::ShoggothBrainHost>(bh, bp);
    }
    const auto brain_interval = milliseconds(3000);  // ambient: a thought every ~3 s (wall)
    auto last_brain = t_start - brain_interval;       // fire the first summary promptly
    uint64_t brain_intents = 0;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (!running) break;
        if (timed && steady_clock::now() >= t_start + seconds(static_cast<long long>(o.seconds))) break;
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) break;

        contracts::InputCommand in{};
        if (GetAsyncKeyState('W') & 0x8000) in.move_z += 1.0f;
        if (GetAsyncKeyState('S') & 0x8000) in.move_z -= 1.0f;
        if (GetAsyncKeyState('D') & 0x8000) in.move_x += 1.0f;
        if (GetAsyncKeyState('A') & 0x8000) in.move_x -= 1.0f;
        if (GetAsyncKeyState(VK_SPACE) & 0x8000) in.buttons |= contracts::kButtonJump;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) in.buttons |= contracts::kButtonRun;

        POINT cur; GetCursorPos(&cur);
        const float look_yaw = static_cast<float>(cur.x - anchor.x) * kSens;
        const float look_pitch = -static_cast<float>(cur.y - anchor.y) * kSens;
        anchor = recenter();

        const auto now = steady_clock::now();
        const double frame_ms = duration<double, std::milli>(now - prev).count();
        accum += duration<float>(now - prev).count();
        prev = now;
        if (accum > 0.25f) accum = 0.25f;  // clamp the spiral of death
        bool firstTick = true;
        while (accum >= tickDt) {
            const contracts::ChunkKey here = contracts::chunk_key_at(0, s.wanderer.pos.x, s.wanderer.pos.z);
            if (here != cached) rebuild(here);
            contracts::InputCommand step = in;
            if (firstTick) { step.look_yaw = look_yaw; step.look_pitch = look_pitch; firstTick = false; }  // mouse delta is per-frame
            tick(s, step, collision);
            app::shoggoth_step(shog, s.wanderer.pos, o.seed, (s.tick % 8u) == 0u);  // M20b: the hunt
            accum -= tickDt;
        }

        // M21b: drive the live brain off-thread. Submit a fresh summary on the ambient
        // cadence; apply any returned intent at this tick boundary (latest-wins). Both
        // calls are non-blocking, so KEEL never enters frame time.
        if (brain) {
            if (now - last_brain >= brain_interval) {
                brain->submit(app::build_shoggoth_summary(shog, s.wanderer.pos, s.tick));
                last_brain = now;
            }
            for (const app::ShoggothIntent& it : brain->poll()) { shog.intent = it; ++brain_intents; }
        }

        if (audioOn) {  // hand the mixer the latest ear pose + footfalls this frame
            const uint64_t steps = footstep_count(s);
            eng.post(audio_listener(s), 1.2f, static_cast<uint32_t>(steps - prevSteps));
            prevSteps = steps;
        }

        const contracts::ChunkKey center = contracts::chunk_key_at(0, s.wanderer.pos.x, s.wanderer.pos.z);
        sm.update(center);
        contracts::CameraPose cam = wanderer_camera(s, aspect);
        apply_head_bob(cam, s);  // M18 head-bob (view-only)
        if (rtOn) {  // M19: ray-traced path (DXR at 2/3 res, upscaled present)
            const uint32_t rw = (o.width * 2u) / 3u, rh = (o.height * 2u) / 3u;
            if (!dxr || dxrW != rw || dxrH != rh) {
                dxr = std::make_unique<br::render_dxr::DxrRenderer>();
                if (dxr->init(rw, rh)) { dxrW = rw; dxrH = rh; dxrCenter = contracts::ChunkKey{0, static_cast<int64_t>(1) << 40, 0}; }
                else { rtOn = false; dxr.reset(); }  // no DXR -> raster fallback
            }
            if (dxr) {
                if (center != dxrCenter) { dxr->build_scene(sm.resident()); dxrCenter = center; lastYaw = 1e9f; }
                const bool moved = (cam.pos[0] != lastCamX || cam.pos[1] != lastCamY || cam.pos[2] != lastCamZ ||
                                    cam.yaw != lastYaw || cam.pitch != lastPitch);
                lastCamX = cam.pos[0]; lastCamY = cam.pos[1]; lastCamZ = cam.pos[2]; lastYaw = cam.yaw; lastPitch = cam.pitch;
                dxr->render_pt_frame(cam, 4u, static_cast<uint32_t>(o.seed) + static_cast<uint32_t>(frames), moved);
                std::vector<uint8_t> rt;
                if (dxr->readback(rt) && renderer.present_overlay_windowed(rt.data(), dxrW, dxrH)) {
                    ++rtFrames;
                    if (!o.out.empty()) lastRt = rt;  // keep the last RT frame for an optional capture
                }
            }
        }
        if (!rtOn) {
            // M20b: inject the shoggoth's procedural body as a per-frame chunk (the
            // changing key re-uploads it each frame so the tentacles writhe).
            app::build_shoggoth_mesh(shogBody, shog.pos, shog.writhe, 1.4f);
            std::vector<contracts::ResidentChunk> withShog = sm.resident();
            withShog.push_back(contracts::ResidentChunk{contracts::ChunkKey{9999, static_cast<int64_t>(frames), 0}, shogBody.data(), static_cast<uint32_t>(shogBody.size())});
            uint32_t drawn = 0;
            if (!renderer.render_chunks_windowed(cam, withShog, 8u, s.tick, &drawn)) {
                std::fprintf(stderr, "render: %s\n", renderer.last_error().c_str());
                ShowCursor(TRUE);
                return 1;
            }
        }
        if (csvOpen && frames > 0) {  // skip frame 0 (includes window/stream warm-up)
            csv.write(contracts::FrameMetrics{ frames, frame_ms, sm.resident_count(), sm.generated_total(), renderer.process_private_bytes() });
        }
        ++frames;
    }
    if (csvOpen) csv.close();
    ShowCursor(TRUE);
    const uint32_t dbg = renderer.debug_error_count();
    const unsigned long long underruns = static_cast<unsigned long long>(eng.underruns());
    std::printf("audio_backend: %s\n", audioOn ? eng.backend() : "off");
    eng.stop();
    std::printf("play_seed: %llu\n", static_cast<unsigned long long>(o.seed));
    std::printf("frames: %llu\n", static_cast<unsigned long long>(frames));
    std::printf("ticks: %llu\n", static_cast<unsigned long long>(s.tick));
    std::printf("distance_m: %.1f\n", static_cast<double>(s.odometer));
    std::printf("audio_underruns: %llu\n", underruns);
    std::printf("rt_frames: %llu\n", static_cast<unsigned long long>(rtFrames));
    if (!o.out.empty() && !lastRt.empty()) {  // M19: optional capture of the windowed RT frame
        stbi_write_png(o.out.c_str(), static_cast<int>(dxrW), static_cast<int>(dxrH), 4, lastRt.data(), static_cast<int>(dxrW) * 4);
        double sum = 0.0;
        for (size_t i = 0; i + 2 < lastRt.size(); i += 4) sum += 0.299 * lastRt[i] + 0.587 * lastRt[i + 1] + 0.114 * lastRt[i + 2];
        std::printf("rt_luma_mean: %.1f\n", sum / static_cast<double>(dxrW * dxrH));
    }
    const unsigned long long brain_req = brain ? brain->requests() : 0ull;
    brain.reset();  // M21b: join the worker thread before exit
    std::printf("brain_intents: %llu\n", static_cast<unsigned long long>(brain_intents));
    std::printf("brain_requests: %llu\n", brain_req);
    std::printf("debug_error_count: %u\n", dbg);
    return dbg == 0 ? 0 : 3;
}

// ----- M16 settings/persistence/windowing/gamepad helpers --------------------
// Borderless-fullscreen toggle: no exclusive mode — the FLIP_DISCARD swapchain just
// resizes to cover the monitor. Saves/restores the windowed style + rect.
struct WinSaved { LONG style = 0; RECT rect = {0, 0, 0, 0}; bool valid = false; };

void set_fullscreen(HWND hwnd, bool on, WinSaved& saved, uint32_t& outW, uint32_t& outH) {
    outW = 0; outH = 0;
    if (on) {
        if (!saved.valid) { saved.style = GetWindowLong(hwnd, GWL_STYLE); GetWindowRect(hwnd, &saved.rect); saved.valid = true; }
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        const int mw = mi.rcMonitor.right - mi.rcMonitor.left;
        const int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
        SetWindowLong(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mw, mh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        outW = static_cast<uint32_t>(mw); outH = static_cast<uint32_t>(mh);
    } else if (saved.valid) {
        SetWindowLong(hwnd, GWL_STYLE, saved.style);
        const int w = saved.rect.right - saved.rect.left, h = saved.rect.bottom - saved.rect.top;
        SetWindowPos(hwnd, HWND_NOTOPMOST, saved.rect.left, saved.rect.top, w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        outW = static_cast<uint32_t>(w); outH = static_cast<uint32_t>(h);
    }
}

// Poll XInput controller 0 -> a normalised GamepadState (the pure map is in gamepad.h).
app::GamepadState poll_gamepad() {
    app::GamepadState g;
    XINPUT_STATE st{};
    if (XInputGetState(0, &st) != ERROR_SUCCESS) return g;  // not connected
    g.connected = true;
    auto norm = [](SHORT v) { return static_cast<float>(v) / 32767.0f; };
    g.lx = norm(st.Gamepad.sThumbLX); g.ly = norm(st.Gamepad.sThumbLY);
    g.rx = norm(st.Gamepad.sThumbRX); g.ry = norm(st.Gamepad.sThumbRY);
    g.a = (st.Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0;
    g.start = (st.Gamepad.wButtons & XINPUT_GAMEPAD_START) != 0;
    g.run = st.Gamepad.bLeftTrigger > 96 || (st.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
    return g;
}

// M16 gate: write a config from the CLI flags, read it back, and APPLY it to a
// headless render — proving the persisted settings round-trip AND drive the engine
// (the rendered frame is at the config's resolution + seed).
int run_config_check(const Options& o) {
    const std::string path = o.config.empty() ? std::string("runs/backrooms.cfg") : o.config;
    app::Config c;
    c.width = static_cast<int>(o.width);
    c.height = static_cast<int>(o.height);
    c.fullscreen = o.fullscreen ? 1 : 0;
    c.master = static_cast<int>(o.master * 100.0f + 0.5f);
    c.sfx = static_cast<int>(o.sfx * 100.0f + 0.5f);
    c.director = o.director ? 1 : 0;
    c.seed = o.seed;
    if (!app::save_config(path, c)) { std::fprintf(stderr, "config save failed: %s\n", path.c_str()); return 1; }
    app::Config rd;
    if (!app::load_config(path, rd)) { std::fprintf(stderr, "config load failed: %s\n", path.c_str()); return 1; }

    // Apply the loaded resolution + seed to a real headless render.
    Renderer renderer;
    if (!renderer.init_headless(static_cast<uint32_t>(rd.width), static_cast<uint32_t>(rd.height))) {
        std::fprintf(stderr, "init: %s\n", renderer.last_error().c_str()); return 1;
    }
    renderer.set_texture_seed(rd.seed);
    contracts::CameraPose cam{};
    cam.pos[1] = br::core::kWandererHalfHeight + 0.02f + br::core::kEyeHeight;
    cam.fov_y = 1.2217305f;
    cam.aspect = static_cast<float>(rd.width) / static_cast<float>(rd.height);
    const std::vector<contracts::ResidentChunk> none;
    uint32_t drawn = 0;
    if (!renderer.render_chunks(cam, none, 0u, 0u, &drawn)) { std::fprintf(stderr, "render: %s\n", renderer.last_error().c_str()); return 1; }
    FrameImage img;
    if (!renderer.readback(img)) { std::fprintf(stderr, "readback: %s\n", renderer.last_error().c_str()); return 1; }

    std::printf("width: %d\n", rd.width);
    std::printf("height: %d\n", rd.height);
    std::printf("rendered_width: %u\n", img.width);
    std::printf("rendered_height: %u\n", img.height);
    std::printf("master: %d\n", rd.master);
    std::printf("sfx: %d\n", rd.sfx);
    std::printf("director: %d\n", rd.director);
    std::printf("seed: %llu\n", static_cast<unsigned long long>(rd.seed));
    std::printf("debug_error_count: %u\n", renderer.debug_error_count());
    return renderer.debug_error_count() == 0 ? 0 : 3;
}

// M16 gate: resolution + fullscreen change smoke. Resize the swapchain across a few
// resolutions + a borderless-fullscreen toggle, presenting each time; debug-clean.
int run_resize_smoke(const Options& o) {
    (void)o;
    HWND hwnd = create_window(1280, 720);
    if (!hwnd) { std::fprintf(stderr, "window creation failed\n"); return 1; }
    SetForegroundWindow(hwnd); SetFocus(hwnd);
    Renderer renderer;
    if (!renderer.init_windowed(hwnd, 1280, 720)) { std::fprintf(stderr, "init: %s\n", renderer.last_error().c_str()); return 1; }
    app::MenuModel m; m.screen = app::Screen::MainMenu;
    auto present_at = [&](uint32_t w, uint32_t h) -> bool {
        std::vector<uint8_t> ovl;
        app::build_menu_overlay(ovl, w, h, m);
        return renderer.present_overlay_windowed(ovl.data(), w, h);
    };

    const struct { uint32_t w, h; } sizes[] = { {1280, 720}, {1920, 1080}, {1024, 768}, {1280, 720} };
    int steps = 0;
    for (const auto& sz : sizes) {
        if (!renderer.resize(sz.w, sz.h)) { std::fprintf(stderr, "resize: %s\n", renderer.last_error().c_str()); return 1; }
        if (!present_at(sz.w, sz.h)) { std::fprintf(stderr, "present: %s\n", renderer.last_error().c_str()); return 1; }
        ++steps;
    }
    WinSaved saved; uint32_t fw = 0, fh = 0;
    set_fullscreen(hwnd, true, saved, fw, fh);
    if (fw && fh) {
        if (!renderer.resize(fw, fh)) { std::fprintf(stderr, "fs resize: %s\n", renderer.last_error().c_str()); return 1; }
        if (!present_at(fw, fh)) { std::fprintf(stderr, "fs present: %s\n", renderer.last_error().c_str()); return 1; }
        ++steps;
    }
    uint32_t ww = 0, wh = 0;
    set_fullscreen(hwnd, false, saved, ww, wh);
    if (ww && wh) {
        if (!renderer.resize(ww, wh)) { std::fprintf(stderr, "restore resize: %s\n", renderer.last_error().c_str()); return 1; }
        if (!present_at(ww, wh)) { std::fprintf(stderr, "restore present: %s\n", renderer.last_error().c_str()); return 1; }
        ++steps;
    }
    const uint32_t dbg = renderer.debug_error_count();
    std::printf("resize_steps: %d\n", steps);
    std::printf("debug_error_count: %u\n", dbg);
    return dbg == 0 ? 0 : 3;
}

// M17: the credits / about screen (text). Shipped in the portable build; also the
// CREDITS.txt source. No asset files — even this is generated by code.
int run_credits(const Options&) {
    std::printf("BACKROOMS SIM\n");
    std::printf("=============\n");
    std::printf("An infinite, never-repeating, fully procedural Backrooms walking\n");
    std::printf("simulation. No asset files -- every wall, light, sound, and frame is\n");
    std::printf("generated from a seed. Native Win32, C++20, D3D12 + DXR path tracer.\n\n");
    std::printf("Engineering: built autonomously by Claude (Anthropic), milestone by\n");
    std::printf("milestone -- every one verified by a machine-checkable gate.\n");
    std::printf("Director (optional): a local LLM via the KEEL sidecar -- sovereign, $0,\n");
    std::printf("and it never breaks bit-exact replay.\n\n");
    std::printf("Tech: deterministic PCG64 sim core, infinite chunk streaming, procedural\n");
    std::printf("materials + fluorescent lighting, Freeverb-style reverb, VHS post.\n");
    std::printf("Third-party: Catch2 (tests), stb (PNG), miniaudio (audio output).\n\n");
    std::printf("version: 2.0\n");
    return 0;
}

// ----- M15 menus + game-state shell -----------------------------------------
app::Screen screen_from_name(const std::string& n) {
    if (n == "splash") return app::Screen::Splash;
    if (n == "pause") return app::Screen::Pause;
    if (n == "settings") return app::Screen::Settings;
    return app::Screen::MainMenu;
}

// Render one menu screen to a PNG (deterministic, CPU-only -> the menu golden).
int run_menu_shot(const Options& o) {
    app::MenuModel m;
    m.screen = screen_from_name(o.screen);
    m.has_session = (m.screen == app::Screen::Pause);  // pause implies a live session
    switch (m.screen) {
        case app::Screen::MainMenu: m.main_sel = static_cast<int>(o.sel); break;
        case app::Screen::Pause:    m.pause_sel = static_cast<int>(o.sel); break;
        case app::Screen::Settings: m.settings_sel = static_cast<int>(o.sel); break;
        default: break;
    }
    std::vector<uint8_t> rgba;
    app::build_menu_overlay(rgba, o.width, o.height, m);
    const std::string out = o.out.empty() ? std::string("runs/menu.png") : o.out;
    if (!stbi_write_png(out.c_str(), static_cast<int>(o.width), static_cast<int>(o.height), 4,
                        rgba.data(), static_cast<int>(o.width) * 4)) {
        std::fprintf(stderr, "menu-shot: write failed\n"); return 1;
    }
    std::printf("menu_screen: %s\n", o.screen.empty() ? "mainmenu" : o.screen.c_str());
    std::printf("menu_sel: %u\n", o.sel);
    std::printf("out: %s\n", out.c_str());
    return 0;
}

// Composite every menu screen through the GPU (post + HUD path) across state
// changes; assert the debug layer stays silent (M15 exit gate #3).
int run_menu_smoke(const Options& o) {
    Renderer renderer;
    if (!renderer.init_headless(o.width, o.height)) {
        std::fprintf(stderr, "init: %s\n", renderer.last_error().c_str()); return 1;
    }
    renderer.set_texture_seed(o.seed);
    renderer.set_post(true, static_cast<uint32_t>(o.seed), 0.0f, true);  // VHS post + HUD composite

    contracts::CameraPose cam{};
    cam.fov_y = 1.2217305f;
    cam.aspect = static_cast<float>(o.width) / static_cast<float>(o.height);
    const std::vector<contracts::ResidentChunk> none;
    const char* names[4] = {"splash", "mainmenu", "pause", "settings"};

    int composited = 0;
    for (int i = 0; i < 8; ++i) {  // two full cycles -> exercises the state changes
        app::MenuModel m;
        m.screen = screen_from_name(names[i % 4]);
        m.has_session = true;
        m.main_sel = i % app::kMainItems;
        m.settings_sel = i % app::kSettingsItems;
        std::vector<uint8_t> hud;
        app::build_menu_overlay(hud, o.width, o.height, m);
        if (!renderer.upload_hud_overlay(hud.data(), o.width, o.height)) {
            std::fprintf(stderr, "hud: %s\n", renderer.last_error().c_str()); return 1;
        }
        uint32_t drawn = 0;
        if (!renderer.render_chunks(cam, none, 0u, 0u, &drawn)) {
            std::fprintf(stderr, "render: %s\n", renderer.last_error().c_str()); return 1;
        }
        FrameImage img;
        if (!renderer.readback(img)) {  // triggers the post + HUD composite
            std::fprintf(stderr, "readback: %s\n", renderer.last_error().c_str()); return 1;
        }
        ++composited;
    }
    const uint32_t dbg = renderer.debug_error_count();
    std::printf("screens_composited: %d\n", composited);
    std::printf("debug_error_count: %u\n", dbg);
    return dbg == 0 ? 0 : 3;
}

// The windowed game shell: boot to the main menu, New Game -> the live walk, Esc ->
// pause, Settings, Quit. Menu screens present the overlay; Play runs the M13 walk +
// M14 audio. Synthetic-input transitions are covered by the headless menu tests; this
// is the interactive shell. `--seconds N` auto-exits (debug-clean smoke for the gate).
int run_game(const Options& o) {
    using namespace br::core;
    using namespace std::chrono;

    // M16: load persisted settings (next to the exe by default). The config drives the
    // initial resolution / fullscreen / volumes / mouse / seed; it is re-saved on exit.
    const std::string cfgPath = o.config.empty() ? std::string("backrooms.cfg") : o.config;
    app::Config cfg;
    if (!app::load_config(cfgPath, cfg)) {
        cfg.width = static_cast<int>(o.width); cfg.height = static_cast<int>(o.height); cfg.seed = o.seed;
        cfg = app::sanitize(cfg);
    }

    uint32_t curW = static_cast<uint32_t>(cfg.width), curH = static_cast<uint32_t>(cfg.height);
    HWND hwnd = create_window(curW, curH);
    if (!hwnd) { std::fprintf(stderr, "window creation failed\n"); return 1; }
    SetForegroundWindow(hwnd); SetFocus(hwnd);
    Renderer renderer;
    if (!renderer.init_windowed(hwnd, curW, curH)) {
        std::fprintf(stderr, "init: %s\n", renderer.last_error().c_str()); return 1;
    }

    app::MenuModel model;
    model.seed = cfg.seed;
    model.settings.master_pct = cfg.master; model.settings.sfx_pct = cfg.sfx;
    model.settings.mouse_pct = cfg.mouse; model.settings.director = cfg.director;
    model.settings.rt = o.rt ? 1 : cfg.renderer;  // M19: --rt forces on; else from config

    WinSaved fsSaved; bool isFull = false;
    auto apply_fullscreen = [&](bool on) {
        if (on == isFull) return;
        uint32_t nw = 0, nh = 0;
        set_fullscreen(hwnd, on, fsSaved, nw, nh);
        if (nw && nh) { renderer.resize(nw, nh); curW = nw; curH = nh; isFull = on; }
    };
    if (cfg.fullscreen) apply_fullscreen(true);

    std::unique_ptr<br::stream::StreamManager> sm;
    WorldState s(model.seed);
    std::vector<Aabb> collision;
    contracts::ChunkKey cached{ 0, static_cast<int64_t>(1) << 40, 0 };
    uint64_t texSeed = model.seed;
    auto rebuild = [&](contracts::ChunkKey c) {
        collision.clear();
        collision.push_back(Aabb{ {-1.0e6f, -1.0f, -1.0e6f}, {1.0e6f, 0.0f, 1.0e6f} });
        for (int64_t dx = -1; dx <= 1; ++dx)
            for (int64_t dz = -1; dz <= 1; ++dz) {
                const contracts::ChunkData cd = contracts::GenerateChunk(texSeed, contracts::ChunkKey{ 0, c.cx + dx, c.cz + dz });
                for (const auto& b : cd.collision) collision.push_back(Aabb{ {b.mn[0], b.mn[1], b.mn[2]}, {b.mx[0], b.mx[1], b.mx[2]} });
            }
        cached = c;
    };
    uint64_t prevSteps = 0;
    auto start_session = [&](uint64_t seed) {
        texSeed = seed;
        renderer.set_texture_seed(seed);
        sm = std::make_unique<br::stream::StreamManager>(seed, static_cast<int>(o.radius), o.workers);
        s = WorldState(seed);
        s.wanderer.pos = Vec3{ 2.0f, kWandererHalfHeight + 0.02f, 2.0f };
        const contracts::ChunkKey c0 = contracts::chunk_key_at(0, s.wanderer.pos.x, s.wanderer.pos.z);
        rebuild(c0);
        sm->update(c0); sm->wait_idle(); sm->update(c0);
        prevSteps = footstep_count(s);
    };

    audio::AudioEngine eng(model.seed, contracts::kAudioSampleRate);
    bool audioOn = o.no_audio ? false : eng.start_device(false);

    const float tickDt = 1.0f / 120.0f;
    auto recenter = [&]() -> POINT { POINT p{ static_cast<LONG>(curW / 2), static_cast<LONG>(curH / 2) }; ClientToScreen(hwnd, &p); SetCursorPos(p.x, p.y); return p; };
    POINT anchor{ static_cast<LONG>(curW / 2), static_cast<LONG>(curH / 2) };
    bool cursorHidden = false;
    bool prevF11 = false, prevPadStart = false, prevF2 = false;

    // M19: lazy DXR renderer for the ray-tracing toggle. Rendered at a reduced internal
    // resolution (perf) + upscaled to the window by present_overlay_windowed. Scene rebuilt
    // when the chunk center changes; accumulation resets when the camera moves.
    std::unique_ptr<br::render_dxr::DxrRenderer> dxr;
    uint32_t dxrW = 0, dxrH = 0;
    contracts::ChunkKey dxrCenter{0, static_cast<int64_t>(1) << 40, 0};
    float lastCamX = 1e9f, lastCamY = 1e9f, lastCamZ = 1e9f, lastYaw = 1e9f, lastPitch = 1e9f;
    app::Shoggoth shog;                       // M20b: the hunting creature (spawned on New Game)
    std::vector<contracts::ChunkVertex> shogBody;

    struct Keys { bool up=false, down=false, left=false, right=false, enter=false, esc=false; } prev;
    auto edge = [](bool now, bool& p) { const bool e = now && !p; p = now; return e; };

    const auto t_start = steady_clock::now();
    auto prevt = t_start;
    float accum = 0.0f;
    const bool timed = (o.seconds > 0);
    uint64_t frames = 0;
    bool running = true;

    // M21b: the LIVE async brain — KEEL inference off the frame thread so the creature
    // thinks while you actually play (mirrors the Director's async host). On by default;
    // --no-shoggoth-brain kills it; graceful no-op if KEEL is down. Only fed while in Play.
    std::unique_ptr<app::ShoggothBrainHost> brain;
    if (!o.no_shoggoth_brain) {
        std::string bh; int bp; parse_host_port(o.director_url, bh, bp);
        brain = std::make_unique<app::ShoggothBrainHost>(bh, bp);
    }
    const auto brain_interval = milliseconds(3000);
    auto last_brain = t_start - brain_interval;
    uint64_t brain_intents = 0;

    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (!running) break;
        if (timed && steady_clock::now() >= t_start + seconds(static_cast<long long>(o.seconds))) break;

        const bool inPlay = (model.screen == app::Screen::Play);
        if (inPlay && !cursorHidden) { ShowCursor(FALSE); anchor = recenter(); cursorHidden = true; }
        if (!inPlay && cursorHidden) { ShowCursor(TRUE); cursorHidden = false; }

        // Edge-detected menu navigation (arrows; WASD doubles as nav outside Play).
        const bool kUp = (GetAsyncKeyState(VK_UP) & 0x8000) || (!inPlay && (GetAsyncKeyState('W') & 0x8000));
        const bool kDn = (GetAsyncKeyState(VK_DOWN) & 0x8000) || (!inPlay && (GetAsyncKeyState('S') & 0x8000));
        const bool kLf = (GetAsyncKeyState(VK_LEFT) & 0x8000) || (!inPlay && (GetAsyncKeyState('A') & 0x8000));
        const bool kRt = (GetAsyncKeyState(VK_RIGHT) & 0x8000) || (!inPlay && (GetAsyncKeyState('D') & 0x8000));
        const bool kEn = (GetAsyncKeyState(VK_RETURN) & 0x8000) || (!inPlay && (GetAsyncKeyState(VK_SPACE) & 0x8000));
        const bool kEs = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        const bool eUp = edge(kUp, prev.up), eDn = edge(kDn, prev.down), eLf = edge(kLf, prev.left);
        const bool eRt = edge(kRt, prev.right), eEn = edge(kEn, prev.enter), eEs = edge(kEs, prev.esc);
        app::UiAction act = app::UiAction::None;
        if (eEs) act = app::UiAction::Back;
        else if (eEn) act = app::UiAction::Activate;
        else if (eUp) act = app::UiAction::Up;
        else if (eDn) act = app::UiAction::Down;
        else if (eLf) act = app::UiAction::Left;
        else if (eRt) act = app::UiAction::Right;

        // F11 toggles borderless fullscreen (resizes the swapchain back buffers).
        const bool f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (f11 && !prevF11) { apply_fullscreen(!isFull); anchor = recenter(); }
        prevF11 = f11;
        const bool f2 = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;  // M19: toggle ray tracing
        if (f2 && !prevF2) model.settings.rt ^= 1;
        prevF2 = f2;

        // Gamepad (M16): Start pauses from play / activates in a menu (movement below).
        const app::GamepadState pad = poll_gamepad();
        const bool padStart = pad.connected && pad.start;
        if (padStart && !prevPadStart && act == app::UiAction::None)
            act = inPlay ? app::UiAction::Back : app::UiAction::Activate;
        prevPadStart = padStart;

        if (act != app::UiAction::None) {
            const app::UiCommand cmd = app::menu_step(model, act);
            if (cmd == app::UiCommand::StartGame) {
                start_session(model.seed);
                shog = app::Shoggoth{};
                shog.pos = s.wanderer.pos; shog.pos.x += 22.0f; shog.pos.z += 6.0f;  // M20b spawn
            }
            else if (cmd == app::UiCommand::QuitApp) running = false;
            // ResumeGame keeps the existing session as-is.
        }
        if (audioOn) {
            eng.set_master_volume(static_cast<float>(model.settings.master_pct) / 100.0f);
            eng.set_sfx_volume(static_cast<float>(model.settings.sfx_pct) / 100.0f);
        }
        if (!running) break;

        const auto now = steady_clock::now();
        accum += duration<float>(now - prevt).count();
        prevt = now;
        if (accum > 0.25f) accum = 0.25f;

        if (model.screen == app::Screen::Play && sm) {
            const float aspect = static_cast<float>(curW) / static_cast<float>(curH);
            const float kSens = 0.0010f + 0.0030f * (static_cast<float>(model.settings.mouse_pct) / 100.0f);
            POINT cur; GetCursorPos(&cur);
            float look_yaw = static_cast<float>(cur.x - anchor.x) * kSens;
            float look_pitch = -static_cast<float>(cur.y - anchor.y) * kSens;
            anchor = recenter();
            contracts::InputCommand in{};
            if (GetAsyncKeyState('W') & 0x8000) in.move_z += 1.0f;
            if (GetAsyncKeyState('S') & 0x8000) in.move_z -= 1.0f;
            if (GetAsyncKeyState('D') & 0x8000) in.move_x += 1.0f;
            if (GetAsyncKeyState('A') & 0x8000) in.move_x -= 1.0f;
            if (GetAsyncKeyState(VK_SPACE) & 0x8000) in.buttons |= contracts::kButtonJump;
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) in.buttons |= contracts::kButtonRun;
            if (pad.connected) {  // M16: gamepad adds to keyboard/mouse this tick
                const contracts::InputCommand gp = app::gamepad_to_input(pad, kSens * 18.0f);
                in.move_x += gp.move_x; in.move_z += gp.move_z;
                look_yaw += gp.look_yaw; look_pitch += gp.look_pitch;
                in.buttons |= gp.buttons;
            }
            bool firstTick = true;
            while (accum >= tickDt) {
                const contracts::ChunkKey here = contracts::chunk_key_at(0, s.wanderer.pos.x, s.wanderer.pos.z);
                if (here != cached) rebuild(here);
                contracts::InputCommand step = in;
                if (firstTick) { step.look_yaw = look_yaw; step.look_pitch = look_pitch; firstTick = false; }
                tick(s, step, collision);
                app::shoggoth_step(shog, s.wanderer.pos, model.seed, (s.tick % 8u) == 0u);  // M20b: the hunt
                accum -= tickDt;
            }
            // M21b: feed the live brain (off-thread) while in Play; apply returned intent.
            if (brain) {
                if (now - last_brain >= brain_interval) {
                    brain->submit(app::build_shoggoth_summary(shog, s.wanderer.pos, s.tick));
                    last_brain = now;
                }
                for (const app::ShoggothIntent& it : brain->poll()) { shog.intent = it; ++brain_intents; }
            }
            if (audioOn) {
                const uint64_t steps = footstep_count(s);
                eng.post(audio_listener(s), 1.2f, static_cast<uint32_t>(steps - prevSteps));
                prevSteps = steps;
            }
            const contracts::ChunkKey center = contracts::chunk_key_at(0, s.wanderer.pos.x, s.wanderer.pos.z);
            sm->update(center);
            contracts::CameraPose cam = wanderer_camera(s, aspect);
            apply_head_bob(cam, s);  // M18 head-bob (view-only)
            if (model.settings.rt) {
                // M19 ray-traced path: DXR at 2/3 internal res -> present (upscaled).
                const uint32_t rw = (curW * 2u) / 3u, rh = (curH * 2u) / 3u;
                if (!dxr || dxrW != rw || dxrH != rh) {
                    dxr = std::make_unique<br::render_dxr::DxrRenderer>();
                    if (dxr->init(rw, rh)) { dxrW = rw; dxrH = rh; dxrCenter = contracts::ChunkKey{0, static_cast<int64_t>(1) << 40, 0}; }
                    else { model.settings.rt = 0; dxr.reset(); }  // DXR unavailable -> raster fallback
                }
                if (dxr) {
                    if (center != dxrCenter) { dxr->build_scene(sm->resident()); dxrCenter = center; lastYaw = 1e9f; }
                    const bool moved = (cam.pos[0] != lastCamX || cam.pos[1] != lastCamY || cam.pos[2] != lastCamZ ||
                                        cam.yaw != lastYaw || cam.pitch != lastPitch);
                    lastCamX = cam.pos[0]; lastCamY = cam.pos[1]; lastCamZ = cam.pos[2]; lastYaw = cam.yaw; lastPitch = cam.pitch;
                    dxr->render_pt_frame(cam, 4u, static_cast<uint32_t>(texSeed) + static_cast<uint32_t>(frames), moved);
                    std::vector<uint8_t> rt;
                    if (dxr->readback(rt)) renderer.present_overlay_windowed(rt.data(), dxrW, dxrH);
                }
            }
            if (!model.settings.rt) {
                app::build_shoggoth_mesh(shogBody, shog.pos, shog.writhe, 1.4f);  // M20b in-world body
                std::vector<contracts::ResidentChunk> withShog = sm->resident();
                withShog.push_back(contracts::ResidentChunk{contracts::ChunkKey{9999, static_cast<int64_t>(frames), 0}, shogBody.data(), static_cast<uint32_t>(shogBody.size())});
                uint32_t drawn = 0;
                if (!renderer.render_chunks_windowed(cam, withShog, 8u, s.tick, &drawn)) {
                    std::fprintf(stderr, "render: %s\n", renderer.last_error().c_str()); ShowCursor(TRUE); return 1;
                }
            }
        } else {
            accum = 0.0f;  // sim time doesn't advance in menus
            std::vector<uint8_t> ovl;
            app::build_menu_overlay(ovl, curW, curH, model);
            if (!renderer.present_overlay_windowed(ovl.data(), curW, curH)) {
                std::fprintf(stderr, "overlay: %s\n", renderer.last_error().c_str()); ShowCursor(TRUE); return 1;
            }
            if (model.screen == app::Screen::Quit) running = false;
        }
        ++frames;
    }
    if (cursorHidden) ShowCursor(TRUE);

    // M16: persist the current settings + window state for next launch.
    cfg.master = model.settings.master_pct; cfg.sfx = model.settings.sfx_pct;
    cfg.mouse = model.settings.mouse_pct; cfg.director = model.settings.director;
    cfg.renderer = model.settings.rt;  // M19 persist the ray-tracing toggle
    cfg.fullscreen = isFull ? 1 : 0; cfg.seed = model.seed;
    if (!isFull) { cfg.width = static_cast<int>(curW); cfg.height = static_cast<int>(curH); }
    app::save_config(cfgPath, app::sanitize(cfg));

    const uint32_t dbg = renderer.debug_error_count();
    const unsigned long long underruns = static_cast<unsigned long long>(audioOn ? eng.underruns() : 0ull);
    eng.stop();
    std::printf("game_seed: %llu\n", static_cast<unsigned long long>(model.seed));
    std::printf("frames: %llu\n", static_cast<unsigned long long>(frames));
    std::printf("final_screen: %d\n", static_cast<int>(model.screen));
    std::printf("audio_underruns: %llu\n", underruns);
    const unsigned long long brain_req = brain ? brain->requests() : 0ull;
    brain.reset();  // M21b: join the worker thread before exit
    std::printf("brain_intents: %llu\n", static_cast<unsigned long long>(brain_intents));
    std::printf("brain_requests: %llu\n", brain_req);
    std::printf("debug_error_count: %u\n", dbg);
    return dbg == 0 ? 0 : 3;
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

// ----- M11 Director eval: schema-valid rate + p95 latency over N scenarios -----
// Gate 1 (schema-valid + lint) and Gate 3 (p95 directive latency < 5 s), measured
// over a real N of varied WandererSummaries against the live KEEL tier.
int run_director_eval(const Options& o) {
    std::string host = "127.0.0.1"; int port = 7071;
    if (!o.director_url.empty()) {
        const size_t colon = o.director_url.rfind(':');
        if (colon != std::string::npos) { host = o.director_url.substr(0, colon); port = std::atoi(o.director_url.c_str() + colon + 1); }
        else host = o.director_url;
    }
    const uint32_t N = (o.eval_count > 0) ? o.eval_count : 100u;
    static const char* kBiomes[5] = { "classic yellow", "cubicle farm", "pipe corridors", "parking garage", "poolrooms" };

    uint32_t valid = 0, unreachable = 0;
    std::vector<double> lat;
    std::vector<std::string> samples;
    for (uint32_t i = 0; i < N; ++i) {
        contracts::WandererSummary sum{};
        sum.tick = 1000u + i * 517u;
        sum.world_seed = o.seed + i;
        sum.level = (i % 9u == 0u) ? -1 : 0;
        sum.chunk_cx = static_cast<int64_t>(i * 13u % 60u) - 30;
        sum.chunk_cz = static_cast<int64_t>(i * 29u % 60u) - 30;
        sum.biome = static_cast<int32_t>(i % 5u);
        sum.distance_m = 80.0f + static_cast<float>(i * 37u % 2400u);
        sum.dwell_seconds = static_cast<float>(i * 11u % 180u);
        sum.route_loops = i % 6u;
        sum.location_hash = static_cast<uint64_t>(sum.chunk_cx) * 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(sum.chunk_cz);

        const std::string prompt = br::director::render_prompt(sum);
        const auto t0 = std::chrono::steady_clock::now();
        const br::director::KeelResponse resp = br::director::keel_complete(host, port, prompt, 20000);
        const auto t1 = std::chrono::steady_clock::now();
        if (!resp.ok) { ++unreachable; continue; }
        lat.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        const br::director::DirectiveResult vr = br::director::validate_directive(resp.content);
        if (vr.ok) {
            ++valid;
            if (samples.size() < 6 && vr.directive.caption[0] != '\0')
                samples.push_back(std::string(kBiomes[sum.biome % 5]) + " -> " + vr.directive.caption);
        }
    }
    const uint32_t answered = N - unreachable;
    std::sort(lat.begin(), lat.end());
    auto pct = [&](double p) -> double {
        if (lat.empty()) return 0.0;
        return lat[static_cast<size_t>(p * static_cast<double>(lat.size() - 1))];
    };

    std::printf("eval_count: %u\n", N);
    std::printf("answered: %u\n", answered);
    std::printf("unreachable: %u\n", unreachable);
    std::printf("schema_valid: %u\n", valid);
    std::printf("schema_valid_rate: %.4f\n", answered > 0 ? static_cast<double>(valid) / answered : 0.0);
    std::printf("latency_p50_ms: %.1f\n", pct(0.50));
    std::printf("latency_p95_ms: %.1f\n", pct(0.95));
    std::printf("latency_max_ms: %.1f\n", lat.empty() ? 0.0 : lat.back());
    for (size_t i = 0; i < samples.size(); ++i) std::printf("sample_%zu: %s\n", i, samples[i].c_str());
    return (unreachable == 0) ? 0 : 1;
}

// ----- M11 Director helpers (shared by the soak + record/replay) --------------
namespace {

// Deterministic 64-bit fold (integer-only -> reproducible across record + replay).
uint64_t fold_u64(uint64_t h, uint64_t x) {
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    h ^= x + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}
uint64_t fold_bytes(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; std::memcpy(&w, b + i, 8); h = fold_u64(h, w); }
    if (i < n) { uint64_t tail = 0; for (size_t j = 0; i < n; ++i, ++j) tail |= static_cast<uint64_t>(b[i]) << (8 * j); h = fold_u64(h, tail); }
    return h;
}

void parse_host_port(const std::string& url, std::string& host, int& port) {
    host = "127.0.0.1"; port = 7071;
    if (url.empty()) return;
    std::string u = url;
    const size_t scheme = u.find("://");  // strip a leading http:// or https:// if present
    if (scheme != std::string::npos) u = u.substr(scheme + 3);
    const size_t slash = u.find('/');     // strip any trailing path
    if (slash != std::string::npos) u = u.substr(0, slash);
    const size_t colon = u.rfind(':');
    if (colon != std::string::npos) { host = u.substr(0, colon); port = std::atoi(u.c_str() + colon + 1); }
    else { host = u; }
}

// Deterministic WandererSummary from the (already-hashed) WorldState (the sim is the
// oracle). The summary feeds only the Director prompt -- it never re-enters the sim.
contracts::WandererSummary build_summary(const br::core::WorldState& s, uint64_t seed) {
    contracts::WandererSummary sum{};
    sum.tick = s.tick;
    sum.world_seed = seed;
    sum.level = 0;
    const contracts::ChunkKey k = contracts::chunk_key_at(0, s.wanderer.pos.x, s.wanderer.pos.z);
    sum.chunk_cx = k.cx; sum.chunk_cz = k.cz;
    sum.biome = static_cast<int32_t>(br::gen::biome_at(seed, 0, k.cx, k.cz));
    sum.distance_m = s.odometer;
    sum.dwell_seconds = 0.0f;
    sum.route_loops = 0;
    sum.location_hash = static_cast<uint64_t>(k.cx) * 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(k.cz);
    return sum;
}

}  // namespace

// ----- M12 noclip intro: the fall from a mundane room into Level 0 (deterministic) -
// The iconic "noclip into the Backrooms": stand in a mundane room, the floor gives
// way, free-fall, then land in the Level-0 maze. Pure scripted sim (seeded core ticks
// + collision phases) -> reproducible; the visual fall is the windowed experience.
int run_intro(const Options& o) {
    using namespace br::core;
    WorldState s(o.seed);
    const float roomY = 8.0f;  // mundane-room floor, +8 m above Level 0
    s.wanderer.pos = Vec3{ 2.0f, roomY + kWandererHalfHeight + 0.02f, 2.0f };  // above the proven-open spawn cell

    static const std::vector<Aabb> kEmpty;  // noclip phase: nothing to stand on
    const std::vector<Aabb> roomFloor = { Aabb{ {-4.0f, roomY - 1.0f, -4.0f}, {8.0f, roomY, 8.0f} } };
    std::vector<Aabb> level0 = { Aabb{ {-1.0e6f, -1.0f, -1.0e6f}, {1.0e6f, 0.0f, 1.0e6f} } };  // Level-0 ground floor
    for (int64_t dx = -1; dx <= 1; ++dx)
        for (int64_t dz = -1; dz <= 1; ++dz) {
            const contracts::ChunkData cd = contracts::GenerateChunk(o.seed, contracts::ChunkKey{ 0, dx, dz });
            for (const auto& b : cd.collision) level0.push_back(Aabb{ {b.mn[0], b.mn[1], b.mn[2]}, {b.mx[0], b.mx[1], b.mx[2]} });
        }

    const uint64_t kIdle = 120;  // stand in the room (~1 s) before the floor gives way
    const uint64_t total = (o.ticks > 0) ? o.ticks : 900u;
    const float landY = kWandererHalfHeight + 0.5f;
    contracts::InputCommand idle{};
    bool noclipped = false, landed = false;
    for (uint64_t t = 0; t < total; ++t) {
        const std::vector<Aabb>* coll;
        if (t < kIdle) coll = &roomFloor;                                         // phase 1: a mundane room
        else if (s.wanderer.pos.y > landY) { coll = &kEmpty; noclipped = true; }  // phase 2: noclip -> free fall
        else { coll = &level0; landed = true; }                                   // phase 3: land in the backrooms
        tick(s, idle, *coll);
    }

    std::printf("intro_seed: %llu\n", static_cast<unsigned long long>(o.seed));
    std::printf("ticks: %llu\n", static_cast<unsigned long long>(total));
    std::printf("noclipped: %d\n", noclipped ? 1 : 0);
    std::printf("landed: %d\n", landed ? 1 : 0);
    std::printf("final_y: %.3f\n", static_cast<double>(s.wanderer.pos.y));
    std::printf("final_hash: %016llx\n", static_cast<unsigned long long>(world_state_hash(s)));
    const bool ok = noclipped && landed && (s.wanderer.pos.y < landY) && (s.wanderer.pos.y > 0.0f);
    return ok ? 0 : 4;
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

    // Optional async Director (enhancement-only, INV-6): generation runs on its own
    // thread so it can't touch frame time (the async-isolation invariant, Gate 2).
    // Off by default; --director enables, --no-director force-disables (kill switch).
    std::unique_ptr<br::director::DirectorHost> director;
    if (o.director && !o.no_director) {
        std::string dh; int dp = 7071;
        parse_host_port(o.director_url, dh, dp);
        director = std::make_unique<br::director::DirectorHost>(dh, dp);
    }
    std::map<uint64_t, contracts::Directive> note_cache;  // Wanderer Notes per location hash
    uint64_t dir_applied = 0;
    std::string last_caption;
    const auto dir_interval = seconds(static_cast<long long>(o.director_interval_s > 0 ? o.director_interval_s : 15u));  // ambient (wall-clock) pacing
    auto last_submit_wall = start - dir_interval;  // fire the first summary immediately

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
        if (director) {
            const auto now = steady_clock::now();
            if (now - last_submit_wall >= dir_interval) { director->submit(build_summary(s, o.seed)); last_submit_wall = now; }
            for (const contracts::Directive& d : director->poll()) {
                ++dir_applied;
                if (d.caption[0] != '\0') last_caption = d.caption;   // The Voice: the latest line
                if (d.kind == contracts::DirectiveKind::WandererNote) {
                    note_cache[build_summary(s, o.seed).location_hash] = d;  // notes cached per location
                }
            }
        }
        ++frame;
    }
    if (csvOpen) csv.close();
    uint64_t dir_requests = 0, dir_produced = 0;
    if (director) { dir_requests = director->requests(); dir_produced = director->produced(); director.reset(); }  // join the worker

    const uint32_t dbg = renderer.debug_error_count();
    std::printf("soak_seed: %llu\n", static_cast<unsigned long long>(o.seed));
    std::printf("director: %d\n", (o.director && !o.no_director) ? 1 : 0);
    std::printf("director_requests: %llu\n", static_cast<unsigned long long>(dir_requests));
    std::printf("director_produced: %llu\n", static_cast<unsigned long long>(dir_produced));
    std::printf("director_applied: %llu\n", static_cast<unsigned long long>(dir_applied));
    std::printf("notes_cached: %llu\n", static_cast<unsigned long long>(note_cache.size()));
    if (!last_caption.empty()) std::printf("director_last_line: %s\n", last_caption.c_str());
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

// ----- M11 Director record/replay: prove Gate 4 (replay bit-identical, model off) -
// (fold/summary helpers are defined above run_soak so the soak can share them.)

// Record: walk the maze with the Director ON (live KEEL), capturing each validated
// directive into the event log at its sim tick. Prints the combined run hash =
// per-tick world_state_hash folded with the applied director-event bytes.
int run_director_record(const Options& o) {
    std::string host; int port; parse_host_port(o.director_url, host, port);
    const uint64_t N = (o.ticks > 0) ? o.ticks : 600u;
    const uint64_t kSubmitEvery = 120u;  // ~1 s of sim between summaries

    MazeWalker w(o.seed);
    std::vector<contracts::DirectorEvent> events;
    uint64_t H = 1469598103934665603ull;  // FNV-1a offset basis
    uint64_t submits = 0;
    for (uint64_t t = 0; t < N; ++t) {
        w.step();
        H = fold_u64(H, br::core::world_state_hash(w.s));
        if (w.s.tick % kSubmitEvery == 0) {
            ++submits;
            const contracts::WandererSummary sum = build_summary(w.s, o.seed);
            const std::optional<contracts::Directive> d = br::director::request_directive(host, port, sum);
            if (d) {
                contracts::DirectorEvent ev{};
                ev.effective_tick = w.s.tick;
                ev.directive = *d;
                events.push_back(ev);
                H = fold_bytes(H, &events.back(), sizeof(contracts::DirectorEvent));  // fold the exact bytes written
            }
        }
    }
    if (o.director_log.empty()) { std::fprintf(stderr, "record: --director-log <path> required\n"); return 2; }
    if (!br::director::write_director_log(o.director_log, o.seed, N, events)) {
        std::fprintf(stderr, "record: failed to write %s\n", o.director_log.c_str()); return 1;
    }
    std::printf("seed: %llu\n", static_cast<unsigned long long>(o.seed));
    std::printf("ticks: %llu\n", static_cast<unsigned long long>(N));
    std::printf("submits: %llu\n", static_cast<unsigned long long>(submits));
    std::printf("director_events: %llu\n", static_cast<unsigned long long>(events.size()));
    std::printf("combined_hash: %016llx\n", static_cast<unsigned long long>(H));
    return 0;
}

// Replay: re-walk the SAME maze (seed + tick count from the log), applying the
// recorded directives at their logged ticks with KEEL never contacted. The combined
// run hash must equal the recorded run's -> the model lives only in the log (Gate 4).
int run_director_replay(const Options& o) {
    if (o.director_log.empty()) { std::fprintf(stderr, "replay: --director-log <path> required\n"); return 2; }
    uint64_t seed = 0, N = 0;
    std::vector<contracts::DirectorEvent> events;
    if (!br::director::read_director_log(o.director_log, seed, N, events)) {
        std::fprintf(stderr, "replay: failed to read %s\n", o.director_log.c_str()); return 1;
    }
    MazeWalker w(seed);
    uint64_t H = 1469598103934665603ull;
    size_t i = 0;
    for (uint64_t t = 0; t < N; ++t) {
        w.step();
        H = fold_u64(H, br::core::world_state_hash(w.s));
        while (i < events.size() && events[i].effective_tick == w.s.tick) {
            H = fold_bytes(H, &events[i], sizeof(contracts::DirectorEvent));
            ++i;
        }
    }
    std::printf("seed: %llu\n", static_cast<unsigned long long>(seed));
    std::printf("ticks: %llu\n", static_cast<unsigned long long>(N));
    std::printf("director_events: %llu\n", static_cast<unsigned long long>(events.size()));
    std::printf("events_applied: %llu\n", static_cast<unsigned long long>(i));
    std::printf("combined_hash: %016llx\n", static_cast<unsigned long long>(H));
    return 0;
}

// ----- M21 Shoggoth brain record/replay: prove the sacred gate (model off) -----
// Record: a wanderer walks the maze; the Shoggoth hunts with its KEEL brain ON. Every
// few seconds it asks KEEL (with the shoggoth system prompt) for an intent, validates
// it, applies it, and logs it at its tick. Prints the combined hash (shoggoth_hash
// folded per tick + the exact event bytes) and writes the log.
int run_shoggoth_record(const Options& o) {
    std::string host; int port; parse_host_port(o.director_url, host, port);
    const uint64_t N = (o.ticks > 0) ? o.ticks : 1800u;
    const uint64_t kBrainEvery = 240u;  // ~2 s of sim between thoughts

    MazeWalker w(o.seed);
    app::Shoggoth sh;
    sh.pos = w.s.wanderer.pos; sh.pos.x += 24.0f;
    std::vector<app::ShoggothEvent> events;
    uint64_t H = 1469598103934665603ull;
    uint64_t thoughts = 0, valid = 0;
    for (uint64_t t = 0; t < N; ++t) {
        w.step();
        if (t % kBrainEvery == 0) {
            ++thoughts;
            const app::ShoggothSummary sum = app::build_shoggoth_summary(sh, w.s.wanderer.pos, t);
            const br::director::KeelResponse resp = br::director::keel_complete(host, port, app::render_shoggoth_prompt(sum), 15000);
            bool ok = false;
            const app::ShoggothIntent intent = resp.ok ? app::parse_shoggoth_intent(resp.content, ok) : app::ShoggothIntent{};
            if (resp.ok) {
                if (ok) {
                    ++valid;
                    sh.intent = intent;
                    events.push_back(app::ShoggothEvent{t, static_cast<int32_t>(intent.action), intent.aggression});
                    H = fold_bytes(H, &events.back(), sizeof(app::ShoggothEvent));
                }
            }
        }
        app::shoggoth_step(sh, w.s.wanderer.pos, o.seed, (t % 8u) == 0u);
        H = fold_u64(H, app::shoggoth_hash(sh));
    }
    if (!o.director_log.empty()) {
        if (!app::write_shoggoth_log(o.director_log, o.seed, N, events)) { std::fprintf(stderr, "record: failed to write %s\n", o.director_log.c_str()); return 1; }
    }
    std::printf("seed: %llu\n", static_cast<unsigned long long>(o.seed));
    std::printf("ticks: %llu\n", static_cast<unsigned long long>(N));
    std::printf("thoughts: %llu\n", static_cast<unsigned long long>(thoughts));
    std::printf("valid_intents: %llu\n", static_cast<unsigned long long>(valid));
    std::printf("combined_hash: %016llx\n", static_cast<unsigned long long>(H));
    return 0;
}

// Replay: re-run the SAME chase (seed + ticks from the log), applying the recorded
// intents at their ticks with KEEL never contacted. The combined hash must equal the
// record's -> the shoggoth's "brain" lives only in the log (the sacred gate).
int run_shoggoth_replay(const Options& o) {
    if (o.director_log.empty()) { std::fprintf(stderr, "replay: --director-log <path> required\n"); return 2; }
    uint64_t seed = 0, N = 0;
    std::vector<app::ShoggothEvent> events;
    if (!app::read_shoggoth_log(o.director_log, seed, N, events)) { std::fprintf(stderr, "replay: cannot read %s\n", o.director_log.c_str()); return 2; }

    MazeWalker w(seed);
    app::Shoggoth sh;
    sh.pos = w.s.wanderer.pos; sh.pos.x += 24.0f;
    uint64_t H = 1469598103934665603ull;
    size_t ei = 0;
    for (uint64_t t = 0; t < N; ++t) {
        w.step();
        while (ei < events.size() && events[ei].effective_tick == t) {
            sh.intent.action = static_cast<app::ShoggothAction>(events[ei].action);
            sh.intent.aggression = events[ei].aggression;
            H = fold_bytes(H, &events[ei], sizeof(app::ShoggothEvent));
            ++ei;
        }
        app::shoggoth_step(sh, w.s.wanderer.pos, seed, (t % 8u) == 0u);
        H = fold_u64(H, app::shoggoth_hash(sh));
    }
    std::printf("seed: %llu\n", static_cast<unsigned long long>(seed));
    std::printf("ticks: %llu\n", static_cast<unsigned long long>(N));
    std::printf("replay_events: %llu\n", static_cast<unsigned long long>(events.size()));
    std::printf("combined_hash: %016llx\n", static_cast<unsigned long long>(H));
    return 0;
}

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

// ----- M14 real-time audio OUTPUT (to the speakers, via miniaudio) ------------
// Same deterministic sim drive as --audiosoak, but the AudioEngine renders into a
// real miniaudio playback device through a lock-free ring (--null forces the
// hardware-free null backend — the gated, CI-safe path). Reports whether the device
// opened, its backend, blocks rendered, and underruns (must be 0). Audible payoff
// is wired into --play; this mode is the headless gate for the output path.
int run_audiodev(const Options& o) {
    using namespace std::chrono;
    using namespace br::core;
    WorldState s(o.seed);
    s.wanderer.pos = Vec3{16.0f, kWandererHalfHeight + 0.02f, 16.0f};
    const std::vector<Aabb>& ground = open_ground();

    audio::AudioEngine eng(o.seed, contracts::kAudioSampleRate);
    eng.set_master_volume(o.master);
    eng.set_sfx_volume(o.sfx);
    if (!eng.start_device(o.null_backend)) {
        std::printf("device_open: 0\n");
        std::printf("backend: none\n");
        // A real device may be absent on a headless host; that is a soft outcome.
        // The null backend, exercised by the gate, always opens — so a failure
        // there is a hard error (exit 6).
        return o.null_backend ? 6 : 0;
    }

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
        in.look_yaw = 0.00008f;  // gentle loop -> stays local
        tick(s, in, ground);
        ++nticks;
        const uint64_t steps = footstep_count(s);
        const uint32_t newf = static_cast<uint32_t>(steps - prev_steps);
        prev_steps = steps;
        footsteps += newf;
        eng.post(audio_listener(s), 1.2f, newf);
    }

    const unsigned long long underruns = static_cast<unsigned long long>(eng.underruns());
    const unsigned long long blocks = static_cast<unsigned long long>(eng.blocks_rendered());
    std::string backend = eng.backend();
    eng.stop();

    std::printf("device_open: 1\n");
    std::printf("backend: %s\n", backend.c_str());
    std::printf("ticks: %llu\n", static_cast<unsigned long long>(nticks));
    std::printf("footsteps: %llu\n", static_cast<unsigned long long>(footsteps));
    std::printf("audio_blocks: %llu\n", blocks);
    std::printf("underruns: %llu\n", underruns);
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

// ----- M20 headless Shoggoth chase: determinism + maze navigation ------------
// A wanderer walks the maze (the M11 MazeWalker); the Shoggoth hunts it. Reports the
// deterministic shoggoth fingerprint (gate runs twice -> identical), how far it
// navigated (not stuck), the closest it got, and whether it engaged the hunt.
int run_shoggoth(const Options& o) {
    MazeWalker w(o.seed);
    app::Shoggoth sh;
    sh.pos = w.s.wanderer.pos;
    sh.pos.x += 24.0f;  // spawn ~6 cells away, within hunt range
    const br::core::Vec3 start = sh.pos;
    const uint64_t N = (o.ticks > 0) ? o.ticks : 2400u;

    float min_dist = 1e9f, max_moved = 0.0f;
    int ever_hunted = 0;
    std::vector<std::pair<float, float>> wtrail, strail;  // for the optional top-down map
    for (uint64_t t = 0; t < N; ++t) {
        w.step();  // the wanderer walks the maze
        app::shoggoth_step(sh, w.s.wanderer.pos, o.seed, (t % 8) == 0);
        if (sh.state != app::ShoggothState::Lurk) ever_hunted = 1;
        const float dx = sh.pos.x - w.s.wanderer.pos.x, dz = sh.pos.z - w.s.wanderer.pos.z;
        const float d = std::sqrt(dx * dx + dz * dz);
        if (d < min_dist) min_dist = d;
        const float mx = sh.pos.x - start.x, mz = sh.pos.z - start.z;
        const float moved = std::sqrt(mx * mx + mz * mz);
        if (moved > max_moved) max_moved = moved;
        if (!o.out.empty() && (t % 6u) == 0u) {
            wtrail.push_back({w.s.wanderer.pos.x, w.s.wanderer.pos.z});
            strail.push_back({sh.pos.x, sh.pos.z});
        }
    }

    // Optional CPU top-down map: the maze (walls from gen) + the wanderer trail (cyan)
    // and the shoggoth trail (red) — a visual of the hunt. Deterministic, GPU-free.
    if (!o.out.empty() && !wtrail.empty()) {
        const int W = 800, H = 800;
        float minx = start.x, maxx = start.x, minz = start.z, maxz = start.z;
        for (const auto& p : wtrail) { minx = std::min(minx, p.first); maxx = std::max(maxx, p.first); minz = std::min(minz, p.second); maxz = std::max(maxz, p.second); }
        for (const auto& p : strail) { minx = std::min(minx, p.first); maxx = std::max(maxx, p.first); minz = std::min(minz, p.second); maxz = std::max(maxz, p.second); }
        const float cx = 0.5f * (minx + maxx), cz = 0.5f * (minz + maxz);
        const float half = std::max(16.0f, 0.5f * std::max(maxx - minx, maxz - minz) + 8.0f);
        std::vector<uint8_t> img(static_cast<size_t>(W) * H * 4u, 255u);
        auto px = [&](float x) { return static_cast<int>((x - (cx - half)) / (2.0f * half) * W); };
        auto py = [&](float z) { return static_cast<int>((z - (cz - half)) / (2.0f * half) * H); };
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            const float wx = (cx - half) + (x + 0.5f) / W * 2.0f * half;
            const float wz = (cz - half) + (y + 0.5f) / H * 2.0f * half;
            int64_t gi, gj; app::world_to_cell(wx, wz, gi, gj);
            std::unordered_map<int64_t, br::gen::ChunkLayout> cache;
            const float fx = wx / br::gen::kCellSize - std::floor(wx / br::gen::kCellSize);
            const float fz = wz / br::gen::kCellSize - std::floor(wz / br::gen::kCellSize);
            uint8_t v = 26;  // open floor (dark)
            const float wallT = 0.12f;
            if ((fx < wallT && !app::maze_open(o.seed, gi, gj, 1, cache)) ||
                (fx > 1 - wallT && !app::maze_open(o.seed, gi, gj, 0, cache)) ||
                (fz < wallT && !app::maze_open(o.seed, gi, gj, 3, cache)) ||
                (fz > 1 - wallT && !app::maze_open(o.seed, gi, gj, 2, cache))) v = 90;  // wall
            uint8_t* d = &img[(static_cast<size_t>(y) * W + x) * 4];
            d[0] = v; d[1] = v; d[2] = static_cast<uint8_t>(v < 80 ? v - 6 : v); d[3] = 255;
        }
        auto dot = [&](float x, float z, uint8_t r, uint8_t g, uint8_t b, int rad) {
            const int X = px(x), Y = py(z);
            for (int dy = -rad; dy <= rad; ++dy) for (int dx2 = -rad; dx2 <= rad; ++dx2) {
                const int xx = X + dx2, yy = Y + dy;
                if (xx < 0 || yy < 0 || xx >= W || yy >= H || dx2 * dx2 + dy * dy > rad * rad) continue;
                uint8_t* d = &img[(static_cast<size_t>(yy) * W + xx) * 4];
                d[0] = r; d[1] = g; d[2] = b;
            }
        };
        for (const auto& p : wtrail) dot(p.first, p.second, 60, 200, 220, 1);   // wanderer trail (cyan)
        for (const auto& p : strail) dot(p.first, p.second, 220, 50, 40, 1);    // shoggoth trail (red)
        dot(w.s.wanderer.pos.x, w.s.wanderer.pos.z, 120, 255, 255, 5);          // wanderer (bright)
        dot(sh.pos.x, sh.pos.z, 255, 80, 60, 6);                                // shoggoth (bright)
        stbi_write_png(o.out.c_str(), W, H, 4, img.data(), W * 4);
    }
    const float fdx = sh.pos.x - w.s.wanderer.pos.x, fdz = sh.pos.z - w.s.wanderer.pos.z;
    std::printf("seed: %llu\n", static_cast<unsigned long long>(o.seed));
    std::printf("ticks: %llu\n", static_cast<unsigned long long>(N));
    std::printf("shoggoth_hash: %016llx\n", static_cast<unsigned long long>(app::shoggoth_hash(sh)));
    std::printf("min_dist: %.2f\n", static_cast<double>(min_dist));
    std::printf("final_dist: %.2f\n", static_cast<double>(std::sqrt(fdx * fdx + fdz * fdz)));
    std::printf("moved: %.1f\n", static_cast<double>(max_moved));
    std::printf("ever_hunted: %d\n", ever_hunted);
    std::printf("state: %d\n", static_cast<int>(sh.state));
    return 0;
}

// ----- M20b render the Shoggoth's procedural body to a PNG (QC + gate) --------
// Places the creature a few metres ahead of a fixed camera, injects its mesh as a
// synthetic resident chunk (collision-proof key), renders lit + reads back. The body
// is procedural (no assets); deterministic for a fixed (seed, tick).
int run_shoggoth_shot(const Options& o) {
    using namespace br::core;
    Renderer renderer;
    if (!renderer.init_headless(o.width, o.height)) { std::fprintf(stderr, "init: %s\n", renderer.last_error().c_str()); return 1; }
    renderer.set_texture_seed(o.seed);

    const float ex = 16.0f, ez = 16.0f;
    const float ey = kWandererHalfHeight + 0.02f + kEyeHeight;
    contracts::CameraPose cam{};
    cam.pos[0] = ex; cam.pos[1] = ey; cam.pos[2] = ez;
    cam.yaw = 0.0f; cam.pitch = -0.06f; cam.fov_y = 1.2217305f;
    cam.aspect = static_cast<float>(o.width) / static_cast<float>(o.height);

    br::stream::StreamManager sm(o.seed, static_cast<int>(o.radius), o.workers);
    const auto center = contracts::chunk_key_at(0, ex, ez);
    sm.update(center); sm.wait_idle(); sm.update(center);
    uint32_t drawn = 0;
    const size_t target = sm.resident_count();
    for (int w = 0; w < 400 && static_cast<size_t>(drawn) < target; ++w)
        renderer.render_chunks(cam, sm.resident(), 64u, 0u, &drawn);

    std::vector<contracts::ChunkVertex> body;
    const Vec3 shog_pos{ex, ey - 0.5f, ez + 3.4f};  // a few metres ahead, in view
    app::build_shoggoth_mesh(body, shog_pos, static_cast<float>(o.ticks) * 0.05f, 1.8f);
    std::vector<contracts::ResidentChunk> withCreature = sm.resident();  // by value (single call)
    const contracts::ResidentChunk creature{contracts::ChunkKey{9999, 0, 0}, body.data(), static_cast<uint32_t>(body.size())};
    withCreature.push_back(creature);

    if (!renderer.render_chunks(cam, withCreature, 256u, o.ticks, &drawn)) { std::fprintf(stderr, "render: %s\n", renderer.last_error().c_str()); return 1; }
    FrameImage img;
    if (!renderer.readback(img)) { std::fprintf(stderr, "readback: %s\n", renderer.last_error().c_str()); return 1; }
    const std::string out = o.out.empty() ? std::string("runs/shoggoth.png") : o.out;
    stbi_write_png(out.c_str(), static_cast<int>(img.width), static_cast<int>(img.height), 4, img.rgba.data(), static_cast<int>(img.width) * 4);

    std::printf("body_verts: %llu\n", static_cast<unsigned long long>(body.size()));
    std::printf("drawn: %u\n", drawn);
    std::printf("debug_error_count: %u\n", renderer.debug_error_count());
    return renderer.debug_error_count() == 0 ? 0 : 3;
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
    if (o.audiodev)   return run_audiodev(o);
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
    if (o.director_eval)  return run_director_eval(o);
    if (o.director_record) return run_director_record(o);
    if (o.director_replay) return run_director_replay(o);
    if (o.intro)      return run_intro(o);
    if (o.play)       return run_play(o);
    if (o.menu_shot)  return run_menu_shot(o);
    if (o.menu_smoke) return run_menu_smoke(o);
    if (o.config_check) return run_config_check(o);
    if (o.resize_smoke) return run_resize_smoke(o);
    if (o.credits)    return run_credits(o);
    if (o.shoggoth)   return run_shoggoth(o);
    if (o.shoggoth_shot) return run_shoggoth_shot(o);
    if (o.shoggoth_record) return run_shoggoth_record(o);
    if (o.shoggoth_replay) return run_shoggoth_replay(o);
    if (o.game)       return run_game(o);
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
