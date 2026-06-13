// goldgen — golden capture / synthesis CLI (tools, dev).
//
// THE only sanctioned writer of /goldens artifacts (INV-8). Updates must be
// accompanied by a DECISIONS.md entry in the same commit.
//
//   goldgen synth   <out.png> [--w W] [--h H] [--seed S] [--perturb N]
//        Write a deterministic procedural RGBA PNG. Identical params -> byte-
//        identical output; --perturb N flips N deterministic pixels. Used by
//        the M0 gate to fabricate a known image pair for hashdiff.
//
//   goldgen capture <src.png> <golden.png>
//        Copy a candidate artifact into the goldens tree.
//
// Exit codes: 0 ok, 2 usage error, 3 I/O error.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

#include <stb_image_write.h>

#include "core/rng.h"

namespace {

int usage() {
    std::fprintf(stderr,
        "usage:\n"
        "  goldgen synth   <out.png> [--w W] [--h H] [--seed S] [--perturb N]\n"
        "  goldgen capture <src.png> <golden.png>\n");
    return 2;
}

uint64_t parse_u64(const char* s) { return std::strtoull(s, nullptr, 0); }

int cmd_synth(int argc, char** argv) {
    if (argc < 3) return usage();
    const char* out = argv[2];
    uint32_t w = 64, h = 64, perturb = 0;
    uint64_t seed = 0;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--w") == 0) w = static_cast<uint32_t>(parse_u64(argv[i + 1]));
        else if (std::strcmp(argv[i], "--h") == 0) h = static_cast<uint32_t>(parse_u64(argv[i + 1]));
        else if (std::strcmp(argv[i], "--seed") == 0) seed = parse_u64(argv[i + 1]);
        else if (std::strcmp(argv[i], "--perturb") == 0) perturb = static_cast<uint32_t>(parse_u64(argv[i + 1]));
        else return usage();
    }
    if (w == 0 || h == 0) { std::fprintf(stderr, "goldgen: w/h must be > 0\n"); return 2; }

    const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
    std::vector<unsigned char> buf(pixels * 4u, 255u);

    // Deterministic procedural fill from the canonical core RNG (INV-1).
    br::core::Pcg64 rng(seed);
    for (size_t i = 0; i < pixels; ++i) {
        const uint64_t u = rng.next_u64();
        buf[i * 4 + 0] = static_cast<unsigned char>(u & 0xff);
        buf[i * 4 + 1] = static_cast<unsigned char>((u >> 8) & 0xff);
        buf[i * 4 + 2] = static_cast<unsigned char>((u >> 16) & 0xff);
        buf[i * 4 + 3] = 255u;  // opaque
    }

    // Deterministic perturbation: flip RGB of N pixels at hashed positions.
    if (perturb > 0) {
        br::core::Pcg64 prng(seed ^ 0x9e3779b97f4a7c15ULL);
        for (uint32_t k = 0; k < perturb; ++k) {
            const size_t idx = static_cast<size_t>(prng.bounded(pixels));
            buf[idx * 4 + 0] = static_cast<unsigned char>(buf[idx * 4 + 0] ^ 0xff);
            buf[idx * 4 + 1] = static_cast<unsigned char>(buf[idx * 4 + 1] ^ 0xff);
            buf[idx * 4 + 2] = static_cast<unsigned char>(buf[idx * 4 + 2] ^ 0xff);
        }
    }

    const int stride = static_cast<int>(w) * 4;
    if (stbi_write_png(out, static_cast<int>(w), static_cast<int>(h), 4, buf.data(), stride) == 0) {
        std::fprintf(stderr, "goldgen: failed to write '%s'\n", out);
        return 3;
    }
    std::printf("wrote %s (%ux%u seed=%llu perturb=%u)\n", out, w, h,
                static_cast<unsigned long long>(seed), perturb);
    return 0;
}

int cmd_capture(int argc, char** argv) {
    if (argc != 4) return usage();
    std::FILE* in = std::fopen(argv[2], "rb");
    if (!in) { std::fprintf(stderr, "goldgen: cannot open '%s'\n", argv[2]); return 3; }
    std::FILE* out = std::fopen(argv[3], "wb");
    if (!out) { std::fclose(in); std::fprintf(stderr, "goldgen: cannot write '%s'\n", argv[3]); return 3; }
    unsigned char chunk[65536];
    size_t n;
    int rc = 0;
    while ((n = std::fread(chunk, 1, sizeof(chunk), in)) > 0) {
        if (std::fwrite(chunk, 1, n, out) != n) { rc = 3; break; }
    }
    std::fclose(in);
    std::fclose(out);
    if (rc == 0) std::printf("captured %s -> %s\n", argv[2], argv[3]);
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    if (std::strcmp(argv[1], "synth") == 0) return cmd_synth(argc, argv);
    if (std::strcmp(argv[1], "capture") == 0) return cmd_capture(argc, argv);
    return usage();
}
