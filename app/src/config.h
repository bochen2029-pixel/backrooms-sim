#pragma once
//
// app/config.h — persisted settings (M16), header-only.
//
// A flat `key=value` text config that saves/loads/applies on launch. `serialize`
// and `parse` are PURE (string <-> Config), so the write->read round-trip is
// unit-testable headlessly. File I/O is a thin inline wrapper. Unknown keys are
// ignored and missing keys keep their defaults, so the format is forward/backward
// tolerant (an older exe reading a newer config never crashes).
//
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace br::app {

struct Config {
    int width = 1920;       // default 1080p; clamp below allows up to 7680x4320 (4K/8K)
    int height = 1080;
    int fullscreen = 0;     // 0 windowed, 1 borderless fullscreen
    int vsync = 1;
    int master = 80;        // 0..100
    int sfx = 90;           // 0..100
    int mouse = 50;         // 0..100 sensitivity
    int director = 0;       // 0/1
    int fov = 70;           // degrees
    int renderer = 0;       // 0 raster, 1 DXR (reserved)
    int model_tier = 0;     // LLM model: 0 AUTO (VRAM-picked), 1 force 9B (vision), 2 force 4B (text). Applies on restart.
    uint64_t seed = 1;
};

inline int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Clamp every field into its valid range (defends against a hand-edited config).
inline Config sanitize(Config c) {
    c.width = clamp_int(c.width, 320, 7680);
    c.height = clamp_int(c.height, 240, 4320);
    c.fullscreen = c.fullscreen ? 1 : 0;
    c.vsync = c.vsync ? 1 : 0;
    c.master = clamp_int(c.master, 0, 100);
    c.sfx = clamp_int(c.sfx, 0, 100);
    c.mouse = clamp_int(c.mouse, 0, 100);
    c.director = c.director ? 1 : 0;
    c.fov = clamp_int(c.fov, 50, 110);
    c.renderer = c.renderer ? 1 : 0;
    c.model_tier = clamp_int(c.model_tier, 0, 2);
    if (c.seed == 0) c.seed = 1;
    return c;
}

inline std::string serialize(const Config& c) {
    std::ostringstream o;
    o << "# Backrooms Sim config (M16). Edit values; unknown keys are ignored.\n";
    o << "width=" << c.width << "\n";
    o << "height=" << c.height << "\n";
    o << "fullscreen=" << c.fullscreen << "\n";
    o << "vsync=" << c.vsync << "\n";
    o << "master=" << c.master << "\n";
    o << "sfx=" << c.sfx << "\n";
    o << "mouse=" << c.mouse << "\n";
    o << "director=" << c.director << "\n";
    o << "fov=" << c.fov << "\n";
    o << "renderer=" << c.renderer << "\n";
    o << "model_tier=" << c.model_tier << "\n";
    o << "seed=" << c.seed << "\n";
    return o.str();
}

inline Config parse(const std::string& text) {
    Config c;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const std::string v = line.substr(eq + 1);
        if (k == "width") c.width = std::atoi(v.c_str());
        else if (k == "height") c.height = std::atoi(v.c_str());
        else if (k == "fullscreen") c.fullscreen = std::atoi(v.c_str());
        else if (k == "vsync") c.vsync = std::atoi(v.c_str());
        else if (k == "master") c.master = std::atoi(v.c_str());
        else if (k == "sfx") c.sfx = std::atoi(v.c_str());
        else if (k == "mouse") c.mouse = std::atoi(v.c_str());
        else if (k == "director") c.director = std::atoi(v.c_str());
        else if (k == "fov") c.fov = std::atoi(v.c_str());
        else if (k == "renderer") c.renderer = std::atoi(v.c_str());
        else if (k == "model_tier") c.model_tier = std::atoi(v.c_str());
        else if (k == "seed") c.seed = std::strtoull(v.c_str(), nullptr, 10);
    }
    return sanitize(c);
}

// Thin file wrappers (not on the pure round-trip path). load_config returns false
// if the file is absent — the caller then keeps defaults and writes one on save.
inline bool load_config(const std::string& path, Config& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = parse(ss.str());
    return true;
}

inline bool save_config(const std::string& path, const Config& c) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << serialize(c);
    return static_cast<bool>(f);
}

}  // namespace br::app
