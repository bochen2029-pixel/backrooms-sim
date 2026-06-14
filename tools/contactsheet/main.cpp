// contactsheet — tile periodic soak screenshots + mechanical screen (tools, M10).
//
// Reads shot_*.png from a soak run's screenshot directory (in name order),
// downscales each to a thumbnail, tiles them into one contact-sheet PNG for agent
// visual review, and runs a mechanical histogram screen that flags all-black or
// all-white frames (a render/stream regression the agent might miss at a glance).
//
//   contactsheet <shots_dir> <out.png> [--cols N] [--thumb-w W] [--thumb-h H]
//
// Exit codes: 0 ok (no degenerate frames), 1 degenerate frame(s) found,
// 2 usage error, 3 I/O error.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <stb_image.h>
#include <stb_image_write.h>

namespace fs = std::filesystem;

namespace {

struct Img {
    int w = 0, h = 0;
    std::vector<unsigned char> px;  // RGBA
};

bool load_rgba(const std::string& path, Img& out) {
    int w = 0, h = 0, n = 0;
    unsigned char* p = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!p) return false;
    out.w = w; out.h = h;
    out.px.assign(p, p + static_cast<size_t>(w) * h * 4u);
    stbi_image_free(p);
    return true;
}

double mean_luma(const Img& im) {
    if (im.px.empty()) return 0.0;
    const size_t n = static_cast<size_t>(im.w) * im.h;
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char* c = &im.px[i * 4];
        s += 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
    }
    return s / static_cast<double>(n);
}

// Nearest-neighbour downscale of `src` into the tile at (x0,y0) of `sheet`.
void blit_thumb(const Img& src, std::vector<unsigned char>& sheet, int sheetW,
                int x0, int y0, int tw, int th) {
    for (int y = 0; y < th; ++y) {
        int sy = static_cast<int>(static_cast<int64_t>(y) * src.h / th);
        if (sy >= src.h) sy = src.h - 1;
        for (int x = 0; x < tw; ++x) {
            int sx = static_cast<int>(static_cast<int64_t>(x) * src.w / tw);
            if (sx >= src.w) sx = src.w - 1;
            const unsigned char* s = &src.px[(static_cast<size_t>(sy) * src.w + sx) * 4];
            unsigned char* d = &sheet[(static_cast<size_t>(y0 + y) * sheetW + (x0 + x)) * 4];
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
        }
    }
}

int usage() {
    std::fprintf(stderr,
        "usage: contactsheet <shots_dir> <out.png> [--cols N] [--thumb-w W] [--thumb-h H]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string dir = argv[1];
    const std::string out = argv[2];
    int cols = 0, tw = 192, th = 108;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--cols") == 0) cols = std::atoi(argv[i + 1]);
        else if (std::strcmp(argv[i], "--thumb-w") == 0) tw = std::atoi(argv[i + 1]);
        else if (std::strcmp(argv[i], "--thumb-h") == 0) th = std::atoi(argv[i + 1]);
        else return usage();
    }
    if (tw < 1 || th < 1) { std::fprintf(stderr, "contactsheet: thumb dims must be > 0\n"); return 2; }

    std::error_code ec;
    if (!fs::is_directory(dir, ec)) { std::fprintf(stderr, "contactsheet: not a directory: %s\n", dir.c_str()); return 3; }
    std::vector<std::string> files;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        const std::string name = e.path().filename().string();
        if (name.rfind("shot_", 0) == 0 && name.size() > 4 && name.substr(name.size() - 4) == ".png")
            files.push_back(e.path().string());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) { std::fprintf(stderr, "contactsheet: no shot_*.png in %s\n", dir.c_str()); return 3; }

    if (cols <= 0) cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(files.size()))));
    if (cols < 1) cols = 1;
    const int rows = (static_cast<int>(files.size()) + cols - 1) / cols;
    const int sheetW = cols * tw, sheetH = rows * th;

    std::vector<unsigned char> sheet(static_cast<size_t>(sheetW) * sheetH * 4u, 0u);
    for (size_t i = 0; i < sheet.size(); i += 4) { sheet[i] = sheet[i + 1] = sheet[i + 2] = 16u; sheet[i + 3] = 255u; }

    int loaded = 0, black = 0, white = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        Img im;
        if (!load_rgba(files[i], im)) { std::fprintf(stderr, "contactsheet: skip unreadable %s\n", files[i].c_str()); continue; }
        const double L = mean_luma(im);
        if (L < 3.0) ++black;
        if (L > 250.0) ++white;
        const int r = static_cast<int>(i) / cols, c = static_cast<int>(i) % cols;
        blit_thumb(im, sheet, sheetW, c * tw, r * th, tw, th);
        ++loaded;
    }

    if (stbi_write_png(out.c_str(), sheetW, sheetH, 4, sheet.data(), sheetW * 4) == 0) {
        std::fprintf(stderr, "contactsheet: failed to write '%s'\n", out.c_str());
        return 3;
    }
    std::printf("tiles: %d\n", loaded);
    std::printf("grid_cols: %d\n", cols);
    std::printf("grid_rows: %d\n", rows);
    std::printf("black_frames: %d\n", black);
    std::printf("white_frames: %d\n", white);
    std::printf("sheet: %s\n", out.c_str());
    return (black == 0 && white == 0) ? 0 : 1;
}
