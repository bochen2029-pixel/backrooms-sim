#pragma once
//
// app/menu.h — the game-state machine + menu model (M15), header-only and PURE.
//
// `menu_step` is a deterministic transition function: (model, action) -> command,
// with no rendering, no wall-clock, no globals. That keeps the whole front-end of
// the game (splash -> main menu -> play -> pause -> settings -> quit) unit-testable
// headlessly by driving synthetic UiActions — the M15 exit gate. The shell (app)
// owns rendering this model and acting on the returned UiCommand.
//
#include <cstdint>
#include <string>

namespace br::app {

// Screens the shell can be showing. `Play` means the live walk is running; the
// menu model is dormant except for Back (Esc) -> Pause.
enum class Screen { Splash, MainMenu, Pause, Settings, Play, Quit };

// Abstract UI intent — keyboard (arrows/WASD + Enter/Esc) or mapped from the mouse.
enum class UiAction { None, Up, Down, Left, Right, Activate, Back };

// What the shell must do as a side effect of a step (beyond the screen change).
// TestConnection pings the KEEL/LLM sidecar (a real request) -> MenuModel.llm_state/llm_text.
// TestMic records the mic, transcribes it, asks the Director, and reports BOTH a caption
// (MenuModel.mic_*) and a SPOKEN reply -- a full voice-loop diagnostic from the Settings screen.
enum class UiCommand { None, StartGame, ResumeGame, QuitApp, TestConnection, TestMic };

// In-memory settings (M15). M16 extends this set + persists it to a config file;
// for M15 they live for the process and drive the live session's volumes/sensitivity.
struct Settings {
    int master_pct = 80;  // 0..100 master volume
    int sfx_pct = 90;     // 0..100 SFX volume
    int mouse_pct = 50;   // 0..100 mouse sensitivity (scaled by the shell)
    int director = 0;     // 0/1 Director on/off
    int subtitles = 1;    // 0/1 Director subtitles on/off (on by default -- the PA voice is hard to make out)
    int rt = 0;           // 0/1 ray tracing on/off (M19; default off = no regression)
    int res_w = 1920;     // chosen render resolution (applied on relaunch; default 1080p)
    int res_h = 1080;
};

// Item counts per screen — shared so the renderer and the logic never disagree.
constexpr int kMainItems = 4;      // New Game, Continue, Settings, Quit
constexpr int kPauseItems = 3;     // Resume, Settings, Quit to Menu
constexpr int kSettingsItems = 10;  // Master, SFX, Mouse, Director, Ray Tracing, Resolution, Test Connection, Test Microphone, Subtitles, Back
constexpr int kSettingsTestConn = 6;  // index of the "Test Connection" row (Activate -> UiCommand::TestConnection)
constexpr int kSettingsTestMic = 7;   // index of the "Test Microphone" row (Activate -> UiCommand::TestMic)
constexpr int kSettingsSubtitles = 8;  // index of the "Subtitles" toggle row

struct MenuModel {
    Screen screen = Screen::Splash;
    int main_sel = 0;
    int pause_sel = 0;
    int settings_sel = 0;
    Screen settings_from = Screen::MainMenu;  // where Settings returns to
    uint64_t seed = 1;                        // New Game seed (Left/Right cycles)
    Settings settings;
    bool has_session = false;  // a session exists -> Continue/Resume enabled
    // Live LLM/Director connection status for the Settings screen (written by the shell after a
    // TestConnection ping). 0 untested · 1 testing · 2 connected · 3 offline. llm_text = a short line.
    int llm_state = 0;
    std::string llm_text;
    // Live MIC / voice-loop diagnostic (TestMic): 0 idle · 1 listening · 2 thinking · 3 replied · 4 error.
    // mic_heard = what whisper heard you say; mic_reply = the Director's reply (shown as a caption + spoken).
    int mic_state = 0;
    std::string mic_heard;
    std::string mic_reply;
};

namespace detail {
inline int wrap(int i, int n) { return ((i % n) + n) % n; }
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Render-resolution presets for the Settings picker (Left/Right; applied on relaunch). Pure.
struct ResPreset { int w, h; };
inline constexpr ResPreset kResPresets[] = { {1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160} };
inline constexpr int kResPresetCount = 4;
// Step (w,h) to the next/prev preset (clamped, no wrap). If (w,h) is not a preset (e.g. a
// detected native resolution), the first step snaps to the nearest preset by width.
inline void res_step(int& w, int& h, int dir) {
    int idx = -1, nearest = 0, nd = 1 << 30;
    for (int i = 0; i < kResPresetCount; ++i) {
        if (kResPresets[i].w == w && kResPresets[i].h == h) { idx = i; break; }
        const int d = (kResPresets[i].w > w) ? (kResPresets[i].w - w) : (w - kResPresets[i].w);
        if (d < nd) { nd = d; nearest = i; }
    }
    int ni = (idx >= 0) ? (idx + dir) : nearest;
    if (ni < 0) ni = 0;
    if (ni >= kResPresetCount) ni = kResPresetCount - 1;
    w = kResPresets[ni].w; h = kResPresets[ni].h;
}

// Adjust the setting the cursor is on by +/- step (Left/Right on the Settings screen).
inline void adjust_setting(Settings& s, int sel, int dir) {
    switch (sel) {
        case 0: s.master_pct = clampi(s.master_pct + dir * 5, 0, 100); break;
        case 1: s.sfx_pct = clampi(s.sfx_pct + dir * 5, 0, 100); break;
        case 2: s.mouse_pct = clampi(s.mouse_pct + dir * 5, 0, 100); break;
        case 3: s.director = dir > 0 ? 1 : 0; break;
        case 4: s.rt = dir > 0 ? 1 : 0; break;
        case 5: res_step(s.res_w, s.res_h, dir); break;  // resolution (applied on relaunch)
        case kSettingsSubtitles: s.subtitles = dir > 0 ? 1 : 0; break;  // Director subtitles on/off
        default: break;  // "Test Connection" + "Back" rows have no Left/Right value
    }
}
}  // namespace detail

inline UiCommand menu_step(MenuModel& m, UiAction a) {
    using S = Screen;
    switch (m.screen) {
        case S::Splash:
            if (a == UiAction::Activate || a == UiAction::Back || a == UiAction::Down)
                m.screen = S::MainMenu;
            return UiCommand::None;

        case S::MainMenu:
            if (a == UiAction::Up) m.main_sel = detail::wrap(m.main_sel - 1, kMainItems);
            else if (a == UiAction::Down) m.main_sel = detail::wrap(m.main_sel + 1, kMainItems);
            else if (m.main_sel == 0 && a == UiAction::Left) m.seed = (m.seed > 1) ? m.seed - 1 : 1;
            else if (m.main_sel == 0 && a == UiAction::Right) m.seed += 1;
            else if (a == UiAction::Activate) {
                switch (m.main_sel) {
                    case 0: m.has_session = true; m.screen = S::Play; return UiCommand::StartGame;
                    case 1: if (m.has_session) { m.screen = S::Play; return UiCommand::ResumeGame; } break;
                    case 2: m.settings_from = S::MainMenu; m.settings_sel = 0; m.screen = S::Settings; break;
                    case 3: m.screen = S::Quit; return UiCommand::QuitApp;
                }
            }
            return UiCommand::None;

        case S::Pause:
            if (a == UiAction::Up) m.pause_sel = detail::wrap(m.pause_sel - 1, kPauseItems);
            else if (a == UiAction::Down) m.pause_sel = detail::wrap(m.pause_sel + 1, kPauseItems);
            else if (a == UiAction::Back) { m.screen = S::Play; return UiCommand::ResumeGame; }
            else if (a == UiAction::Activate) {
                switch (m.pause_sel) {
                    case 0: m.screen = S::Play; return UiCommand::ResumeGame;
                    case 1: m.settings_from = S::Pause; m.settings_sel = 0; m.screen = S::Settings; break;
                    case 2: m.screen = S::MainMenu; m.main_sel = 0; break;  // session kept (Continue)
                }
            }
            return UiCommand::None;

        case S::Settings:
            if (a == UiAction::Up) m.settings_sel = detail::wrap(m.settings_sel - 1, kSettingsItems);
            else if (a == UiAction::Down) m.settings_sel = detail::wrap(m.settings_sel + 1, kSettingsItems);
            else if (a == UiAction::Left) detail::adjust_setting(m.settings, m.settings_sel, -1);
            else if (a == UiAction::Right) detail::adjust_setting(m.settings, m.settings_sel, +1);
            else if (a == UiAction::Back) m.screen = m.settings_from;
            else if (a == UiAction::Activate) {
                if (m.settings_sel == kSettingsTestConn) return UiCommand::TestConnection;  // ping the LLM
                if (m.settings_sel == kSettingsTestMic) return UiCommand::TestMic;          // mic -> Director -> caption + voice
                if (m.settings_sel == kSettingsItems - 1) m.screen = m.settings_from;       // "Back" item
            }
            return UiCommand::None;

        case S::Play:
            if (a == UiAction::Back) { m.pause_sel = 0; m.screen = S::Pause; }
            return UiCommand::None;

        case S::Quit:
            return UiCommand::None;
    }
    return UiCommand::None;
}

}  // namespace br::app
