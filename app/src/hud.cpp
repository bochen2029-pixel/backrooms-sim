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
        case 'C': { static const Glyph g = {{0b01110,0b10001,0b10000,0b10000,0b10000,0b10001,0b01110}}; return &g; }
        case 'D': { static const Glyph g = {{0b11110,0b10001,0b10001,0b10001,0b10001,0b10001,0b11110}}; return &g; }
        case 'E': { static const Glyph g = {{0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b11111}}; return &g; }
        case 'F': { static const Glyph g = {{0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b10000}}; return &g; }
        case 'H': { static const Glyph g = {{0b10001,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001}}; return &g; }
        case 'I': { static const Glyph g = {{0b01110,0b00100,0b00100,0b00100,0b00100,0b00100,0b01110}}; return &g; }
        case 'K': { static const Glyph g = {{0b10001,0b10010,0b10100,0b11000,0b10100,0b10010,0b10001}}; return &g; }
        case 'L': { static const Glyph g = {{0b10000,0b10000,0b10000,0b10000,0b10000,0b10000,0b11111}}; return &g; }
        case 'M': { static const Glyph g = {{0b10001,0b11011,0b10101,0b10101,0b10001,0b10001,0b10001}}; return &g; }
        case 'N': { static const Glyph g = {{0b10001,0b11001,0b10101,0b10011,0b10001,0b10001,0b10001}}; return &g; }
        case 'O': { static const Glyph g = {{0b01110,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110}}; return &g; }
        case 'P': { static const Glyph g = {{0b11110,0b10001,0b10001,0b11110,0b10000,0b10000,0b10000}}; return &g; }
        case 'S': { static const Glyph g = {{0b01111,0b10000,0b10000,0b01110,0b00001,0b00001,0b11110}}; return &g; }
        case 'T': { static const Glyph g = {{0b11111,0b00100,0b00100,0b00100,0b00100,0b00100,0b00100}}; return &g; }
        case 'U': { static const Glyph g = {{0b10001,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110}}; return &g; }
        case 'V': { static const Glyph g = {{0b10001,0b10001,0b10001,0b10001,0b10001,0b01010,0b00100}}; return &g; }
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

}  // namespace br::app
