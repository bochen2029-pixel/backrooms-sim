// app/main.cpp — composition root + CLI. M1: drives the D3D12 renderer in
// headless (offscreen -> PNG) or windowed (swapchain) mode and reports the
// D3D12 debug-layer error count + memory usage for the gates.
//
//   backrooms --headless [--frames N] [--out path.png] [--width W] [--height H]
//   backrooms --window   [--frames N | --seconds S] [--width W] [--height H]
//   backrooms --version
//
// Exit codes: 0 ok, 1 init/render failure, 2 usage error, 3 debug-layer messages.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <stb_image_write.h>

#include "core/version.h"
#include "render_d3d12/renderer.h"

namespace {

struct Options {
    bool headless = false;
    bool windowed = false;
    bool version = false;
    uint32_t frames = 1;
    uint32_t seconds = 0;
    uint32_t width = 320;
    uint32_t height = 180;
    std::string out;
};

int usage() {
    std::fprintf(stderr,
        "usage: backrooms [--headless|--window] [--frames N | --seconds S]\n"
        "                 [--out file.png] [--width W] [--height H] [--version]\n");
    return 2;
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&](uint32_t& dst) {
            if (i + 1 >= argc) return false;
            dst = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            return true;
        };
        if (std::strcmp(a, "--headless") == 0) o.headless = true;
        else if (std::strcmp(a, "--window") == 0) o.windowed = true;
        else if (std::strcmp(a, "--version") == 0) o.version = true;
        else if (std::strcmp(a, "--frames") == 0) { if (!next(o.frames)) return false; }
        else if (std::strcmp(a, "--seconds") == 0) { if (!next(o.seconds)) return false; }
        else if (std::strcmp(a, "--width") == 0) { if (!next(o.width)) return false; }
        else if (std::strcmp(a, "--height") == 0) { if (!next(o.height)) return false; }
        else if (std::strcmp(a, "--out") == 0) { if (i + 1 >= argc) return false; o.out = argv[++i]; }
        else return false;
    }
    if (o.width == 0 || o.height == 0) return false;
    return true;
}

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
    HWND hwnd = CreateWindowExW(
        0, kClass, L"Backrooms Sim", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd) ShowWindow(hwnd, SW_SHOWNOACTIVATE);  // don't steal focus
    return hwnd;
}

void pump_messages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

int run_render(const Options& o) {
    using br::render_d3d12::Renderer;
    using br::render_d3d12::FrameImage;

    Renderer renderer;
    HWND hwnd = nullptr;
    if (o.windowed) {
        hwnd = create_window(o.width, o.height);
        if (!hwnd) { std::fprintf(stderr, "window creation failed\n"); return 1; }
        if (!renderer.init_windowed(hwnd, o.width, o.height)) {
            std::fprintf(stderr, "renderer init failed: %s\n", renderer.last_error().c_str());
            return 1;
        }
    } else {
        if (!renderer.init_headless(o.width, o.height)) {
            std::fprintf(stderr, "renderer init failed: %s\n", renderer.last_error().c_str());
            return 1;
        }
    }

    const uint64_t mem_start = renderer.process_private_bytes();
    uint64_t rendered = 0;
    if (o.seconds > 0) {
        const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(o.seconds) * 1000ULL;
        while (GetTickCount64() < end) {
            if (o.windowed) pump_messages();
            if (!renderer.render_clear_frame()) {
                std::fprintf(stderr, "render failed: %s\n", renderer.last_error().c_str());
                return 1;
            }
            ++rendered;
        }
    } else {
        for (uint32_t i = 0; i < o.frames; ++i) {
            if (o.windowed) pump_messages();
            if (!renderer.render_clear_frame()) {
                std::fprintf(stderr, "render failed: %s\n", renderer.last_error().c_str());
                return 1;
            }
            ++rendered;
        }
    }
    const uint64_t mem_end = renderer.process_private_bytes();

    if (!o.windowed && !o.out.empty()) {
        FrameImage img;
        if (!renderer.readback(img)) {
            std::fprintf(stderr, "readback failed: %s\n", renderer.last_error().c_str());
            return 1;
        }
        if (stbi_write_png(o.out.c_str(), static_cast<int>(img.width),
                           static_cast<int>(img.height), 4, img.rgba.data(),
                           static_cast<int>(img.width) * 4) == 0) {
            std::fprintf(stderr, "PNG write failed: %s\n", o.out.c_str());
            return 1;
        }
    }

    const uint32_t dbg = renderer.debug_error_count();
    std::printf("frames: %llu\n", static_cast<unsigned long long>(rendered));
    std::printf("mem_start_bytes: %llu\n", static_cast<unsigned long long>(mem_start));
    std::printf("mem_end_bytes: %llu\n", static_cast<unsigned long long>(mem_end));
    std::printf("mem_delta_bytes: %lld\n",
                static_cast<long long>(static_cast<int64_t>(mem_end) - static_cast<int64_t>(mem_start)));
    std::printf("debug_error_count: %u\n", dbg);
    return dbg == 0 ? 0 : 3;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, o)) return usage();

    if (o.version) {
        std::printf("%s\n", br::core::core_version());
        return 0;
    }
    if (o.headless || o.windowed) {
        return run_render(o);
    }

    // No render mode requested: print the build banner (M0 behaviour).
    std::printf("Backrooms Sim v%s\n", br::core::core_version());
    std::printf("usage: backrooms --headless --out frame.png  (see --version, --window)\n");
    return 0;
}
