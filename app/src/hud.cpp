#include "hud.h"

#include <cstdio>
#include <cstring>

namespace br::app {

namespace {

// 5x7 bitmap font: 7 rows per glyph, low 5 bits each (bit 4 = leftmost column).
struct Glyph { uint8_t row[7]; };

const Glyph* glyph_for(char ch) {
    static const Glyph kSpace = {{0,0,0,0,0,0,0}};
    switch (ch) {
        case '0': { static const Glyph g = {{0b01110,0b10001,0b10011,0b10101,0b11001,0b10001,0b01110}}; return &g; }
        case '1': { static const Glyph g = {{0b00100,0b01100,0b00100,0b00100,0b00100,0b00100,0b01110}}; return &g; }
        case '2': { static const Glyph g = {{0b01110,0b10001,0b00001,0b00010,0b00100,0b01000,0b11111}}; return &g; }
        case '3': { static const Glyph g = {{0b11111,0b00010,0b00100,0b00010,0b00001,0b10001,0b01110}}; return &g; }
        case '4': { static const Glyph g = {{0b00010,0b00110,0b01010,0b10010,0b11111,0b00010,0b00010}}; return &g; }
        case '5': { static const Glyph g = {{0b11111,0b10000,0b11110,0b00001,0b00001,0b10001,0b01110}}; return &g; }
        case '6': { static const Glyph g = {{0b00110,0b01000,0b10000,0b11110,0b10001,0b10001,0b01110}}; return &g; }
        case '7': { static const Glyph g = {{0b11111,0b00001,0b00010,0b00100,0b01000,0b01000,0b01000}}; return &g; }
        case '8': { static const Glyph g = {{0b01110,0b10001,0b10001,0b01110,0b10001,0b10001,0b01110}}; return &g; }
        case '9': { static const Glyph g = {{0b01110,0b10001,0b10001,0b01111,0b00001,0b00010,0b01100}}; return &g; }
        case ':': { static const Glyph g = {{0b00000,0b00100,0b00100,0b00000,0b00100,0b00100,0b00000}}; return &g; }
        case ',': { static const Glyph g = {{0b00000,0b00000,0b00000,0b00000,0b00100,0b00100,0b01000}}; return &g; }
        case '.': { static const Glyph g = {{0b00000,0b00000,0b00000,0b00000,0b00000,0b00110,0b00110}}; return &g; }
        case '-': { static const Glyph g = {{0b00000,0b00000,0b00000,0b11111,0b00000,0b00000,0b00000}}; return &g; }
        case 'A': { static const Glyph g = {{0b01110,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001}}; return &g; }
        case 'B': { static const Glyph g = {{0b11110,0b10001,0b10001,0b11110,0b10001,0b10001,0b11110}}; return &g; }
        case 'C': { static const Glyph g = {{0b01110,0b10001,0b10000,0b10000,0b10000,0b10001,0b01110}}; return &g; }
        case 'D': { static const Glyph g = {{0b11110,0b10001,0b10001,0b10001,0b10001,0b10001,0b11110}}; return &g; }
        case 'E': { static const Glyph g = {{0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b11111}}; return &g; }
        case 'F': { static const Glyph g = {{0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b10000}}; return &g; }
        case 'G': { static const Glyph g = {{0b01110,0b10001,0b10000,0b10111,0b10001,0b10001,0b01111}}; return &g; }
        case 'H': { static const Glyph g = {{0b10001,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001}}; return &g; }
        case 'I': { static const Glyph g = {{0b01110,0b00100,0b00100,0b00100,0b00100,0b00100,0b01110}}; return &g; }
        case 'K': { static const Glyph g = {{0b10001,0b10010,0b10100,0b11000,0b10100,0b10010,0b10001}}; return &g; }
        case 'L': { static const Glyph g = {{0b10000,0b10000,0b10000,0b10000,0b10000,0b10000,0b11111}}; return &g; }
        case 'M': { static const Glyph g = {{0b10001,0b11011,0b10101,0b10101,0b10001,0b10001,0b10001}}; return &g; }
        case 'N': { static const Glyph g = {{0b10001,0b11001,0b10101,0b10011,0b10001,0b10001,0b10001}}; return &g; }
        case 'O': { static const Glyph g = {{0b01110,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110}}; return &g; }
        case 'P': { static const Glyph g = {{0b11110,0b10001,0b10001,0b11110,0b10000,0b10000,0b10000}}; return &g; }
        case 'Q': { static const Glyph g = {{0b01110,0b10001,0b10001,0b10001,0b10101,0b10010,0b01101}}; return &g; }
        case 'R': { static const Glyph g = {{0b11110,0b10001,0b10001,0b11110,0b10100,0b10010,0b10001}}; return &g; }
        case 'S': { static const Glyph g = {{0b01111,0b10000,0b10000,0b01110,0b00001,0b00001,0b11110}}; return &g; }
        case 'T': { static const Glyph g = {{0b11111,0b00100,0b00100,0b00100,0b00100,0b00100,0b00100}}; return &g; }
        case 'U': { static const Glyph g = {{0b10001,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110}}; return &g; }
        case 'V': { static const Glyph g = {{0b10001,0b10001,0b10001,0b10001,0b10001,0b01010,0b00100}}; return &g; }
        case 'W': { static const Glyph g = {{0b10001,0b10001,0b10001,0b10101,0b10101,0b11011,0b10001}}; return &g; }
        case 'X': { static const Glyph g = {{0b10001,0b10001,0b01010,0b00100,0b01010,0b10001,0b10001}}; return &g; }
        case 'Y': { static const Glyph g = {{0b10001,0b10001,0b01010,0b00100,0b00100,0b00100,0b00100}}; return &g; }
        default: return &kSpace;
    }
}

// Draw `text` at pixel (x0,y0) with integer `scale`, colour (r,g,b,a).
void draw_text(std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, int x0, int y0,
               int scale, const char* text, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    int cx = x0;
    for (const char* p = text; *p; ++p) {
        const Glyph* gl = glyph_for(*p);
        for (int ry = 0; ry < 7; ++ry) {
            for (int rxb = 0; rxb < 5; ++rxb) {
                if (!(gl->row[ry] & (1 << (4 - rxb)))) continue;
                for (int sy = 0; sy < scale; ++sy) {
                    for (int sx = 0; sx < scale; ++sx) {
                        const int px = cx + rxb * scale + sx;
                        const int py = y0 + ry * scale + sy;
                        if (px < 0 || py < 0 || px >= static_cast<int>(w) || py >= static_cast<int>(h)) continue;
                        uint8_t* d = &rgba[(static_cast<size_t>(py) * w + px) * 4];
                        d[0] = r; d[1] = g; d[2] = b; d[3] = a;
                    }
                }
            }
        }
        cx += 6 * scale;  // 5px glyph + 1px gap
    }
}

// Fill a clipped rectangle (menu panels + selection highlights).
void fill_rect(std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, int x, int y, int rw, int rh,
               uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int py = y; py < y + rh; ++py) {
        if (py < 0 || py >= static_cast<int>(h)) continue;
        for (int px = x; px < x + rw; ++px) {
            if (px < 0 || px >= static_cast<int>(w)) continue;
            uint8_t* d = &rgba[(static_cast<size_t>(py) * w + px) * 4];
            d[0] = r; d[1] = g; d[2] = b; d[3] = a;
        }
    }
}

int text_px(const char* s, int scale) {
    int n = 0;
    for (const char* p = s; *p; ++p) ++n;
    return n * 6 * scale;
}

void draw_centered(std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, int cx, int y, int scale,
                   const char* text, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    draw_text(rgba, w, h, cx - text_px(text, scale) / 2, y, scale, text, r, g, b, a);
}

}  // namespace

std::string hud_timestamp(uint64_t sim_ticks) {
    const uint64_t total = sim_ticks / 120u;  // seconds (120 Hz)
    const uint64_t hh = (total / 3600u) % 100u;
    const uint64_t mm = (total / 60u) % 60u;
    const uint64_t ss = total % 60u;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02llu:%02llu:%02llu",
                  static_cast<unsigned long long>(hh), static_cast<unsigned long long>(mm),
                  static_cast<unsigned long long>(ss));
    return std::string(buf);
}

void build_hud_overlay(std::vector<uint8_t>& rgba, uint32_t width, uint32_t height,
                       const HudValues& v) {
    rgba.assign(static_cast<size_t>(width) * height * 4u, 0u);  // transparent

    const int scale = (width >= 1600) ? 3 : 2;
    const int lh = 8 * scale + 2 * scale;  // line height
    int y = 6 * scale;
    const int x = 6 * scale;
    const uint8_t cr = 190, cg = 255, cb = 200, ca = 220;  // CRT phosphor green

    char line[64];
    std::snprintf(line, sizeof(line), "TIME %s", hud_timestamp(v.sim_ticks).c_str());
    draw_text(rgba, width, height, x, y, scale, line, cr, cg, cb, ca); y += lh;
    std::snprintf(line, sizeof(line), "SEED %llu", static_cast<unsigned long long>(v.seed));
    draw_text(rgba, width, height, x, y, scale, line, cr, cg, cb, ca); y += lh;
    std::snprintf(line, sizeof(line), "ODO %dM", static_cast<int>(v.odometer_m));
    draw_text(rgba, width, height, x, y, scale, line, cr, cg, cb, ca); y += lh;
    std::snprintf(line, sizeof(line), "CHUNK %lld,%lld",
                  static_cast<long long>(v.chunk_x), static_cast<long long>(v.chunk_z));
    draw_text(rgba, width, height, x, y, scale, line, cr, cg, cb, ca); y += lh;
    std::snprintf(line, sizeof(line), "LVL %d", v.level);
    draw_text(rgba, width, height, x, y, scale, line, cr, cg, cb, ca); y += lh;
    std::snprintf(line, sizeof(line), "FPS %u", v.fps);
    draw_text(rgba, width, height, x, y, scale, line, cr, cg, cb, ca);
}

void build_menu_overlay(std::vector<uint8_t>& rgba, uint32_t width, uint32_t height,
                        const MenuModel& m) {
    rgba.assign(static_cast<size_t>(width) * height * 4u, 0u);
    // Dark, near-opaque backdrop: dims the world behind a pause menu; ~black otherwise.
    fill_rect(rgba, width, height, 0, 0, static_cast<int>(width), static_cast<int>(height), 8, 10, 9, 224);

    const int cx = static_cast<int>(width) / 2;
    const int base = (width >= 1600) ? 3 : 2;
    const uint8_t nr = 190, ng = 255, nb = 200;  // normal phosphor green
    const uint8_t sr = 225, sg = 255, sb = 235;  // selected (brighter)
    const uint8_t dr = 90, dg = 130, db = 100;   // disabled / dim

    auto title = [&](const char* t) {
        draw_centered(rgba, width, height, cx, height / 5, base * 3, t, nr, ng, nb, 255);
    };
    // A vertical list of centered rows; the selected row gets a highlight bar.
    auto items = [&](const char* const* labels, const bool* enabled, int n, int sel, int y0) {
        const int lh = base * 16;
        const int sc = base * 2;
        for (int i = 0; i < n; ++i) {
            const int y = y0 + i * lh;
            const bool on = (i == sel);
            if (on)
                fill_rect(rgba, width, height, cx - base * 92, y - base * 3, base * 184, lh - base * 4,
                          30, 60, 40, 200);
            uint8_t r = on ? sr : nr, g = on ? sg : ng, b = on ? sb : nb;
            if (enabled && !enabled[i]) { r = dr; g = dg; b = db; }
            draw_centered(rgba, width, height, cx, y, sc, labels[i], r, g, b, 255);
        }
    };

    switch (m.screen) {
        case Screen::Splash:
            title("BACKROOMS");
            draw_centered(rgba, width, height, cx, height * 3 / 5, base * 2, "PRESS ANY KEY", nr, ng, nb, 255);
            break;
        case Screen::MainMenu: {
            title("BACKROOMS");
            const char* labels[kMainItems] = {"NEW GAME", "CONTINUE", "SETTINGS", "QUIT"};
            const bool en[kMainItems] = {true, m.has_session, true, true};
            items(labels, en, kMainItems, m.main_sel, height * 2 / 5);
            char sd[32];
            std::snprintf(sd, sizeof(sd), "SEED %llu", static_cast<unsigned long long>(m.seed));
            draw_centered(rgba, width, height, cx, static_cast<int>(height) - base * 24, base * 2, sd, nr, ng, nb, 200);
            break;
        }
        case Screen::Pause: {
            title("PAUSED");
            const char* labels[kPauseItems] = {"RESUME", "SETTINGS", "QUIT TO MENU"};
            items(labels, nullptr, kPauseItems, m.pause_sel, height * 2 / 5);
            break;
        }
        case Screen::Settings: {
            title("SETTINGS");
            char rows[kSettingsItems][32];
            std::snprintf(rows[0], 32, "MASTER  %d", m.settings.master_pct);
            std::snprintf(rows[1], 32, "SFX  %d", m.settings.sfx_pct);
            std::snprintf(rows[2], 32, "MOUSE  %d", m.settings.mouse_pct);
            std::snprintf(rows[3], 32, "DIRECTOR  %s", m.settings.director ? "ON" : "OFF");
            std::snprintf(rows[4], 32, "RAY TRACING  %s", m.settings.rt ? "ON" : "OFF");
            std::snprintf(rows[5], 32, "RESOLUTION  %dx%d", m.settings.res_w, m.settings.res_h);
            std::snprintf(rows[6], 32, "TEST CONNECTION");
            std::snprintf(rows[7], 32, "SUBTITLES  %s", m.settings.subtitles ? "ON" : "OFF");
            std::snprintf(rows[8], 32, "BACK");
            const char* labels[kSettingsItems] = {rows[0], rows[1], rows[2], rows[3], rows[4], rows[5], rows[6], rows[7], rows[8]};
            items(labels, nullptr, kSettingsItems, m.settings_sel, height * 2 / 5);
            // A hint line + the live LLM status, under the list.
            const int hy = static_cast<int>(height) - base * 20;
            if (m.settings_sel == 5)
                draw_centered(rgba, width, height, cx, hy, base, "* APPLIES ON RESTART", nr, ng, nb, 220);
            else if (m.settings_sel == 3)
                draw_centered(rgba, width, height, cx, hy, base, "NEEDS THE LOCAL LLM - RUN TEST CONNECTION", nr, ng, nb, 220);
            else if (m.settings_sel == kSettingsTestConn)
                draw_centered(rgba, width, height, cx, hy, base, "ENTER - PING THE DIRECTOR LLM (KEEL)", nr, ng, nb, 220);
            else if (m.settings_sel == kSettingsSubtitles)
                draw_centered(rgba, width, height, cx, hy, base, "SHOW THE DIRECTOR LINES ON SCREEN AS YOU PLAY", nr, ng, nb, 220);
            // LLM connection status (set by the TestConnection ping). Always shown so it stays visible.
            if (!m.llm_text.empty()) {
                uint8_t lr = nr, lg = ng, lb = nb;
                if (m.llm_state == 3) { lr = 230; lg = 120; lb = 90; }       // offline -> amber
                else if (m.llm_state == 1) { lr = dr; lg = dg; lb = db; }    // testing -> dim
                draw_centered(rgba, width, height, cx, hy + base * 10, base, m.llm_text.c_str(), lr, lg, lb, 235);
            }
            break;
        }
        case Screen::Play:
        case Screen::Quit:
            break;  // the live world (or an exiting app) — no overlay
    }
}

void build_caption_overlay(std::vector<uint8_t>& rgba, uint32_t width, uint32_t height, const std::string& text) {
    rgba.assign(static_cast<size_t>(width) * height * 4u, 0u);  // transparent
    if (text.empty()) return;
    // Uppercase-fold to the bitmap font's charset (lowercase -> upper; unsupported glyphs render as space).
    std::string t = text;
    for (char& c : t) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    const int scale = (width >= 1600) ? 3 : 2;
    const int cx = static_cast<int>(width) / 2;
    const int y = static_cast<int>(height) - 11 * scale * 3;  // a few lines up from the bottom edge
    const int tw = text_px(t.c_str(), scale);
    fill_rect(rgba, width, height, cx - tw / 2 - 6 * scale, y - 3 * scale, tw + 12 * scale, 7 * scale + 6 * scale, 6, 8, 7, 175);  // dim backing bar
    draw_centered(rgba, width, height, cx, y, scale, t.c_str(), 200, 255, 210, 240);  // CRT phosphor green
}

}  // namespace br::app
