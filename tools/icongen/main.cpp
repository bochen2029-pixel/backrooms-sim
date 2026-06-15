// icongen — generate the procedural app icon (.ico) at BUILD time (tools, dev).
//
// No asset files: the icon is drawn from code — Backrooms-yellow wallpaper with
// subtle seeded grain, a dark central doorway, a bright fluorescent bar, and a
// floor line — at 256x256 RGBA, PNG-encoded, wrapped in a single-image ICO
// container (Vista+ PNG-in-ICO). Deterministic: same output every build.
//
//   icongen <out.ico>
//
// Exit: 0 ok, 2 usage, 3 I/O.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <stb_image_write.h>

#include "core/rng.h"

namespace {
void png_sink(void* ctx, void* data, int len) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    const auto* p = static_cast<uint8_t*>(data);
    v->insert(v->end(), p, p + len);
}
void put_u16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x & 0xff); v.push_back((x >> 8) & 0xff); }
void put_u32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((x >> (i * 8)) & 0xff); }
uint8_t clamp8(float f) { return static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, f))); }
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: icongen <out.ico>\n"); return 2; }

    const int N = 256;
    std::vector<uint8_t> img(static_cast<size_t>(N) * N * 4);
    br::core::Pcg64 rng(0xBACC0017ull);  // deterministic grain seed
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            const float u = x / static_cast<float>(N), v = y / static_cast<float>(N);
            const float n = (rng.next_u64() & 0xff) / 255.0f;
            float r = 200.0f + (n - 0.5f) * 26.0f;   // Backrooms wallpaper yellow
            float g = 188.0f + (n - 0.5f) * 26.0f;
            float b = 108.0f + (n - 0.5f) * 22.0f;
            if (u > 0.40f && u < 0.60f && v > 0.28f && v < 0.92f) { r = 24; g = 21; b = 15; }   // doorway
            if (v > 0.10f && v < 0.16f && u > 0.24f && u < 0.76f) { r = 250; g = 250; b = 226; } // fluorescent
            if (v > 0.90f && v < 0.95f) { r = 120; g = 110; b = 68; }                            // floor line
            const size_t i = (static_cast<size_t>(y) * N + x) * 4;
            img[i] = clamp8(r); img[i + 1] = clamp8(g); img[i + 2] = clamp8(b); img[i + 3] = 255;
        }
    }

    std::vector<uint8_t> png;
    if (!stbi_write_png_to_func(png_sink, &png, N, N, 4, img.data(), N * 4)) {
        std::fprintf(stderr, "icongen: PNG encode failed\n"); return 3;
    }

    // ICO = ICONDIR (6) + one ICONDIRENTRY (16) + the PNG blob.
    std::vector<uint8_t> ico;
    put_u16(ico, 0);  // reserved
    put_u16(ico, 1);  // type = icon
    put_u16(ico, 1);  // image count
    ico.push_back(0); // width  0 => 256
    ico.push_back(0); // height 0 => 256
    ico.push_back(0); // palette color count
    ico.push_back(0); // reserved
    put_u16(ico, 1);  // color planes
    put_u16(ico, 32); // bits per pixel
    put_u32(ico, static_cast<uint32_t>(png.size()));  // bytes in resource
    put_u32(ico, 22); // offset to the image (6 + 16)
    ico.insert(ico.end(), png.begin(), png.end());

    FILE* f = std::fopen(argv[1], "wb");
    if (!f) { std::fprintf(stderr, "icongen: cannot open %s\n", argv[1]); return 3; }
    std::fwrite(ico.data(), 1, ico.size(), f);
    std::fclose(f);
    std::printf("wrote %s (%zu bytes)\n", argv[1], ico.size());
    return 0;
}
