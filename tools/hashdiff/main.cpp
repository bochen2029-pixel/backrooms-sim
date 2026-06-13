// hashdiff — image hash + perceptual diff CLI (tools, dev).
//
// Used by gates from M1 onward to compare headless renders against goldens.
//
//   hashdiff hash  <image>          -> prints 16-hex FNV-1a hash of (w,h,pixels)
//   hashdiff diff  <a> <b>          -> prints mean absolute channel difference
//                                      (0 == identical; >0 == differs)
//   hashdiff equal <a> <b>          -> exit 0 if identical, 1 if differs
//
// Images are decoded to 8-bit RGBA via stb_image. Exit codes: 0 ok,
// 2 usage error, 3 I/O error.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <stb_image.h>

namespace {

struct Image {
    int w = 0;
    int h = 0;
    unsigned char* px = nullptr;  // RGBA, w*h*4 bytes
    ~Image() { if (px) stbi_image_free(px); }
};

bool load_rgba(const char* path, Image& out) {
    int n = 0;
    out.px = stbi_load(path, &out.w, &out.h, &n, 4);
    return out.px != nullptr;
}

uint64_t fnv1a(const Image& img) {
    uint64_t h = 1469598103934665603ULL;
    const uint64_t prime = 1099511628211ULL;
    auto mix = [&](unsigned char b) { h ^= b; h *= prime; };
    // Fold dimensions in first so different-sized images never collide.
    const uint32_t w = static_cast<uint32_t>(img.w);
    const uint32_t ht = static_cast<uint32_t>(img.h);
    for (int i = 0; i < 4; ++i) mix(static_cast<unsigned char>((w >> (i * 8)) & 0xff));
    for (int i = 0; i < 4; ++i) mix(static_cast<unsigned char>((ht >> (i * 8)) & 0xff));
    const size_t bytes = static_cast<size_t>(img.w) * static_cast<size_t>(img.h) * 4u;
    for (size_t i = 0; i < bytes; ++i) mix(img.px[i]);
    return h;
}

// Mean absolute per-channel difference. Mismatched dimensions are reported as
// the maximum possible difference (255) so the gate sees a clear nonzero.
double mean_abs_diff(const Image& a, const Image& b) {
    if (a.w != b.w || a.h != b.h) return 255.0;
    const size_t bytes = static_cast<size_t>(a.w) * static_cast<size_t>(a.h) * 4u;
    if (bytes == 0) return 0.0;
    uint64_t acc = 0;
    for (size_t i = 0; i < bytes; ++i) {
        const int d = static_cast<int>(a.px[i]) - static_cast<int>(b.px[i]);
        acc += static_cast<uint64_t>(d < 0 ? -d : d);
    }
    return static_cast<double>(acc) / static_cast<double>(bytes);
}

int usage() {
    std::fprintf(stderr,
        "usage:\n"
        "  hashdiff hash  <image>\n"
        "  hashdiff diff  <a> <b>\n"
        "  hashdiff equal <a> <b>\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const char* cmd = argv[1];

    if (std::strcmp(cmd, "hash") == 0) {
        if (argc != 3) return usage();
        Image img;
        if (!load_rgba(argv[2], img)) {
            std::fprintf(stderr, "hashdiff: cannot load '%s'\n", argv[2]);
            return 3;
        }
        std::printf("%016llx\n", static_cast<unsigned long long>(fnv1a(img)));
        return 0;
    }

    if (std::strcmp(cmd, "diff") == 0 || std::strcmp(cmd, "equal") == 0) {
        if (argc != 4) return usage();
        Image a, b;
        if (!load_rgba(argv[2], a)) {
            std::fprintf(stderr, "hashdiff: cannot load '%s'\n", argv[2]);
            return 3;
        }
        if (!load_rgba(argv[3], b)) {
            std::fprintf(stderr, "hashdiff: cannot load '%s'\n", argv[3]);
            return 3;
        }
        const double d = mean_abs_diff(a, b);
        if (std::strcmp(cmd, "equal") == 0) {
            return d == 0.0 ? 0 : 1;
        }
        std::printf("%.6f\n", d);
        return 0;
    }

    return usage();
}
