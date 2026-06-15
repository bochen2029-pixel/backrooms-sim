// test_menu — M15 game-state machine: the splash -> main menu -> play -> pause ->
// settings -> quit transitions are a deterministic, pure function of synthetic
// UiActions (no rendering, no wall-clock). This is the M15 "state-transition tests
// driven by synthetic input" exit gate.
#include <catch2/catch_test_macros.hpp>

#include "menu.h"

using namespace br::app;

namespace {
// Feed a sequence of actions through the model; return the last command emitted.
UiCommand drive(MenuModel& m, std::initializer_list<UiAction> actions) {
    UiCommand last = UiCommand::None;
    for (UiAction a : actions) last = menu_step(m, a);
    return last;
}
}  // namespace

TEST_CASE("splash advances to the main menu on any key", "[m15][menu]") {
    MenuModel m;
    REQUIRE(m.screen == Screen::Splash);
    menu_step(m, UiAction::Activate);
    REQUIRE(m.screen == Screen::MainMenu);
}

TEST_CASE("the canonical flow: menu -> play -> pause -> menu is deterministic", "[m15][menu]") {
    MenuModel m;
    // Splash -> MainMenu, New Game (sel 0) -> StartGame + Play.
    REQUIRE(drive(m, {UiAction::Activate}) == UiCommand::None);
    REQUIRE(m.screen == Screen::MainMenu);
    REQUIRE(menu_step(m, UiAction::Activate) == UiCommand::StartGame);
    REQUIRE(m.screen == Screen::Play);
    REQUIRE(m.has_session);

    // Esc in Play -> Pause.
    menu_step(m, UiAction::Back);
    REQUIRE(m.screen == Screen::Pause);

    // Pause: Down twice to "Quit to Menu" (sel 2), Activate -> MainMenu.
    drive(m, {UiAction::Down, UiAction::Down});
    REQUIRE(m.pause_sel == 2);
    REQUIRE(menu_step(m, UiAction::Activate) == UiCommand::None);
    REQUIRE(m.screen == Screen::MainMenu);
    REQUIRE(m.has_session);  // the session is kept (Continue still works)

    // Running it again from a fresh model yields the identical screen path.
    MenuModel m2;
    drive(m2, {UiAction::Activate, UiAction::Activate, UiAction::Back,
               UiAction::Down, UiAction::Down, UiAction::Activate});
    REQUIRE(m2.screen == m.screen);
    REQUIRE(m2.pause_sel == m.pause_sel);
}

TEST_CASE("Continue is inert until a session exists", "[m15][menu]") {
    MenuModel m;
    menu_step(m, UiAction::Activate);          // -> MainMenu
    menu_step(m, UiAction::Down);              // sel 1 = Continue
    REQUIRE(m.main_sel == 1);
    REQUIRE(menu_step(m, UiAction::Activate) == UiCommand::None);  // no session yet
    REQUIRE(m.screen == Screen::MainMenu);     // stayed put

    // Start + leave a session, then Continue resumes it.
    m.main_sel = 0;
    menu_step(m, UiAction::Activate);          // StartGame -> Play, has_session
    menu_step(m, UiAction::Back);              // -> Pause
    drive(m, {UiAction::Down, UiAction::Down, UiAction::Activate});  // Quit to Menu
    REQUIRE(m.screen == Screen::MainMenu);
    m.main_sel = 1;                            // Continue
    REQUIRE(menu_step(m, UiAction::Activate) == UiCommand::ResumeGame);
    REQUIRE(m.screen == Screen::Play);
}

TEST_CASE("menu selection wraps both directions", "[m15][menu]") {
    MenuModel m;
    menu_step(m, UiAction::Activate);  // MainMenu, sel 0
    menu_step(m, UiAction::Up);        // wrap to last
    REQUIRE(m.main_sel == kMainItems - 1);
    menu_step(m, UiAction::Down);      // wrap back to 0
    REQUIRE(m.main_sel == 0);
}

TEST_CASE("settings adjust, clamp, and round-trip to the origin screen", "[m15][menu]") {
    MenuModel m;
    menu_step(m, UiAction::Activate);   // MainMenu
    m.main_sel = 2;                     // Settings
    menu_step(m, UiAction::Activate);
    REQUIRE(m.screen == Screen::Settings);
    REQUIRE(m.settings_from == Screen::MainMenu);

    // Master (sel 0): two rights = +10, clamped at 100 from the top.
    const int base = m.settings.master_pct;
    drive(m, {UiAction::Right, UiAction::Right});
    REQUIRE(m.settings.master_pct == base + 10);
    for (int i = 0; i < 40; ++i) menu_step(m, UiAction::Right);
    REQUIRE(m.settings.master_pct == 100);     // clamps, never overflows
    for (int i = 0; i < 40; ++i) menu_step(m, UiAction::Left);
    REQUIRE(m.settings.master_pct == 0);

    // Director toggle (sel 3).
    m.settings_sel = 3;
    menu_step(m, UiAction::Right); REQUIRE(m.settings.director == 1);
    menu_step(m, UiAction::Left);  REQUIRE(m.settings.director == 0);

    // Resolution (sel 5): Left/Right cycle the presets, clamped (no wrap), applied on relaunch.
    m.settings_sel = 5;
    m.settings.res_w = 1920; m.settings.res_h = 1080;       // 1080p
    menu_step(m, UiAction::Right); REQUIRE(m.settings.res_w == 2560);  // -> 1440p
    menu_step(m, UiAction::Right); REQUIRE((m.settings.res_w == 3840 && m.settings.res_h == 2160));  // -> 4K
    menu_step(m, UiAction::Right); REQUIRE(m.settings.res_w == 3840);  // clamps at the top preset
    menu_step(m, UiAction::Left);  REQUIRE(m.settings.res_w == 2560);
    for (int i = 0; i < 5; ++i) menu_step(m, UiAction::Left);
    REQUIRE((m.settings.res_w == 1280 && m.settings.res_h == 720));    // clamps at the bottom preset

    // Esc returns to the origin (MainMenu).
    menu_step(m, UiAction::Back);
    REQUIRE(m.screen == Screen::MainMenu);

    // From Pause, Settings returns to Pause.
    m.main_sel = 0; menu_step(m, UiAction::Activate);  // Play
    menu_step(m, UiAction::Back);                      // Pause
    m.pause_sel = 1; menu_step(m, UiAction::Activate);  // Settings
    REQUIRE(m.settings_from == Screen::Pause);
    m.settings_sel = kSettingsItems - 1;               // "Back" item
    menu_step(m, UiAction::Activate);
    REQUIRE(m.screen == Screen::Pause);
}

TEST_CASE("New Game seed cycles with Left/Right and never drops below 1", "[m15][menu]") {
    MenuModel m;
    menu_step(m, UiAction::Activate);  // MainMenu, sel 0 (New Game)
    m.seed = 1;
    menu_step(m, UiAction::Left);
    REQUIRE(m.seed == 1);              // floor at 1
    menu_step(m, UiAction::Right);
    REQUIRE(m.seed == 2);
}

TEST_CASE("Quit from the main menu emits QuitApp", "[m15][menu]") {
    MenuModel m;
    menu_step(m, UiAction::Activate);  // MainMenu
    m.main_sel = 3;                    // Quit
    REQUIRE(menu_step(m, UiAction::Activate) == UiCommand::QuitApp);
    REQUIRE(m.screen == Screen::Quit);
}
