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
#include <mmsystem.h>   // PlaySound -> the Director's procedural PA voice (winmm)
#include <dxgi.h>       // DXGI adapter VRAM query -> the LLM model-tier auto-select (ADR-076)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
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
#include "keel_broker.h"
#include "shoggoth_brain_host.h"
#include "shoggoth_vision.h"
#include "shoggoth_vision_host.h"
#include "flares.h"
#include "ladder.h"
#include "director_vision.h"
#include "mic_capture.h"
#include "director_chat.h"
#include "shoggoth_hearing.h"
#include "tts.h"
#include "base64.h"

namespace contracts = br::contracts;
namespace audio = br::audio;
namespace app = br::app;
using br::render_d3d12::Renderer;
using br::render_d3d12::FrameImage;

// D3D12 Agility SDK redist (RELEASE/bundle only): the exe exports these so the OS d3d12.dll loads the BUNDLED
// D3D12Core.dll + d3d12SDKLayers.dll from ".\D3D12\". That is what makes ADR-077's forced validation layer
// available on a CLEAN end-user Win11 that lacks the "Graphics Tools" optional feature -- without it, RT crashes
// there (the non-validated-device + windowed-FLIP fault). The DEBUG build omits these (no .\D3D12\ beside
// build\bin), so the gates use the OS D3D12 exactly as before. [ADR-081]
#ifdef BR_RELEASE
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 619; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }
#endif

namespace {

struct Options {
    bool headless = false, windowed = false, scene = false, sim = false, stream = false;
    bool walkbot = false, topdown = false, version = false, shot = false;
    bool render_wav = false, footsteps = false, audiosoak = false, audio = false;
    bool biomeat = false, descend = false, ascend = false, vstream = false, shaftfall = false, abyss = false, post = false, dxr_probe = false, dxr_test = false, dxr = false;
    bool livedescent = false;   // M30: headless proof that the live-walk holed floor lets you fall through a down-stair hole + land
    bool descentsoak = false;   // M30: deep-descent soak (repeated deep falls; bounded memory/residency + determinism)
    bool strollcheck = false;   // screensaver: headless proof the Stroller navigates naturally (low faceplant ratio)
    bool screensaver = false, scr_config = false;   // Windows .scr: /s run, /p <hwnd> preview, /c config
    std::string scr_preview;                         // /p <hwnd>: parent HWND to render the preview into
    bool dxr_depth = false, dxr_pt = false, dxr_fps = false, dxr_ghost = false, dxr_walk = false;
    bool dxr_denoise = false;   // headless: does the spatial denoiser bring a noisy few-spp frame closer to ground truth?
    bool dxr_stoch = false;     // headless oracle: does stochastic single-light NEE (RIS) converge to the full-NEE image? (unbiasedness)
    uint32_t rt_scale = 0;      // run_game initial RT internal-resolution scale index (0 Quality 2/3, 1 Balanced 1/2, 2 Performance 1/3); F3 cycles it live
    bool no_vsync = false;      // run_game: start with vsync OFF (uncapped FPS, lower latency, tearing); V toggles it live. Default vsync ON.
    bool llm_test = false;      // CLI: exercise the in-game "Test Connection" probe + print the status line
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
    bool shoggoth_dxr_shot = false;     // M25 render the shoggoth body in the DXR (ray-traced) path
    bool shoggoth_record = false, shoggoth_replay = false;  // M21 brain record/replay
    bool no_shoggoth_brain = false;     // M21b: kill switch for the live async brain in --play/--game
    bool auto_play = false;             // headless: --game enters Play immediately + reports mouse-look drift (spin guard)
    bool shoggoth_vision_record = false;  // M22: the Shoggoth sees -- POV snapshot -> vision intent
    bool shoggoth_hearing_record = false;  // M23: the Shoggoth hears -- soundscape -> whisper -> intent
    std::string whisper_exe, whisper_model;  // M23: whisper.cpp CLI + model (defaults below)
    bool tts_say = false;                  // M24: procedural TTS -> WAV (the Backrooms PA voice)
    bool tts_check = false;                 // M24: TTS -> whisper round-trip (intelligibility check)
    bool caption_shot = false;              // render the Director subtitle over a gray bg -> PNG (visibility proof)
    bool flashlight = false;                // --dxr-pt QC: render with the eye-torch ON (off by default)
    bool game_shot = false;                 // --game-shot: walk the Stroller into the maze, then render one framed shot (--rt for PT)
    bool ladder_walk = false;               // --ladder-walk: headless probe -- walk the infinite ladder down then up, flag any fall-through
    bool ladder_shot = false;               // --ladder-shot: QC -- frame the infinite ladder (--pose 0 spawn approach, 1 on-ladder descent)
    bool drop_flares = false;               // --game-shot QC: drop a line of green flares ahead of the camera (RT A/B)
    bool recolor_shot = false;              // --recolor-shot: POC -- the LLM recolours the walls from what you --say
    bool mic_test = false;                  // ADR-074: capture mic + VAD + whisper, print transcripts (verify voice input)
    bool chat_test = false;                 // ADR-074: TTS->whisper->Director reply (verify the conversation glue, no mic)
    bool shoggoth_pa_record = false;       // M24: PA voice spoken into the soundscape -> heard as words
    std::string say_text;                  // M24: text for --tts-say / the PA line
    uint32_t eval_count = 100;          // --director-eval: scenario count
    uint32_t director_interval_s = 15;  // --soak --director: ambient seconds between summaries (wall clock)
    uint32_t frames = 1, seconds = 0, width = 320, height = 180, ticks = 0;
    uint32_t ticks_per_frame = 30, radius = 6, workers = 4, km = 1, pose = 0, spp = 256;
    uint32_t shot_every = 600;   // soak: write a screenshot every N rendered frames
    int32_t level = 0;           // M26: spawn/scene level for --shot (Phase IV infinite-Z)
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
        // Windows screensaver args (slash-prefixed): /s run fullscreen, /p <hwnd> preview, /c[:hwnd] config.
        if (a[0] == '/' && (a[1] == 's' || a[1] == 'S')) { o.screensaver = true; continue; }
        if (a[0] == '/' && (a[1] == 'c' || a[1] == 'C')) { o.scr_config = true; continue; }
        if (a[0] == '/' && (a[1] == 'p' || a[1] == 'P')) {
            o.screensaver = true;
            const char* hp = a + 2;
            if (*hp == ':' || *hp == '=') ++hp;                 // /p:HWND or /p=HWND
            if (*hp) o.scr_preview = hp;                        // /pHWND
            else if (i + 1 < argc) o.scr_preview = argv[++i];   // /p <HWND>
            continue;
        }
        if (std::strcmp(a, "--headless") == 0) o.headless = true;
        else if (std::strcmp(a, "--window") == 0) o.windowed = true;
        else if (std::strcmp(a, "--scene") == 0) o.scene = true;
        else if (std::strcmp(a, "--sim") == 0) o.sim = true;
        else if (std::strcmp(a, "--stream") == 0) o.stream = true;
        else if (std::strcmp(a, "--walkbot") == 0) o.walkbot = true;
        else if (std::strcmp(a, "--topdown") == 0) o.topdown = true;
        else if (std::strcmp(a, "--shot") == 0) o.shot = true;
        else if (std::strcmp(a, "--level") == 0) { if (i + 1 >= argc) return false; o.level = static_cast<int32_t>(std::strtol(argv[++i], nullptr, 10)); }
        else if (std::strcmp(a, "--pose") == 0) { if (!u32(o.pose)) return false; }
        else if (std::strcmp(a, "--render-wav") == 0) o.render_wav = true;
        else if (std::strcmp(a, "--footsteps") == 0) o.footsteps = true;
        else if (std::strcmp(a, "--audiolog") == 0) { if (!str(o.audiolog)) return false; }
        else if (std::strcmp(a, "--audiosoak") == 0) o.audiosoak = true;
        else if (std::strcmp(a, "--audio") == 0) o.audio = true;
        else if (std::strcmp(a, "--biomeat") == 0) o.biomeat = true;
        else if (std::strcmp(a, "--descend") == 0) o.descend = true;
        else if (std::strcmp(a, "--ascend") == 0) o.ascend = true;
        else if (std::strcmp(a, "--vstream") == 0) o.vstream = true;
        else if (std::strcmp(a, "--shaftfall") == 0) o.shaftfall = true;
        else if (std::strcmp(a, "--livedescent") == 0) o.livedescent = true;
        else if (std::strcmp(a, "--descentsoak") == 0) o.descentsoak = true;
        else if (std::strcmp(a, "--strollcheck") == 0) o.strollcheck = true;
        else if (std::strcmp(a, "--abyss") == 0) o.abyss = true;
        else if (std::strcmp(a, "--post") == 0) o.post = true;
        else if (std::strcmp(a, "--dxr-probe") == 0) o.dxr_probe = true;
        else if (std::strcmp(a, "--dxr-test") == 0) o.dxr_test = true;
        else if (std::strcmp(a, "--dxr") == 0) o.dxr = true;
        else if (std::strcmp(a, "--dxr-depth") == 0) o.dxr_depth = true;
        else if (std::strcmp(a, "--dxr-pt") == 0) o.dxr_pt = true;
        else if (std::strcmp(a, "--dxr-fps") == 0) o.dxr_fps = true;
        else if (std::strcmp(a, "--dxr-ghost") == 0) o.dxr_ghost = true;
        else if (std::strcmp(a, "--dxr-walk") == 0) o.dxr_walk = true;
        else if (std::strcmp(a, "--dxr-denoise") == 0) o.dxr_denoise = true;
        else if (std::strcmp(a, "--dxr-stoch") == 0) o.dxr_stoch = true;
        else if (std::strcmp(a, "--llm-test") == 0) o.llm_test = true;
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
        else if (std::strcmp(a, "--shoggoth-dxr-shot") == 0) o.shoggoth_dxr_shot = true;
        else if (std::strcmp(a, "--shoggoth-record") == 0) o.shoggoth_record = true;
        else if (std::strcmp(a, "--shoggoth-replay") == 0) o.shoggoth_replay = true;
        else if (std::strcmp(a, "--no-shoggoth-brain") == 0) o.no_shoggoth_brain = true;
        else if (std::strcmp(a, "--auto-play") == 0) o.auto_play = true;
        else if (std::strcmp(a, "--shoggoth-vision-record") == 0) o.shoggoth_vision_record = true;
        else if (std::strcmp(a, "--shoggoth-hearing-record") == 0) o.shoggoth_hearing_record = true;
        else if (std::strcmp(a, "--whisper-exe") == 0) { if (!str(o.whisper_exe)) return false; }
        else if (std::strcmp(a, "--whisper-model") == 0) { if (!str(o.whisper_model)) return false; }
        else if (std::strcmp(a, "--tts-say") == 0) o.tts_say = true;
        else if (std::strcmp(a, "--tts-check") == 0) o.tts_check = true;
        else if (std::strcmp(a, "--caption-shot") == 0) o.caption_shot = true;
        else if (std::strcmp(a, "--mic-test") == 0) o.mic_test = true;
        else if (std::strcmp(a, "--chat-test") == 0) o.chat_test = true;
        else if (std::strcmp(a, "--shoggoth-pa-record") == 0) o.shoggoth_pa_record = true;
        else if (std::strcmp(a, "--say") == 0) { if (!str(o.say_text)) return false; }
        else if (std::strcmp(a, "--shot-every") == 0) { if (!u32(o.shot_every)) return false; }
        else if (std::strcmp(a, "--spp") == 0) { if (!u32(o.spp)) return false; }
        else if (std::strcmp(a, "--rt-scale") == 0) { if (!u32(o.rt_scale)) return false; }
        else if (std::strcmp(a, "--no-vsync") == 0) o.no_vsync = true;
        else if (std::strcmp(a, "--flashlight") == 0) o.flashlight = true;
        else if (std::strcmp(a, "--game-shot") == 0) o.game_shot = true;
        else if (std::strcmp(a, "--ladder-walk") == 0) o.ladder_walk = true;
        else if (std::strcmp(a, "--ladder-shot") == 0) o.ladder_shot = true;
        else if (std::strcmp(a, "--recolor-shot") == 0) o.recolor_shot = true;
        else if (std::strcmp(a, "--flares") == 0) o.drop_flares = true;
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

// Raw mouse input (WM_INPUT) for first-person look. Unlike GetCursorPos/SetCursorPos recentering, raw input
// reports RELATIVE motion straight from the HID stack -- immune to DPI scaling, the desktop's pointer
// ballistics, cursor edge-clamping, focus changes, and cursor warping (a non-interactive desktop parks the
// cursor at 0,0 every frame -> the old scheme read a constant huge delta -> a runaway spin). A still mouse
// emits no WM_INPUT, so an idle frame yields a zero delta and the view can never self-rotate.
static bool register_raw_mouse(HWND hwnd) {
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;   // generic desktop controls
    rid.usUsage     = 0x02;   // mouse
    rid.dwFlags     = 0;      // deliver only while this window has focus (no RIDEV_INPUTSINK)
    rid.hwndTarget  = hwnd;
    return RegisterRawInputDevices(&rid, 1, sizeof(rid)) == TRUE;
}

// Pull the relative mouse delta out of a WM_INPUT lParam and add it to (dx,dy). Absolute-coordinate devices
// (RDP, tablets, touch) are skipped rather than misread as a giant delta. Usable from a window proc directly.
static void read_raw_mouse_delta(LPARAM lParam, long& dx, long& dy) {
    BYTE buf[128];
    UINT sz = sizeof(buf);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buf, &sz, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
        return;
    const RAWINPUT* ri = reinterpret_cast<const RAWINPUT*>(buf);
    if (ri->header.dwType == RIM_TYPEMOUSE && !(ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
        dx += ri->data.mouse.lLastX;
        dy += ri->data.mouse.lLastY;
    }
}
// Same, but driven from an MSG in an inline message pump. No-op for non-WM_INPUT messages.
static void accumulate_raw_mouse(const MSG& msg, long& dx, long& dy) {
    if (msg.message == WM_INPUT) read_raw_mouse_delta(msg.lParam, dx, dy);
}

// Regression telemetry: every time we WARP the OS cursor (SetCursorPos), bump this. With raw-input look, a
// warp should be RARE (a one-time park when capture begins), NEVER per-frame -- a per-frame warp is exactly
// the "my cursor fights me" bug. The --auto-play guard asserts this stays tiny over a whole Play session.
static long g_cursorWarps = 0;

// Screensaver window. As a screensaver, ANY key / click / wheel / app-deactivation ends it (the .scr
// contract) -- EXCEPT SPACE, which drops into a playable WASD walk (g_scrPlay). Once playing, keys +
// clicks are game input (the loop reads them via GetAsyncKeyState), so they no longer exit; ESC exits.
// Mouse-move is caught in the loop (it exits while idle, but steers the look while playing).
static volatile bool g_scrQuit = false;
static volatile bool g_scrPlay = false;  // SPACE -> become a playable WASD/mouse walk
static long g_scrRawDX = 0, g_scrRawDY = 0;  // relative mouse delta for SPACE-play look (filled in scr_proc)
LRESULT CALLBACK scr_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
            if (g_scrPlay) return 0;                            // playing: keys are game input, not an exit
            if (wp == VK_SPACE) { g_scrPlay = true; return 0; }  // SPACE -> drop into the playable walk
            g_scrQuit = true; return 0;                         // any other key -> exit (the .scr contract)
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_MOUSEWHEEL:
            if (!g_scrPlay) g_scrQuit = true;                   // playing: clicks/wheel are ignored, not an exit
            return 0;
        case WM_INPUT:
            if (g_scrPlay) read_raw_mouse_delta(lp, g_scrRawDX, g_scrRawDY);  // accumulate look ONLY while playing
            return DefWindowProcW(hwnd, msg, wp, lp);           // WM_INPUT needs DefWindowProc cleanup
        case WM_ACTIVATEAPP: if (wp == FALSE) g_scrQuit = true; return 0;  // losing focus always exits
        case WM_CLOSE:   DestroyWindow(hwnd); return 0;
        case WM_DESTROY: PostQuitMessage(0);  return 0;
        default:         return DefWindowProcW(hwnd, msg, wp, lp);
    }
}
// A borderless, topmost, cursor-hidden window covering the PRIMARY display.
HWND create_screensaver_window(uint32_t& w, uint32_t& h) {
    const wchar_t* kClass = L"BackroomsScreensaver";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = scr_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    wc.hCursor = nullptr;  // no cursor
    RegisterClassExW(&wc);
    w = static_cast<uint32_t>(GetSystemMetrics(SM_CXSCREEN));
    h = static_cast<uint32_t>(GetSystemMetrics(SM_CYSCREEN));
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, kClass, L"Backrooms", WS_POPUP,
                                0, 0, static_cast<int>(w), static_cast<int>(h),
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd) { ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd); }
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

// A richer, ORGANIC head-bob for the SCREENSAVER. The shared head_bob (above) is a clean cosine/sine
// -- correct, but metronomic. A real gait is NOT a perfect sine: the two footfalls per stride differ
// slightly (a dominant leg), the vigor drifts over time, the dip shape isn't a pure cosine, and the
// head nods + never sits perfectly level. This layers all of that on -- still driven purely by the
// deterministic odometer/speed (view-only, reproducible, never touches WorldState), with three SLOW
// INCOMMENSURATE modulators so the motion is quasi-periodic (it never exactly repeats). The vertical
// term stays <= 0 (the head only ever dips). Screensaver-only; the game keeps the crisp M18 bob.
void apply_organic_bob(contracts::CameraPose& cam, const br::core::WorldState& s) {
    const float hspeed = std::sqrt(s.wanderer.vel.x * s.wanderer.vel.x + s.wanderer.vel.z * s.wanderer.vel.z);
    const float lo = 0.1f, hi = br::core::kWalkSpeed * 0.6f;             // ease the bob in/out with speed
    float t = (hi > lo) ? (hspeed - lo) / (hi - lo) : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float ramp = t * t * (3.0f - 2.0f * t);
    if (ramp <= 0.0f) return;                                            // standing still -> no bob

    const float kTwoPi = 6.28318530718f;
    const float d = s.odometer;                                          // gait phase from walked distance
    const bool running = hspeed > (br::core::kWalkSpeed + br::core::kRunSpeed) * 0.5f;
    const float ph = d * (running ? 0.42f : 0.31f) * kTwoPi;             // stride cadence (cycles / m)

    // Slow, incommensurate gait modulators (periods ~48 m / ~94 m / ~126 m of walking) -> the bob's
    // vigor, its left/right asymmetry, and its sway all drift independently: never a clean repeat.
    const float vigor = 1.0f + 0.13f * std::sin(d * 0.13f);              // +/-13% amplitude wander
    const float asym  = 0.13f * std::sin(d * 0.067f + 1.7f);            // a subtle dominant-leg limp (drifts)
    const float drift = std::sin(d * 0.05f + 0.5f);                      // a slow not-quite-level lean

    const float ampV = (running ? 0.082f : 0.058f) * ramp * vigor;       // ~6 cm walk: clearly felt, still natural
    const float ampL = (running ? 0.052f : 0.034f) * ramp;

    // Vertical: two dips/stride, but alternate their depth (asymmetry) + a small 2nd harmonic so the
    // dip shape is not a pure cosine. Always <= 0.
    const float dip   = 0.5f * (1.0f - std::cos(2.0f * ph));             // 0..1, two humps per cycle
    const float which = std::sin(ph);                                    // -1..1, weights one footfall vs the next
    const float shape = dip * (1.0f - asym * which);                     // deepen one dip, lighten the other
    const float h2    = 0.10f * 0.5f * (1.0f - std::cos(4.0f * ph));     // subtle non-sinusoidal shaping
    float dy = -ampV * (shape + h2);
    if (dy > 0.0f) dy = 0.0f;

    const float dx  = ampL * (std::sin(ph) + 0.15f * std::sin(d * 0.09f));            // figure-8 sway + slow wander
    const float nod = (running ? 0.020f : 0.013f) * ramp * (0.6f * std::sin(2.0f * ph) + 0.4f * drift);  // step nod + lean

    cam.pos[1] += dy;
    const float rx = std::cos(s.wanderer.yaw), rz = -std::sin(s.wanderer.yaw);
    cam.pos[0] += rx * dx;
    cam.pos[2] += rz * dx;
    cam.pitch += nod;                                                    // the head nods + is never perfectly level
}

// Defined below in the Director-helpers anonymous namespace; forward-declared here in
// that SAME anonymous namespace (multiple `namespace {}` blocks in a TU are one and the
// same) so the interactive --play / --game loops can resolve the KEEL host:port for the
// M21b live brain without duplicating the scheme/path-stripping logic.
namespace { void parse_host_port(const std::string& url, std::string& host, int& port); }

// ADR-074: the half-duplex echo window. speak_pa() stamps "the PA voice is busy until T (ms)" here;
// MicCapture is gated off until then (synced each frame) so the facility never transcribes ITSELF.
static std::atomic<uint64_t> g_paSuspendUntilMs{0};

// whisper_transcribe (defined far below, with the Shoggoth-hearing path) shells out to whisper-cli;
// forward-declared so the --game voice loop can hand it to the DirectorChatHost as the transcriber.
std::string whisper_transcribe(const std::string& wav, const std::string& exe, const std::string& model);

// Speak a line through the procedural PA VOICE (M24 formant TTS) on the default device -- async + non-blocking,
// so the player HEARS the Director's narration as intercom words. Presentation only (no sim state -> INV-1
// untouched). A fresh line replaces any still-playing one (PA lines are spaced seconds apart). winmm's
// PlaySound keeps it dead simple -- no real-time mixer surgery; it layers over the game audio like a loudspeaker.
static void speak_pa(const std::string& text, uint32_t sr) {
    const std::vector<int16_t> pcm = br::app::synthesize_speech(text, sr);
    if (pcm.empty()) return;
    const uint32_t dataBytes = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
    std::vector<uint8_t> wav; wav.reserve(44 + dataBytes);
    auto put32 = [&](uint32_t v){ wav.push_back(uint8_t(v)); wav.push_back(uint8_t(v >> 8)); wav.push_back(uint8_t(v >> 16)); wav.push_back(uint8_t(v >> 24)); };
    auto put16 = [&](uint16_t v){ wav.push_back(uint8_t(v)); wav.push_back(uint8_t(v >> 8)); };
    auto tag = [&](const char* t){ wav.insert(wav.end(), t, t + 4); };
    tag("RIFF"); put32(36 + dataBytes); tag("WAVE");
    tag("fmt "); put32(16); put16(1); put16(1); put32(sr); put32(sr * 2u); put16(2); put16(16);   // PCM16 mono
    tag("data"); put32(dataBytes);
    const uint8_t* pb = reinterpret_cast<const uint8_t*>(pcm.data());
    wav.insert(wav.end(), pb, pb + dataBytes);
    static std::vector<uint8_t> g_pa;   // must outlive async playback -> kept until the next line replaces it
    PlaySoundW(nullptr, nullptr, SND_PURGE);   // stop/release any still-playing line first
    g_pa = std::move(wav);
    PlaySoundW(reinterpret_cast<LPCWSTR>(g_pa.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    // Half-duplex echo gate: while this line plays (+ a ~0.6 s tail) the mic is suspended so the
    // Director is never re-heard through the speakers (ADR-074). Cheap; read by the voice loop.
    const uint64_t ms = static_cast<uint64_t>(pcm.size()) * 1000ull / (sr ? sr : 1u);
    g_paSuspendUntilMs.store(GetTickCount64() + ms + 600ull);
}

// ===== ADR-076: self-contained portable bundle ============================================
// The portable build ships its runtime + models UNDER the exe: runtime\llama\, runtime\keel\,
// runtime\whisper\, models\. These resolve exe-relative; if absent (this dev tree, where the exe is
// in build[-release]\bin), they fall back to the IN-REPO dist\Backrooms bundle (ADR-078) -- so the
// repo keeps building/running and NOTHING outside C:\backrooms is ever needed (no C:\llama.cpp etc.).
static std::wstring exe_dir_w() {
    wchar_t buf[MAX_PATH]; const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf, (n > 0 && n < MAX_PATH) ? n : 0u);
    const size_t s = p.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? std::wstring() : p.substr(0, s + 1);   // includes trailing slash
}
static std::string exe_dir_a() {
    char buf[MAX_PATH]; const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf, (n > 0 && n < MAX_PATH) ? n : 0u);
    const size_t s = p.find_last_of("\\/");
    return (s == std::string::npos) ? std::string() : p.substr(0, s + 1);
}
static std::wstring parent_dir_w(const std::wstring& path) {
    const size_t s = path.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? std::wstring() : path.substr(0, s + 1);
}
// The bundled path under the exe if it exists (the portable bundle), else the IN-REPO dev bundle at
// dist\Backrooms (two levels up from build[-release]\bin). ADR-078: never C:\ -- the repo's own dist\
// runtime is the dev fallback, so nothing outside C:\backrooms is ever needed. (The 2nd arg is now
// vestigial; the fallback is computed in-repo. Kept optional so existing call sites still compile.)
static std::wstring bundled_w(const wchar_t* rel, const wchar_t* = nullptr) {
    const std::wstring cand = exe_dir_w() + rel;
    if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) return cand;
    return exe_dir_w() + L"..\\..\\dist\\Backrooms\\" + rel;   // dev tree -> <repo>\dist\Backrooms\<rel>
}
static std::string bundled_a(const char* rel, const char* = nullptr) {
    const std::string cand = exe_dir_a() + rel;
    if (GetFileAttributesA(cand.c_str()) != INVALID_FILE_ATTRIBUTES) return cand;
    return exe_dir_a() + "..\\..\\dist\\Backrooms\\" + rel;
}
// whisper-cli + model defaults: the bundle ships whisper-cli + ggml-base.en under the exe; the dev
// tree now falls back to the IN-REPO dist\Backrooms\ copy (ADR-078) -- base.en on the dev box too.
static std::string default_whisper_exe()   { return bundled_a("runtime\\whisper\\whisper-cli.exe"); }
static std::string default_whisper_model() { return bundled_a("models\\ggml-base.en.bin"); }

// Largest adapter's dedicated VRAM in MB (DXGI). 0 if unavailable -> treated as unknown (-> default 9B tier).
static unsigned detect_vram_mb() {
    IDXGIFactory* f = nullptr;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&f))) || !f) return 0u;
    unsigned best = 0u;
    IDXGIAdapter* a = nullptr;
    for (UINT i = 0; f->EnumAdapters(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC d{};
        if (SUCCEEDED(a->GetDesc(&d))) {
            const unsigned mb = static_cast<unsigned>(d.DedicatedVideoMemory / (1024ull * 1024ull));
            if (mb > best) best = mb;
        }
        a->Release();
    }
    f->Release();
    return best;
}

static HANDLE g_llmJob = nullptr;                  // holds the LLM-stack children; KILL_ON_JOB_CLOSE
static std::atomic<bool> g_visionAvailable{true};  // 9B+mmproj tier -> vision; 4B tier -> text-only Director
static std::atomic<int> g_modelTier{0};            // Settings override: 0 AUTO (VRAM-picked), 1 force 9B, 2 force 4B (read at sidecar launch)

// Launch a console exe HIDDEN (no window) inside the kill-on-close job; stdio -> a log file. The MS
// pattern (create-suspended -> assign-to-job -> resume) keeps the child from escaping the job.
static void launch_hidden_in_job(const std::wstring& exe, const std::wstring& args,
                                 const std::wstring& cwd, const std::wstring& logPath) {
    std::wstring cl = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> buf(cl.begin(), cl.end()); buf.push_back(L'\0');
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE hLog = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{}; si.cb = sizeof(si);
    if (hLog != INVALID_HANDLE_VALUE) { si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = hLog; si.hStdError = hLog; }
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr,
                                   (hLog != INVALID_HANDLE_VALUE) ? TRUE : FALSE,
                                   CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                   cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog);
    if (!ok) return;
    if (g_llmJob) AssignProcessToJobObject(g_llmJob, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);   // the job keeps the child alive; we don't need these
}

// Bring up the LLM stack the Director/voice need -- self-contained from the bundle (or the C:\ dev install).
// HIDDEN + job-managed: no windows, and the servers die when the game exits/crashes (the job handle is held
// for the process lifetime). Idempotent: a port already answering is left alone. llama-server :8080 starts
// FIRST (keel probes + reuses it). Fire-once; a missing runtime is a graceful no-op (deterministic AI only).
// VRAM picks the model tier: >= ~11 GB -> 9B + mmproj (vision); else 4B (text); unknown -> 9B (the default).
static void try_start_sidecar() {
    static bool tried = false;
    if (tried) return;
    tried = true;

    const std::wstring llamaExe = bundled_w(L"runtime\\llama\\llama-server.exe");
    const std::wstring keelExe  = bundled_w(L"runtime\\keel\\keel-serve.exe");
    const std::wstring model9b  = bundled_w(L"models\\Qwen3.5-9B-Q5_K_M.gguf");
    const std::wstring model4b  = bundled_w(L"models\\Qwen3.5-4B-Q4_K_M.gguf");
    const std::wstring mmproj   = bundled_w(L"models\\mmproj-F16.gguf");
    const std::wstring logDir   = exe_dir_w() + L"logs\\"; CreateDirectoryW(logDir.c_str(), nullptr);

    const unsigned vram = detect_vram_mb();
    const int tier = g_modelTier.load();   // Settings override: 1 force 9B, 2 force 4B, 0 AUTO (VRAM-picked)
    const bool use9b = (tier == 1) ? true : ((tier == 2) ? false : ((vram == 0u) || (vram >= 11000u)));
    g_visionAvailable.store(use9b);
    const std::wstring model = use9b ? model9b : model4b;

    if (!g_llmJob) {
        g_llmJob = CreateJobObjectW(nullptr, nullptr);
        if (g_llmJob) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
            jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(g_llmJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
        }
    }
    // 1) llama-server :8080 FIRST (keel reuses it). Skip if a model server already answers there.
    if (!br::director::service_up("127.0.0.1", 8080, 700) &&
        GetFileAttributesW(llamaExe.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring args = L"-m \"" + model + L"\"";
        if (use9b) args += L" --mmproj \"" + mmproj + L"\"";
        args += L" --host 127.0.0.1 --port 8080 -ngl 99 -c 8192";
        launch_hidden_in_job(llamaExe, args, parent_dir_w(llamaExe), logDir + L"llama.log");
    }
    // 2) keel-serve :7071 (cwd = its dir; reads keel.lock; reuses :8080). Skip if already up.
    if (!br::director::service_up("127.0.0.1", 7071, 700) &&
        GetFileAttributesW(keelExe.c_str()) != INVALID_FILE_ATTRIBUTES) {
        launch_hidden_in_job(keelExe, L"keel.lock", parent_dir_w(keelExe), logDir + L"keel.log");
    }
}

// Async LLM connection probe for the in-game Settings "Test Connection". Runs the REAL keel_complete OFF the
// UI thread (so the menu never freezes), best-effort-starts the sidecar, and reports a short UPPERCASE status
// line the 5x7 bitmap font can draw -- "CONNECTED  LOCAL  640MS  -  <the directive it generated>" or an error.
struct LlmProbe {
    std::atomic<int> state{0};   // 0 untested, 1 testing, 2 connected, 3 offline
    std::mutex mtx;
    std::string text;            // guarded status line (already uppercased + charset-folded)
    std::thread th;
    ~LlmProbe() { if (th.joinable()) th.join(); }

    void start(std::string host, int port) {
        if (state.load() == 1) return;                 // a test is already running
        if (th.joinable()) th.join();
        state.store(1);
        { std::lock_guard<std::mutex> lk(mtx); text = "TESTING..."; }
        th = std::thread([this, host, port]() {
            try_start_sidecar();                       // best-effort: bring the stack up if it is not already
            contracts::WandererSummary sum{};
            sum.tick = 130000u; sum.world_seed = 1u; sum.level = 0;
            sum.biome = 1; sum.chunk_cx = 7; sum.chunk_cz = -2;
            sum.distance_m = 1240.0f; sum.dwell_seconds = 95.0f; sum.route_loops = 3;
            sum.location_hash = 7u;
            const std::string prompt = br::director::render_prompt(sum);
            const auto t0 = std::chrono::steady_clock::now();
            const br::director::KeelResponse resp = br::director::keel_complete(host, port, prompt, 6000);
            const long ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count());
            std::string line; int st;
            if (resp.ok) {
                st = 2;
                char head[80];
                std::snprintf(head, sizeof(head), "CONNECTED  %s  %ldMS",
                              resp.tier.empty() ? "LOCAL" : resp.tier.c_str(), ms);
                line = head;
                const br::director::DirectiveResult dr = br::director::validate_directive(resp.content);
                if (dr.ok && dr.directive.caption[0] != '\0') { line += "  -  "; line += dr.directive.caption; }
            } else {
                st = 3;
                line = "OFFLINE - " + (resp.error.empty() ? std::string("NO REPLY") : resp.error);
            }
            for (char& ch : line) {   // fold to the bitmap font's charset (A-Z 0-9 space . , : -); others -> space
                if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 32);
                else if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                           ch == ' ' || ch == '.' || ch == ',' || ch == ':' || ch == '-')) ch = ' ';
            }
            if (line.size() > 58) line.resize(58);
            { std::lock_guard<std::mutex> lk(mtx); text = line; }
            state.store(st);
        });
    }
    void sync(app::MenuModel& m) {                     // copy the latest status into the model for rendering
        m.llm_state = state.load();
        std::lock_guard<std::mutex> lk(mtx); m.llm_text = text;
    }
};

// ADR-074: the Settings "TEST MICROPHONE" diagnostic -- a full voice-loop self-test, off the UI thread.
// Records the mic (VAD, ~9 s window), transcribes (whisper), asks the Director (text chat), and reports
// BOTH the heard text + the reply (shown as a caption); the reply is also SPOKEN (the frame thread takes
// it via take_fresh_reply -> speak_pa). Lets the operator confirm mic + whisper + LLM + TTS end-to-end
// without starting a game and regardless of the Director toggle. Mirrors LlmProbe's async lifecycle.
struct MicProbe {
    std::atomic<int> state{0};   // 0 idle, 1 listening, 2 thinking, 3 replied, 4 error
    std::mutex mtx;
    std::string heard, reply;    // guarded; folded to the bitmap-font charset (for the overlay)
    std::string reply_raw;       // guarded; unfolded (for the PA voice)
    std::atomic<bool> reply_fresh{false};
    std::thread th;
    ~MicProbe() { if (th.joinable()) th.join(); }

    static void fold(std::string& line) {   // to the font charset (A-Z 0-9 space . , : - ? !); cap length
        for (char& ch : line) {
            if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 32);
            else if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == ' ' ||
                       ch == '.' || ch == ',' || ch == ':' || ch == '-' || ch == '?' || ch == '!')) ch = ' ';
        }
        if (line.size() > 64) line.resize(64);
    }
    void seterr(std::string h, std::string msg) {
        fold(h); fold(msg);
        { std::lock_guard<std::mutex> lk(mtx); heard = std::move(h); reply = std::move(msg); reply_raw.clear(); }
        state.store(4);
    }

    void start(std::string host, int port, std::string wexe, std::string wmodel) {
        const int s = state.load();
        if (s == 1 || s == 2) return;                  // a test is already running
        if (th.joinable()) th.join();
        state.store(1);
        { std::lock_guard<std::mutex> lk(mtx); heard.clear(); reply.clear(); reply_raw.clear(); }
        reply_fresh.store(false);
        th = std::thread([this, host, port, wexe, wmodel]() {
            try_start_sidecar();
            app::MicCapture mic;
            if (!mic.start()) { seterr("", "NO MICROPHONE DEVICE FOUND"); return; }
            std::vector<int16_t> utter;
            const uint64_t end = GetTickCount64() + 9000ull;   // up to ~9 s to speak
            bool got = false;
            while (GetTickCount64() < end) { if (mic.poll(utter)) { got = true; break; } Sleep(15); }
            mic.stop();
            if (!got) { seterr("", "NO SPEECH HEARD - SPEAK, THEN PAUSE"); return; }
            state.store(2);   // thinking
            char tb[MAX_PATH]; const DWORD tn = GetTempPathA(MAX_PATH, tb);
            const std::string wav = ((tn > 0 && tn < MAX_PATH) ? std::string(tb) : std::string("runs\\")) + "br_mic_settings.wav";
            std::string err;
            if (!audio::write_wav(wav, utter, app::MicCapture::kRate, static_cast<uint16_t>(1), err)) { seterr("", "AUDIO WRITE FAILED"); return; }
            const std::string h = whisper_transcribe(wav, wexe, wmodel);
            if (!app::plausible_utterance(h)) { seterr(h, "COULD NOT MAKE THAT OUT - TRY AGAIN"); return; }
            const std::string prompt = app::render_director_chat_prompt(h, std::string(), false);
            const br::director::KeelResponse resp = br::director::keel_complete(host, port, prompt, 30000);
            if (!resp.ok) { seterr(h, "LLM OFFLINE - " + (resp.error.empty() ? std::string("NO REPLY") : resp.error)); return; }
            const std::string rep = app::clean_vision_line(resp.content);
            std::string hh = h, rf = rep; fold(hh); fold(rf);
            { std::lock_guard<std::mutex> lk(mtx); heard = std::move(hh); reply = std::move(rf); reply_raw = rep; }
            reply_fresh.store(true);
            state.store(3);   // replied
        });
    }
    void sync(app::MenuModel& m) {
        m.mic_state = state.load();
        std::lock_guard<std::mutex> lk(mtx); m.mic_heard = heard; m.mic_reply = reply;
    }
    std::string take_fresh_reply() {   // the unfolded reply to SPEAK, once (latched); "" otherwise
        if (!reply_fresh.exchange(false)) return std::string();
        std::lock_guard<std::mutex> lk(mtx); return reply_raw;
    }
};

// M30 (live descent): build the live-walk collision world for the wanderer's CURRENT floor --
// the chunks' wall/stair/pillar collision over the 3x3 neighbourhood PLUS a PER-CELL solid floor
// at this level's baseY, but with HOLES at the open cells (down-stair holes + shaft voids, via
// gen::floor_hole_at -- the SAME predicate GenerateChunk cuts the floor mesh with, so collision
// matches what you see). The old single {-1e6..1e6} ground plane sealed every hole, so you could
// see the abyss but never fall in; the holed floor lets you fall through and the level BELOW's
// floor soft-catches you on the next per-level rebuild (the despair gradient, live). Each cell's
// slab is baseY-1..baseY -- top surface at baseY, identical to the old plane, so walking on solid
// ground is unchanged. Presentation/interaction only: never touches WorldState, so determinism
// (INV-1) is untouched (run_play/run_game/run_screensaver are interactive, non-gated paths).
static void build_walk_collision(std::vector<br::core::Aabb>& out, uint64_t seed, contracts::ChunkKey c) {
    out.clear();
    const float baseY = contracts::level_base_y(c.level);
    const float cs = br::gen::kCellSize;
    const int G = br::gen::kCellsPerChunk;
    for (int64_t dx = -1; dx <= 1; ++dx)
        for (int64_t dz = -1; dz <= 1; ++dz) {
            const contracts::ChunkKey k{ c.level, c.cx + dx, c.cz + dz };
            const float ox = static_cast<float>(k.cx) * contracts::kChunkSize;
            const float oz = static_cast<float>(k.cz) * contracts::kChunkSize;
            for (int i = 0; i < G; ++i)
                for (int j = 0; j < G; ++j) {
                    if (br::gen::floor_hole_at(seed, k.level, k.cx, k.cz, i, j)) continue;  // open -> you fall through
                    const float x0 = ox + static_cast<float>(i) * cs, x1 = ox + static_cast<float>(i + 1) * cs;
                    const float z0 = oz + static_cast<float>(j) * cs, z1 = oz + static_cast<float>(j + 1) * cs;
                    out.push_back(br::core::Aabb{ {x0, baseY - 1.0f, z0}, {x1, baseY, z1} });
                }
            const contracts::ChunkData cd = contracts::GenerateChunk(seed, k);
            for (const auto& b : cd.collision)
                out.push_back(br::core::Aabb{ {b.mn[0], b.mn[1], b.mn[2]}, {b.mx[0], b.mx[1], b.mx[2]} });
        }
}

// M30 telegraph (locked design decision 6 -- shaft entry is accidental but ALWAYS telegraphed): a draft
// swells as the wanderer nears an OPEN shaft on the current floor (you can stumble in, but you were
// warned ~1-2 cells out). Scans the 3x3 chunks for a shaft whose floor is open at this level (i.e. one
// you could fall into), returns 0..1 by nearest shaft-cell distance. Pure presentation -- off the sim
// hash (fed only to the real-time mixer); a dead audio device just means silence, never a stall.
static float draft_intensity_near_shaft(uint64_t seed, int32_t level, float px, float pz) {
    const float cs = br::gen::kCellSize;
    const contracts::ChunkKey c = contracts::chunk_key_at(level, px, pz);
    float best = 1.0e9f;
    for (int64_t dx = -1; dx <= 1; ++dx)
        for (int64_t dz = -1; dz <= 1; ++dz) {
            const br::gen::ShaftSpec sh = br::gen::shaft_at(seed, c.cx + dx, c.cz + dz);
            if (!br::gen::shaft_floor_open(sh, level)) continue;  // not a shaft you can fall into here
            const float sxc = static_cast<float>(c.cx + dx) * contracts::kChunkSize + (static_cast<float>(sh.cell_i) + 0.5f) * cs;
            const float szc = static_cast<float>(c.cz + dz) * contracts::kChunkSize + (static_cast<float>(sh.cell_j) + 0.5f) * cs;
            const float d = std::sqrt((px - sxc) * (px - sxc) + (pz - szc) * (pz - szc));
            if (d < best) best = d;
        }
    const float kFar = 8.0f, kNear = 1.0f;  // start ~2 cells out, full at the lip
    if (best >= kFar) return 0.0f;
    if (best <= kNear) return 1.0f;
    return (kFar - best) / (kFar - kNear);
}

// GLM 01 Tier 1 — has the path-tracer view changed enough to need an accumulator reset? When the camera is
// still (no move/look) the interactive PT path should KEEP accumulating (reset=false) so the image converges
// to a clean frame at 1 spp/frame instead of 4-spp-from-scratch every frame (the noise+cost root cause). A
// tiny epsilon absorbs float wobble; head-bob/mouse-look move the pose past it, which correctly resets.
static inline bool pt_view_moved(const contracts::CameraPose& a, const contracts::CameraPose& b) {
    const float dp = (a.pos[0] - b.pos[0]) * (a.pos[0] - b.pos[0]) + (a.pos[1] - b.pos[1]) * (a.pos[1] - b.pos[1])
                   + (a.pos[2] - b.pos[2]) * (a.pos[2] - b.pos[2]);
    return dp > 1e-8f || std::fabs(a.yaw - b.yaw) > 1e-5f || std::fabs(a.pitch - b.pitch) > 1e-5f;
}

int run_play(const Options& o) {
    using namespace br::core;
    using namespace std::chrono;
    HWND hwnd = create_window(o.width, o.height);
    if (!hwnd) { std::fprintf(stderr, "window creation failed\n"); return 1; }
    SetForegroundWindow(hwnd); SetFocus(hwnd);
    register_raw_mouse(hwnd);   // relative-delta look (no cursor-position spin)
    Renderer renderer;
    if (!renderer.init_windowed(hwnd, o.width, o.height)) { std::fprintf(stderr, "init: %s\n", renderer.last_error().c_str()); return 1; }
    renderer.set_texture_seed(o.seed);

    br::stream::StreamManager sm(o.seed, static_cast<int>(o.radius), o.workers);
    WorldState s(o.seed);
    s.wanderer.pos = Vec3{ 2.0f, kWandererHalfHeight + 0.02f, 2.0f };

    std::vector<Aabb> collision;
    contracts::ChunkKey cached{ 0, static_cast<int64_t>(1) << 40, 0 };
    auto rebuild = [&](contracts::ChunkKey c) { build_walk_collision(collision, o.seed, c); app::ladder::apply_to_collision(collision, c); cached = c; };  // M30 holed floor + the infinite 45-deg ladder
    // M19: lazy DXR for --play --rt (ray tracing at 2/3 internal res, upscaled to the window).
    std::unique_ptr<br::render_dxr::DxrRenderer> dxr;
    uint32_t dxrW = 0, dxrH = 0;
    contracts::ChunkKey dxrCenter{0, static_cast<int64_t>(1) << 40, 0};
    contracts::CameraPose dxrPrevCam{}; bool dxrHaveCam = false;  // GLM 01 Tier 1: PT temporal-accumulation state
    bool rtOn = o.rt;
    uint64_t rtFrames = 0;
    std::vector<uint8_t> lastRt;
    // M20b: a Shoggoth hunts the wanderer, its procedural body rendered in-world.
    app::Shoggoth shog;
    shog.pos = s.wanderer.pos; shog.pos.x += 22.0f; shog.pos.z += 6.0f;  // spawn a few cells away
    std::vector<contracts::ChunkVertex> shogBody;
    contracts::ChunkKey c0 = contracts::chunk_key_at(contracts::level_from_y(s.wanderer.pos.y), s.wanderer.pos.x, s.wanderer.pos.z);
    rebuild(c0);
    sm.update(c0); sm.wait_idle(); sm.update(c0);

    const float aspect = static_cast<float>(o.width) / static_cast<float>(o.height);
    const float kSens = 0.0022f;  // mouse radians/pixel
    ShowCursor(FALSE);
    POINT ctr{ static_cast<LONG>(o.width / 2), static_cast<LONG>(o.height / 2) };
    auto recenter = [&]() { POINT p = ctr; ClientToScreen(hwnd, &p); SetCursorPos(p.x, p.y); ++g_cursorWarps; };
    recenter();
    long rawDX = 0, rawDY = 0;   // relative mouse delta this frame (WM_INPUT) -- look without cursor-position spin

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
            accumulate_raw_mouse(msg, rawDX, rawDY);
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

        const float look_yaw = static_cast<float>(rawDX) * kSens;
        const float look_pitch = -static_cast<float>(rawDY) * kSens;
        rawDX = 0; rawDY = 0;
        // (no per-frame SetCursorPos: raw input doesn't read cursor position, so warping it only fights the user)

        const auto now = steady_clock::now();
        const double frame_ms = duration<double, std::milli>(now - prev).count();
        accum += duration<float>(now - prev).count();
        prev = now;
        if (accum > 0.25f) accum = 0.25f;  // clamp the spiral of death
        bool firstTick = true;
        while (accum >= tickDt) {
            const contracts::ChunkKey here = contracts::chunk_key_at(contracts::level_from_y(s.wanderer.pos.y), s.wanderer.pos.x, s.wanderer.pos.z);
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

        const int32_t curLevel = contracts::level_from_y(s.wanderer.pos.y);
        if (audioOn) eng.set_draft(draft_intensity_near_shaft(o.seed, curLevel, s.wanderer.pos.x, s.wanderer.pos.z));  // M30 telegraph
        const int32_t extraLevel = (s.wanderer.pos.y - contracts::level_base_y(curLevel) > 2.0f)
                                       ? curLevel + 1 : curLevel - 1;  // M28: climbing -> above, else see down
        const contracts::ChunkKey center = contracts::chunk_key_at(curLevel, s.wanderer.pos.x, s.wanderer.pos.z);
        const br::gen::ShaftSpec shaft = br::gen::shaft_at(o.seed, center.cx, center.cz);  // M30: open the abyss band over a shaft
        if (shaft.present && curLevel > shaft.top_level - shaft.depth && curLevel <= shaft.top_level) {
            const int32_t below = curLevel - shaft.top_level + shaft.depth;  // floors of void beneath
            sm.update(center, curLevel - ((below < 4) ? below : 4), curLevel);
        } else {
            sm.update(center, extraLevel);  // M28: the wanderer's floor + one adjacent
        }
        contracts::CameraPose cam = wanderer_camera(s, aspect);
        apply_head_bob(cam, s);  // M18 head-bob (view-only)
        if (rtOn) {  // M19: ray-traced path (DXR at 2/3 res, upscaled present)
            const uint32_t rw = (o.width * 2u) / 3u, rh = (o.height * 2u) / 3u;
            if (!dxr || dxrW != rw || dxrH != rh) {
                dxr = std::make_unique<br::render_dxr::DxrRenderer>();
                if (renderer.native_device5() && dxr->init(rw, rh, renderer.native_device5())) { dxrW = rw; dxrH = rh; dxrCenter = contracts::ChunkKey{0, static_cast<int64_t>(1) << 40, 0}; }   // RT_PERF item A: share the raster Device5 (else RT off)
                else { rtOn = false; dxr.reset(); }  // no DXR -> raster fallback
            }
            if (dxr) {
                const bool sceneRebuilt = (center != dxrCenter);
                if (sceneRebuilt) { dxr->build_scene(sm.resident()); dxrCenter = center; }
                // M25: the Shoggoth's body in RT -- a dynamic creature BLAS updated each
                // frame (the chunk BLASes stay cached, so this stays cheap), material 7 so
                // the PT shades it salmon. It shows + writhes in the ray-traced path too.
                app::build_shoggoth_mesh(shogBody, shog.pos, shog.writhe, 1.4f);
                for (auto& v : shogBody) v.material = 7.0f;
                dxr->update_creature(shogBody.data(), static_cast<uint32_t>(shogBody.size()));
                // GLM 01 Tier 1: temporal accumulation -- reset only on view-move / scene-rebuild / first frame, so a
                // static view converges clean at 1 spp/frame instead of 4-spp-from-scratch (see pt_view_moved + run_game).
                const bool ptReset = !dxrHaveCam || sceneRebuilt || pt_view_moved(cam, dxrPrevCam);
                dxr->render_pt_frame(cam, ptReset ? 4u : 1u, static_cast<uint32_t>(o.seed) + static_cast<uint32_t>(frames),
                                     ptReset, true, static_cast<uint32_t>(frames), /*aa=*/true, /*stochastic_lights=*/true,
                                     /*want_readback=*/!o.out.empty());   // E35: --out capture is the only reader here
                dxrPrevCam = cam; dxrHaveCam = true;
                // RT_PERF item A: present the PT output as a same-device GPU texture (no per-frame CPU readback).
                if (renderer.present_pt_texture(dxr->pt_output(), /*draw_caption=*/false)) {
                    ++rtFrames;
                    if (!o.out.empty()) dxr->readback(lastRt);  // optional --out capture only (rare; readback just then)
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

// Downscale a clean RGBA frame (the RT readback) to the proven 384x216 vision size, PNG-encode it in
// memory, and base64 it -- the payload director::keel_complete_vision wants. Box-averages source pixels
// (cheap; runs once per ~28 s cadence on the frame thread). Empty string on bad input.
static std::string encode_pov_b64(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
    constexpr uint32_t SW = 384, SH = 216;
    if (w == 0 || h == 0 || rgba.size() < static_cast<size_t>(w) * h * 4u) return std::string();
    std::vector<uint8_t> dst(static_cast<size_t>(SW) * SH * 4u);   // NB: not "small" -- dxgi.h pulls rpcndr.h's `#define small char`
    for (uint32_t y = 0; y < SH; ++y) {
        uint32_t sy0 = y * h / SH, sy1 = (y + 1u) * h / SH;
        if (sy1 <= sy0) sy1 = sy0 + 1u;
        if (sy1 > h) sy1 = h;
        for (uint32_t x = 0; x < SW; ++x) {
            uint32_t sx0 = x * w / SW, sx1 = (x + 1u) * w / SW;
            if (sx1 <= sx0) sx1 = sx0 + 1u;
            if (sx1 > w) sx1 = w;
            uint32_t acc[4] = {0, 0, 0, 0}, n = 0;
            for (uint32_t sy = sy0; sy < sy1; ++sy)
                for (uint32_t sx = sx0; sx < sx1; ++sx) {
                    const uint8_t* p = &rgba[(static_cast<size_t>(sy) * w + sx) * 4u];
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3]; ++n;
                }
            uint8_t* d = &dst[(static_cast<size_t>(y) * SW + x) * 4u];
            const uint32_t den = n ? n : 1u;
            for (int k = 0; k < 4; ++k) d[k] = static_cast<uint8_t>(acc[k] / den);
        }
    }
    std::vector<uint8_t> png;
    stbi_write_png_to_func([](void* ctx, void* data, int size) {
        auto* v = static_cast<std::vector<uint8_t>*>(ctx);
        const uint8_t* p = static_cast<const uint8_t*>(data);
        v->insert(v->end(), p, p + size);
    }, &png, static_cast<int>(SW), static_cast<int>(SH), 4, dst.data(), static_cast<int>(SW) * 4);
    if (png.empty()) return std::string();
    return app::base64_encode(png.data(), png.size());
}

// The entity-aware sensor line for the vision prompt (the operator's "observation + entity-aware" choice):
// when the Shoggoth is within ~28 m, a short non-visual cue the narrator MAY weave in; empty (pure
// observation) when it is far. Distance only -- the creature may ALSO be in the frame (RT material 7).
static std::string vision_entity_context(const app::Shoggoth& shog, const br::core::Vec3& wanderer) {
    const float dx = shog.pos.x - wanderer.x, dz = shog.pos.z - wanderer.z;
    const float dist = std::sqrt(dx * dx + dz * dz);
    if (dist > 28.0f) return std::string();
    const char* prox = (dist < 8.0f) ? "very close" : (dist < 16.0f ? "near" : "in the area");
    char buf[96];
    std::snprintf(buf, sizeof(buf), "a large living entity is %s, roughly %d metres away.", prox, static_cast<int>(dist + 0.5f));
    return std::string(buf);
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
        // First launch (no config): default to the monitor's NATIVE resolution (DPI-independent
        // via EnumDisplaySettings); fall back to the flag size. The user can change it in Settings.
        DEVMODE dm = {}; dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsWidth > 0) {
            cfg.width = static_cast<int>(dm.dmPelsWidth); cfg.height = static_cast<int>(dm.dmPelsHeight);
        } else {
            cfg.width = static_cast<int>(o.width); cfg.height = static_cast<int>(o.height);
        }
        cfg.seed = o.seed;
        cfg = app::sanitize(cfg);
    }

    uint32_t curW = static_cast<uint32_t>(cfg.width), curH = static_cast<uint32_t>(cfg.height);
    HWND hwnd = create_window(curW, curH);
    if (!hwnd) { std::fprintf(stderr, "window creation failed\n"); return 1; }
    SetForegroundWindow(hwnd); SetFocus(hwnd);
    register_raw_mouse(hwnd);   // first-person look reads relative WM_INPUT deltas (no cursor-position spin)
    Renderer renderer;
    if (!renderer.init_windowed(hwnd, curW, curH)) {
        std::fprintf(stderr, "init: %s\n", renderer.last_error().c_str()); return 1;
    }

    app::MenuModel model;
    model.seed = cfg.seed;
    model.settings.master_pct = cfg.master; model.settings.sfx_pct = cfg.sfx;
    model.settings.mouse_pct = cfg.mouse; model.settings.director = (cfg.director || o.director) ? 1 : 0;  // --director forces on
    model.settings.rt = o.rt ? 1 : cfg.renderer;  // M19: --rt forces on; else from config
    model.settings.res_w = cfg.width; model.settings.res_h = cfg.height;  // the in-menu resolution picker
    model.settings.model_tier = cfg.model_tier;   // AI model tier (0 AUTO / 1 9B / 2 4B); drives g_modelTier (read at sidecar launch)
    g_modelTier.store(cfg.model_tier);

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
    auto rebuild = [&](contracts::ChunkKey c) { build_walk_collision(collision, texSeed, c); app::ladder::apply_to_collision(collision, c); cached = c; };  // M30 holed floor + the infinite 45-deg ladder
    uint64_t prevSteps = 0;
    auto start_session = [&](uint64_t seed) {
        texSeed = seed;
        renderer.set_texture_seed(seed);
        sm = std::make_unique<br::stream::StreamManager>(seed, static_cast<int>(o.radius), o.workers);
        s = WorldState(seed);
        s.wanderer.pos = Vec3{ 2.0f, kWandererHalfHeight + 0.02f, 2.0f };
        const contracts::ChunkKey c0 = contracts::chunk_key_at(contracts::level_from_y(s.wanderer.pos.y), s.wanderer.pos.x, s.wanderer.pos.z);
        rebuild(c0);
        sm->update(c0); sm->wait_idle(); sm->update(c0);
        prevSteps = footstep_count(s);
    };

    audio::AudioEngine eng(model.seed, contracts::kAudioSampleRate);
    bool audioOn = o.no_audio ? false : eng.start_device(false);

    const float tickDt = 1.0f / 120.0f;
    // First-person look comes from raw WM_INPUT deltas (rawDX/rawDY) accumulated in the message pump. The
    // cursor is hidden + clipped to the window while playing; recenter() just parks it, but the LOOK no
    // longer depends on its position -- so a parked/warped/edge-clamped cursor can't spin the view.
    auto recenter = [&]() { POINT p{ static_cast<LONG>(curW / 2), static_cast<LONG>(curH / 2) }; ClientToScreen(hwnd, &p); SetCursorPos(p.x, p.y); ++g_cursorWarps; };
    auto clip_to_window = [&](bool on) {
        if (!on) { ClipCursor(nullptr); return; }
        RECT rc; if (!GetClientRect(hwnd, &rc)) return;
        POINT tl{ rc.left, rc.top }, brp{ rc.right, rc.bottom };
        ClientToScreen(hwnd, &tl); ClientToScreen(hwnd, &brp);
        RECT scr{ tl.x, tl.y, brp.x, brp.y }; ClipCursor(&scr);
    };
    long rawDX = 0, rawDY = 0;   // relative mouse delta accumulated this frame (from WM_INPUT)
    bool cursorHidden = false, firstPlayLook = true;
    bool prevF11 = false, prevPadStart = false, prevF2 = false, prevF = false;
    bool flashOn = false, dxrFlashApplied = false;   // F: ray-traced eye-torch flashlight (RT only, off by default)
    app::FlareField flares; bool prevR = false, flaresChanged = false;   // R: green flare breadcrumbs (RT-lit, ring-recycled)

    // M19: lazy DXR renderer for the ray-tracing toggle. Rendered at a reduced internal
    // resolution (perf) + upscaled to the window by present_overlay_windowed. Scene rebuilt
    // when the chunk center changes; accumulation resets when the camera moves.
    std::unique_ptr<br::render_dxr::DxrRenderer> dxr;
    uint32_t dxrW = 0, dxrH = 0;
    contracts::ChunkKey dxrCenter{0, static_cast<int64_t>(1) << 40, 0};
    contracts::CameraPose dxrPrevCam{}; bool dxrHaveCam = false;  // GLM 01 Tier 1: PT temporal-accumulation state
    // RT internal-resolution scale (F3 cycles): the path tracer renders at num/den of the window, upscaled on present.
    // Lower = proportionally fewer primary/GI/shadow rays -- the "render low / display high" perf knob; the temporal
    // accumulation + denoiser carry quality. Default Quality (2/3) = the prior hardcoded behavior.
    static const struct { uint32_t num, den; const char* name; } kRtScales[] = { {2,3,"Quality"}, {1,2,"Balanced"}, {1,3,"Performance"} };
    int rtScaleIdx = (o.rt_scale < 3u) ? static_cast<int>(o.rt_scale) : 0; bool prevF3 = false;
    bool vsyncOn = !o.no_vsync; bool prevV = false;   // V toggles vsync; OFF = uncapped FPS (real perf, lower latency, tearing)
    renderer.set_vsync(vsyncOn);
    app::Shoggoth shog;                       // M20b: the hunting creature (spawned on New Game)
    std::vector<contracts::ChunkVertex> shogBody;
    std::vector<contracts::ChunkVertex> ladderMesh; int64_t ladderCell = (static_cast<int64_t>(1) << 62);  // the infinite ladder's render mesh (rebuilt only when the player crosses a 24 m cell)
    std::vector<std::vector<contracts::ChunkVertex>> ladderCarvePool;  // persistent per-frame scratch: band-carved copies of the cz==0 world chunks

    struct Keys { bool up=false, down=false, left=false, right=false, enter=false, esc=false; } prev;
    auto edge = [](bool now, bool& p) { const bool e = now && !p; p = now; return e; };

    const auto t_start = steady_clock::now();
    auto prevt = t_start;
    float accum = 0.0f;
    const bool timed = (o.seconds > 0);
    uint64_t frames = 0;
    uint64_t rtFrames = 0;   // ADR-077: count live windowed ray-traced presents so the M30 gate can prove the
                             // dual-device RT path actually runs (M9 only ever exercised DXR in isolation).
    bool running = true;

    // M21b: the LIVE async brain — KEEL inference off the frame thread so the creature
    // thinks while you actually play (mirrors the Director's async host). On by default;
    // --no-shoggoth-brain kills it; graceful no-op if KEEL is down. Only fed while in Play.
    // Phase C.2: ONE shared KeelBroker arbitrates the single llama-server backend across all live hosts
    // (priority: player-speech > shoggoth-vision > director-vision > shoggoth-brain; single multimodal slot;
    // concurrency cap). Declared FIRST so it outlives every host (destroyed last); shut down explicitly at
    // cleanup before the hosts join. Each host below takes broker.get() (nullptr would be the legacy direct path).
    auto broker = std::make_unique<app::KeelBroker>();
    std::unique_ptr<app::ShoggothBrainHost> brain;
    if (!o.no_shoggoth_brain) {
        try_start_sidecar();   // best-effort: bring the LLM up so the creature can think (graceful if it can't)
        std::string bh; int bp; parse_host_port(o.director_url, bh, bp);
        brain = std::make_unique<app::ShoggothBrainHost>(bh, bp, 8000u, broker.get());
    }
    const auto brain_interval = milliseconds(3000);
    auto last_brain = t_start - brain_interval;
    uint64_t brain_intents = 0;
    std::string lastShogUtter;                       // Phase E: the creature's last spoken murmur (de-dupe)
    auto lastShogSpoke = t_start - milliseconds(10000);  // + a cooldown so it murmurs, never chatters
    LlmProbe llmProbe;   // Settings "Test Connection" -> async LLM ping (status shown in the Settings overlay)
    MicProbe micProbe;   // Settings "Test Microphone" -> async mic->whisper->Director->caption + spoken reply

    // Director (M11) narration: when Director is ON, ask the LLM (off the frame thread) for a Directive every
    // ~18 s and SPEAK its caption through the procedural PA voice -- narration you HEAR while you walk. Created
    // lazily on first Play frame with Director on; graceful no-op if the sidecar is down.
    std::unique_ptr<br::director::DirectorHost> director;
    const auto director_interval = milliseconds(18000);
    auto last_director = t_start - director_interval;   // fire the first line promptly
    std::string last_pa_line;
    // VLM-vision narration (RT path): the Director SEES the player's rendered frame and narrates ONLY what
    // is visible (director_vision.h) -- the grounded counterpart to the text DirectorHost above, which stays
    // as the RASTER fallback (the windowed raster path has no frame readback to send). The clean RT readback
    // is downscaled + handed to an off-thread Qwen-VL narrator every ~28 s. Created lazily on the first Play
    // frame with Director + RT on; graceful no-op if the VLM/sidecar is down.
    std::unique_ptr<app::DirectorVisionHost> visionDir;
    const auto vision_interval = milliseconds(28000);                  // sparse ~28 s cadence (hides VLM latency)
    auto last_vision = t_start - vision_interval + milliseconds(8000);   // first POV ~8 s in
    // Phase D LIVE: the creature reasons from a RENDERED POV, not just its text sense. A 2nd headless device
    // renders the Shoggoth's vantage offscreen (shoggoth_vision.h camera) and an off-thread qwen-VL host
    // (shoggoth_vision_host.h) turns it into a validated intent whose target_kind drives resolve_target -- so
    // its MOTION follows what it SEES. The chunk upload is budget-spread across a short warm window (never a
    // big-budget stall on the frame thread). Gated on the live brain + a vision-capable KEEL; graceful no-op
    // otherwise (the text brain keeps driving). Presentation only (INV-1 untouched), like the live text brain.
    std::unique_ptr<app::ShoggothVisionHost> shogVision;
    std::unique_ptr<Renderer> shogPov;                                   // 2nd headless device for the creature's vantage
    const auto svision_interval = milliseconds(25000);                   // sparse; offset from the Director's ~28 s vision
    auto last_svision = t_start - svision_interval + milliseconds(6000);  // first creature POV ~6 s in
    bool svWarming = false; int svWarmFrames = 0;                         // warm-window state: spread the upload, then snapshot
    constexpr int kSvWarmFrames = 24;                                    // frames to spread the chunk upload over (no hitch)
    constexpr uint32_t kSvBudget = 16u;                                  // chunk meshes uploaded per warm frame
    uint64_t svision_intents = 0;                                        // vision intents applied to the live creature
    // Phase H / apparition Phase 2a: the PLAYER's-POV apparition read. Every 3rd creature-vision cycle renders the
    // wanderer's view (not the creature's) on the SAME 2nd device, so the apparition sense runs on WHAT THE PLAYER
    // SEES; a present verdict makes the PA murmur about it and thins the soundscape for a few decaying seconds --
    // "it reacted to the face I was looking at". Live + presentation-only (INV-1 untouched), like the flares/voice.
    uint64_t svCycle = 0; bool svInFlightPlayer = false;                 // cadence parity + the in-flight read's mode
    auto apparitionUntil = t_start;                                      // soundscape stays thinned until here (decaying)
    auto lastApparitionSpoke = t_start - milliseconds(60000);            // murmur cooldown so it never chatters
    uint8_t apparitionKind = 0, apparitionSector = 0, apparitionStrength = 0;  // last player-POV verdict (telemetry / Phase 2b)
    float apparitionWindowS = 9.0f;                                      // active dip window length (strength-scaled), the decay normalizer
    uint64_t svision_player_reads = 0, apparition_hits = 0;             // telemetry: player reads done / apparitions seen
    // ADR-074: two-way voice. A live mic + VAD (mic_capture.h) feeds an off-thread whisper+VLM
    // conversation host (director_chat.h); the reply is spoken back through the PA voice. Created
    // lazily with Director on; graceful no-op if there's no mic / the VLM is down. Echo-gated via
    // g_paSuspendUntilMs so the Director never transcribes its own voice.
    std::unique_ptr<app::MicCapture> mic;
    std::unique_ptr<app::DirectorChatHost> chat;
    bool voiceTried = false;             // open the device once (don't retry a missing mic every frame)
    bool wantChatPov = false;            // an utterance awaits the next RT POV grab
    std::string pendingChatWav, pendingChatCtx;
    uint32_t micUtterN = 0;              // rotating WAV name per utterance (bounded, no cross-turn race)
    const std::string wexe = o.whisper_exe.empty() ? default_whisper_exe() : o.whisper_exe;
    const std::string wmodel = o.whisper_model.empty() ? default_whisper_model() : o.whisper_model;
    char tmpbuf[MAX_PATH]; const DWORD tmpn = GetTempPathA(MAX_PATH, tmpbuf);
    const std::string tmpDir = (tmpn > 0 && tmpn < MAX_PATH) ? std::string(tmpbuf) : std::string("runs\\");
    uint64_t director_spoke = 0;
    std::string capText;                 // the current on-screen subtitle text (Settings -> Subtitles)...
    auto capUntil = t_start;             // ...shown until this time (a few seconds per line)
    std::vector<uint8_t> capOvl;         // its rasterised RGBA, uploaded to the renderer when the line changes
    std::string lastCapUploaded;         // RT_PERF item A: the caption text last uploaded to the GPU overlay (upload only on change)

    // Headless spin guard (--auto-play): drop straight into Play and watch the mouse-look delta with a
    // STILL mouse. Raw input emits nothing when the mouse is idle, so the delta must stay ~0 rad/frame; the
    // old cursor-recenter scheme pinned it at the ±0.5 clamp. Lets the build verify "no runaway spin" headless.
    float maxDyaw = 0.0f; uint64_t lookFrames = 0, clampedFrames = 0;
    const long warps0 = g_cursorWarps;   // baseline: count SetCursorPos warps DURING this Play session (must stay tiny)
    if (o.auto_play) {
        start_session(model.seed);
        shog = app::Shoggoth{};
        shog.pos = s.wanderer.pos; shog.pos.x += 22.0f; shog.pos.z += 6.0f;
        model.screen = app::Screen::Play;
    }

    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            accumulate_raw_mouse(msg, rawDX, rawDY);   // relative look delta (immune to cursor position)
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (!running) break;
        if (timed && steady_clock::now() >= t_start + seconds(static_cast<long long>(o.seconds))) break;

        const bool inPlay = (model.screen == app::Screen::Play);
        const bool focused = (GetForegroundWindow() == hwnd);   // capture + Play input ONLY while we are foreground
        const bool wantCapture = inPlay && focused;
        // Capture is strictly EDGE-PAIRED -- ShowCursor keeps a SIGNED counter, so it must never be called
        // unbalanced (else the cursor strands visible/hidden). Hide+clip on gaining capture, show+release on
        // losing it (alt-tab releases instantly -> the cursor is free on the desktop, never trapped).
        if (wantCapture && !cursorHidden) { ShowCursor(FALSE); recenter(); clip_to_window(true); rawDX = rawDY = 0; cursorHidden = true; firstPlayLook = true; }
        if (!wantCapture && cursorHidden) { ShowCursor(TRUE); clip_to_window(false); cursorHidden = false; }
        if (cursorHidden) clip_to_window(true);   // re-assert the confine each frame (Windows drops it on focus/mode changes); ClipCursor never MOVES the cursor, so it cannot fight

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

        // F11 toggles borderless fullscreen (resizes the swapchain back buffers). Foreground-gated so it can't
        // fire while you are alt-tabbed away; the per-frame clip re-assert handles the confine after the resize.
        const bool f11 = focused && (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (f11 && !prevF11) { apply_fullscreen(!isFull); if (cursorHidden) clip_to_window(true); }
        prevF11 = f11;
        const bool f2 = focused && (GetAsyncKeyState(VK_F2) & 0x8000) != 0;  // M19: toggle ray tracing
        if (f2 && !prevF2) model.settings.rt ^= 1;
        prevF2 = f2;
        const bool f3 = focused && (GetAsyncKeyState(VK_F3) & 0x8000) != 0;  // F3: cycle RT internal resolution (Quality 2/3 -> Balanced 1/2 -> Performance 1/3)
        if (f3 && !prevF3) { rtScaleIdx = (rtScaleIdx + 1) % 3; dxrHaveCam = false; }  // res change -> the rw/rh below re-inits the DxrRenderer; force a clean accumulator reset
        prevF3 = f3;
        const bool vkey = focused && (GetAsyncKeyState('V') & 0x8000) != 0;  // V: toggle vsync (OFF = uncapped FPS / lower latency / tearing)
        if (vkey && !prevV) { vsyncOn = !vsyncOn; renderer.set_vsync(vsyncOn); }
        prevV = vkey;
        const bool fkey = focused && (GetAsyncKeyState('F') & 0x8000) != 0;  // F: toggle the RT flashlight (eye-torch)
        if (fkey && !prevF) flashOn = !flashOn;
        prevF = fkey;
        const bool rkey = focused && (GetAsyncKeyState('R') & 0x8000) != 0;  // R: drop a green flare breadcrumb at your feet (RT-lit)
        if (rkey && !prevR && model.screen == app::Screen::Play) {
            const int32_t flvl = contracts::level_from_y(s.wanderer.pos.y);
            flares.drop(Vec3{ s.wanderer.pos.x, contracts::level_base_y(flvl) + 0.15f, s.wanderer.pos.z });  // on the floor
            flaresChanged = true;   // lighting changed -> the PT accumulator must re-converge (see the RT block)
        }
        prevR = rkey;
        g_modelTier.store(model.settings.model_tier);  // a Settings change applies at the next sidecar launch (so a pre-Play menu pick takes effect this session)

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
                last_vision = steady_clock::now() - vision_interval + milliseconds(8000);  // first VLM POV ~8 s into a fresh session
            }
            else if (cmd == app::UiCommand::QuitApp) running = false;
            else if (cmd == app::UiCommand::TestConnection) {   // Settings: ping the LLM (async, off the UI thread)
                std::string h; int p; parse_host_port(o.director_url, h, p);
                llmProbe.start(h, p);
            }
            else if (cmd == app::UiCommand::TestMic) {          // Settings: full voice-loop test (mic -> caption + voice)
                std::string h; int p; parse_host_port(o.director_url, h, p);
                micProbe.start(h, p, wexe, wmodel);
            }
            // ResumeGame keeps the existing session as-is.
        }
        llmProbe.sync(model);   // reflect the latest LLM probe status into the Settings overlay
        micProbe.sync(model);   // reflect the mic-test status into the Settings overlay
        { const std::string spoke = micProbe.take_fresh_reply(); if (!spoke.empty()) speak_pa(spoke, contracts::kAudioSampleRate); }  // speak the reply (diagnostic)
        if (audioOn) {
            // Phase H/2a atmosphere: while a recent PLAYER-POV apparition lingers, thin the soundscape -- a soft
            // decaying dip to 0.6x, never a hard cut. Presentation-only; touches no sim/replay state.
            float atmoDip = 1.0f;
            const auto tnow = steady_clock::now();
            if (tnow < apparitionUntil) {
                const float frac = clampf(duration<float>(apparitionUntil - tnow).count() / apparitionWindowS, 0.0f, 1.0f);
                const float maxDip = 0.20f + 0.13f * static_cast<float>(apparitionStrength);  // strength 1->0.33 .. 3->0.59 deep
                atmoDip = 1.0f - maxDip * frac;   // deeper + longer for a vivid apparition, easing back to 1.0x
            }
            eng.set_master_volume(atmoDip * static_cast<float>(model.settings.master_pct) / 100.0f);
            eng.set_sfx_volume(atmoDip * static_cast<float>(model.settings.sfx_pct) / 100.0f);
        }
        if (!running) break;

        const auto now = steady_clock::now();
        accum += duration<float>(now - prevt).count();
        prevt = now;
        if (accum > 0.25f) accum = 0.25f;

        if (model.screen == app::Screen::Play && sm) {
            const float aspect = static_cast<float>(curW) / static_cast<float>(curH);
            const float kSens = 0.0010f + 0.0030f * (static_cast<float>(model.settings.mouse_pct) / 100.0f);
            // Look from the raw mouse delta accumulated this frame. The look value does NOT depend on where the
            // cursor is (no SetCursorPos here) -- so it can neither self-spin NOR fight the user's cursor.
            float look_yaw = static_cast<float>(rawDX) * kSens;
            float look_pitch = -static_cast<float>(rawDY) * kSens;
            rawDX = 0; rawDY = 0;
            // Defence in depth: drop the first Play frame's delta, then clamp, so no single spike whips the view.
            if (firstPlayLook) { look_yaw = 0.0f; look_pitch = 0.0f; firstPlayLook = false; }
            look_yaw = clampf(look_yaw, -0.5f, 0.5f);
            look_pitch = clampf(look_pitch, -0.5f, 0.5f);
            if (o.auto_play) { const float a = std::fabs(look_yaw); if (a > maxDyaw) maxDyaw = a; if (a >= 0.49f) ++clampedFrames; ++lookFrames; }  // spin guard: measure BEFORE the focus gate
            contracts::InputCommand in{};
            if (focused) {   // ignore Play input while alt-tabbed -- a normal game doesn't walk/look in the background
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
            } else { look_yaw = 0.0f; look_pitch = 0.0f; }   // not foreground -> no look either
            bool firstTick = true;
            while (accum >= tickDt) {
                const contracts::ChunkKey here = contracts::chunk_key_at(contracts::level_from_y(s.wanderer.pos.y), s.wanderer.pos.x, s.wanderer.pos.z);
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
                for (app::ShoggothIntent it : brain->poll()) {
                    // Vision owns the SEEN target: the perception fields drive resolve_target, so the
                    // text brain (which cannot see) only updates action/aggression/voice -- its 3 s
                    // cadence can't clobber what the creature saw between the sparse ~25 s vision frames.
                    // Behaviour-neutral when vision is off (these stay None/defaults, as before).
                    it.target_kind = shog.intent.target_kind; it.sector = shog.intent.sector;
                    it.proximity = shog.intent.proximity; it.snap = shog.intent.snap;
                    shog.intent = it; ++brain_intents;
                }
                // Phase E: the creature SPEAKS its murmur when the wanderer is NEAR (<6 m), it has
                // something NEW to say, and a cooldown has passed -- impressionistic dread, not chatter.
                // Presentation-only (no sim state, not hashed/logged) -> determinism untouched.
                if (audioOn && !shog.intent.utterance.empty() && shog.intent.utterance != lastShogUtter) {
                    const float vdx = s.wanderer.pos.x - shog.pos.x, vdz = s.wanderer.pos.z - shog.pos.z;
                    if (vdx * vdx + vdz * vdz < 36.0f && now - lastShogSpoke > milliseconds(7000)) {
                        speak_pa(shog.intent.utterance, contracts::kAudioSampleRate);
                        lastShogUtter = shog.intent.utterance;
                        lastShogSpoke = now;
                    }
                }
            }
            // Phase D LIVE: the creature reasons from a RENDERED POV. Render its vantage offscreen on a 2nd
            // headless device, then every ~25 s hand the snapshot to the off-thread qwen-VL host. The chunk
            // upload is budget-spread across a short warm window (kSvWarmFrames frames at kSvBudget meshes each)
            // so the frame thread never eats a big-budget stall. The returned intent (with target_kind) is
            // applied to shog.intent -> resolve_target -> its MOTION follows what it SEES. Gated on the live
            // brain + a vision-capable KEEL; graceful no-op otherwise (the text brain keeps driving). Presentation
            // only -- the intent enters via shog.intent exactly like the live text brain (INV-1 untouched).
            if (brain && g_visionAvailable.load()) {
                if (!shogVision) {
                    std::string vh; int vp; parse_host_port(o.director_url, vh, vp);
                    shogVision = std::make_unique<app::ShoggothVisionHost>(vh, vp, 30000u, broker.get());
                    shogPov = std::make_unique<Renderer>();
                    if (!shogPov->init_headless(384u, 216u)) shogPov.reset();   // no 2nd device -> graceful no-op
                    else shogPov->set_texture_seed(texSeed);
                }
                if (shogPov) {
                    // Phase H/2a: decide the cycle's vantage at warm-start and hold it for the window. Most cycles
                    // are the CREATURE's POV (its sight drives motion); every 3rd is the PLAYER's view, so the
                    // apparition read runs on what the PLAYER sees. A player cycle skips one ~25 s motion update --
                    // invisible for a slow lurker -- and the creature path is byte-unchanged on non-player cycles.
                    if (!svWarming && now - last_svision >= svision_interval) {
                        svWarming = true; svWarmFrames = kSvWarmFrames;
                        svInFlightPlayer = (svCycle % 3u) == 2u; ++svCycle;
                    }
                    const contracts::CameraPose svCam = svInFlightPlayer
                        ? br::core::wanderer_camera(s, 384.0f / 216.0f)
                        : app::shoggoth_pov_camera(shog, 384.0f / 216.0f);
                    if (svWarming) {
                        uint32_t drawn = 0;
                        if (!shogPov->render_chunks(svCam, sm->resident(), kSvBudget, s.tick, &drawn)) {
                            shogPov.reset(); svWarming = false;   // device lost -> stop (graceful; text brain drives on)
                        } else if (--svWarmFrames <= 0) {
                            svWarming = false; last_svision = now;
                            FrameImage img;
                            if (shogPov->readback(img)) {
                                const std::string b64 = encode_pov_b64(img.rgba, img.width, img.height);   // 384x216 -> PNG -> base64
                                if (!b64.empty()) shogVision->submit(b64, app::build_shoggoth_summary(shog, s.wanderer.pos, s.tick));
                            }
                        }
                    }
                }
                for (const app::ShoggothIntent& it : shogVision->poll()) {
                    if (svInFlightPlayer) {
                        // PLAYER-POV read: the creature reacts to what the PLAYER sees. Take ONLY the apparition
                        // verdict -- this frame is not the creature's vantage, so never touch its seen target/motion.
                        ++svision_player_reads;
                        if (it.apparition) {
                            ++apparition_hits;
                            apparitionKind = it.app_kind; apparitionSector = it.app_sector;
                            apparitionStrength = it.app_strength ? it.app_strength : 2;   // 1..3 (defensive default)
                            apparitionWindowS = 5.0f + 2.0f * static_cast<float>(apparitionStrength);   // 7..11 s, longer for a vivid one
                            apparitionUntil = now + milliseconds(static_cast<long long>(apparitionWindowS * 1000.0f));
                            // The PA murmurs only for a CLEAR/VIVID one (strength>=2) -- a faint hint shifts the
                            // atmosphere alone -- on a cooldown so it never chatters (facility-wide voice).
                            if (audioOn && apparitionStrength >= 2 && !it.utterance.empty() && it.utterance != lastShogUtter
                                && now - lastApparitionSpoke > milliseconds(12000)) {
                                speak_pa(it.utterance, contracts::kAudioSampleRate);
                                lastShogUtter = it.utterance; lastApparitionSpoke = now;
                            }
                        }
                    } else {
                        shog.intent = it; ++svision_intents;   // creature-POV: full intent drives motion (as before)
                    }
                }
            }
            // Director (M11) TEXT narration -- RASTER fallback only. The RT path uses the VLM-vision narrator
            // (below); when RT is off there is no frame readback to send, so raster keeps the text-stats Director.
            // Off-thread Directive -> SPEAK its caption via the procedural PA voice. Presentation only (INV-1 safe).
            if (model.settings.director && !(model.settings.rt && g_visionAvailable.load())) {
                if (!director) {
                    try_start_sidecar();
                    std::string dh; int dp; parse_host_port(o.director_url, dh, dp);
                    director = std::make_unique<br::director::DirectorHost>(dh, dp);
                }
                if (now - last_director >= director_interval) {
                    contracts::WandererSummary sum{};   // same shape as build_summary (which lives in a later anon ns)
                    sum.tick = s.tick; sum.world_seed = model.seed; sum.level = contracts::level_from_y(s.wanderer.pos.y);
                    const contracts::ChunkKey kk = contracts::chunk_key_at(sum.level, s.wanderer.pos.x, s.wanderer.pos.z);
                    sum.chunk_cx = kk.cx; sum.chunk_cz = kk.cz;
                    sum.biome = static_cast<int32_t>(br::gen::biome_at(model.seed, 0, kk.cx, kk.cz));
                    sum.distance_m = s.odometer; sum.dwell_seconds = 0.0f; sum.route_loops = 0;
                    sum.location_hash = static_cast<uint64_t>(kk.cx) * 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(kk.cz);
                    director->submit(sum);
                    last_director = now;
                }
                for (const contracts::Directive& d : director->poll()) {
                    if (d.caption[0] != '\0' && last_pa_line != d.caption) {
                        last_pa_line = d.caption;
                        if (audioOn) speak_pa(d.caption, contracts::kAudioSampleRate);   // PA voice
                        if (model.settings.subtitles) {                                  // + on-screen subtitle
                            capText = d.caption; capUntil = now + seconds(6);
                            app::build_caption_overlay(capOvl, curW, curH, capText);
                            renderer.upload_caption_overlay(capOvl.data(), curW, curH);  // upload once; drawing it is then free
                        }
                        ++director_spoke;
                    }
                }
            }
            // Director VISION (RT path): poll the off-thread Qwen-VL narrator. A fresh line is SPOKEN (PA voice)
            // and shown as a subtitle -- for RT we only set capText/capUntil here; the RT readback block below
            // composites it into the frame (raster uploads its own overlay). The POV is grabbed + submitted from
            // the clean readback further down. Presentation only (INV-1 safe).
            if (model.settings.director && model.settings.rt && g_visionAvailable.load()) {
                if (!visionDir) {
                    try_start_sidecar();
                    std::string dh; int dp; parse_host_port(o.director_url, dh, dp);
                    visionDir = std::make_unique<app::DirectorVisionHost>(dh, dp, 30000u, broker.get());
                }
                for (const std::string& line : visionDir->poll()) {
                    if (!line.empty() && last_pa_line != line) {
                        last_pa_line = line;
                        if (audioOn) speak_pa(line, contracts::kAudioSampleRate);
                        if (model.settings.subtitles) { capText = line; capUntil = now + seconds(8); }
                        ++director_spoke;
                    }
                }
            }
            // ADR-074: TWO-WAY VOICE -- the wanderer talks (mic + VAD), the Director answers IN REGISTER.
            // Lazy-create the mic + the off-thread conversation host with Director on; poll the mic for a
            // completed utterance (RT: grab the POV at readback so it answers about what you SEE; raster:
            // text-only turn); poll the host for a reply -> speak it + subtitle. All slow work is off-thread.
            if (model.settings.director) {
                if (!voiceTried) {
                    voiceTried = true;
                    try_start_sidecar();
                    std::string dh; int dp; parse_host_port(o.director_url, dh, dp);
                    mic = std::make_unique<app::MicCapture>();
                    if (!mic->start()) mic.reset();   // no capture device -> no voice (graceful)
                    chat = std::make_unique<app::DirectorChatHost>(dh, dp,
                        [wexe, wmodel](const std::string& w) { return whisper_transcribe(w, wexe, wmodel); }, 30000u, broker.get());
                }
                if (mic) {
                    mic->suspend_until(g_paSuspendUntilMs.load());   // half-duplex: don't re-hear the Director
                    std::vector<int16_t> utter;
                    if (mic->poll(utter)) {
                        char wp[MAX_PATH];
                        std::snprintf(wp, sizeof(wp), "%sbr_mic_%u.wav", tmpDir.c_str(), (micUtterN++ & 15u));
                        std::string err;
                        if (audio::write_wav(std::string(wp), utter, app::MicCapture::kRate, static_cast<uint16_t>(1), err)) {
                            const std::string ctx = vision_entity_context(shog, s.wanderer.pos);
                            if (model.settings.rt && g_visionAvailable.load()) { pendingChatWav = wp; pendingChatCtx = ctx; wantChatPov = true; }
                            else if (chat) chat->submit(std::string(wp), std::string(), ctx);   // raster / 4B: text-only turn
                        }
                    }
                }
                if (chat) {
                    for (const app::DirectorExchange& ex : chat->poll()) {
                        if (ex.reply.empty()) continue;
                        last_pa_line = ex.reply;
                        if (audioOn) speak_pa(ex.reply, contracts::kAudioSampleRate);
                        if (model.settings.subtitles) {
                            capText = ex.reply; capUntil = now + seconds(8);
                            if (!model.settings.rt) { app::build_caption_overlay(capOvl, curW, curH, capText); renderer.upload_caption_overlay(capOvl.data(), curW, curH); }
                        }
                        ++director_spoke;
                    }
                }
            }
            if (audioOn) {
                const uint64_t steps = footstep_count(s);
                eng.post(audio_listener(s), 1.2f, static_cast<uint32_t>(steps - prevSteps));
                prevSteps = steps;
            }
            const int32_t curLevel = contracts::level_from_y(s.wanderer.pos.y);
            if (audioOn) eng.set_draft(draft_intensity_near_shaft(texSeed, curLevel, s.wanderer.pos.x, s.wanderer.pos.z));  // M30 telegraph
            const int32_t extraLevel = (s.wanderer.pos.y - contracts::level_base_y(curLevel) > 2.0f)
                                           ? curLevel + 1 : curLevel - 1;  // M28: climbing -> above, else see down
            const contracts::ChunkKey center = contracts::chunk_key_at(curLevel, s.wanderer.pos.x, s.wanderer.pos.z);
            const br::gen::ShaftSpec shaft = br::gen::shaft_at(texSeed, center.cx, center.cz);  // M30: open the abyss band over a shaft
            const bool onLadder = (s.wanderer.pos.z > app::ladder::kAnchorZ - app::ladder::kHalfW - 2.0f
                                   && s.wanderer.pos.z < app::ladder::kAnchorZ + app::ladder::kHalfW + 2.0f);
            if (onLadder) {
                sm->update(center, curLevel - 2, curLevel + 1);  // infinite ladder: keep a few floors so the stairwell reads through them
            } else if (shaft.present && curLevel > shaft.top_level - shaft.depth && curLevel <= shaft.top_level) {
                const int32_t below = curLevel - shaft.top_level + shaft.depth;  // floors of void beneath
                sm->update(center, curLevel - ((below < 4) ? below : 4), curLevel);
            } else {
                sm->update(center, extraLevel);  // M28: the wanderer's floor + one adjacent
            }
            contracts::CameraPose cam = wanderer_camera(s, aspect);
            apply_head_bob(cam, s);  // M18 head-bob (view-only)
            // Director subtitle active this frame? Shown in BOTH the ray-traced and raster paths.
            const bool showCap = model.settings.director && model.settings.subtitles
                                 && !capText.empty() && now < capUntil;
            if (model.settings.rt) {
                // M19 ray-traced path: DXR at 2/3 internal res -> present (upscaled).
                const uint32_t rw = (curW * kRtScales[rtScaleIdx].num) / kRtScales[rtScaleIdx].den,
                               rh = (curH * kRtScales[rtScaleIdx].num) / kRtScales[rtScaleIdx].den;   // F3-cycled internal RT res
                if (!dxr || dxrW != rw || dxrH != rh) {
                    dxr = std::make_unique<br::render_dxr::DxrRenderer>();
                    if (renderer.native_device5() && dxr->init(rw, rh, renderer.native_device5())) { dxrW = rw; dxrH = rh; dxrCenter = contracts::ChunkKey{0, static_cast<int64_t>(1) << 40, 0}; }   // RT_PERF item A: share the raster Device5 (else RT off)
                    else { model.settings.rt = 0; dxr.reset(); }  // DXR unavailable -> raster fallback
                }
                if (dxr) {
                    const bool sceneRebuilt = (center != dxrCenter);
                    if (sceneRebuilt) { dxr->build_scene(sm->resident()); dxrCenter = center; }
                    // M25: the Shoggoth's body in RT -- a dynamic creature BLAS updated each
                    // frame (chunk BLASes stay cached), material 7 so the PT shades it salmon.
                    app::build_shoggoth_mesh(shogBody, shog.pos, shog.writhe, 1.4f);
                    for (auto& v : shogBody) v.material = 7.0f;
                    dxr->update_creature(shogBody.data(), static_cast<uint32_t>(shogBody.size()));
                    // GLM 01 Tier 1: temporal accumulation. Reset the PT accumulator only when the view actually
                    // moved (or the chunk scene rebuilt / first frame); a static view then converges clean at 1 spp/
                    // frame instead of 4-spp-from-scratch every frame (the noise+cost root cause). Motion keeps 4 spp
                    // (masked by movement) + denoise. The creature ghosts slightly while you stand still and it
                    // writhes (GLM 1a) -- acceptable for v1; SVGF temporal reproject is the follow-up if it bothers.
                    dxr->set_flashlight(flashOn);                       // F-toggled eye-torch (RT only)
                    const bool flashChanged = (flashOn != dxrFlashApplied);   // lighting changed -> must re-converge
                    dxrFlashApplied = flashOn;
                    float flareGpu[256];   // up to 64 flares x float4 {x,y,z,intensity}
                    const uint32_t nFlares = flares.pack_nearest(Vec3{ cam.pos[0], cam.pos[1], cam.pos[2] }, 2.2f, 64u, flareGpu);
                    dxr->set_flares(flareGpu, nFlares);                 // green breadcrumbs near the eye (RT)
                    // Apparition Phase 2b.2: the dread dim on the RT path too. Mirrors the raster computation (same 2a
                    // window/strength). Applied POST-accumulation in the PT shader -> deliberately NOT part of ptReset
                    // (changing it needs no accumulator reset -> the dim decays smoothly, no re-noising).
                    float dreadRt = 1.0f;
                    if (now < apparitionUntil) {
                        const float frac = clampf(duration<float>(apparitionUntil - now).count() / apparitionWindowS, 0.0f, 1.0f);
                        const float maxDim = 0.18f + 0.12f * static_cast<float>(apparitionStrength);  // keep in sync with the raster dread
                        dreadRt = 1.0f - maxDim * frac;
                    }
                    dxr->set_dread(dreadRt);
                    const bool ptReset = !dxrHaveCam || sceneRebuilt || flashChanged || flaresChanged || pt_view_moved(cam, dxrPrevCam);
                    flaresChanged = false;
                    // E35: request the color+depth CPU-readback copies ONLY on the sparse frames something actually
                    // reads the POV back (Director vision cadence / a waiting voice turn). Every other frame is
                    // presented same-device and the copies are ~30 MB of dead PCIe traffic at a 4K-Quality internal res.
                    const bool visionDue = model.settings.director && visionDir && (now - last_vision >= vision_interval);
                    const bool chatPovDue = model.settings.director && chat && wantChatPov;
                    dxr->render_pt_frame(cam, ptReset ? 4u : 1u, static_cast<uint32_t>(texSeed) + static_cast<uint32_t>(frames),
                                         ptReset, true, static_cast<uint32_t>(frames), /*aa=*/true, /*stochastic_lights=*/true,
                                         /*want_readback=*/(visionDue || chatPovDue));
                    dxrPrevCam = cam; dxrHaveCam = true;
                    // RT_PERF item A: present the PT output as a SAME-DEVICE GPU texture -- no per-frame CPU
                    // readback. The readback now happens ONLY when the Director VLM / chat needs the player POV.
                    std::vector<uint8_t> rt;
                    if (visionDue) {
                        last_vision = now;
                        if (dxr->readback(rt)) {
                            std::string b64 = encode_pov_b64(rt, dxrW, dxrH);
                            if (!b64.empty()) visionDir->submit(std::move(b64), vision_entity_context(shog, s.wanderer.pos));
                        }
                    }
                    // ADR-074: a waiting voice turn grabs the player POV so the Director answers about what you SEE.
                    if (chatPovDue) {
                        if (rt.empty()) dxr->readback(rt);   // reuse this frame's readback if vision already grabbed it
                        if (!rt.empty()) { wantChatPov = false; chat->submit(std::move(pendingChatWav), encode_pov_b64(rt, dxrW, dxrH), std::move(pendingChatCtx)); }
                    }
                    // The caption is GPU-blended by present_pt_texture; upload it to the overlay only when it changes.
                    if (showCap && capText != lastCapUploaded) {
                        app::build_caption_overlay(capOvl, curW, curH, capText);
                        renderer.upload_caption_overlay(capOvl.data(), curW, curH);
                        lastCapUploaded = capText;
                    }
                    if (renderer.present_pt_texture(dxr->pt_output(), showCap)) ++rtFrames;  // same-device blit, no readback
                }
            }
            if (!model.settings.rt) {
                app::build_shoggoth_mesh(shogBody, shog.pos, shog.writhe, 1.4f);  // M20b in-world body
                std::vector<contracts::ResidentChunk> withShog;
                app::ladder::carve_residents(sm->resident(), ladderCarvePool, withShog);  // open the band shaft out of the world mesh
                withShog.push_back(contracts::ResidentChunk{contracts::ChunkKey{9999, static_cast<int64_t>(frames), 0}, shogBody.data(), static_cast<uint32_t>(shogBody.size())});
                const int64_t lcell = static_cast<int64_t>(std::floor(cam.pos[0] / 24.0f));   // the infinite 45-deg ladder (raster):
                if (lcell != ladderCell) { app::ladder::build_mesh(ladderMesh, (static_cast<float>(lcell) + 0.5f) * 24.0f, 38.0f); ladderCell = lcell; }  // rebuild + re-upload only on a 24 m cell cross (a STABLE key, so it never starves the 8-upload budget)
                withShog.push_back(contracts::ResidentChunk{contracts::ChunkKey{9998, lcell, 0}, ladderMesh.data(), static_cast<uint32_t>(ladderMesh.size())});
                // Director SUBTITLES (raster path): alpha-blended overlay drawn over the world inside
                // render_chunks_windowed (same frame, no HUD/post pass) while the line is fresh (~6 s).
                uint32_t drawn = 0;
                // Apparition Phase 2b: while a recent PLAYER-POV verdict lingers, the fluorescents SAG -- a soft,
                // decaying dim that scales with app_strength (the visual half, paired with the 2a soundscape thin).
                // Presentation-only; windowed raster path only -> goldens untouched. Reuses the 2a window/strength.
                float dread = 1.0f;
                if (now < apparitionUntil) {
                    const float frac = clampf(duration<float>(apparitionUntil - now).count() / apparitionWindowS, 0.0f, 1.0f);
                    const float maxDim = 0.18f + 0.12f * static_cast<float>(apparitionStrength);  // strength 1->0.30 .. 3->0.54 deep
                    dread = 1.0f - maxDim * frac;   // fluorescents ease to ~0.7x..0.46x at the verdict, back to full
                }
                renderer.set_dread(dread);
                if (!renderer.render_chunks_windowed(cam, withShog, 8u, s.tick, &drawn, showCap)) {
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
    ClipCursor(nullptr);   // release the mouse confine on the way out

    // M16: persist the current settings + window state for next launch.
    cfg.master = model.settings.master_pct; cfg.sfx = model.settings.sfx_pct;
    cfg.mouse = model.settings.mouse_pct; cfg.director = model.settings.director;
    cfg.renderer = model.settings.rt;  // M19 persist the ray-tracing toggle
    cfg.model_tier = model.settings.model_tier;  // persist the AI model tier (applies next launch)
    cfg.fullscreen = isFull ? 1 : 0; cfg.seed = model.seed;
    cfg.width = model.settings.res_w; cfg.height = model.settings.res_h;  // the picked resolution (applies next launch)
    app::save_config(cfgPath, app::sanitize(cfg));

    const uint32_t dbg = renderer.debug_error_count();
    const unsigned long long underruns = static_cast<unsigned long long>(audioOn ? eng.underruns() : 0ull);
    eng.stop();
    std::printf("game_seed: %llu\n", static_cast<unsigned long long>(model.seed));
    std::printf("frames: %llu\n", static_cast<unsigned long long>(frames));
    std::printf("final_screen: %d\n", static_cast<int>(model.screen));
    std::printf("audio_underruns: %llu\n", underruns);
    const unsigned long long brain_req = brain ? brain->requests() : 0ull;
    if (broker) broker->shutdown();  // Phase C.2: wake every host worker blocked in acquire() so the resets/joins below cannot deadlock
    brain.reset();  // M21b: join the worker thread before exit
    const unsigned long long dir_req = director ? director->requests() : 0ull;
    const unsigned long long dir_prod = director ? director->produced() : 0ull;
    director.reset();  // join the Director worker thread before exit
    const unsigned long long vis_req = visionDir ? visionDir->requests() : 0ull;
    const unsigned long long vis_prod = visionDir ? visionDir->produced() : 0ull;
    visionDir.reset();  // join the vision narrator worker thread before exit
    const unsigned long long svis_req = shogVision ? shogVision->requests() : 0ull;   // Phase D LIVE: the creature's eyes
    const unsigned long long svis_prod = shogVision ? shogVision->produced() : 0ull;
    shogVision.reset();  // join the creature-vision worker thread before exit
    shogPov.reset();     // release the 2nd headless device (creature POV)
    const unsigned long long chat_req = chat ? chat->requests() : 0ull;
    const unsigned long long chat_prod = chat ? chat->produced() : 0ull;
    mic.reset();   // stop mic capture
    chat.reset();  // join the conversation worker thread before exit
    PlaySoundW(nullptr, nullptr, SND_PURGE);  // stop any PA line still playing
    std::printf("brain_intents: %llu\n", static_cast<unsigned long long>(brain_intents));
    std::printf("brain_requests: %llu\n", brain_req);
    std::printf("director_spoke: %llu\n", static_cast<unsigned long long>(director_spoke));
    std::printf("director_requests: %llu\n", dir_req);
    std::printf("director_produced: %llu\n", static_cast<unsigned long long>(dir_prod));
    std::printf("vision_requests: %llu\n", vis_req);
    std::printf("vision_produced: %llu\n", vis_prod);
    std::printf("svision_requests: %llu\n", svis_req);    // Phase D LIVE: creature-POV vision calls attempted
    std::printf("svision_produced: %llu\n", svis_prod);   // valid intents the qwen-VL eye yielded
    std::printf("svision_intents: %llu\n", static_cast<unsigned long long>(svision_intents));  // applied to the live creature
    std::printf("svision_player_reads: %llu\n", static_cast<unsigned long long>(svision_player_reads));  // Phase 2a: player-POV apparition reads
    std::printf("apparition_hits: %llu (last kind=%u where=%u strength=%u)\n", static_cast<unsigned long long>(apparition_hits),
                static_cast<unsigned>(apparitionKind), static_cast<unsigned>(apparitionSector),
                static_cast<unsigned>(apparitionStrength));  // present verdicts on the player's view
    std::printf("chat_requests: %llu\n", chat_req);
    std::printf("chat_produced: %llu\n", chat_prod);
    std::printf("rt_frames: %llu\n", static_cast<unsigned long long>(rtFrames));   // ADR-077: live RT presents (0 => RT crashed or silently fell back to raster)
    if (!last_pa_line.empty()) std::printf("director_last_line: %s\n", last_pa_line.c_str());
    std::printf("debug_error_count: %u\n", dbg);
    if (o.auto_play) {
        // Two guards over a Play session. (1) The view must not SELF-spin: the old bug pinned look at the +-0.5
        // clamp EVERY frame, so the spin signature is "almost all frames clamped". We measure the CLAMPED
        // FRACTION (robust to a real mouse twitch on an interactive desktop -- a twitch clamps a few frames, a
        // spin clamps ~all); max_dyaw is reported for info only. (2) We must barely WARP the cursor -- a per-frame
        // SetCursorPos is the "cursor fights me" bug (it'd be ~1 warp per frame); ~1 is expected (the capture park).
        const long warps = g_cursorWarps - warps0;
        const double clamped_frac = (lookFrames > 0) ? static_cast<double>(clampedFrames) / static_cast<double>(lookFrames) : 0.0;
        const bool spin_ok = (lookFrames > 0) && (clamped_frac < 0.5);   // a real spin is ~1.0; idle/twitchy is ~0
        const bool nofight_ok = (warps < 10);
        std::printf("lookcheck_max_dyaw: %.5f\n", static_cast<double>(maxDyaw));
        std::printf("lookcheck_clamped_frac: %.3f\n", clamped_frac);
        std::printf("lookcheck_frames: %llu\n", static_cast<unsigned long long>(lookFrames));
        std::printf("lookcheck_cursor_warps: %ld\n", warps);
        std::printf("lookcheck: %s\n", (spin_ok && nofight_ok) ? "PASS" : "FAIL");
        if (!spin_ok || !nofight_ok) return 4;
    }
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
        const auto c = contracts::chunk_key_at(contracts::level_from_y(s.wanderer.pos.y), s.wanderer.pos.x, s.wanderer.pos.z);
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
        const auto center = contracts::chunk_key_at(contracts::level_from_y(s.wanderer.pos.y), s.wanderer.pos.x, s.wanderer.pos.z);
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
    const float ey = contracts::level_base_y(o.level) + br::core::kWandererHalfHeight + 0.02f + br::core::kEyeHeight;
    contracts::CameraPose cam{};
    cam.pos[0] = ex; cam.pos[1] = ey; cam.pos[2] = ez;
    cam.yaw = pz.yaw; cam.pitch = pz.pitch;
    cam.fov_y = 1.2217305f;  // ~70 deg, matches the wanderer camera
    cam.aspect = static_cast<float>(o.width) / static_cast<float>(o.height);

    br::stream::StreamManager sm(o.seed, static_cast<int>(o.radius), o.workers);
    const auto center = contracts::chunk_key_at(o.level, ex, ez);  // M26: scene at the requested level
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

// A PATH-PLANNING wanderer for the SCREENSAVER (cosmetic, non-gated). The earlier versions navigated
// LOCALLY (a cardinal grid, then a short look-ahead feeler fan toward a blind goal), so they would
// commit toward a goal, walk into a concave dead-end the feelers could not see, stall, and turn around
// -- the back-and-forth the operator saw. This version CHEATS with X-ray vision: it BFS-pathfinds the
// DETERMINISTIC maze (the very generator the world is built from, via app::maze_open) to a far,
// guaranteed-REACHABLE cell, then FOLLOWS that route with look-ahead (pure-pursuit) steering -- aiming
// a few metres ahead along the planned path, so it corners smoothly and, at every instant, already
// knows where it is going. Because the route is always a valid path, it never reaches a dead-end it
// has to back out of; it re-plans forward before the path runs out, so the look-ahead never lapses.
// Holes/stairs are excluded from the path (it stays on its floor). View-only; never touches WorldState.
// WalkBot stays for the gated soak/PT paths; the Stroller is screensaver-only.
struct Stroller {
    br::core::Pcg64 rng;
    std::vector<std::pair<int64_t, int64_t>> path;  // the planned cell route (BFS) -- always reachable
    size_t path_i = 0;
    int age = 0, stuck = 0, steer_cd = 0;
    br::core::Vec3 last_pos;
    float desired = 0.0f;       // continuous free-angle steering target
    double sway = 0.0;

    Stroller(uint64_t seed, br::core::Vec3 spawn) : rng(seed), last_pos(spawn) {}

    static float wrap_pi(float a) {
        const float kTwoPi = 6.2831853f;
        while (a > 3.14159265f) a -= kTwoPi;
        while (a < -3.14159265f) a += kTwoPi;
        return a;
    }
    // Is global cell (gi,gj) an open floor hole (a pit / stair mouth) on this floor? -> keep paths off it.
    static bool cell_hole(uint64_t seed, int32_t level, int64_t gi, int64_t gj) {
        const int G = br::gen::kCellsPerChunk;
        const int64_t cx = br::app::floor_div(gi, G), cz = br::app::floor_div(gj, G);
        return br::gen::floor_hole_at(seed, level, cx, cz,
                                      static_cast<int>(gi - cx * G), static_cast<int>(gj - cz * G));
    }
    // Distance (m) to the nearest blocker along `yaw` (chest-height probe) -- for the local pillar dodge
    // (pillars sit at cell centres, invisible to the maze graph).
    static float clearance(uint64_t seed, int32_t level, const br::core::Vec3& p, float yaw,
                           float maxReach, const std::vector<br::core::Aabb>& col) {
        const float fx = std::sin(yaw), fz = std::cos(yaw);
        const float r = br::core::kWandererRadius + 0.18f, cs = br::gen::kCellSize;
        for (float d = 0.5f; d <= maxReach; d += 0.45f) {
            const float x = p.x + fx * d, z = p.z + fz * d;
            const contracts::ChunkKey k = contracts::chunk_key_at(level, x, z);
            const int i = static_cast<int>((x - static_cast<float>(k.cx) * contracts::kChunkSize) / cs);
            const int j = static_cast<int>((z - static_cast<float>(k.cz) * contracts::kChunkSize) / cs);
            if (i >= 0 && i < 8 && j >= 0 && j < 8 &&
                br::gen::floor_hole_at(seed, level, k.cx, k.cz, i, j)) return d;
            for (const br::core::Aabb& b : col)
                if (p.y > b.mn.y + 0.05f && p.y < b.mx.y - 0.05f &&
                    x + r > b.mn.x && x - r < b.mx.x && z + r > b.mn.z && z - r < b.mx.z) return d;
        }
        return maxReach;
    }

    // Local context steering: over a 24-direction fan, pick the heading that best balances heading
    // TOWARD `goalDir` (the path's lead), staying CLEAR of walls (margin -> no hugging / faceplant), and
    // smoothness (don't whip). The PATH guarantees goalDir is never a dead-end; this just keeps the local
    // movement off the walls and fluid. Returns a FREE continuous angle.
    float steer(uint64_t seed, int32_t level, const br::core::Vec3& p, float goalDir, float facing,
                const std::vector<br::core::Aabb>& col) {
        const float kTwoPi = 6.2831853f, reach = 6.5f;
        float best = -1.0e9f, bestYaw = goalDir;
        for (int k = 0; k < 24; ++k) {
            const float a = static_cast<float>(k) * (kTwoPi / 24.0f);
            const float clr = clearance(seed, level, p, a, reach, col);
            if (clr < 1.2f) continue;                                  // wall right there -> not an option
            const float score = 1.7f * std::cos(wrap_pi(a - goalDir))  // mostly: follow the path's lead
                              + 0.9f * (clr / reach)                   // prefer open lanes (margin from walls)
                              + 0.5f * std::cos(wrap_pi(a - facing));   // smoothness
            if (score > best) { best = score; bestYaw = a; }
        }
        if (best < -1.0e8f) bestYaw = goalDir;                         // boxed (rare on a valid path) -> head to it
        return bestYaw;
    }

    // X-ray BFS over the deterministic maze from (sgi,sgj): flood a bounded window, then path to a FAR,
    // mostly-FORWARD reachable cell (a long traversal, not a U-turn). Fills `path` (start..goal). Because
    // the goal is reachable by construction, FOLLOWING the path can never hit a dead-end.
    void plan(uint64_t seed, int32_t level, int64_t sgi, int64_t sgj, float facing) {
        constexpr int R = 26, W = 2 * R + 1;                       // ~104 m look-ahead window
        std::vector<int8_t> par(static_cast<size_t>(W) * W, -1);   // -1 unvisited; 4 start; else dir 0..3 stepped IN
        auto idx = [&](int64_t gi, int64_t gj) -> int {
            const int di = static_cast<int>(gi - sgi) + R, dj = static_cast<int>(gj - sgj) + R;
            return (di < 0 || di >= W || dj < 0 || dj >= W) ? -1 : di * W + dj;
        };
        std::unordered_map<int64_t, br::gen::ChunkLayout> cache;
        std::queue<std::pair<int64_t, int64_t>> q;
        static const int dgi[4] = {1, -1, 0, 0}, dgj[4] = {0, 0, 1, -1};
        par[static_cast<size_t>(idx(sgi, sgj))] = 4;
        q.push({sgi, sgj});
        int64_t bgi = sgi, bgj = sgj; float bestScore = -1.0e9f;
        while (!q.empty()) {
            const auto cur = q.front(); q.pop();
            const int64_t ci = cur.first, cj = cur.second, ddi = ci - sgi, ddj = cj - sgj;
            const float d2 = static_cast<float>(ddi * ddi + ddj * ddj);
            if (d2 > 1.0f) {
                const float dirYaw = std::atan2(static_cast<float>(ddi), static_cast<float>(ddj));
                const float cosrel = std::cos(wrap_pi(dirYaw - facing));                  // 1 ahead, -1 behind
                const float fclamp = (cosrel > 0.0f) ? cosrel : 0.0f;                     // forward cone only
                const float score = std::sqrt(d2) * (0.15f + 0.85f * fclamp)              // STRONGLY prefer far + FORWARD
                                  + 0.3f * static_cast<float>(rng.next_double());          // a touch of variety
                if (score > bestScore) { bestScore = score; bgi = ci; bgj = cj; }
            }
            for (int d = 0; d < 4; ++d) {
                if (!br::app::maze_open(seed, ci, cj, d, cache, level)) continue;
                const int64_t ni = ci + dgi[d], nj = cj + dgj[d];
                const int ix = idx(ni, nj);
                if (ix < 0 || par[static_cast<size_t>(ix)] != -1) continue;
                if (cell_hole(seed, level, ni, nj)) continue;                              // stay off pits/stairs
                par[static_cast<size_t>(ix)] = static_cast<int8_t>(d);
                q.push({ni, nj});
            }
        }
        // Reconstruct start..goal by walking the parent directions back, then reverse.
        path.clear(); path_i = 0;
        std::vector<std::pair<int64_t, int64_t>> rev;
        int64_t ci = bgi, cj = bgj;
        for (int guard = 0; guard < W * W; ++guard) {
            rev.push_back({ci, cj});
            if (ci == sgi && cj == sgj) break;
            const int ix = idx(ci, cj); if (ix < 0) break;
            const int8_t pd = par[static_cast<size_t>(ix)];
            if (pd < 0 || pd > 3) break;
            ci -= dgi[pd]; cj -= dgj[pd];
        }
        for (auto it = rev.rbegin(); it != rev.rend(); ++it) path.push_back(*it);
        if (path.empty()) path.push_back({sgi, sgj});
    }

    contracts::InputCommand step(const br::core::WorldState& s, uint64_t seed,
                                 const std::vector<br::core::Aabb>& col) {
        contracts::InputCommand in{};
        const br::core::Vec3 p = s.wanderer.pos;
        const int32_t lvl = contracts::level_from_y(p.y);
        const float face = s.wanderer.yaw;

        const float mdx = p.x - last_pos.x, mdz = p.z - last_pos.z;
        last_pos = p;
        stuck = (std::sqrt(mdx * mdx + mdz * mdz) < 0.004f) ? stuck + 1 : 0;
        ++age;

        int64_t sgi, sgj; br::app::world_to_cell(p.x, p.z, sgi, sgj);

        // Re-plan only when we've REACHED the planned goal (or wedged, or as a stale-safety) -- NOT
        // continuously. Committing to each full path is what stops the goal flip-flopping behind us and
        // U-turning. The new path starts at the current cell, so the hand-off is seamless.
        const bool reached = (path_i + 1 >= path.size());
        if (path.empty() || (reached && age > 20) || stuck > 90 || age > 1500) {
            plan(seed, lvl, sgi, sgj, face);
            age = 0; if (stuck > 90) stuck = 0;
        }
        // Advance along the path by PROGRESS, not proximity: step to the next waypoint whenever it is no
        // farther than the current one. Robust to corner-cutting, so path_i never stalls BEHIND the walker
        // -- a stalled index is what made the look-ahead point backward and the walker ping-pong.
        while (path_i + 1 < path.size()) {
            const float a0x = br::app::cell_center(path[path_i].first), a0z = br::app::cell_center(path[path_i].second);
            const float a1x = br::app::cell_center(path[path_i + 1].first), a1z = br::app::cell_center(path[path_i + 1].second);
            const float d0 = (a0x - p.x) * (a0x - p.x) + (a0z - p.z) * (a0z - p.z);
            const float d1 = (a1x - p.x) * (a1x - p.x) + (a1z - p.z) * (a1z - p.z);
            if (d1 <= d0 + 0.01f) ++path_i; else break;
        }
        // Look-ahead lead: a point ~Ld metres ALONG the path, AROUND corners. This is fed to the steer
        // fan below (not straight to the walker), so it ANTICIPATES the next turn -- the fan veers toward
        // the open lane EARLY instead of marching at the corner wall, which is what keeps the camera off
        // the walls in tight bends (low faceplant). The fan's clearance term stops it driving into a wall.
        const float Ld = 9.0f;
        float ax = br::app::cell_center(path[path_i].first), az = br::app::cell_center(path[path_i].second);
        float acc = std::sqrt((ax - p.x) * (ax - p.x) + (az - p.z) * (az - p.z));
        for (size_t li = path_i; acc < Ld && li + 1 < path.size(); ++li) {
            const float x0 = br::app::cell_center(path[li].first), z0 = br::app::cell_center(path[li].second);
            const float x1 = br::app::cell_center(path[li + 1].first), z1 = br::app::cell_center(path[li + 1].second);
            acc += std::sqrt((x1 - x0) * (x1 - x0) + (z1 - z0) * (z1 - z0));
            ax = x1; az = z1;
        }
        const float pathDir = std::atan2(ax - p.x, az - p.z);   // where the path leads (a few corners ahead)

        // Steer toward the path's lead with the local feeler fan -> it follows the guaranteed route but
        // flows down corridor centres + around pillars instead of grazing them. Recomputed a few times a
        // second; smoothly chased in between.
        if (steer_cd <= 0) { desired = steer(seed, lvl, p, pathDir, face, col); steer_cd = 5; }
        --steer_cd;
        sway += 0.010;
        const float target = desired + 0.03f * static_cast<float>(std::sin(sway));  // a subtle human weave
        const float dyaw = wrap_pi(target - face);
        in.look_yaw = br::core::clampf(dyaw, -0.035f, 0.035f);   // a calm CONTINUOUS turn -- free angle, no snapping
        const float ad = std::fabs(dyaw);
        in.move_z = (ad < 0.6f) ? 1.0f : (ad > 1.3f ? 0.3f : 1.0f - 0.7f * (ad - 0.6f) / 0.7f);  // keep flowing; ease only a hard turn
        return in;
    }
};

// Headless proof that the screensaver's Stroller navigates NATURALLY (the operator's bug: the old
// WalkBot faceplanted wall-to-wall and stared at walls). Drives the Stroller with the same holed
// collision the screensaver uses, for --ticks, and reports: distance covered (it isn't stuck),
// the explored span (it ranges out, doesn't circle one spot), and the FACEPLANT RATIO -- the
// fraction of ticks with a wall within 1.2 m straight along the CAMERA facing (what the viewer
// sees). A natural walker looks DOWN open corridors, so that ratio is low; WalkBot's was high.
int run_strollcheck(const Options& o) {
    using namespace br::core;
    WorldState s(o.seed);
    s.wanderer.pos = Vec3{ 2.0f, kWandererHalfHeight + 0.02f, 2.0f };
    Stroller bot(o.seed ^ 0x5170990000000001ull, s.wanderer.pos);
    std::vector<Aabb> col;
    contracts::ChunkKey cached{ 0x7fffffff, 0, 0 };
    auto rebuild = [&](contracts::ChunkKey c) { build_walk_collision(col, o.seed, c); cached = c; };
    rebuild(contracts::chunk_key_at(0, s.wanderer.pos.x, s.wanderer.pos.z));

    const uint32_t ticks = (o.ticks > 0) ? o.ticks : 36000u;  // ~5 min of walking @120 Hz
    uint64_t faceplant = 0;
    float dist = 0.0f; Vec3 prev = s.wanderer.pos;
    float minx = prev.x, maxx = prev.x, minz = prev.z, maxz = prev.z;
    // Organic head-bob tracking: the deepest dip (amplitude), the SPREAD of footfall-dip depths
    // (varying dips => organic, not a perfect repeating sine), and the pitch-nod range.
    float bobLo = 0.0f, nodLo = 1.0e9f, nodHi = -1.0e9f;
    float dipMin = 1.0e9f, dipMax = -1.0e9f, by0 = 0.0f, by1 = 0.0f;
    int bsamp = 0;
    // Free-angle check: mean angular distance of the camera facing from the nearest CARDINAL (0/90/
    // 180/270) over moving ticks. A 90-degree-locked vacuum sits ~0; a human moving at free angles
    // (diagonal cuts, smooth continuous turns) sits well off zero.
    double offcard_sum = 0.0; uint64_t move_ticks = 0;
    // Back-and-forth check (the operator's complaint): sample the heading once a second and count
    // U-turns -- a net >135-degree flip from a second ago. Blind nav dead-ends a lot (many U-turns);
    // a planned path almost never has to reverse. 300 samples over a 36000-tick run.
    float uturn_ref = s.wanderer.yaw; uint64_t uturns = 0; int uturn_t = 0;
    // True back-and-forth check: net displacement over a 5 s window. A walker that keeps traversing
    // advances; one stuck oscillating returns near where it was. Count ticks whose 5 s-ago position is
    // < 4 m away (effectively not getting anywhere). Low = it makes real progress, not ping-pong.
    std::vector<float> ringx(600, 0.0f), ringz(600, 0.0f); uint64_t stall_ct = 0;
    for (uint32_t t = 0; t < ticks; ++t) {
        const contracts::ChunkKey here = contracts::chunk_key_at(contracts::level_from_y(s.wanderer.pos.y), s.wanderer.pos.x, s.wanderer.pos.z);
        if (here != cached) rebuild(here);
        tick(s, bot.step(s, o.seed, col), col);
        // Sample the organic bob on a zeroed camera to read the pure vertical offset + nod.
        contracts::CameraPose bc{}; bc.pos[1] = 0.0f; bc.pitch = 0.0f;
        apply_organic_bob(bc, s);
        const float by = bc.pos[1];
        if (by < bobLo) bobLo = by;
        if (bc.pitch < nodLo) nodLo = bc.pitch;
        if (bc.pitch > nodHi) nodHi = bc.pitch;
        if (bsamp >= 2 && by1 < by0 && by1 < by && by1 < -0.020f) {  // a real footfall dip (>2 cm local min, not a harmonic ripple)
            const float depth = -by1;
            if (depth < dipMin) dipMin = depth;
            if (depth > dipMax) dipMax = depth;
        }
        by0 = by1; by1 = by; ++bsamp;
        const float fx = std::sin(s.wanderer.yaw), fz = std::cos(s.wanderer.yaw);  // the CAMERA facing
        const float r = kWandererRadius + 0.1f;
        bool wallClose = false;
        for (float d = 0.5f; d <= 1.2f && !wallClose; d += 0.3f) {
            const float x = s.wanderer.pos.x + fx * d, z = s.wanderer.pos.z + fz * d;
            for (const Aabb& b : col)
                if (s.wanderer.pos.y > b.mn.y + 0.05f && s.wanderer.pos.y < b.mx.y - 0.05f &&
                    x + r > b.mn.x && x - r < b.mx.x && z + r > b.mn.z && z - r < b.mx.z) { wallClose = true; break; }
        }
        if (wallClose) ++faceplant;
        const float dx = s.wanderer.pos.x - prev.x, dz = s.wanderer.pos.z - prev.z;
        const float stepd = std::sqrt(dx * dx + dz * dz);
        dist += stepd; prev = s.wanderer.pos;
        if (stepd > 0.005f) {  // moving: how far is the facing from the nearest cardinal?
            float yp = s.wanderer.yaw - 6.2831853f * std::floor(s.wanderer.yaw / 6.2831853f);  // [0,2pi)
            float m = yp - 1.5707963f * std::floor(yp / 1.5707963f);                            // [0,pi/2)
            offcard_sum += static_cast<double>(std::min(m, 1.5707963f - m));
            ++move_ticks;
        }
        if (++uturn_t >= 120) {  // ~1 s: did the heading flip > 135 degrees (a reversal / dead-end turn-around)?
            uturn_t = 0;
            float du = s.wanderer.yaw - uturn_ref;
            while (du > 3.14159265f) du -= 6.2831853f; while (du < -3.14159265f) du += 6.2831853f;
            if (std::fabs(du) > 2.356f) ++uturns;
            uturn_ref = s.wanderer.yaw;
        }
        if (t >= 600) {
            const float bx = s.wanderer.pos.x - ringx[t % 600], bz = s.wanderer.pos.z - ringz[t % 600];
            if (bx * bx + bz * bz < 16.0f) ++stall_ct;   // moved < 4 m net in the last 5 s
        }
        ringx[t % 600] = s.wanderer.pos.x; ringz[t % 600] = s.wanderer.pos.z;
        const float px = s.wanderer.pos.x, pz = s.wanderer.pos.z;
        if (px < minx) minx = px; if (px > maxx) maxx = px;
        if (pz < minz) minz = pz; if (pz > maxz) maxz = pz;
    }
    const double facer = static_cast<double>(faceplant) / static_cast<double>(ticks);
    const float span = std::max(maxx - minx, maxz - minz);
    const float bobAmpCm = -bobLo * 100.0f;                                        // deepest dip
    const float dipSpreadCm = (dipMax > dipMin) ? (dipMax - dipMin) * 100.0f : 0.0f;  // dips vary => organic
    const float nodDeg = (nodHi > nodLo) ? (nodHi - nodLo) * 57.29578f : 0.0f;
    const double offcardDeg = (move_ticks > 0) ? (offcard_sum / static_cast<double>(move_ticks)) * 57.29578 : 0.0;
    std::printf("seed: %llu\n", static_cast<unsigned long long>(o.seed));
    std::printf("ticks: %u\n", ticks);
    std::printf("distance_m: %.1f\n", static_cast<double>(dist));
    std::printf("explore_span_m: %.1f\n", static_cast<double>(span));
    std::printf("faceplant_ratio: %.4f\n", facer);
    std::printf("offcardinal_deg: %.2f\n", offcardDeg);
    const double stallFrac = (ticks > 600) ? static_cast<double>(stall_ct) / static_cast<double>(ticks - 600) : 0.0;
    std::printf("uturns: %llu\n", static_cast<unsigned long long>(uturns));  // reversals over the run (lower = less back-and-forth)
    std::printf("stall_frac: %.4f\n", stallFrac);
    std::printf("bob_amp_cm: %.2f\n", static_cast<double>(bobAmpCm));
    std::printf("dip_spread_cm: %.2f\n", static_cast<double>(dipSpreadCm));
    std::printf("nod_deg: %.2f\n", static_cast<double>(nodDeg));
    std::printf("final_level: %d\n", contracts::level_from_y(s.wanderer.pos.y));
    // Natural + PLANNED: covers ground, explores, makes real net progress (low stall = it doesn't
    // ping-pong in place), at FREE angles (off-cardinal), rarely faceplants, with a clear organic bob.
    const bool ok = (dist > static_cast<float>(ticks) * 0.012f) && (span > 40.0f) && (facer < 0.30) &&
                    (stallFrac < 0.45) && (offcardDeg > 6.0) && (bobAmpCm > 3.0f) && (dipSpreadCm > 0.8f);
    std::printf("stroll_ok: %d\n", ok ? 1 : 0);
    return ok ? 0 : 6;
}

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
        const contracts::ChunkKey here = contracts::chunk_key_at(contracts::level_from_y(s.wanderer.pos.y), s.wanderer.pos.x, s.wanderer.pos.z);
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
// CLI mirror of the in-game Settings "Test Connection": run the exact LlmProbe (best-effort autostart + a real
// keel_complete ping) and print the status line the menu would show. Exit 0 = connected, 1 = offline. Doubles
// as the operator's command-line "is the LLM up?" check: backrooms --llm-test [--director-url http://...:7071].
int run_llm_test(const Options& o) {
    std::string host; int port;
    parse_host_port(o.director_url, host, port);
    std::printf("llm_url: %s:%d\n", host.c_str(), port);
    std::fflush(stdout);
    LlmProbe probe;
    probe.start(host, port);
    while (probe.state.load() == 1) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    app::MenuModel m; probe.sync(m);
    std::printf("llm_state: %d\n", m.llm_state);          // 2 = connected, 3 = offline
    std::printf("llm_status: %s\n", m.llm_text.c_str());  // the exact UPPERCASE line the Settings screen shows
    return (m.llm_state == 2) ? 0 : 1;
}

int run_director_probe(const Options& o) {
    std::string host; int port;
    parse_host_port(o.director_url, host, port);   // strips http:// + the path (the old inline parse kept the scheme -> gle 12005)

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
    const contracts::ChunkKey k = contracts::chunk_key_at(contracts::level_from_y(s.wanderer.pos.y), s.wanderer.pos.x, s.wanderer.pos.z);
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
        const float baseY = contracts::level_base_y(c.level);  // M26: the wanderer's current floor
        // M30: a flat SEALED floor (no holes) on purpose -- this is a GATED level-0 soak/PT path;
        // letting the walk-bot fall down a shaft/down-stair would perturb its pacing + the gate.
        // Live descent (build_walk_collision, holed floor) is for the interactive walks only.
        collision.push_back(Aabb{ {-1.0e6f, baseY - 1.0f, -1.0e6f}, {1.0e6f, baseY, 1.0e6f} });
        for (int64_t dx = -1; dx <= 1; ++dx)
            for (int64_t dz = -1; dz <= 1; ++dz) {
                const contracts::ChunkData cd = contracts::GenerateChunk(o.seed, contracts::ChunkKey{ c.level, c.cx + dx, c.cz + dz });
                for (const auto& b : cd.collision) collision.push_back(Aabb{ {b.mn[0], b.mn[1], b.mn[2]}, {b.mx[0], b.mx[1], b.mx[2]} });
            }
        cached = c;
    };

    contracts::ChunkKey c0 = contracts::chunk_key_at(contracts::level_from_y(s.wanderer.pos.y), s.wanderer.pos.x, s.wanderer.pos.z);
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
            const contracts::ChunkKey here = contracts::chunk_key_at(contracts::level_from_y(s.wanderer.pos.y), s.wanderer.pos.x, s.wanderer.pos.z);
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
            contracts::chunk_key_at(contracts::level_from_y(s.wanderer.pos.y), s.wanderer.pos.x, s.wanderer.pos.z);
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

// M29: the prey the per-floor Shoggoth senses -- the wanderer, shifted onto o.level's floor at the
// midpoint so a cross-seam escape is reproducible. SHARED by every record path + replay so they stay
// byte-identical (closes AUDIT_2026-06-15.md:207, where the vision/hearing/PA record paths lacked it).
// At --level 0 (every M21/M22/M23/M24 gate) prey == the wanderer, so those gates are byte-unchanged.
inline br::core::Vec3 shoggoth_prey(const br::core::Vec3& wanderer, int32_t level, uint64_t t, uint64_t N) {
    br::core::Vec3 prey = wanderer;
    if (level != 0 && t >= N / 2) prey.y += contracts::level_base_y(level);
    return prey;
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
        // M29: optionally the wanderer changes floor at the midpoint (escape across a seam). The
        // per-floor Shoggoth (pinned to its spawn level) then can't sense the prey. --level 0 (the
        // M21 sacred gate) leaves prey == the wanderer, so M21 is byte-identical.
        br::core::Vec3 prey = w.s.wanderer.pos;
        if (o.level != 0 && t >= N / 2) prey.y += contracts::level_base_y(o.level);
        if (t % kBrainEvery == 0) {
            ++thoughts;
            const app::ShoggothSummary sum = app::build_shoggoth_summary(sh, prey, t);
            const br::director::KeelResponse resp = br::director::keel_complete(host, port, app::render_shoggoth_prompt(sum), 15000);
            bool ok = false;
            const app::ShoggothIntent intent = resp.ok ? app::parse_shoggoth_intent(resp.content, ok) : app::ShoggothIntent{};
            if (resp.ok) {
                if (ok) {
                    ++valid;
                    sh.intent = intent;
                    events.push_back(app::event_from_intent(t, intent));
                    H = fold_bytes(H, &events.back(), sizeof(app::ShoggothEvent));
                }
            }
        }
        app::shoggoth_step(sh, prey, o.seed, (t % 8u) == 0u);
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
    std::printf("final_state: %d\n", static_cast<int>(sh.state));  // M29: 0=Lurk (escaped after a floor change)
    return 0;
}

// M22: the Shoggoth SEES. Identical chase to --shoggoth-record (same MazeWalker, spawn,
// step cadence, ShoggothEvent log) so --shoggoth-replay reproduces it unchanged -- BUT
// each thought first renders the creature's POV to an OFFSCREEN snapshot and sends it
// (text + image) to KEEL's local VISION tier (qwen-VL + mmproj). The validated intent is
// logged exactly as in M21, so a replay with the model OFFLINE (and the snapshot never
// re-rendered) is bit-identical -- the sacred gate, now with eyes. RECORD-time only.
int run_shoggoth_vision_record(const Options& o) {
    using namespace br::core;
    std::string host; int port; parse_host_port(o.director_url, host, port);
    const uint64_t N = (o.ticks > 0) ? o.ticks : 1800u;
    const uint64_t kBrainEvery = 240u;  // ~2 s of sim between thoughts
    const uint32_t SW = 384u, SH = 216u;  // POV snapshot size (16:9, small + fast for vision)
    const float aspect = static_cast<float>(SW) / static_cast<float>(SH);

    Renderer renderer;
    if (!renderer.init_headless(SW, SH)) { std::fprintf(stderr, "init: %s\n", renderer.last_error().c_str()); return 1; }
    renderer.set_texture_seed(o.seed);
    br::stream::StreamManager sm(o.seed, static_cast<int>(o.radius), o.workers);

    MazeWalker w(o.seed);
    app::Shoggoth sh;
    sh.pos = w.s.wanderer.pos; sh.pos.x += 24.0f;
    std::vector<app::ShoggothEvent> events;
    uint64_t H = 1469598103934665603ull;
    uint64_t thoughts = 0, valid = 0, snapshots = 0;
    for (uint64_t t = 0; t < N; ++t) {
        w.step();
        if (t % kBrainEvery == 0) {
            ++thoughts;
            // 1) Render the Shoggoth's POV of the Backrooms ahead -> readback RGBA.
            const contracts::ChunkKey center = contracts::chunk_key_at(0, sh.pos.x, sh.pos.z);
            sm.update(center); sm.wait_idle(); sm.update(center);
            const contracts::CameraPose cam = app::shoggoth_pov_camera(sh, aspect);
            uint32_t drawn = 0;
            const size_t resident_target = sm.resident_count();
            for (int wk = 0; wk < 400 && static_cast<size_t>(drawn) < resident_target; ++wk)
                renderer.render_chunks(cam, sm.resident(), 64u, t, &drawn);
            if (!renderer.render_chunks(cam, sm.resident(), 256u, t, &drawn)) { std::fprintf(stderr, "render: %s\n", renderer.last_error().c_str()); return 1; }
            FrameImage img;
            if (!renderer.readback(img)) { std::fprintf(stderr, "readback: %s\n", renderer.last_error().c_str()); return 1; }
            ++snapshots;
            if (!o.out.empty() && t == 0) stbi_write_png(o.out.c_str(), static_cast<int>(img.width), static_cast<int>(img.height), 4, img.rgba.data(), static_cast<int>(img.width) * 4);  // QC: dump the first POV
            // 2) Encode the snapshot to PNG (in memory) -> base64.
            std::vector<uint8_t> png;
            stbi_write_png_to_func([](void* ctx, void* data, int size) {
                auto* v = static_cast<std::vector<uint8_t>*>(ctx);
                const uint8_t* p = static_cast<const uint8_t*>(data);
                v->insert(v->end(), p, p + size);
            }, &png, static_cast<int>(img.width), static_cast<int>(img.height), 4, img.rgba.data(), static_cast<int>(img.width) * 4);
            const std::string b64 = app::base64_encode(png.data(), png.size());
            // 3) Ask the vision model what to do, given what it SEES + senses.
            const app::ShoggothSummary sum = app::build_shoggoth_summary(sh, shoggoth_prey(w.s.wanderer.pos, o.level, t, N), t);
            const br::director::KeelResponse resp = br::director::keel_complete_vision(host, port, app::render_shoggoth_vision_prompt(sum), b64, 30000);
            bool ok = false;
            const app::ShoggothIntent intent = resp.ok ? app::parse_shoggoth_intent(resp.content, ok) : app::ShoggothIntent{};
            if (resp.ok && ok) {
                ++valid;
                sh.intent = intent;  // identical apply+log path as --shoggoth-record
                events.push_back(app::event_from_intent(t, intent));
                H = fold_bytes(H, &events.back(), sizeof(app::ShoggothEvent));
            }
        }
        app::shoggoth_step(sh, shoggoth_prey(w.s.wanderer.pos, o.level, t, N), o.seed, (t % 8u) == 0u);
        H = fold_u64(H, app::shoggoth_hash(sh));
    }
    if (!o.director_log.empty()) {
        if (!app::write_shoggoth_log(o.director_log, o.seed, N, events)) { std::fprintf(stderr, "record: failed to write %s\n", o.director_log.c_str()); return 1; }
    }
    const uint32_t dbg = renderer.debug_error_count();
    std::printf("seed: %llu\n", static_cast<unsigned long long>(o.seed));
    std::printf("ticks: %llu\n", static_cast<unsigned long long>(N));
    std::printf("thoughts: %llu\n", static_cast<unsigned long long>(thoughts));
    std::printf("snapshots: %llu\n", static_cast<unsigned long long>(snapshots));
    std::printf("valid_intents: %llu\n", static_cast<unsigned long long>(valid));
    std::printf("combined_hash: %016llx\n", static_cast<unsigned long long>(H));
    std::printf("debug_error_count: %u\n", dbg);
    return dbg == 0 ? 0 : 3;
}

// M23: render ~2.5 s of the Backrooms soundscape AT THE SHOGGOTH'S EARS to a WAV — the
// fluorescent drone (the synth bed) plus the wanderer's footfalls, louder the nearer the
// prey is. A separate synth stream, so it perturbs nothing. Deterministic; record-time only.
bool shoggoth_listen_wav(const app::Shoggoth& sh, const br::core::Vec3& wanderer, uint64_t seed,
                         const std::string& wav_path) {
    using namespace br::core;
    const uint32_t sr = contracts::kAudioSampleRate;
    const uint32_t fpt = sr / 120u;
    const uint32_t ticks = 300u;  // ~2.5 s clip
    audio::Synth synth(seed ^ 0x5EE110D000000001ull, sr);
    synth.set_reverb_seconds(0.8f);
    contracts::AudioListener lis{};
    lis.pos[0] = sh.pos.x; lis.pos[1] = sh.pos.y + kEyeHeight; lis.pos[2] = sh.pos.z;
    lis.yaw = sh.yaw; lis.speed = 0.0f;
    const float dx = wanderer.x - sh.pos.x, dz = wanderer.z - sh.pos.z;
    const float dist = std::sqrt(dx * dx + dz * dz);
    float prox = 1.0f - dist / 40.0f;  // near = 1, far (>=40 m) = 0
    prox = prox < 0.0f ? 0.0f : (prox > 1.0f ? 1.0f : prox);
    std::vector<int16_t> pcm; pcm.reserve(static_cast<size_t>(ticks) * fpt * 2u);
    std::vector<float> block(static_cast<size_t>(fpt) * 2u);
    for (uint32_t t = 0; t < ticks; ++t) {
        if (prox > 0.05f && (t % 48u) == 0u) synth.trigger_footstep(0.3f + 0.7f * prox);  // the wanderer's tread
        synth.render(lis, block.data(), fpt);
        for (uint32_t i = 0; i < fpt * 2u; ++i) pcm.push_back(audio::to_pcm16(block[i] * 0.85f));
    }
    std::string err;
    return audio::write_wav(wav_path, pcm, sr, static_cast<uint16_t>(contracts::kAudioChannels), err);
}

// M23: shell out to whisper.cpp's CLI to transcribe the WAV -> a sound-event tag. Writes
// "<wav>.txt", which we read back + trim. A missing exe / failed run -> "" (graceful no-op,
// the creature simply hears "silence"). RECORD-time only; whisper is never run at replay.
std::string whisper_transcribe(const std::string& wav, const std::string& exe, const std::string& model) {
    const std::string txt = wav + ".txt";
    std::remove(txt.c_str());  // clear any stale transcript
    const std::string cmd = "\"" + exe + "\" -m \"" + model + "\" -f \"" + wav +
                            "\" -otxt -np -l en -nth 0.60";
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<char> cl(cmd.begin(), cmd.end()); cl.push_back('\0');
    if (!CreateProcessA(nullptr, cl.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi))
        return std::string();  // whisper-cli unavailable -> graceful no-op
    WaitForSingleObject(pi.hProcess, 120000);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    std::ifstream f(txt, std::ios::binary);
    if (!f) return std::string();
    std::string raw, line;
    while (std::getline(f, line)) { raw += line; raw += '\n'; }
    return app::clean_transcript(raw);
}

// ADR-074: verify live voice INPUT in isolation -- capture the mic for N seconds with the VAD,
// write each detected utterance to a WAV, run whisper, and print what it heard. No game, no VLM
// (the conversational reply is de-risked separately). Confirms waveIn + VAD + whisper end-to-end
// without launching the window. Graceful exit if there is no capture device.
int run_mic_test(const Options& o) {
    app::MicCapture mic;
    if (!mic.start()) { std::fprintf(stderr, "mic: no capture device (waveInOpen failed)\n"); std::printf("mic_device: 0\n"); return 1; }
    std::printf("mic_device: 1\n");
    const std::string wexe = o.whisper_exe.empty() ? default_whisper_exe() : o.whisper_exe;
    const std::string wmodel = o.whisper_model.empty() ? default_whisper_model() : o.whisper_model;
    const uint32_t secs = (o.seconds > 0) ? o.seconds : 12u;
    std::printf("listening %u s -- speak into the mic...\n", secs);
    std::fflush(stdout);
    const uint64_t end = GetTickCount64() + static_cast<uint64_t>(secs) * 1000ull;
    uint32_t utterances = 0;
    std::vector<int16_t> pcm;
    while (GetTickCount64() < end) {
        if (mic.poll(pcm)) {
            ++utterances;
            const double dur = static_cast<double>(pcm.size()) / static_cast<double>(app::MicCapture::kRate);
            std::string err;
            const std::string wav = std::string("runs\\mic_utter.wav");
            if (audio::write_wav(wav, pcm, app::MicCapture::kRate, static_cast<uint16_t>(1), err)) {
                const std::string text = whisper_transcribe(wav, wexe, wmodel);
                std::printf("utterance %u (%.1f s): \"%s\"\n", utterances, dur, text.c_str());
            } else {
                std::printf("utterance %u (%.1f s): wav write failed: %s\n", utterances, dur, err.c_str());
            }
            std::fflush(stdout);
        }
        Sleep(15);
    }
    mic.stop();
    std::printf("mic_utterances: %u\n", utterances);
    return 0;
}

// ADR-074: verify the conversation GLUE end-to-end without a mic -- synthesize a spoken line with the
// PA TTS (a stand-in utterance), transcribe it with whisper, then ask the Director in character
// (optionally seeing a POV image via --out). Proves transcribe -> plausible_utterance -> chat prompt
// -> Qwen-VL -> reply. --say sets the utterance; --out is an optional POV PNG the Director "sees".
int run_chat_test(const Options& o) {
    std::string host; int port; parse_host_port(o.director_url, host, port);
    const std::string wexe = o.whisper_exe.empty() ? default_whisper_exe() : o.whisper_exe;
    const std::string wmodel = o.whisper_model.empty() ? default_whisper_model() : o.whisper_model;
    const std::string said = o.say_text.empty() ? std::string("what is this place and how do I get out") : o.say_text;
    const std::vector<int16_t> pcm = app::synthesize_speech(said, app::MicCapture::kRate);  // stand-in for the mic
    std::string err;
    const std::string wav = std::string("runs\\chat_test_in.wav");
    if (!audio::write_wav(wav, pcm, app::MicCapture::kRate, static_cast<uint16_t>(1), err)) { std::fprintf(stderr, "wav: %s\n", err.c_str()); return 1; }
    const std::string heard = whisper_transcribe(wav, wexe, wmodel);
    std::printf("spoken: \"%s\"\n", said.c_str());
    std::printf("heard:  \"%s\"\n", heard.c_str());
    std::printf("plausible: %d\n", app::plausible_utterance(heard) ? 1 : 0);
    if (!app::plausible_utterance(heard)) { std::printf("(rejected as non-speech -> no reply)\n"); return 0; }
    std::string b64;
    if (!o.out.empty()) {
        std::ifstream f(o.out, std::ios::binary);
        if (f) { std::vector<uint8_t> png((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); b64 = app::base64_encode(png.data(), png.size()); }
    }
    const std::string prompt = app::render_director_chat_prompt(heard, std::string(), !b64.empty());
    const br::director::KeelResponse resp = b64.empty()
        ? br::director::keel_complete(host, port, prompt, 30000)
        : br::director::keel_complete_vision(host, port, prompt, b64, 30000);
    if (!resp.ok) { std::fprintf(stderr, "keel: %s\n", resp.error.c_str()); return 1; }
    std::printf("director: \"%s\"\n", app::clean_vision_line(resp.content).c_str());
    return 0;
}

// M23: the Shoggoth HEARS. Identical chase to --shoggoth-record (so --shoggoth-replay
// reproduces it) -- BUT each thought first renders the soundscape at the creature's ears,
// runs whisper.cpp over it, and feeds the heard tag into the (text) brain. The validated
// intent is logged exactly as in M21, so a replay with whisper AND the model OFFLINE is
// bit-identical -- the sacred gate, now with ears. RECORD-time only.
int run_shoggoth_hearing_record(const Options& o) {
    using namespace br::core;
    std::string host; int port; parse_host_port(o.director_url, host, port);
    const std::string wexe = o.whisper_exe.empty() ? default_whisper_exe() : o.whisper_exe;
    const std::string wmodel = o.whisper_model.empty() ? default_whisper_model() : o.whisper_model;
    const uint64_t N = (o.ticks > 0) ? o.ticks : 1800u;
    const uint64_t kBrainEvery = 240u;
    const std::string wav = o.out.empty() ? std::string("runs/shoggoth_hearing.wav") : o.out;

    MazeWalker w(o.seed);
    app::Shoggoth sh;
    sh.pos = w.s.wanderer.pos; sh.pos.x += 24.0f;
    std::vector<app::ShoggothEvent> events;
    uint64_t H = 1469598103934665603ull;
    uint64_t thoughts = 0, valid = 0, listens = 0, heard_nonempty = 0;
    std::string last_heard;
    for (uint64_t t = 0; t < N; ++t) {
        w.step();
        if (t % kBrainEvery == 0) {
            ++thoughts;
            std::string heard;
            if (shoggoth_listen_wav(sh, w.s.wanderer.pos, o.seed, wav)) {
                ++listens;
                heard = whisper_transcribe(wav, wexe, wmodel);
                if (!heard.empty()) { ++heard_nonempty; last_heard = heard; }
            }
            const app::ShoggothSummary sum = app::build_shoggoth_summary(sh, shoggoth_prey(w.s.wanderer.pos, o.level, t, N), t);
            const br::director::KeelResponse resp = br::director::keel_complete(host, port, app::render_shoggoth_hearing_prompt(sum, heard), 15000);
            bool ok = false;
            const app::ShoggothIntent intent = resp.ok ? app::parse_shoggoth_intent(resp.content, ok) : app::ShoggothIntent{};
            if (resp.ok && ok) {
                ++valid;
                sh.intent = intent;  // identical apply+log path as --shoggoth-record
                events.push_back(app::event_from_intent(t, intent));
                H = fold_bytes(H, &events.back(), sizeof(app::ShoggothEvent));
            }
        }
        app::shoggoth_step(sh, shoggoth_prey(w.s.wanderer.pos, o.level, t, N), o.seed, (t % 8u) == 0u);
        H = fold_u64(H, app::shoggoth_hash(sh));
    }
    if (!o.director_log.empty()) {
        if (!app::write_shoggoth_log(o.director_log, o.seed, N, events)) { std::fprintf(stderr, "record: failed to write %s\n", o.director_log.c_str()); return 1; }
    }
    std::printf("seed: %llu\n", static_cast<unsigned long long>(o.seed));
    std::printf("ticks: %llu\n", static_cast<unsigned long long>(N));
    std::printf("thoughts: %llu\n", static_cast<unsigned long long>(thoughts));
    std::printf("listens: %llu\n", static_cast<unsigned long long>(listens));
    std::printf("heard_nonempty: %llu\n", static_cast<unsigned long long>(heard_nonempty));
    std::printf("valid_intents: %llu\n", static_cast<unsigned long long>(valid));
    std::printf("last_heard: %s\n", last_heard.empty() ? "(silence)" : last_heard.c_str());
    std::printf("combined_hash: %016llx\n", static_cast<unsigned long long>(H));
    return 0;
}

// M24: render the shoggoth's soundscape WITH a spoken Backrooms PA announcement mixed in
// — the fluorescent drone + the wanderer's footfalls as a quiet bed, the procedural TTS
// PA voice over the top — so whisper.cpp reads the PA back as WORDS (M23 only ever got
// coarse ambient tags). Deterministic; record-time only.
bool shoggoth_pa_listen_wav(const app::Shoggoth& sh, const br::core::Vec3& wanderer, uint64_t seed,
                            const std::string& pa_text, const std::string& wav_path) {
    using namespace br::core;
    const uint32_t sr = contracts::kAudioSampleRate;
    const uint32_t fpt = sr / 120u;
    const std::vector<int16_t> pa = app::synthesize_speech(pa_text, sr);  // mono PA voice
    const uint32_t pa_off = static_cast<uint32_t>(0.25f * sr);            // a beat before it speaks
    const uint32_t total = pa_off + static_cast<uint32_t>(pa.size()) + static_cast<uint32_t>(0.35f * sr);
    audio::Synth synth(seed ^ 0x5EE110D000000001ull, sr);
    synth.set_reverb_seconds(0.7f);
    contracts::AudioListener lis{};
    lis.pos[0] = sh.pos.x; lis.pos[1] = sh.pos.y + kEyeHeight; lis.pos[2] = sh.pos.z;
    lis.yaw = sh.yaw; lis.speed = 0.0f;
    const float dx = wanderer.x - sh.pos.x, dz = wanderer.z - sh.pos.z;
    const float dist = std::sqrt(dx * dx + dz * dz);
    float prox = 1.0f - dist / 40.0f;
    prox = prox < 0.0f ? 0.0f : (prox > 1.0f ? 1.0f : prox);
    std::vector<int16_t> pcm(static_cast<size_t>(total) * 2u, 0);
    std::vector<float> block(static_cast<size_t>(fpt) * 2u);
    uint32_t written = 0, tk = 0;
    while (written < total) {
        if (prox > 0.05f && (tk % 48u) == 0u) synth.trigger_footstep(0.3f + 0.7f * prox);
        synth.render(lis, block.data(), fpt);
        for (uint32_t i = 0; i < fpt && written < total; ++i, ++written) {
            float l = block[2 * i] * 0.28f, r = block[2 * i + 1] * 0.28f;  // quiet ambient bed
            const int64_t pidx = static_cast<int64_t>(written) - static_cast<int64_t>(pa_off);
            if (pidx >= 0 && pidx < static_cast<int64_t>(pa.size())) {
                const float pv = static_cast<float>(pa[static_cast<size_t>(pidx)]) / 32768.0f;
                l += pv; r += pv;  // the PA voice, full level, centred over the bed
            }
            pcm[2 * written]     = audio::to_pcm16(l * 0.92f);
            pcm[2 * written + 1] = audio::to_pcm16(r * 0.92f);
        }
        ++tk;
    }
    std::string err;
    return audio::write_wav(wav_path, pcm, sr, static_cast<uint16_t>(contracts::kAudioChannels), err);
}

// M24: the Shoggoth hears the Backrooms PA VOICE. Identical chase to --shoggoth-record (so
// --shoggoth-replay reproduces it) -- but each thought renders the soundscape with a spoken
// PA announcement (procedural TTS) mixed in, runs whisper over it (recovering real WORDS),
// and feeds the heard line to the brain. Intent logged as in M21 -> replay with whisper, the
// TTS, AND the model OFFLINE is bit-identical. The richer hearing loop. RECORD-time only.
int run_shoggoth_pa_record(const Options& o) {
    using namespace br::core;
    std::string host; int port; parse_host_port(o.director_url, host, port);
    const std::string wexe = o.whisper_exe.empty() ? default_whisper_exe() : o.whisper_exe;
    // The PA carries WORDS, so default to the stronger model for clean word recovery
    // (M23's ambient tags were fine on base.en; speech wants large-v3-turbo).
    const std::string wmodel = o.whisper_model.empty() ? default_whisper_model() : o.whisper_model;
    const uint64_t N = (o.ticks > 0) ? o.ticks : 1800u;
    const uint64_t kBrainEvery = 240u;
    const std::string wav = o.out.empty() ? std::string("runs/shoggoth_pa.wav") : o.out;
    static const char* kPaLines[] = {
        "WARNING. LEVEL THREE CONTAINMENT BREACH.",
        "EVACUATE SECTOR FIVE.",
        "DANGER. DO NOT RUN. STAY CALM.",
        "ALERT. THE BIOME IS LOOPING.",
        "WARNING. EVACUATE. DANGER LEVEL FIVE.",
    };
    const uint64_t kPaCount = 5u;

    MazeWalker w(o.seed);
    app::Shoggoth sh;
    sh.pos = w.s.wanderer.pos; sh.pos.x += 24.0f;
    std::vector<app::ShoggothEvent> events;
    uint64_t H = 1469598103934665603ull;
    uint64_t thoughts = 0, valid = 0, listens = 0, heard_nonempty = 0;
    std::string last_heard;
    for (uint64_t t = 0; t < N; ++t) {
        w.step();
        if (t % kBrainEvery == 0) {
            const std::string pa = kPaLines[thoughts % kPaCount];
            ++thoughts;
            std::string heard;
            if (shoggoth_pa_listen_wav(sh, w.s.wanderer.pos, o.seed, pa, wav)) {
                ++listens;
                heard = whisper_transcribe(wav, wexe, wmodel);
                if (!heard.empty()) { ++heard_nonempty; last_heard = heard; }
            }
            const app::ShoggothSummary sum = app::build_shoggoth_summary(sh, shoggoth_prey(w.s.wanderer.pos, o.level, t, N), t);
            const br::director::KeelResponse resp = br::director::keel_complete(host, port, app::render_shoggoth_hearing_prompt(sum, heard), 15000);
            bool ok = false;
            const app::ShoggothIntent intent = resp.ok ? app::parse_shoggoth_intent(resp.content, ok) : app::ShoggothIntent{};
            if (resp.ok && ok) {
                ++valid;
                sh.intent = intent;
                events.push_back(app::event_from_intent(t, intent));
                H = fold_bytes(H, &events.back(), sizeof(app::ShoggothEvent));
            }
        }
        app::shoggoth_step(sh, shoggoth_prey(w.s.wanderer.pos, o.level, t, N), o.seed, (t % 8u) == 0u);
        H = fold_u64(H, app::shoggoth_hash(sh));
    }
    if (!o.director_log.empty()) {
        if (!app::write_shoggoth_log(o.director_log, o.seed, N, events)) { std::fprintf(stderr, "record: failed to write %s\n", o.director_log.c_str()); return 1; }
    }
    std::printf("seed: %llu\n", static_cast<unsigned long long>(o.seed));
    std::printf("ticks: %llu\n", static_cast<unsigned long long>(N));
    std::printf("thoughts: %llu\n", static_cast<unsigned long long>(thoughts));
    std::printf("listens: %llu\n", static_cast<unsigned long long>(listens));
    std::printf("heard_nonempty: %llu\n", static_cast<unsigned long long>(heard_nonempty));
    std::printf("valid_intents: %llu\n", static_cast<unsigned long long>(valid));
    std::printf("last_heard: %s\n", last_heard.empty() ? "(silence)" : last_heard.c_str());
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
        // M29: replay the SAME floor change the record injected (the gate passes a matching --level).
        br::core::Vec3 prey = w.s.wanderer.pos;
        if (o.level != 0 && t >= N / 2) prey.y += contracts::level_base_y(o.level);
        while (ei < events.size() && events[ei].effective_tick == t) {
            app::apply_event_to_intent(events[ei], sh.intent);
            H = fold_bytes(H, &events[ei], sizeof(app::ShoggothEvent));
            ++ei;
        }
        app::shoggoth_step(sh, prey, seed, (t % 8u) == 0u);
        H = fold_u64(H, app::shoggoth_hash(sh));
    }
    std::printf("seed: %llu\n", static_cast<unsigned long long>(seed));
    std::printf("ticks: %llu\n", static_cast<unsigned long long>(N));
    std::printf("replay_events: %llu\n", static_cast<unsigned long long>(events.size()));
    std::printf("combined_hash: %016llx\n", static_cast<unsigned long long>(H));
    std::printf("final_state: %d\n", static_cast<int>(sh.state));  // M29: matches record (deterministic)
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

// ----- M24 procedural TTS: speak a line of text -> a 16 kHz mono WAV (the PA voice) ---
int run_tts_say(const Options& o) {
    const std::string text = o.say_text.empty()
        ? std::string("WARNING. CONTAINMENT BREACH. LEVEL THREE.") : o.say_text;
    const uint32_t sr = 16000u;  // whisper-native rate (best recovery)
    const std::vector<int16_t> pcm = app::synthesize_speech(text, sr);
    const std::string out = o.out.empty() ? std::string("runs/say.wav") : o.out;
    std::string err;
    if (!audio::write_wav(out, pcm, sr, 1u, err)) { std::fprintf(stderr, "wav: %s\n", err.c_str()); return 1; }
    std::printf("text: %s\n", text.c_str());
    std::printf("samples: %llu\n", static_cast<unsigned long long>(pcm.size()));
    std::printf("sr: %u\n", sr);
    std::printf("duration_s: %.2f\n", static_cast<double>(pcm.size()) / static_cast<double>(sr));
    return 0;
}

// M24: the TTS->STT intelligibility check (the gate's keystone): synthesize a PA line,
// run whisper over it (via the app's robust CreateProcess path), and report what was
// heard + how many of the spoken words came back. Proves the procedural voice is readable.
// Headless visibility proof for the Director subtitle: rasterise the caption (build_caption_overlay) and
// alpha-composite it over a mid-gray "world" in CPU -> PNG, exactly as the in-game alpha-blend would look.
int run_caption_shot(const Options& o) {
    const uint32_t W = o.width ? o.width : 960u, H = o.height ? o.height : 540u;
    const std::string text = o.say_text.empty() ? std::string("EVACUATE SECTOR FIVE - CONTAINMENT BREACH DETECTED") : o.say_text;
    std::vector<uint8_t> cap;
    app::build_caption_overlay(cap, W, H, text);
    // The "world" behind the subtitle: a flat gray by default, or an ACTUAL ray-traced frame with --rt
    // (mirrors the operator's renderer=1 path -- the caption is CPU-composited into the RT readback exactly here).
    std::vector<uint8_t> img(static_cast<size_t>(W) * H * 4u, 92u);
    for (size_t p = 0; p < static_cast<size_t>(W) * H; ++p) img[p * 4 + 3] = 255u;
    if (o.rt) {
        br::stream::StreamManager sm(o.seed, static_cast<int>(o.radius), o.workers);
        const auto ctr = contracts::chunk_key_at(0, 16.0f, 16.0f);
        sm.update(ctr); sm.wait_idle(); sm.update(ctr);
        br::render_dxr::DxrRenderer r;
        contracts::CameraPose cam{};
        cam.pos[0] = 16.0f; cam.pos[1] = br::core::kWandererHalfHeight + 0.02f + br::core::kEyeHeight; cam.pos[2] = 16.0f;
        cam.yaw = 0.7853982f; cam.fov_y = 1.2217305f; cam.aspect = static_cast<float>(W) / static_cast<float>(H);
        if (r.init(W, H) && r.build_scene(sm.resident()) && r.render_pt_frame(cam, 64u, static_cast<uint32_t>(o.seed), true, true, 0u))
            r.readback(img);  // overwrite the gray bg with the real RT frame
    }
    for (size_t p = 0; p < static_cast<size_t>(W) * H; ++p) {
        const uint8_t* s = &cap[p * 4]; uint8_t* d = &img[p * 4];
        const float a = static_cast<float>(s[3]) / 255.0f;
        for (int k = 0; k < 3; ++k) d[k] = static_cast<uint8_t>(static_cast<float>(s[k]) * a + static_cast<float>(d[k]) * (1.0f - a));
        d[3] = 255;
    }
    const std::string out = o.out.empty() ? std::string("runs/caption.png") : o.out;
    if (!stbi_write_png(out.c_str(), static_cast<int>(W), static_cast<int>(H), 4, img.data(), static_cast<int>(W) * 4)) {
        std::fprintf(stderr, "caption-shot: PNG write failed\n"); return 1;
    }
    std::printf("caption: %s\nout: %s\n", text.c_str(), out.c_str());
    return 0;
}

int run_tts_check(const Options& o) {
    const std::string text = o.say_text.empty() ? std::string("EVACUATE SECTOR FIVE") : o.say_text;
    const uint32_t sr = 16000u;
    const std::vector<int16_t> pcm = app::synthesize_speech(text, sr);
    const std::string wav = o.out.empty() ? std::string("runs/tts_check.wav") : o.out;
    std::string werr;
    if (!audio::write_wav(wav, pcm, sr, 1u, werr)) { std::fprintf(stderr, "wav: %s\n", werr.c_str()); return 1; }
    const std::string wexe = o.whisper_exe.empty() ? default_whisper_exe() : o.whisper_exe;
    const std::string wmodel = o.whisper_model.empty() ? default_whisper_model() : o.whisper_model;
    const std::string heard = whisper_transcribe(wav, wexe, wmodel);

    // lowercase the transcript, then count how many spoken words (>=3 letters) are
    // recovered as a prefix substring (e.g. "evacuate" -> "evac", "sector" -> "sect").
    std::string low = heard;
    for (char& c : low) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    auto contains = [&](const std::string& sub) { return low.find(sub) != std::string::npos; };
    int spoken = 0, recovered = 0;
    std::string w;
    auto check_word = [&]() {
        if (w.size() < 3) { w.clear(); return; }
        ++spoken;
        std::string key = w.substr(0, w.size() < 5 ? w.size() : 5);
        for (char& c : key) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (contains(key)) ++recovered;
        w.clear();
    };
    for (char c : text) { if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) w.push_back(c); else check_word(); }
    check_word();

    std::printf("said: %s\n", text.c_str());
    std::printf("heard: %s\n", heard.empty() ? "(nothing)" : heard.c_str());
    std::printf("spoken_words: %d\n", spoken);
    std::printf("recovered_words: %d\n", recovered);
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

// ----- M27 verticality: scripted ASCENT of a procedural in-world stairwell -----
// Finds a real up-stair on level 0, generates that chunk (its stairwell steps + walls +
// the carved-open cell), stands the wanderer at the stair cell's low (-X) approach, and
// walks +X. The M27 step-up locomotion carries the capsule up the 0.5 m risers and through
// the ceiling hole. Reports the climb height + the determinism hash (the gate runs it
// twice). Proves the procedural stairs are actually climbable, not just decorative.
int run_ascend(const Options& o) {
    using namespace br::core;
    // Find a level-0 up-stair whose cell is fully interior (all four walls carved open).
    int64_t scx = 0, scz = 0;
    int ci = 0, cj = 0;
    bool found = false;
    for (int64_t r = 0; r < 64 && !found; ++r)
        for (int64_t cz = -r; cz <= r && !found; ++cz)
            for (int64_t cx = -r; cx <= r && !found; ++cx) {
                const br::gen::StairSpec st = br::gen::stair_at(o.seed, 0, cx, cz);
                if (st.present && st.cell_i >= 1 && st.cell_i <= 6 && st.cell_j >= 1 && st.cell_j <= 6) {
                    scx = cx; scz = cz; ci = st.cell_i; cj = st.cell_j; found = true;
                }
            }
    if (!found) { std::printf("no_stair_found: 1\n"); return 6; }

    const contracts::ChunkData cd = contracts::GenerateChunk(o.seed, contracts::ChunkKey{0, scx, scz});
    const float baseY = contracts::level_base_y(0);
    const float ox = static_cast<float>(scx) * contracts::kChunkSize;
    const float oz = static_cast<float>(scz) * contracts::kChunkSize;
    const float cs = br::gen::kCellSize;

    std::vector<Aabb> col;
    col.push_back(Aabb{{ox - 1.0f, baseY - 1.0f, oz - 1.0f},
                       {ox + contracts::kChunkSize + 1.0f, baseY, oz + contracts::kChunkSize + 1.0f}});  // level-0 floor
    for (const auto& b : cd.collision)
        col.push_back(Aabb{{b.mn[0], b.mn[1], b.mn[2]}, {b.mx[0], b.mx[1], b.mx[2]}});

    WorldState s(o.seed);
    s.wanderer.pos = Vec3{ox + static_cast<float>(ci) * cs - 0.3f,         // a short run-up, -X of the steps
                          baseY + kWandererHalfHeight + 0.02f,
                          oz + (static_cast<float>(cj) + 0.5f) * cs};       // cell centre in Z
    s.wanderer.yaw = 0.0f;                                                 // move_x with yaw 0 walks +X

    const float startY = s.wanderer.pos.y;
    float maxY = startY;
    contracts::InputCommand in{};
    in.move_x = 1.0f;  // walk +X into and up the stairwell
    const uint32_t ticks = (o.ticks > 0) ? o.ticks : 1200u;
    for (uint32_t t = 0; t < ticks; ++t) {
        tick(s, in, col);
        if (s.wanderer.pos.y > maxY) maxY = s.wanderer.pos.y;
    }
    const float climb = maxY - startY;
    const bool climbed = (climb >= 3.0f);  // rose past the level-0 ceiling (3 m) up the stairwell

    std::printf("seed: %llu\n", static_cast<unsigned long long>(o.seed));
    std::printf("stair_chunk: %lld %lld cell %d %d\n",
                static_cast<long long>(scx), static_cast<long long>(scz), ci, cj);
    std::printf("start_y: %.3f\n", static_cast<double>(startY));
    std::printf("max_y: %.3f\n", static_cast<double>(maxY));
    std::printf("climb_m: %.3f\n", static_cast<double>(climb));
    std::printf("level_at_max: %d\n", contracts::level_from_y(maxY));
    std::printf("climbed: %d\n", climbed ? 1 : 0);
    std::printf("final_hash: %016llx\n", static_cast<unsigned long long>(world_state_hash(s)));
    return climbed ? 0 : 6;
}

// ----- M28: vertical-streaming see-through proof (headless) -------------------
// Finds a level-0 up-stair, points a camera up at its ceiling hole, and renders the
// scene twice: with TWO floors resident (level 0 + 1) and with one. Both must be
// debug-clean; two-floor residency must be exactly 2x the one-floor ring (bounded);
// and the renders must DIFFER (the floor above shows through the hole = see-through).
int run_vstream(const Options& o) {
    using namespace br::core;
    int64_t scx = 0, scz = 0;
    int ci = 0, cj = 0;
    bool found = false;
    for (int64_t r = 0; r < 64 && !found; ++r)
        for (int64_t cz = -r; cz <= r && !found; ++cz)
            for (int64_t cx = -r; cx <= r && !found; ++cx) {
                const br::gen::StairSpec st = br::gen::stair_at(o.seed, 0, cx, cz);
                if (st.present && st.cell_i >= 1 && st.cell_i <= 6 && st.cell_j >= 1 && st.cell_j <= 6) {
                    scx = cx; scz = cz; ci = st.cell_i; cj = st.cell_j; found = true;
                }
            }
    if (!found) { std::printf("no_stair_found: 1\n"); return 6; }

    const float cs = br::gen::kCellSize;
    // Stand on the clear low approach (-X of the risers, which start 0.3 m in) and look
    // straight up through the ceiling hole the up-stair cuts -> level 1 shows through it.
    const float camx = static_cast<float>(scx) * contracts::kChunkSize + static_cast<float>(ci) * cs + 0.15f;
    const float camz = static_cast<float>(scz) * contracts::kChunkSize + (static_cast<float>(cj) + 0.5f) * cs;
    const float camy = contracts::level_base_y(0) + kWandererHalfHeight + 0.02f + kEyeHeight;
    contracts::CameraPose cam{};
    cam.pos[0] = camx; cam.pos[1] = camy; cam.pos[2] = camz;
    cam.yaw = 0.0f; cam.pitch = 1.5f;  // look straight up through the ceiling hole
    cam.fov_y = 1.2217305f;
    cam.aspect = static_cast<float>(o.width) / static_cast<float>(o.height);
    const contracts::ChunkKey center = contracts::chunk_key_at(0, camx, camz);

    auto render_levels = [&](int32_t extra, std::vector<uint8_t>& rgba, size_t& resident, uint32_t& dbg) -> bool {
        Renderer renderer;
        if (!renderer.init_headless(o.width, o.height)) { std::fprintf(stderr, "init: %s\n", renderer.last_error().c_str()); return false; }
        renderer.set_texture_seed(o.seed);
        br::stream::StreamManager sm(o.seed, static_cast<int>(o.radius), o.workers);
        sm.update(center, extra);
        sm.wait_idle();
        sm.update(center, extra);
        resident = sm.resident_count();
        uint32_t drawn = 0;
        const size_t target = sm.resident_count();
        for (int w = 0; w < 1200 && static_cast<size_t>(drawn) < target; ++w)
            if (!renderer.render_chunks(cam, sm.resident(), 64u, 0u, &drawn)) { std::fprintf(stderr, "render: %s\n", renderer.last_error().c_str()); return false; }
        if (!renderer.render_chunks(cam, sm.resident(), 64u, 0u, &drawn)) { std::fprintf(stderr, "render: %s\n", renderer.last_error().c_str()); return false; }
        FrameImage img;
        if (!renderer.readback(img)) { std::fprintf(stderr, "readback: %s\n", renderer.last_error().c_str()); return false; }
        rgba = img.rgba;
        dbg = renderer.debug_error_count();
        return true;
    };

    std::vector<uint8_t> rgba2, rgba1;
    size_t res2 = 0, res1 = 0;
    uint32_t dbg2 = 0, dbg1 = 0;
    if (!render_levels(1, rgba2, res2, dbg2)) return 1;   // level 0 + 1: see up through the ceiling hole
    if (!render_levels(0, rgba1, res1, dbg1)) return 1;   // level 0 only (extra == center.level -> single ring)

    double acc = 0.0;
    const size_t n = (rgba1.size() < rgba2.size()) ? rgba1.size() : rgba2.size();
    for (size_t i = 0; i < n; ++i) {
        const int d = static_cast<int>(rgba1[i]) - static_cast<int>(rgba2[i]);
        acc += (d < 0) ? -d : d;
    }
    const double see_through = (n > 0) ? acc / static_cast<double>(n) : 0.0;

    std::printf("stair_chunk: %lld %lld cell %d %d\n", static_cast<long long>(scx), static_cast<long long>(scz), ci, cj);
    std::printf("resident_1level: %llu\n", static_cast<unsigned long long>(res1));
    std::printf("resident_2level: %llu\n", static_cast<unsigned long long>(res2));
    std::printf("see_through_diff: %.4f\n", see_through);
    std::printf("debug_error_count: %u\n", dbg1 + dbg2);
    const bool ok = (dbg1 == 0 && dbg2 == 0) && (res1 > 0) && (res2 == res1 * 2) && (see_through > 0.5);
    std::printf("vstream_ok: %d\n", ok ? 1 : 0);
    return ok ? 0 : 6;
}

// ----- M30: scripted soft-catch FALL down an open shaft (headless) ------------
// Finds a real shaft, drops the wanderer in at its top level, and lets gravity carry it
// down the void. The bottom level's solid floor catches it via swept collision -- a soft
// landing (there is no health/fail-state, by design). Reports the fall depth + the
// determinism hash (the gate runs it twice). Proves the bounded soft-catch fall.
int run_shaftfall(const Options& o) {
    using namespace br::core;
    int64_t scx = 0, scz = 0;
    bool found = false;
    for (int64_t r = 0; r < 200 && !found; ++r)
        for (int64_t cz = -r; cz <= r && !found; ++cz)
            for (int64_t cx = -r; cx <= r && !found; ++cx)
                if (br::gen::shaft_at(o.seed, cx, cz).present) { scx = cx; scz = cz; found = true; }
    if (!found) { std::printf("no_shaft_found: 1\n"); return 6; }
    const br::gen::ShaftSpec sh = br::gen::shaft_at(o.seed, scx, scz);

    const float cs = br::gen::kCellSize;
    const float cellx = static_cast<float>(scx) * contracts::kChunkSize + (static_cast<float>(sh.cell_i) + 0.5f) * cs;
    const float cellz = static_cast<float>(scz) * contracts::kChunkSize + (static_cast<float>(sh.cell_j) + 0.5f) * cs;
    const float botY = contracts::level_base_y(sh.top_level - sh.depth);  // the landing floor

    std::vector<Aabb> col;  // only a solid floor at the bottom -> the void above is a free fall
    col.push_back(Aabb{{cellx - cs, botY - 1.0f, cellz - cs}, {cellx + cs, botY, cellz + cs}});

    WorldState s(o.seed);
    s.wanderer.pos = Vec3{cellx, contracts::level_base_y(sh.top_level) + kWandererHalfHeight + 0.5f, cellz};
    const int32_t startLevel = contracts::level_from_y(s.wanderer.pos.y);
    contracts::InputCommand in{};  // no input: pure gravity
    const uint32_t ticks = (o.ticks > 0) ? o.ticks : 1200u;
    float maxFall = 0.0f;
    for (uint32_t t = 0; t < ticks; ++t) {
        tick(s, in, col);
        if (-s.wanderer.vel.y > maxFall) maxFall = -s.wanderer.vel.y;
    }
    const int32_t endLevel = contracts::level_from_y(s.wanderer.pos.y);
    const bool landed = s.wanderer.on_ground && endLevel == (sh.top_level - sh.depth);

    std::printf("seed: %llu\n", static_cast<unsigned long long>(o.seed));
    std::printf("shaft_chunk: %lld %lld cell %d %d\n",
                static_cast<long long>(scx), static_cast<long long>(scz), sh.cell_i, sh.cell_j);
    std::printf("top_level: %d\n", sh.top_level);
    std::printf("depth: %d\n", sh.depth);
    std::printf("start_level: %d\n", startLevel);
    std::printf("end_level: %d\n", endLevel);
    std::printf("fell_floors: %d\n", startLevel - endLevel);
    std::printf("max_fall_speed: %.2f\n", static_cast<double>(maxFall));
    std::printf("landed: %d\n", landed ? 1 : 0);
    std::printf("final_hash: %016llx\n", static_cast<unsigned long long>(world_state_hash(s)));
    return landed ? 0 : 6;
}

// ----- M30: scripted LIVE DESCENT through a procedural DOWN-STAIR hole (headless) -
// Live descent is the despair gradient in-game: the interactive walks (run_play/run_game/
// run_screensaver) now build their floor PER CELL with HOLES at the open cells (down-stair
// holes + shaft voids) instead of one sealed ground plane, so you fall through real openings.
// This proves THAT exact path with no window -- it builds the SAME build_walk_collision world
// and rebuilds it per level as the wanderer falls, exactly like the live loop. Asserts both
// halves of a correct holed floor: (A) a SOLID cell still HOLDS the wanderer up, and (B) the
// open DOWN-STAIR hole DROPS it a floor + the level below soft-catches it. Reports the descent +
// the determinism hash (the gate runs it twice). Model-free, deterministic.
int run_livedescent(const Options& o) {
    using namespace br::core;
    // Find a CLEAN level-0 down-stair hole: an interior cell where the level BELOW (-1) has an
    // up-stair (so it pokes up through level 0's floor -> floor_hole_at is true), and where level 0
    // does NOT itself build an up-stairwell at the same cell (a co-located up-stair fills the cell
    // with steps that catch you on level 0 -- a stacked stair-junction, traversable but not a clean
    // fall). Picking a clean hole is fixture choice, exactly like run_ascend picks an interior up-stair.
    int64_t scx = 0, scz = 0;
    int ci = 0, cj = 0;
    bool found = false;
    for (int64_t r = 0; r < 64 && !found; ++r)
        for (int64_t cz = -r; cz <= r && !found; ++cz)
            for (int64_t cx = -r; cx <= r && !found; ++cx) {
                const br::gen::StairSpec dn = br::gen::stair_at(o.seed, -1, cx, cz);  // up-stair below -> my floor hole
                const br::gen::StairSpec up = br::gen::stair_at(o.seed,  0, cx, cz);  // my own up-stairwell?
                const bool clear = !(up.present && up.cell_i == dn.cell_i && up.cell_j == dn.cell_j);
                if (dn.present && clear && dn.cell_i >= 1 && dn.cell_i <= 6 && dn.cell_j >= 1 && dn.cell_j <= 6) {
                    scx = cx; scz = cz; ci = dn.cell_i; cj = dn.cell_j; found = true;
                }
            }
    if (!found) { std::printf("no_downhole_found: 1\n"); return 6; }

    const float cs = br::gen::kCellSize;
    const float holex = static_cast<float>(scx) * contracts::kChunkSize + (static_cast<float>(ci) + 0.5f) * cs;
    const float holez = static_cast<float>(scz) * contracts::kChunkSize + (static_cast<float>(cj) + 0.5f) * cs;

    // Per-tick rebuild keyed on the wanderer's current floor -- IDENTICAL to run_play's loop.
    std::vector<Aabb> col;
    contracts::ChunkKey cached{ 999, 0, 0 };  // sentinel -> always rebuilds on the first step
    auto step_world = [&](WorldState& w, uint32_t ticks) {
        const contracts::InputCommand in{};  // no input: pure gravity
        for (uint32_t t = 0; t < ticks; ++t) {
            const contracts::ChunkKey here = contracts::chunk_key_at(
                contracts::level_from_y(w.wanderer.pos.y), w.wanderer.pos.x, w.wanderer.pos.z);
            if (here != cached) { build_walk_collision(col, o.seed, here); cached = here; }
            tick(w, in, col);
        }
    };

    // (A) a SOLID cell holds the wanderer up. Pick the first interior cell that is NOT a hole.
    int si = ci, sj = cj;
    for (int jj = 1; jj <= 6 && si == ci && sj == cj; ++jj)
        for (int ii = 1; ii <= 6 && si == ci && sj == cj; ++ii)
            if (!br::gen::floor_hole_at(o.seed, 0, scx, scz, ii, jj)) { si = ii; sj = jj; }
    const float solidx = static_cast<float>(scx) * contracts::kChunkSize + (static_cast<float>(si) + 0.5f) * cs;
    const float solidz = static_cast<float>(scz) * contracts::kChunkSize + (static_cast<float>(sj) + 0.5f) * cs;
    WorldState ssolid(o.seed);
    ssolid.wanderer.pos = Vec3{ solidx, contracts::level_base_y(0) + kWandererHalfHeight + 0.5f, solidz };
    cached = contracts::ChunkKey{ 999, 0, 0 };
    step_world(ssolid, 240u);
    const bool solid_holds = (contracts::level_from_y(ssolid.wanderer.pos.y) == 0) && ssolid.wanderer.on_ground;

    // (B) the open down-stair hole drops you a floor; the level below soft-catches you.
    WorldState s(o.seed);
    s.wanderer.pos = Vec3{ holex, contracts::level_base_y(0) + kWandererHalfHeight + 0.5f, holez };
    const int32_t startLevel = contracts::level_from_y(s.wanderer.pos.y);
    cached = contracts::ChunkKey{ 999, 0, 0 };
    step_world(s, 600u);
    const int32_t endLevel = contracts::level_from_y(s.wanderer.pos.y);
    const bool descended = (endLevel < startLevel) && s.wanderer.on_ground;

    std::printf("seed: %llu\n", static_cast<unsigned long long>(o.seed));
    std::printf("downhole_chunk: %lld %lld cell %d %d\n",
                static_cast<long long>(scx), static_cast<long long>(scz), ci, cj);
    std::printf("solid_cell: %d %d\n", si, sj);
    std::printf("solid_holds: %d\n", solid_holds ? 1 : 0);
    std::printf("start_level: %d\n", startLevel);
    std::printf("end_level: %d\n", endLevel);
    std::printf("descended: %d\n", descended ? 1 : 0);
    std::printf("landed: %d\n", s.wanderer.on_ground ? 1 : 0);
    std::printf("final_hash: %016llx\n", static_cast<unsigned long long>(world_state_hash(s)));
    return (solid_holds && descended) ? 0 : 6;
}

// ----- M30: DEEP-DESCENT SOAK (headless, long-haul) ---------------------------
// The ROADMAP §3 DONE criterion: "a deep-descent soak holds determinism + bounded memory."
// Repeatedly falls the wanderer down a deep shaft (5..10 floors) using the FULL live machinery
// -- the holed per-cell floor (build_walk_collision, per-level rebuild), the abyss band-residency
// (StreamManager::update(center, lo, hi)), and a headless render each frame -- so every descent
// churns level transitions, streaming load/evict, collision rebuilds, and GPU uploads. On landing
// it teleports back to the shaft top and falls again (N cycles), which is what stresses the new
// vertical paths over the long haul. Asserts: many cycles completed (each reaches the bottom = no
// stuck), residency stays BOUNDED (the band never balloons), process memory is FLAT (no leak), and
// -- run under --ticks -- the world hash is reproducible (the gate runs it twice). Model-free.
int run_descentsoak(const Options& o) {
    using namespace std::chrono;
    using namespace br::core;
    // The deepest interior shaft near origin -> the deepest single drop we can soak deterministically.
    int64_t scx = 0, scz = 0; br::gen::ShaftSpec sh; bool found = false;
    for (int64_t r = 0; r < 200 && !found; ++r)
        for (int64_t cz = -r; cz <= r && !found; ++cz)
            for (int64_t cx = -r; cx <= r && !found; ++cx) {
                const br::gen::ShaftSpec c = br::gen::shaft_at(o.seed, cx, cz);
                if (c.present && c.cell_i >= 1 && c.cell_i <= 6 && c.cell_j >= 1 && c.cell_j <= 6) {
                    scx = cx; scz = cz; sh = c; found = true;
                }
            }
    if (!found) { std::printf("no_shaft_found: 1\n"); return 6; }

    Renderer renderer;
    if (!renderer.init_headless(o.width, o.height)) { std::fprintf(stderr, "init: %s\n", renderer.last_error().c_str()); return 1; }
    renderer.set_texture_seed(o.seed);
    br::stream::StreamManager sm(o.seed, static_cast<int>(o.radius), o.workers);
    const float aspect = static_cast<float>(o.width) / static_cast<float>(o.height);

    const float cs = br::gen::kCellSize;
    const float cellx = static_cast<float>(scx) * contracts::kChunkSize + (static_cast<float>(sh.cell_i) + 0.5f) * cs;
    const float cellz = static_cast<float>(scz) * contracts::kChunkSize + (static_cast<float>(sh.cell_j) + 0.5f) * cs;
    const int32_t topL = sh.top_level;
    const int32_t botL = sh.top_level - sh.depth;
    const int32_t kBand = 4;  // floors streamed below the wanderer for the abyss (bounds residency)

    WorldState s(o.seed);
    auto spawn_top = [&]() {
        s.wanderer.pos = Vec3{ cellx, contracts::level_base_y(topL) + kWandererHalfHeight + 0.5f, cellz };
        s.wanderer.vel = Vec3{ 0.0f, 0.0f, 0.0f };
    };
    spawn_top();

    std::vector<Aabb> col;
    contracts::ChunkKey cached{ 0x7fffffff, 0, 0 };  // sentinel -> rebuild on first tick
    auto rebuild = [&](contracts::ChunkKey c) { build_walk_collision(col, o.seed, c); cached = c; };

    const bool wall = (o.seconds > 0);
    const uint64_t tick_target = (o.ticks > 0) ? o.ticks : 60000u;
    const auto start = steady_clock::now();
    const auto wall_end = start + seconds(static_cast<long long>(o.seconds));
    const uint32_t tpf = (o.ticks_per_frame > 0) ? o.ticks_per_frame : 8u;

    uint64_t frame = 0, cycles = 0, stuck = 0, mem_first = 0, mem_last = 0;
    size_t max_resident = 0;
    uint64_t cycle_start_tick = 0;
    bool reached_bottom_each = true;
    const contracts::InputCommand grav{};  // pure gravity -- no input

    for (;;) {
        if (wall) { if ((frame & 63u) == 0 && steady_clock::now() >= wall_end) break; }
        else if (s.tick >= tick_target) break;

        for (uint32_t k = 0; k < tpf; ++k) {
            const contracts::ChunkKey here = contracts::chunk_key_at(
                contracts::level_from_y(s.wanderer.pos.y), s.wanderer.pos.x, s.wanderer.pos.z);
            if (here != cached) rebuild(here);
            tick(s, grav, col);
            // Landed at the shaft bottom -> count a cycle, teleport back to the top, fall again.
            if (s.wanderer.on_ground && contracts::level_from_y(s.wanderer.pos.y) <= botL) {
                ++cycles;
                spawn_top();
                cached = contracts::ChunkKey{ 0x7fffffff, 0, 0 };  // force a rebuild at the top
                cycle_start_tick = s.tick;
            } else if (s.tick - cycle_start_tick > 4000u) {
                // Should fall + land in well under 4000 ticks; if not, it's stuck mid-shaft.
                ++stuck; cycle_start_tick = s.tick;
            }
        }

        // Abyss band: stream the current floor + up to kBand below (clamped to the shaft bottom).
        const int32_t curL = contracts::level_from_y(s.wanderer.pos.y);
        const int32_t loL = ((curL - kBand) > botL) ? (curL - kBand) : botL;
        const contracts::ChunkKey center = contracts::chunk_key_at(curL, s.wanderer.pos.x, s.wanderer.pos.z);
        sm.update(center, loL, curL);
        if (sm.resident_count() > max_resident) max_resident = sm.resident_count();

        const contracts::CameraPose cam = wanderer_camera(s, aspect);
        uint32_t drawn = 0;
        if (!renderer.render_chunks(cam, sm.resident(), 16u, s.tick, &drawn)) { std::fprintf(stderr, "render: %s\n", renderer.last_error().c_str()); return 1; }
        const uint64_t mem = renderer.process_private_bytes();
        if (frame == 0 || frame == 64) mem_first = mem;  // baseline AFTER warmup (frame 64) -> measures steady-state growth, not allocator warmup
        mem_last = mem;
        ++frame;
    }

    if (stuck > 0) reached_bottom_each = false;
    const double mem_growth_mb = (static_cast<double>(mem_last) - static_cast<double>(mem_first)) / (1024.0 * 1024.0);
    // Residency is bounded by the band: (kBand+1) levels x the (2r+1)^2 ring, + a small slack for
    // in-flight/transition chunks. A leak or an unbounded ring would blow past this.
    const size_t ring = static_cast<size_t>(2 * o.radius + 1) * static_cast<size_t>(2 * o.radius + 1);
    const size_t resident_cap = static_cast<size_t>(kBand + 2) * ring;

    std::printf("seed: %llu\n", static_cast<unsigned long long>(o.seed));
    std::printf("shaft_chunk: %lld %lld cell %d %d\n",
                static_cast<long long>(scx), static_cast<long long>(scz), sh.cell_i, sh.cell_j);
    std::printf("shaft_depth: %d\n", sh.depth);
    std::printf("frames: %llu\n", static_cast<unsigned long long>(frame));
    std::printf("descent_cycles: %llu\n", static_cast<unsigned long long>(cycles));
    std::printf("stuck_windows: %llu\n", static_cast<unsigned long long>(stuck));
    std::printf("max_resident: %llu\n", static_cast<unsigned long long>(max_resident));
    std::printf("resident_cap: %llu\n", static_cast<unsigned long long>(resident_cap));
    std::printf("mem_first_mb: %.2f\n", static_cast<double>(mem_first) / (1024.0 * 1024.0));
    std::printf("mem_last_mb: %.2f\n", static_cast<double>(mem_last) / (1024.0 * 1024.0));
    std::printf("mem_growth_mb: %.2f\n", mem_growth_mb);
    std::printf("final_hash: %016llx\n", static_cast<unsigned long long>(world_state_hash(s)));
    const bool ok = reached_bottom_each && (cycles >= 5) &&
                    (max_resident > 0 && max_resident <= resident_cap) &&
                    (mem_growth_mb < 32.0);  // post-warmup steady-state growth (~3 MB observed); a real leak is 100s of MB
    std::printf("descentsoak_ok: %d\n", ok ? 1 : 0);
    return ok ? 0 : 6;
}

// ----- M30: headless ABYSS render proof (look DOWN an open shaft) -------------
// Finds a shaft, points a camera down its void from the top floor, and renders with a BAND
// of floors resident (the abyss) vs just the top floor. The band shows several floors down
// (then black where the bounded ring ends = fog-to-black); the renders must DIFFER + be
// debug-clean, and the band's residency is exactly (renderDepth+1)x the one-floor ring.
int run_abyss(const Options& o) {
    using namespace br::core;
    int64_t scx = 0, scz = 0;
    br::gen::ShaftSpec sh;
    bool found = false;
    for (int64_t r = 0; r < 200 && !found; ++r)
        for (int64_t cz = -r; cz <= r && !found; ++cz)
            for (int64_t cx = -r; cx <= r && !found; ++cx) {
                const br::gen::ShaftSpec s = br::gen::shaft_at(o.seed, cx, cz);
                if (s.present && s.cell_i >= 1 && s.cell_i <= 6 && s.cell_j >= 1 && s.cell_j <= 6) {
                    scx = cx; scz = cz; sh = s; found = true;
                }
            }
    if (!found) { std::printf("no_shaft_found: 1\n"); return 6; }

    const float cs = br::gen::kCellSize;
    const float camx = static_cast<float>(scx) * contracts::kChunkSize + (static_cast<float>(sh.cell_i) + 0.5f) * cs;
    const float camz = static_cast<float>(scz) * contracts::kChunkSize + (static_cast<float>(sh.cell_j) + 0.5f) * cs;
    const float camy = contracts::level_base_y(sh.top_level) + kWandererHalfHeight + 0.02f + kEyeHeight;
    contracts::CameraPose cam{};
    cam.pos[0] = camx; cam.pos[1] = camy; cam.pos[2] = camz;
    cam.yaw = 0.0f; cam.pitch = -1.3f;  // look down into the void
    cam.fov_y = 1.2217305f;
    cam.aspect = static_cast<float>(o.width) / static_cast<float>(o.height);
    const contracts::ChunkKey center = contracts::chunk_key_at(sh.top_level, camx, camz);
    const int32_t renderDepth = (sh.depth < 4) ? sh.depth : 4;  // a few floors down, then fog-to-black

    auto render_band = [&](int32_t lo, int32_t hi, std::vector<uint8_t>& rgba, size_t& resident, uint32_t& dbg) -> bool {
        Renderer renderer;
        if (!renderer.init_headless(o.width, o.height)) { std::fprintf(stderr, "init: %s\n", renderer.last_error().c_str()); return false; }
        renderer.set_texture_seed(o.seed);
        br::stream::StreamManager sm(o.seed, static_cast<int>(o.radius), o.workers);
        sm.update(center, lo, hi);
        sm.wait_idle();
        sm.update(center, lo, hi);
        resident = sm.resident_count();
        uint32_t drawn = 0;
        const size_t target = sm.resident_count();
        for (int w = 0; w < 2000 && static_cast<size_t>(drawn) < target; ++w)
            if (!renderer.render_chunks(cam, sm.resident(), 64u, 0u, &drawn)) { std::fprintf(stderr, "render: %s\n", renderer.last_error().c_str()); return false; }
        if (!renderer.render_chunks(cam, sm.resident(), 64u, 0u, &drawn)) { std::fprintf(stderr, "render: %s\n", renderer.last_error().c_str()); return false; }
        FrameImage img;
        if (!renderer.readback(img)) { std::fprintf(stderr, "readback: %s\n", renderer.last_error().c_str()); return false; }
        rgba = img.rgba;
        dbg = renderer.debug_error_count();
        return true;
    };

    std::vector<uint8_t> deep, shallow;
    size_t resDeep = 0, resShallow = 0;
    uint32_t dbgDeep = 0, dbgShallow = 0;
    if (!render_band(sh.top_level - renderDepth, sh.top_level, deep, resDeep, dbgDeep)) return 1;
    if (!render_band(sh.top_level, sh.top_level, shallow, resShallow, dbgShallow)) return 1;

    double acc = 0.0;
    const size_t n = (deep.size() < shallow.size()) ? deep.size() : shallow.size();
    for (size_t i = 0; i < n; ++i) {
        const int d = static_cast<int>(deep[i]) - static_cast<int>(shallow[i]);
        acc += (d < 0) ? -d : d;
    }
    const double abyss_diff = (n > 0) ? acc / static_cast<double>(n) : 0.0;

    std::printf("seed: %llu\n", static_cast<unsigned long long>(o.seed));
    std::printf("shaft_chunk: %lld %lld cell %d %d\n", static_cast<long long>(scx), static_cast<long long>(scz), sh.cell_i, sh.cell_j);
    std::printf("top_level: %d\n", sh.top_level);
    std::printf("render_depth: %d\n", renderDepth);
    std::printf("resident_shallow: %llu\n", static_cast<unsigned long long>(resShallow));
    std::printf("resident_deep: %llu\n", static_cast<unsigned long long>(resDeep));
    std::printf("abyss_diff: %.4f\n", abyss_diff);
    std::printf("debug_error_count: %u\n", dbgDeep + dbgShallow);
    const bool ok = (dbgDeep == 0 && dbgShallow == 0) && (resShallow > 0) &&
                    (resDeep == resShallow * static_cast<size_t>(renderDepth + 1)) && (abyss_diff > 0.5);
    std::printf("abyss_ok: %d\n", ok ? 1 : 0);
    return ok ? 0 : 6;
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
// A FRAMED screenshot (marketing / QC): walk the natural Stroller into the maze so the camera sits mid-corridor
// (the fixed-(16,16) --shot / --dxr-pt sit in the spawn cell, often against a wall), then render ONE clean frame
// from its POV -- ray-traced (--rt, converged --spp) or raster. --seed varies the world, --ticks how far to walk.
// The Stroller looks DOWN corridors (low faceplant), so the framing is natural. Presentation/QC only -- no gate,
// no golden; never enables the flashlight.

// ----- POC: "the world RECOLOURS based on what you SAY" -----------------------
// The local-LLM Director already HEARS you (mic -> whisper). This proves the next step: your arbitrary words ->
// the LLM understands -> the walls visibly change colour. Headless QC so it's testable from the CLI: build a
// raster scene, render it, ask KEEL to pick a colour from --say, CPU-grade the readback toward that hue, write
// <out>_before.png + <out>_after.png. Presentation-only -- no sim/replay/gate touched. Needs KEEL up (keel-up.ps1).
static std::string render_recolor_prompt(const std::string& phrase) {
    return std::string(
        "You control the wall colour of an endless, monotonous YELLOW backrooms. The lone wanderer just said aloud: \"")
        + phrase +
        "\". Decide the wall colour they would want NOW. If they dislike the yellow, want a change, name a colour, or "
        "set a mood, pick a fitting colour and reply with ONLY 'r,g,b' (each 0-255). Disliking the current yellow "
        "means change it to a DIFFERENT colour. Examples: \"i hate this yellow\" -> 200,30,30 ; \"make it blue\" -> "
        "40,80,220 ; \"too cold and clinical\" -> 230,120,40 ; \"calmer\" -> 90,150,120 ; \"i'm so bored\" -> 150,40,180 . "
        "Reply NONE only if they said something with NO bearing on how the place looks or feels (e.g. \"what time is it\"). "
        "Reply with ONLY 'r,g,b' or NONE.";
}
// Pull the first three 0-255 integers out of the model's reply (tolerates 'r,g,b', 'rgb(...)', or prose).
static bool parse_recolor(const std::string& reply, uint8_t& r, uint8_t& g, uint8_t& b) {
    int v[3]; int n = 0; size_t i = 0;
    while (i < reply.size() && n < 3) {
        if (reply[i] >= '0' && reply[i] <= '9') {
            int x = 0; while (i < reply.size() && reply[i] >= '0' && reply[i] <= '9') { x = x * 10 + (reply[i] - '0'); ++i; }
            v[n++] = (x > 255) ? 255 : x;
        } else { ++i; }
    }
    if (n < 3) return false;
    r = static_cast<uint8_t>(v[0]); g = static_cast<uint8_t>(v[1]); b = static_cast<uint8_t>(v[2]);
    return true;
}
// CPU colour-grade RGBA toward a target hue, preserving each pixel's brightness (lit walls stay lit -- a vivid
// recolour, not a dim multiply). strength 0..1.
static void apply_recolor(std::vector<uint8_t>& rgba, uint8_t tr, uint8_t tg, uint8_t tb, float strength) {
    const float t[3] = { tr / 255.0f, tg / 255.0f, tb / 255.0f };
    float tl = 0.2126f * t[0] + 0.7152f * t[1] + 0.0722f * t[2]; if (tl < 1e-3f) tl = 1e-3f;
    const float hue[3] = { t[0] / tl, t[1] / tl, t[2] / tl };   // unit-luminance target colour
    for (size_t p = 0; p + 3 < rgba.size(); p += 4) {
        const float c[3] = { rgba[p] / 255.0f, rgba[p + 1] / 255.0f, rgba[p + 2] / 255.0f };
        const float L = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
        for (int k = 0; k < 3; ++k) {
            float out = c[k] * (1.0f - strength) + (L * hue[k]) * strength;
            if (out < 0.0f) out = 0.0f;
            if (out > 1.0f) out = 1.0f;
            rgba[p + k] = static_cast<uint8_t>(out * 255.0f + 0.5f);
        }
    }
}
int run_recolor_shot(const Options& o) {
    using namespace br::core;
    // 1) build + frame a raster scene (Stroller walk -> POV), like --game-shot
    WorldState s(o.seed);
    s.wanderer.pos = Vec3{ 2.0f, kWandererHalfHeight + 0.02f, 2.0f };
    Stroller bot(o.seed ^ 0x5170990000000001ull, s.wanderer.pos);
    std::vector<Aabb> col; contracts::ChunkKey cached{ 0x7fffffff, 0, 0 };
    auto rebuild = [&](contracts::ChunkKey c) { build_walk_collision(col, o.seed, c); cached = c; };
    rebuild(contracts::chunk_key_at(0, s.wanderer.pos.x, s.wanderer.pos.z));
    const uint64_t steps = (o.ticks > 0) ? o.ticks : 1800u;
    for (uint64_t t = 0; t < steps; ++t) {
        const contracts::ChunkKey here = contracts::chunk_key_at(contracts::level_from_y(s.wanderer.pos.y), s.wanderer.pos.x, s.wanderer.pos.z);
        if (here != cached) rebuild(here);
        tick(s, bot.step(s, o.seed, col), col);
    }
    contracts::CameraPose cam{};
    cam.pos[0] = s.wanderer.pos.x; cam.pos[1] = s.wanderer.pos.y + kEyeHeight; cam.pos[2] = s.wanderer.pos.z;
    cam.yaw = s.wanderer.yaw; cam.pitch = 0.0f; cam.fov_y = 1.2217305f; cam.aspect = static_cast<float>(o.width) / static_cast<float>(o.height);
    br::stream::StreamManager sm(o.seed, static_cast<int>(o.radius), o.workers);
    const contracts::ChunkKey center = contracts::chunk_key_at(contracts::level_from_y(s.wanderer.pos.y), s.wanderer.pos.x, s.wanderer.pos.z);
    sm.update(center); sm.wait_idle(); sm.update(center);
    Renderer renderer;
    if (!renderer.init_headless(o.width, o.height)) { std::fprintf(stderr, "init: %s\n", renderer.last_error().c_str()); return 1; }
    renderer.set_texture_seed(o.seed);
    uint32_t drawn = 0; const size_t tgt = sm.resident_count();
    for (int wk = 0; wk < 400 && static_cast<size_t>(drawn) < tgt; ++wk) renderer.render_chunks(cam, sm.resident(), 64u, 0u, &drawn);
    renderer.render_chunks(cam, sm.resident(), 256u, 0u, &drawn);
    FrameImage img; if (!renderer.readback(img)) { std::fprintf(stderr, "readback: %s\n", renderer.last_error().c_str()); return 1; }
    // 2) write the BEFORE frame
    std::string base = o.out.empty() ? std::string("runs/recolor") : o.out;
    if (base.size() > 4 && base.substr(base.size() - 4) == ".png") base = base.substr(0, base.size() - 4);
    const std::string beforePng = base + "_before.png";
    stbi_write_png(beforePng.c_str(), static_cast<int>(img.width), static_cast<int>(img.height), 4, img.rgba.data(), static_cast<int>(img.width) * 4);
    // 3) ask the LLM for a colour from the utterance, then 4) grade toward it
    const std::string phrase = o.say_text.empty() ? std::string("I hate this yellow") : o.say_text;
    std::string host; int port; parse_host_port(o.director_url, host, port);
    const br::director::KeelResponse resp = br::director::keel_complete(host, port, render_recolor_prompt(phrase), 8000);
    std::printf("said: \"%s\"\n", phrase.c_str());
    std::printf("director_reply: %s\n", resp.ok ? resp.content.c_str() : "(KEEL unreachable -- run scripts\\keel-up.ps1)");
    uint8_t tr = 0, tg = 0, tb = 0; const bool ok = resp.ok && parse_recolor(resp.content, tr, tg, tb);
    if (ok) {
        std::printf("recolor: YES rgb(%u,%u,%u)\n", tr, tg, tb);
        apply_recolor(img.rgba, tr, tg, tb, 0.78f);
        const std::string afterPng = base + "_after.png";
        stbi_write_png(afterPng.c_str(), static_cast<int>(img.width), static_cast<int>(img.height), 4, img.rgba.data(), static_cast<int>(img.width) * 4);
        std::printf("out: %s + %s\n", beforePng.c_str(), afterPng.c_str());
    } else {
        std::printf("recolor: NONE (no colour intent, or KEEL down) -- only %s written\n", beforePng.c_str());
    }
    return 0;
}

// Headless walkability probe for the infinite ladder: stand on the crossing, walk +X (down) then back -X (up)
// using the REAL game collision (build_walk_collision + the band carve in apply_to_collision, rebuilt on every
// chunk/level key change exactly like run_game). Directly tests the operator's "mysteriously drop between floors"
// report -- any single tick that plunges more than one step proves a seam between the deep slabs across a level
// rebuild. Deterministic; no render, no logging.
int run_ladder_walk(const Options& o) {
    using namespace br::core;
    WorldState s(o.seed);
    s.wanderer.pos = Vec3{ app::ladder::kAnchorX,
                           app::ladder::surface_y(app::ladder::kAnchorX) + kWandererHalfHeight + 0.02f,
                           app::ladder::kAnchorZ };
    s.wanderer.yaw = 0.0f;   // yaw 0 -> move_x strafes world +X (down the descent), -X back up

    std::vector<Aabb> col;
    contracts::ChunkKey cached{ 0x7fffffff, 1, 1 };
    auto rebuild = [&](contracts::ChunkKey c) {
        build_walk_collision(col, o.seed, c); app::ladder::apply_to_collision(col, c); cached = c; };
    auto rekey = [&]() {
        const contracts::ChunkKey here = contracts::chunk_key_at(
            contracts::level_from_y(s.wanderer.pos.y), s.wanderer.pos.x, s.wanderer.pos.z);
        if (here != cached) rebuild(here);
    };
    rekey();

    const uint32_t half = (o.ticks > 0) ? o.ticks : 1800u;
    const float startY = s.wanderer.pos.y;
    float worstDrop = 0.0f, worstAtX = 0.0f;
    uint32_t air = 0, maxAir = 0;

    contracts::InputCommand down{}; down.move_x = 1.0f;   // descend +X
    for (uint32_t t = 0; t < half; ++t) {
        const float y0 = s.wanderer.pos.y;
        rekey();
        tick(s, down, col);
        const float drop = y0 - s.wanderer.pos.y;          // +ve == descended this tick
        if (drop > worstDrop) { worstDrop = drop; worstAtX = s.wanderer.pos.x; }
        if (!s.wanderer.on_ground) { ++air; if (air > maxAir) maxAir = air; } else air = 0;
    }
    const float lowY = s.wanderer.pos.y, lowX = s.wanderer.pos.x;

    float worstRise = 0.0f;
    contracts::InputCommand up{}; up.move_x = -1.0f;       // ascend -X
    for (uint32_t t = 0; t < half; ++t) {
        const float y0 = s.wanderer.pos.y;
        rekey();
        tick(s, up, col);
        const float rise = s.wanderer.pos.y - y0;
        if (rise > worstRise) worstRise = rise;
    }
    const float endY = s.wanderer.pos.y;
    const float descended = startY - lowY, climbed = endY - lowY;

    const bool noFall = worstDrop <= 0.70f;                // a step is 0.5 m; > 0.7 in one tick == a slab seam
    const bool wentDown = descended > 6.0f;                // got down at least ~1.5 levels
    const bool cameBack = climbed > 4.0f;                  // and climbed back up at least ~1 level
    const bool pass = noFall && wentDown && cameBack;

    std::printf("ladder_walk seed=%llu\n", static_cast<unsigned long long>(o.seed));
    std::printf("start_y: %.2f  low_y: %.2f (x=%.1f)  end_y: %.2f\n",
                (double)startY, (double)lowY, (double)lowX, (double)endY);
    std::printf("descended: %.2f m (%.1f levels)  climbed_back: %.2f m\n",
                (double)descended, (double)descended / 4.0, (double)climbed);
    std::printf("worst_single_tick_drop: %.3f m (at x=%.1f)   [fall-through if > 0.70]\n",
                (double)worstDrop, (double)worstAtX);
    std::printf("worst_single_tick_rise: %.3f m   max_consecutive_airborne_ticks: %u\n",
                (double)worstRise, maxAir);
    std::printf("VERDICT: %s\n", pass ? "PASS (walkable both ways, no fall-through)" : "FAIL");
    return pass ? 0 : 3;
}

int run_game_shot(const Options& o) {
    using namespace br::core;
    WorldState s(o.seed);
    s.wanderer.pos = Vec3{ 2.0f, kWandererHalfHeight + 0.02f, 2.0f };
    Stroller bot(o.seed ^ 0x5170990000000001ull, s.wanderer.pos);
    std::vector<Aabb> col;
    contracts::ChunkKey cached{ 0x7fffffff, 0, 0 };
    auto rebuild = [&](contracts::ChunkKey c) { build_walk_collision(col, o.seed, c); cached = c; };
    rebuild(contracts::chunk_key_at(0, s.wanderer.pos.x, s.wanderer.pos.z));
    const uint64_t steps = o.ladder_shot ? 0u : ((o.ticks > 0) ? o.ticks : 1800u);   // ladder QC stays at spawn; else ~15 s into the maze
    for (uint64_t t = 0; t < steps; ++t) {
        const contracts::ChunkKey here = contracts::chunk_key_at(contracts::level_from_y(s.wanderer.pos.y), s.wanderer.pos.x, s.wanderer.pos.z);
        if (here != cached) rebuild(here);
        tick(s, bot.step(s, o.seed, col), col);
    }
    contracts::CameraPose cam{};
    cam.pos[0] = s.wanderer.pos.x; cam.pos[1] = s.wanderer.pos.y + kEyeHeight; cam.pos[2] = s.wanderer.pos.z;
    cam.yaw = s.wanderer.yaw; cam.pitch = 0.0f;
    cam.fov_y = 1.2217305f; cam.aspect = static_cast<float>(o.width) / static_cast<float>(o.height);
    if (o.ladder_shot && o.pose == 1u) {   // ladder QC: stand up-ramp on the stair, look DOWN the descent through the floor-holes
        cam.pos[0] = app::ladder::kAnchorX - 2.0f;                   // up-ramp (X=0 -> Y=2, a level up)
        cam.pos[2] = app::ladder::kAnchorZ;                          // band centre
        cam.pos[1] = app::ladder::surface_y(cam.pos[0]) + kEyeHeight; // eye height above the step
        cam.yaw = 1.5707963f; cam.pitch = -0.48f;                   // +X, look down the stairwell
    } else if (o.ladder_shot) {            // ladder QC: from the SPAWN, look +Z toward the ladder (a short walk away, glowing)
        cam.pos[0] = 2.0f; cam.pos[1] = 1.0f + kEyeHeight; cam.pos[2] = 2.0f;   // spawn, eye height
        cam.yaw = 0.0f; cam.pitch = -0.04f;                                     // +Z toward the band Z[6,10]
    }

    br::stream::StreamManager sm(o.seed, static_cast<int>(o.radius), o.workers);
    const contracts::ChunkKey center = contracts::chunk_key_at(contracts::level_from_y(s.wanderer.pos.y), s.wanderer.pos.x, s.wanderer.pos.z);
    if (o.ladder_shot) {   // keep several floors resident so the QC shot shows the stairwell through them (like the real game)
        sm.update(center, center.level - 3, center.level + 1); sm.wait_idle();
        sm.update(center, center.level - 3, center.level + 1);
    } else {
        sm.update(center); sm.wait_idle(); sm.update(center);
    }
    const std::string out = o.out.empty() ? std::string("runs/game_shot.png") : o.out;
    uint32_t dbg = 0;

    if (o.rt) {
        br::render_dxr::DxrRenderer r;
        if (!r.init(o.width, o.height)) { std::fprintf(stderr, "dxr init: %s\n", r.last_error().c_str()); return 1; }
        if (!r.build_scene(sm.resident())) { std::fprintf(stderr, "dxr scene: %s\n", r.last_error().c_str()); return 1; }
        if (o.drop_flares) {   // QC A/B: a short line of green flares receding ahead along the view, on the floor
            app::FlareField ff;
            const float cy = cam.pos[1] - kEyeHeight + 0.15f;
            const float dx = std::sin(cam.yaw), dz = std::cos(cam.yaw);
            for (int k = 1; k <= 6; ++k) ff.drop(Vec3{ cam.pos[0] + dx * static_cast<float>(k) * 1.8f, cy, cam.pos[2] + dz * static_cast<float>(k) * 1.8f });
            float fg[256]; const uint32_t nf = ff.pack_nearest(Vec3{ cam.pos[0], cam.pos[1], cam.pos[2] }, 2.4f, 64u, fg);
            r.set_flares(fg, nf);
        }
        const uint32_t spp = (o.spp > 0) ? o.spp : 320u;
        if (!r.render_pt(cam, spp, static_cast<uint32_t>(o.seed))) { std::fprintf(stderr, "dxr pt: %s\n", r.last_error().c_str()); return 1; }
        std::vector<uint8_t> rgba;
        if (!r.readback(rgba)) { std::fprintf(stderr, "readback: %s\n", r.last_error().c_str()); return 1; }
        stbi_write_png(out.c_str(), static_cast<int>(o.width), static_cast<int>(o.height), 4, rgba.data(), static_cast<int>(o.width) * 4);
        dbg = r.debug_error_count();
    } else {
        Renderer renderer;
        if (!renderer.init_headless(o.width, o.height)) { std::fprintf(stderr, "init: %s\n", renderer.last_error().c_str()); return 1; }
        renderer.set_texture_seed(o.seed);
        std::vector<std::vector<contracts::ChunkVertex>> carvePool;
        std::vector<contracts::ResidentChunk> withLadder;
        app::ladder::carve_residents(sm.resident(), carvePool, withLadder);  // open the band shaft out of the world mesh
        std::vector<contracts::ChunkVertex> ladderMesh;
        app::ladder::build_mesh(ladderMesh, cam.pos[0], 40.0f);
        withLadder.push_back(contracts::ResidentChunk{contracts::ChunkKey{9998, 0, 0}, ladderMesh.data(), static_cast<uint32_t>(ladderMesh.size())});
        uint32_t drawn = 0;
        const size_t targetN = withLadder.size();
        for (int wk = 0; wk < 400 && static_cast<size_t>(drawn) < targetN; ++wk) renderer.render_chunks(cam, withLadder, 64u, 0u, &drawn);
        renderer.render_chunks(cam, withLadder, 256u, 0u, &drawn);
        FrameImage img;
        if (!renderer.readback(img)) { std::fprintf(stderr, "readback: %s\n", renderer.last_error().c_str()); return 1; }
        stbi_write_png(out.c_str(), static_cast<int>(img.width), static_cast<int>(img.height), 4, img.rgba.data(), static_cast<int>(img.width) * 4);
        dbg = renderer.debug_error_count();
    }
    std::printf("shot_pos: %.1f %.1f yaw %.2f\n", static_cast<double>(s.wanderer.pos.x), static_cast<double>(s.wanderer.pos.z), static_cast<double>(cam.yaw));
    std::printf("debug_error_count: %u\n", dbg);
    std::printf("out: %s\n", out.c_str());
    return dbg == 0 ? 0 : 3;
}

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
    if (o.flashlight) r.set_flashlight(true);   // QC: render the eye-torch (off by default -> the goldens are unaffected)
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

// Headless denoiser check: the edge-aware spatial filter must bring a noisy FEW-spp frame CLOSER to the
// converged ground truth than the raw noisy frame (not merely blur it). Renders a high-spp reference, then a
// low-spp frame denoise-OFF and denoise-ON, and reports the mean abs channel error (0..255) of each vs the ref.
int run_dxr_denoise(const Options& o) {
    const uint32_t W = o.width ? o.width : 320u, H = o.height ? o.height : 180u;
    const float ex = 16.0f, ez = 16.0f;
    const float ey = br::core::kWandererHalfHeight + 0.02f + br::core::kEyeHeight;
    contracts::CameraPose cam{};
    cam.pos[0] = ex; cam.pos[1] = ey; cam.pos[2] = ez;
    cam.yaw = 0.7853982f; cam.pitch = 0.0f;     // a corner-ish view: walls, baseboard, a ceiling light
    cam.fov_y = 1.2217305f; cam.aspect = static_cast<float>(W) / static_cast<float>(H);

    br::stream::StreamManager sm(o.seed, static_cast<int>(o.radius), o.workers);
    const auto center = contracts::chunk_key_at(0, ex, ez);
    sm.update(center); sm.wait_idle(); sm.update(center);

    br::render_dxr::DxrRenderer r;
    if (!r.init(W, H)) { std::fprintf(stderr, "dxr init: %s\n", r.last_error().c_str()); return 1; }
    if (!r.build_scene(sm.resident())) { std::fprintf(stderr, "dxr scene: %s\n", r.last_error().c_str()); return 1; }

    const uint32_t kRefSpp = o.spp ? o.spp : 512u;
    const uint32_t kLowSpp = 4u;
    const uint32_t seed = static_cast<uint32_t>(o.seed);

    std::vector<uint8_t> ref, noisy, den;
    if (!r.render_pt_frame(cam, kRefSpp, seed, true, false, 0u) || !r.readback(ref))   { std::fprintf(stderr, "ref: %s\n", r.last_error().c_str()); return 1; }
    if (!r.render_pt_frame(cam, kLowSpp, seed, true, false, 0u) || !r.readback(noisy)) { std::fprintf(stderr, "noisy: %s\n", r.last_error().c_str()); return 1; }
    if (!r.render_pt_frame(cam, kLowSpp, seed, true, true,  0u) || !r.readback(den))   { std::fprintf(stderr, "den: %s\n", r.last_error().c_str()); return 1; }

    auto mad = [&](const std::vector<uint8_t>& a) {
        double s = 0.0; const size_t n = static_cast<size_t>(W) * H;
        for (size_t p = 0; p < n; ++p)
            for (int k = 0; k < 3; ++k) { int dpx = static_cast<int>(a[p*4+k]) - static_cast<int>(ref[p*4+k]); if (dpx < 0) dpx = -dpx; s += dpx; }
        return s / (static_cast<double>(n) * 3.0);
    };
    const double err_off = mad(noisy);
    const double err_on  = mad(den);
    const uint32_t dbg = r.debug_error_count();
    std::printf("ref_spp: %u\n", kRefSpp);
    std::printf("low_spp: %u\n", kLowSpp);
    std::printf("err_off: %.3f\n", err_off);     // mean abs channel error vs ground truth, denoiser OFF
    std::printf("err_on: %.3f\n", err_on);       // ... denoiser ON
    std::printf("err_ratio: %.3f\n", err_off > 1e-6 ? err_on / err_off : 1.0);
    std::printf("debug_error_count: %u\n", dbg);
    if (!o.out.empty()) {
        stbi_write_png(o.out.c_str(), static_cast<int>(W), static_cast<int>(H), 4, den.data(), static_cast<int>(W) * 4);
        std::string np = o.out; const size_t dot = np.rfind(".png");
        if (dot != std::string::npos) np.insert(dot, "_noisy"); else np += "_noisy.png";
        stbi_write_png(np.c_str(), static_cast<int>(W), static_cast<int>(H), 4, noisy.data(), static_cast<int>(W) * 4);
    }
    return dbg == 0 ? 0 : 3;
}

// ----- Stochastic-NEE unbiasedness oracle (RT sampling step 2) ----------------------
// RIS single-light NEE must converge to the SAME image as full-grid NEE (it's an unbiased estimator). This renders a
// full-NEE converged reference, an independent full-NEE render (the Monte-Carlo NOISE FLOOR), and the SAME scene
// accumulated over `spp` 1-spp stochastic frames (mirroring the interactive accumulate path). If RIS is unbiased the
// stochastic-vs-ref error sits at ~the noise floor; a systematic bias would push it well above. Same scene as
// run_dxr_denoise so it shares the harness.
int run_dxr_stoch(const Options& o) {
    const uint32_t W = o.width ? o.width : 320u, H = o.height ? o.height : 180u;
    const float ex = 16.0f, ez = 16.0f;
    contracts::CameraPose cam{};
    cam.pos[0] = ex; cam.pos[1] = 1.6f; cam.pos[2] = ez;
    cam.yaw = 0.7853982f; cam.pitch = 0.0f;
    cam.fov_y = 1.2217305f; cam.aspect = static_cast<float>(W) / static_cast<float>(H);

    br::stream::StreamManager sm(o.seed, static_cast<int>(o.radius), o.workers);
    const auto center = contracts::chunk_key_at(0, ex, ez);
    sm.update(center); sm.wait_idle(); sm.update(center);

    br::render_dxr::DxrRenderer r;
    if (!r.init(W, H)) { std::fprintf(stderr, "dxr init: %s\n", r.last_error().c_str()); return 1; }
    if (!r.build_scene(sm.resident())) { std::fprintf(stderr, "dxr scene: %s\n", r.last_error().c_str()); return 1; }

    const uint32_t spp  = o.spp ? o.spp : 512u;
    const uint32_t seed = static_cast<uint32_t>(o.seed);

    std::vector<uint8_t> ref, stoch, ctrl;
    // full-NEE reference (converged) ...
    if (!r.render_pt_frame(cam, spp, seed, true, false, 0u, false, false) || !r.readback(ref)) { std::fprintf(stderr, "ref: %s\n", r.last_error().c_str()); return 1; }
    // ... an independent full-NEE render (different seed) = the pure Monte-Carlo noise floor ...
    if (!r.render_pt_frame(cam, spp, seed + 12345u, true, false, 0u, false, false) || !r.readback(ctrl)) { std::fprintf(stderr, "ctrl: %s\n", r.last_error().c_str()); return 1; }
    // ... stochastic RIS: accumulate `spp` frames of 1-spp (mirrors the interactive accumulate path) -> converges.
    for (uint32_t i = 0; i < spp; ++i)
        if (!r.render_pt_frame(cam, 1u, seed + i, i == 0u, false, i, false, /*stochastic_lights=*/true)) { std::fprintf(stderr, "stoch: %s\n", r.last_error().c_str()); return 1; }
    if (!r.readback(stoch)) { std::fprintf(stderr, "stoch readback: %s\n", r.last_error().c_str()); return 1; }

    auto mad = [&](const std::vector<uint8_t>& a) {
        double s = 0.0; const size_t n = static_cast<size_t>(W) * H;
        for (size_t p = 0; p < n; ++p)
            for (int k = 0; k < 3; ++k) { int dpx = static_cast<int>(a[p*4+k]) - static_cast<int>(ref[p*4+k]); if (dpx < 0) dpx = -dpx; s += dpx; }
        return s / (static_cast<double>(n) * 3.0);
    };
    const double err_stoch = mad(stoch);
    const double err_floor = mad(ctrl);
    const uint32_t dbg = r.debug_error_count();
    std::printf("spp: %u\n", spp);
    std::printf("err_stoch_vs_ref: %.4f\n", err_stoch);   // mean abs channel error (0..255): accumulated RIS vs full NEE
    std::printf("err_noise_floor: %.4f\n", err_floor);    // ... two independent full-NEE renders (the noise floor)
    std::printf("err_excess: %.4f\n", err_stoch - err_floor);  // bias would surface as error ABOVE the floor
    std::printf("debug_error_count: %u\n", dbg);
    // Unbiased <=> the stochastic error sits within ~the noise floor (small slack for RIS's higher per-sample variance).
    const bool unbiased = (err_stoch - err_floor) < 1.5 && dbg == 0u;
    std::printf("stoch_unbiased: %s\n", unbiased ? "PASS" : "FAIL");
    if (!o.out.empty()) stbi_write_png(o.out.c_str(), static_cast<int>(W), static_cast<int>(H), 4, stoch.data(), static_cast<int>(W) * 4);
    return unbiased ? 0 : 3;
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
        const float baseY = contracts::level_base_y(c.level);  // M26: the wanderer's current floor
        // M30: a flat SEALED floor (no holes) on purpose -- this is a GATED level-0 soak/PT path;
        // letting the walk-bot fall down a shaft/down-stair would perturb its pacing + the gate.
        // Live descent (build_walk_collision, holed floor) is for the interactive walks only.
        collision.push_back(Aabb{ {-1.0e6f, baseY - 1.0f, -1.0e6f}, {1.0e6f, baseY, 1.0e6f} });
        for (int64_t dx = -1; dx <= 1; ++dx)
            for (int64_t dz = -1; dz <= 1; ++dz) {
                const contracts::ChunkData cd = contracts::GenerateChunk(o.seed, contracts::ChunkKey{ c.level, c.cx + dx, c.cz + dz });
                for (const auto& b : cd.collision) collision.push_back(Aabb{ {b.mn[0], b.mn[1], b.mn[2]}, {b.mx[0], b.mx[1], b.mx[2]} });
            }
        cached = c;
    };

    br::stream::StreamManager sm(o.seed, static_cast<int>(o.radius), o.workers);
    br::render_dxr::DxrRenderer r;
    if (!r.init(o.width, o.height)) { std::fprintf(stderr, "dxr init: %s\n", r.last_error().c_str()); return 1; }

    contracts::ChunkKey center = contracts::chunk_key_at(contracts::level_from_y(s.wanderer.pos.y), s.wanderer.pos.x, s.wanderer.pos.z);
    rebuild_collision(center);
    sm.update(center); sm.wait_idle(); sm.update(center);
    if (!r.build_scene(sm.resident())) { std::fprintf(stderr, "dxr scene: %s\n", r.last_error().c_str()); return 1; }
    contracts::ChunkKey sceneCenter = center;

    uint32_t rebuilds = 0, frames = 0;
    const uint64_t kMaxTicks = 1500000;
    const uint64_t kRenderEvery = 120;  // ~1 s of sim between PT frames
    uint64_t lastRender = 0;

    while (s.odometer < target_m && s.tick < kMaxTicks) {
        const contracts::ChunkKey here = contracts::chunk_key_at(contracts::level_from_y(s.wanderer.pos.y), s.wanderer.pos.x, s.wanderer.pos.z);
        if (here != cached) rebuild_collision(here);
        tick(s, bot.step(s), collision);

        if (s.tick - lastRender >= kRenderEvery) {
            const contracts::ChunkKey rc = contracts::chunk_key_at(contracts::level_from_y(s.wanderer.pos.y), s.wanderer.pos.x, s.wanderer.pos.z);
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

// M25: render the Shoggoth's procedural body in the DXR (PATH-TRACED) path -- the creature
// injected as one more ResidentChunk into the ray-traced scene, proving it shows up in RT
// (M20b's in-world body was raster-only). Counts the warm salmon-orange creature pixels:
// the body is ~(230,120,95) -- distinctly R>G>B -- whereas the Backrooms walls are yellow
// (R~=G), so the count isolates the creature from the world.
int run_shoggoth_dxr_shot(const Options& o) {
    using namespace br::core;
    const float ex = 16.0f, ez = 16.0f;
    const float ey = kWandererHalfHeight + 0.02f + kEyeHeight;
    // The camera stands back and looks at the creature centred just ahead (in an open
    // cell). pose 0 = world + creature; pose 1 = world only (baseline); pose 2 = creature
    // only (the clean "it renders salmon in DXR" proof, no maze to occlude it).
    const Vec3 shog_pos{ex, ey - 0.30f, ez + 1.2f};
    contracts::CameraPose cam{};
    cam.pos[0] = ex; cam.pos[1] = ey; cam.pos[2] = ez - 2.6f;
    cam.yaw = 0.0f; cam.pitch = -0.05f; cam.fov_y = 1.2217305f;
    cam.aspect = static_cast<float>(o.width) / static_cast<float>(o.height);

    br::stream::StreamManager sm(o.seed, static_cast<int>(o.radius), o.workers);
    const auto center = contracts::chunk_key_at(0, ex, ez);
    sm.update(center); sm.wait_idle(); sm.update(center);

    std::vector<contracts::ChunkVertex> body;
    app::build_shoggoth_mesh(body, shog_pos, static_cast<float>(o.ticks) * 0.05f, 1.6f);
    for (auto& v : body) v.material = 7.0f;  // M25: tag the creature so the PT shades it salmon (raster uses color)
    const contracts::ResidentChunk creature{contracts::ChunkKey{9999, 0, 0}, body.data(), static_cast<uint32_t>(body.size())};

    std::vector<contracts::ResidentChunk> scene;
    if (o.pose != 2u) scene = sm.resident();          // world (pose 0 + 1)
    if (o.pose != 1u) scene.push_back(creature);      // creature (pose 0 + 2)

    br::render_dxr::DxrRenderer r;
    if (!r.init(o.width, o.height)) { std::fprintf(stderr, "dxr init: %s\n", r.last_error().c_str()); return 1; }
    if (!r.build_scene(scene)) { std::fprintf(stderr, "dxr scene: %s\n", r.last_error().c_str()); return 1; }
    if (!r.render_pt(cam, o.spp, static_cast<uint32_t>(o.seed))) { std::fprintf(stderr, "dxr pt: %s\n", r.last_error().c_str()); return 1; }
    std::vector<uint8_t> rgba;
    if (!r.readback(rgba)) { std::fprintf(stderr, "dxr readback: %s\n", r.last_error().c_str()); return 1; }
    if (!o.out.empty()) {
        stbi_write_png(o.out.c_str(), static_cast<int>(r.width()), static_cast<int>(r.height()), 4, rgba.data(), static_cast<int>(r.width()) * 4);
    }
    // Count the creature's warm salmon pixels. The body albedo (0.90, 0.42, 0.34) has
    // R > 1.5*G, which the Backrooms yellow wallpaper (0.80, 0.72) never satisfies -- so
    // R>1.5*G (+ G>=B) isolates the creature from the world at any brightness.
    uint64_t salmon = 0;
    const size_t px = static_cast<size_t>(r.width()) * r.height();
    for (size_t p = 0; p < px; ++p) {
        const uint8_t* c = &rgba[p * 4];
        if (c[0] > 35 && static_cast<float>(c[0]) > 1.5f * static_cast<float>(c[1]) && c[1] >= c[2]) ++salmon;
    }
    std::printf("creature_verts: %llu\n", static_cast<unsigned long long>(body.size()));
    std::printf("salmon_px: %llu\n", static_cast<unsigned long long>(salmon));
    std::printf("width: %u\n", r.width());
    std::printf("height: %u\n", r.height());
    return 0;
}

}  // namespace

// Tighten the OS timer/wait granularity (default ~15.6 ms) to 1 ms for the
// duration of the process, so GPU fence-wait wakeups pace frames smoothly.
struct TimerPeriodGuard {
    TimerPeriodGuard() { timeBeginPeriod(1); }
    ~TimerPeriodGuard() { timeEndPeriod(1); }
};

// ----- Windows screensaver: an auto-driven cinematic walk through a random Backrooms -------
// /s = fullscreen (cursor hidden; exits on ANY key/click/mouse-move/focus-loss); /p <hwnd> =
// render the Settings preview pane; /c = a tiny config dialog. VHS post ON, the procedural
// audio ambience + the wandering Shoggoth, a new random seed each launch. Presentation only --
// off the gated sim paths, so a wall-clock seed here never touches INV-1. `--seconds N` caps the
// run (for a headless smoke test); with no cap it runs until input (the screensaver contract).
int run_screensaver(const Options& o) {
    using namespace br::core;
    using namespace std::chrono;

    if (o.scr_config && o.scr_preview.empty()) {
        MessageBoxW(nullptr,
            L"Backrooms screensaver\n\nA new, infinite, never-repeating Backrooms every time it starts.\n"
            L"Nothing to configure -- move the mouse or press any key to exit.",
            L"Backrooms Screensaver", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    uint64_t seed = (static_cast<uint64_t>(GetTickCount64()) * 2654435761ull) ^ 0x9e3779b97f4a7c15ull;
    if (seed == 0) seed = 1;
    const uint64_t texSeed = seed;

    const bool preview = !o.scr_preview.empty();
    HWND hwnd = nullptr;
    uint32_t W = 0, H = 0;
    bool ownWindow = false;
    if (preview) {
        hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(std::strtoull(o.scr_preview.c_str(), nullptr, 0)));
        if (!hwnd || !IsWindow(hwnd)) return 0;
        RECT rc{}; GetClientRect(hwnd, &rc);
        W = static_cast<uint32_t>(rc.right - rc.left); H = static_cast<uint32_t>(rc.bottom - rc.top);
        if (W < 16) W = 320;
        if (H < 16) H = 240;
    } else {
        hwnd = create_screensaver_window(W, H);
        if (!hwnd) { std::fprintf(stderr, "screensaver: window creation failed\n"); return 1; }
        ShowCursor(FALSE);
        ownWindow = true;
        register_raw_mouse(hwnd);   // SPACE-play look uses relative WM_INPUT deltas (no cursor-position spin)
    }

    Renderer renderer;
    if (!renderer.init_windowed(hwnd, W, H)) {
        std::fprintf(stderr, "screensaver init: %s\n", renderer.last_error().c_str());
        if (ownWindow) ShowCursor(TRUE);
        return preview ? 0 : 1;   // a blank preview pane is fine; a failed /s is an error
    }
    renderer.set_texture_seed(texSeed);

    WorldState s(seed);
    s.wanderer.pos = Vec3{ 2.0f, kWandererHalfHeight + 0.02f, 2.0f };
    Stroller bot(seed ^ 0x5170990000000001ull, s.wanderer.pos);   // the autonomous camera -- natural hallway navigation
    app::Shoggoth shog; shog.pos = s.wanderer.pos; shog.pos.x += 22.0f; shog.pos.z += 6.0f;

    std::vector<Aabb> collision;
    contracts::ChunkKey cached{ 0, static_cast<int64_t>(1) << 40, 0 };
    auto rebuild = [&](contracts::ChunkKey c) { build_walk_collision(collision, texSeed, c); cached = c; };  // M30: holed floor -> live descent (the screensaver can wander into the abyss)
    rebuild(contracts::chunk_key_at(0, s.wanderer.pos.x, s.wanderer.pos.z));

    const int rad = (o.radius > 0) ? static_cast<int>(o.radius) : 6;
    auto sm = std::make_unique<br::stream::StreamManager>(seed, rad, (o.workers > 0 ? o.workers : 4u));

    audio::AudioEngine eng(seed, contracts::kAudioSampleRate);
    const bool audioOn = (!o.no_audio && !preview) ? eng.start_device(false) : false;
    if (audioOn) { eng.set_master_volume(0.6f); eng.set_sfx_volume(0.8f); }
    uint64_t prevSteps = 0;

    std::unique_ptr<app::ShoggothBrainHost> brain;   // optional ambience; graceful if :7071 is down
    if (!o.no_shoggoth_brain && !preview) {
        std::string bh; int bp; parse_host_port(o.director_url, bh, bp);
        brain = std::make_unique<app::ShoggothBrainHost>(bh, bp);
    }

    const auto t0 = steady_clock::now();
    auto prevt = t0; auto last_brain = t0 - seconds(3);
    const float tickDt = 1.0f / 120.0f;
    float accum = 0.0f;
    std::vector<contracts::ChunkVertex> shogBody;
    uint64_t frames = 0;
    bool haveAnchor = false; POINT anchor{};
    const double cap = (o.seconds > 0) ? static_cast<double>(o.seconds) : 0.0;  // 0 = run until input
    // SPACE drops into a playable WASD/mouse walk; the screensaver auto-walk (the Stroller) hands over.
    bool playMode = false, firstLook = true; POINT lookAnchor{}; const float kSens = 0.0022f;

    while (!g_scrQuit) {
        pump_messages();
        if (g_scrQuit) break;
        if (preview && !IsWindow(hwnd)) break;        // host closed the settings dialog
        // SPACE (caught in scr_proc) hands the camera to the player: WASD walk + mouse look; ESC exits.
        if (g_scrPlay && !playMode) {
            playMode = true;
            lookAnchor.x = static_cast<LONG>(W / 2); lookAnchor.y = static_cast<LONG>(H / 2);
            SetCursorPos(lookAnchor.x, lookAnchor.y); ++g_cursorWarps; firstLook = true;   // park the hidden cursor ONCE
            RECT clip{ 0, 0, static_cast<LONG>(W), static_cast<LONG>(H) }; ClipCursor(&clip);  // confine it (no per-frame warp)
            g_scrRawDX = g_scrRawDY = 0;   // start the look from a clean zero delta
        }
        if (playMode) {
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { g_scrQuit = true; break; }
        } else if (ownWindow) {                       // idle screensaver: any cursor move ends it (.scr contract)
            POINT cp;
            if (GetCursorPos(&cp)) {
                if (!haveAnchor) { anchor = cp; haveAnchor = true; }
                else if ((cp.x > anchor.x ? cp.x - anchor.x : anchor.x - cp.x) > 6 ||
                         (cp.y > anchor.y ? cp.y - anchor.y : anchor.y - cp.y) > 6) { g_scrQuit = true; break; }
            }
        }
        const auto now = steady_clock::now();
        if (cap > 0.0 && duration<double>(now - t0).count() >= cap) break;
        accum += duration<float>(now - prevt).count(); prevt = now;
        if (accum > 0.25f) accum = 0.25f;
        const float aspect = static_cast<float>(W) / static_cast<float>(H);

        // Play-mode input: WASD move, Shift run, Space jump, mouse look (relative; recentred each frame).
        contracts::InputCommand plyIn{}; float plyYaw = 0.0f, plyPitch = 0.0f;
        if (playMode) {
            if (GetAsyncKeyState('W') & 0x8000) plyIn.move_z += 1.0f;
            if (GetAsyncKeyState('S') & 0x8000) plyIn.move_z -= 1.0f;
            if (GetAsyncKeyState('D') & 0x8000) plyIn.move_x += 1.0f;
            if (GetAsyncKeyState('A') & 0x8000) plyIn.move_x -= 1.0f;
            if (GetAsyncKeyState(VK_SPACE) & 0x8000) plyIn.buttons |= contracts::kButtonJump;
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) plyIn.buttons |= contracts::kButtonRun;
            if (!firstLook) { plyYaw = static_cast<float>(g_scrRawDX) * kSens;       // relative WM_INPUT delta
                              plyPitch = -static_cast<float>(g_scrRawDY) * kSens; }  // (no cursor-position spin)
            firstLook = false;
            g_scrRawDX = g_scrRawDY = 0;                  // consume this frame's delta
            // (no per-frame SetCursorPos -- the cursor is ClipCursor-confined below, not warped, so it can't fight)
        }

        bool firstTick = true;
        while (accum >= tickDt) {
            const contracts::ChunkKey here = contracts::chunk_key_at(contracts::level_from_y(s.wanderer.pos.y), s.wanderer.pos.x, s.wanderer.pos.z);
            if (here != cached) rebuild(here);
            if (playMode) {
                contracts::InputCommand in = plyIn;
                if (firstTick) { in.look_yaw = plyYaw; in.look_pitch = plyPitch; firstTick = false; }  // mouse delta is per-frame
                tick(s, in, collision);
            } else {
                tick(s, bot.step(s, texSeed, collision), collision);          // auto-driven, no keyboard
            }
            app::shoggoth_step(shog, s.wanderer.pos, seed, (s.tick % 8u) == 0u);
            accum -= tickDt;
        }
        if (brain) {
            if (now - last_brain >= seconds(3)) { brain->submit(app::build_shoggoth_summary(shog, s.wanderer.pos, s.tick)); last_brain = now; }
            for (const app::ShoggothIntent& it : brain->poll()) shog.intent = it;
        }
        if (audioOn) {
            const uint64_t steps = footstep_count(s);
            eng.post(audio_listener(s), 1.2f, static_cast<uint32_t>(steps - prevSteps));
            prevSteps = steps;
        }
        // M28/M30 streaming: open the abyss band downward over a shaft, else the 2-floor see-through.
        const int32_t curLevel = contracts::level_from_y(s.wanderer.pos.y);
        if (audioOn) eng.set_draft(draft_intensity_near_shaft(texSeed, curLevel, s.wanderer.pos.x, s.wanderer.pos.z));  // M30 telegraph
        const int32_t extraLevel = (s.wanderer.pos.y - contracts::level_base_y(curLevel) > 2.0f) ? curLevel + 1 : curLevel - 1;
        const contracts::ChunkKey center = contracts::chunk_key_at(curLevel, s.wanderer.pos.x, s.wanderer.pos.z);
        const br::gen::ShaftSpec shaft = br::gen::shaft_at(texSeed, center.cx, center.cz);
        if (shaft.present && curLevel > shaft.top_level - shaft.depth && curLevel <= shaft.top_level) {
            const int32_t below = curLevel - shaft.top_level + shaft.depth;
            sm->update(center, curLevel - ((below < 4) ? below : 4), curLevel);
        } else {
            sm->update(center, extraLevel);
        }

        contracts::CameraPose cam = wanderer_camera(s, aspect);
        apply_organic_bob(cam, s);   // screensaver: a richer, non-metronomic human gait bob
        renderer.set_post(true, static_cast<uint32_t>(texSeed), duration<float>(now - t0).count(), false);  // VHS, animated
        app::build_shoggoth_mesh(shogBody, shog.pos, shog.writhe, 1.4f);
        std::vector<contracts::ResidentChunk> withShog = sm->resident();
        withShog.push_back(contracts::ResidentChunk{ contracts::ChunkKey{9999, static_cast<int64_t>(frames), 0}, shogBody.data(), static_cast<uint32_t>(shogBody.size()) });
        uint32_t drawn = 0;
        if (!renderer.render_chunks_windowed(cam, withShog, 8u, s.tick, &drawn)) break;  // device lost / host closed
        ++frames;
    }

    ClipCursor(nullptr);   // release any SPACE-play confine
    if (ownWindow) { ShowCursor(TRUE); if (IsWindow(hwnd)) DestroyWindow(hwnd); }
    return 0;
}

int main(int argc, char** argv) {
    // Be DPI-aware so physical pixels (EnumDisplaySettings, the swapchain) and the cursor coords
    // (GetCursorPos/SetCursorPos) agree. Without this, on a SCALED display (125%/150%/...) the windowed
    // game sizes itself in physical pixels but the cursor is virtualised to logical pixels, so the
    // mouse-look recenter never matches the read-back -> a constant per-frame delta -> the view SPINS.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
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
    if (o.ascend)     return run_ascend(o);
    if (o.vstream)    return run_vstream(o);
    if (o.shaftfall)  return run_shaftfall(o);
    if (o.livedescent) return run_livedescent(o);
    if (o.descentsoak) return run_descentsoak(o);
    if (o.strollcheck) return run_strollcheck(o);
    if (o.abyss)      return run_abyss(o);
    if (o.dxr_probe)  return run_dxr_probe(o);
    if (o.dxr_test)   return run_dxr_test(o);
    if (o.dxr_depth)  return run_dxr_depth(o);
    if (o.game_shot)  return run_game_shot(o);
    if (o.ladder_walk) return run_ladder_walk(o);
    if (o.recolor_shot) return run_recolor_shot(o);
    if (o.dxr_pt)     return run_dxr_pt(o);
    if (o.dxr_fps)    return run_dxr_fps(o);
    if (o.dxr_ghost)  return run_dxr_ghost(o);
    if (o.dxr_walk)   return run_dxr_walk(o);
    if (o.dxr_denoise) return run_dxr_denoise(o);
    if (o.dxr_stoch) return run_dxr_stoch(o);
    if (o.llm_test)   return run_llm_test(o);
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
    if (o.shoggoth_dxr_shot) return run_shoggoth_dxr_shot(o);
    if (o.shoggoth_record) return run_shoggoth_record(o);
    if (o.shoggoth_vision_record) return run_shoggoth_vision_record(o);
    if (o.shoggoth_hearing_record) return run_shoggoth_hearing_record(o);
    if (o.shoggoth_pa_record) return run_shoggoth_pa_record(o);
    if (o.tts_say) return run_tts_say(o);
    if (o.tts_check) return run_tts_check(o);
    if (o.caption_shot) return run_caption_shot(o);
    if (o.mic_test) return run_mic_test(o);
    if (o.chat_test) return run_chat_test(o);
    if (o.shoggoth_replay) return run_shoggoth_replay(o);
    if (o.screensaver || o.scr_config) return run_screensaver(o);
    if (o.game)       return run_game(o);
    if (o.soak)       return run_soak(o);
    if (o.sim)     return run_sim(o);
    if (o.walkbot) return run_walkbot(o);
    if (o.topdown) return run_topdown(o);
    if (o.shot)    return run_shot(o);
    if (o.stream)  return run_stream(o);
    if (o.scene)   return run_scene(o);
    if (o.headless || o.windowed) return run_clear(o);

    // No mode flag (e.g. a double-click) -> launch the GAME. (Before this, a no-arg run just printed the
    // version and exited instantly, which looked like an immediate crash when double-clicked.)
    return run_game(o);
}
