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
#include "render_d3d12/renderer.h"

namespace contracts = br::contracts;
using br::render_d3d12::Renderer;
using br::render_d3d12::FrameImage;

namespace {

struct Options {
    bool headless = false, windowed = false, scene = false, sim = false, version = false;
    uint32_t frames = 1, seconds = 0, width = 320, height = 180, ticks = 0;
    uint64_t seed = 1u;
    std::string out, record, replay, hashlog;
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
        else if (std::strcmp(a, "--version") == 0) o.version = true;
        else if (std::strcmp(a, "--frames") == 0) { if (!u32(o.frames)) return false; }
        else if (std::strcmp(a, "--seconds") == 0) { if (!u32(o.seconds)) return false; }
        else if (std::strcmp(a, "--width") == 0) { if (!u32(o.width)) return false; }
        else if (std::strcmp(a, "--height") == 0) { if (!u32(o.height)) return false; }
        else if (std::strcmp(a, "--ticks") == 0) { if (!u32(o.ticks)) return false; }
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

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, o)) return usage();

    if (o.version) { std::printf("%s\n", br::core::core_version()); return 0; }
    if (o.sim)     return run_sim(o);
    if (o.scene)   return run_scene(o);
    if (o.headless || o.windowed) return run_clear(o);

    std::printf("Backrooms Sim v%s\n", br::core::core_version());
    std::printf("modes: --headless --scene --sim --window  (see --version)\n");
    return 0;
}
