#include "render_d3d12/texgen.h"

namespace br::render_d3d12 {

namespace {
constexpr int N = kTexSize;

uint32_t px_hash(uint64_t seed, int x, int y) {
    uint64_t h = seed * 0x9e3779b97f4a7c15ULL;
    h ^= static_cast<uint64_t>(static_cast<uint32_t>(x)) * 0x85ebca6bULL;
    h ^= static_cast<uint64_t>(static_cast<uint32_t>(y)) * 0xc2b2ae35ULL;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 29;
    return static_cast<uint32_t>(h);
}

int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

void set_px(std::vector<uint8_t>& t, int x, int y, int r, int g, int b) {
    const size_t i = static_cast<size_t>(y * N + x) * 4u;
    t[i + 0] = static_cast<uint8_t>(clamp8(r));
    t[i + 1] = static_cast<uint8_t>(clamp8(g));
    t[i + 2] = static_cast<uint8_t>(clamp8(b));
    t[i + 3] = 255u;
}
}  // namespace

void generate_texture(TexKind kind, uint64_t seed, std::vector<uint8_t>& rgba) {
    rgba.assign(static_cast<size_t>(N) * N * 4u, 255u);
    const uint64_t s = seed ^ (static_cast<uint64_t>(kind) * 0x9e3779b97f4a7c15ULL);

    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            const uint32_t hh = px_hash(s, x, y);
            const int grain = static_cast<int>(hh & 31u) - 16;  // +/-16
            int r = 0, g = 0, b = 0;
            switch (kind) {
                case TexKind::Wallpaper: {
                    const int stripe = ((x % 32) < 2) ? 12 : 0;
                    const int grad = (N - y) / 16;
                    r = 198 + grain + stripe + grad;
                    g = 178 + grain + stripe + grad;
                    b = 92 + grain / 2 + stripe;
                    break;
                }
                case TexKind::Carpet: {
                    const int speck = static_cast<int>((hh >> 5) & 31u) - 16;
                    r = 96 + speck;
                    g = 86 + speck;
                    b = 56 + speck / 2;
                    break;
                }
                case TexKind::CeilingTile: {
                    const int line = ((x % 64) < 2 || (y % 64) < 2) ? -45 : 0;
                    r = 212 + grain / 2 + line;
                    g = 210 + grain / 2 + line;
                    b = 198 + grain / 2 + line;
                    break;
                }
                case TexKind::Fluorescent: {
                    const int band = ((y % 24) < 3) ? -20 : 0;
                    r = 246 + grain / 3 + band;
                    g = 246 + grain / 3 + band;
                    b = 236 + grain / 3 + band;
                    break;
                }
                case TexKind::Baseboard:
                default: {
                    const int scuff = static_cast<int>((hh >> 8) & 63u) - 32;
                    const int topline = (y < 6) ? 30 : 0;
                    r = 74 + scuff / 2 + topline;
                    g = 64 + scuff / 2 + topline;
                    b = 44 + scuff / 3 + topline;
                    break;
                }
            }
            set_px(rgba, x, y, r, g, b);
        }
    }
}

uint64_t texture_hash(const std::vector<uint8_t>& rgba) {
    uint64_t h = 1469598103934665603ULL;
    for (uint8_t byte : rgba) {
        h ^= byte;
        h *= 1099511628211ULL;
    }
    return h;
}

}  // namespace br::render_d3d12
