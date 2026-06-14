#pragma once
//
// app/hud.h — CPU-rasterised HUD overlay (M8). The app composes a transparent
// RGBA overlay (5x7 bitmap font) from sim values; the renderer's VHS post pass
// composites it on top. Deterministic, so the timestamp golden is reproducible.
//
#include <cstdint>
#include <string>
#include <vector>

#include "menu.h"

namespace br::app {

struct HudValues {
    uint64_t sim_ticks = 0;   // -> timestamp HH:MM:SS at 120 Hz
    uint64_t seed = 0;
    float odometer_m = 0.0f;
    int64_t chunk_x = 0;
    int64_t chunk_z = 0;
    int32_t level = 0;
    uint32_t fps = 0;
};

// Render the HUD into `rgba` (sized width*height*4, RGBA, transparent background).
void build_hud_overlay(std::vector<uint8_t>& rgba, uint32_t width, uint32_t height,
                       const HudValues& v);

// "HH:MM:SS" for a 120 Hz tick count (also written to telemetry for the gate).
std::string hud_timestamp(uint64_t sim_ticks);

// Render the current menu screen into `rgba` (M15): a near-opaque dark backdrop,
// the title, the item list with the selected row highlighted, and (Settings) the
// live setting values. Deterministic for a fixed model -> menu-render goldens.
void build_menu_overlay(std::vector<uint8_t>& rgba, uint32_t width, uint32_t height,
                        const MenuModel& m);

}  // namespace br::app
