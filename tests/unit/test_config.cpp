// test_config — M16 persisted settings: the write->read round-trip is exact, the
// format tolerates unknown/missing keys, and sanitize() clamps a hand-edited file.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "config.h"

using namespace br::app;

namespace {
void require_equal(const Config& a, const Config& b) {
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    REQUIRE(a.fullscreen == b.fullscreen);
    REQUIRE(a.vsync == b.vsync);
    REQUIRE(a.master == b.master);
    REQUIRE(a.sfx == b.sfx);
    REQUIRE(a.mouse == b.mouse);
    REQUIRE(a.director == b.director);
    REQUIRE(a.fov == b.fov);
    REQUIRE(a.renderer == b.renderer);
    REQUIRE(a.model_tier == b.model_tier);
    REQUIRE(a.rt_scale == b.rt_scale);
    REQUIRE(a.seed == b.seed);
}
}  // namespace

TEST_CASE("config write -> read round-trips identically", "[m16][config]") {
    Config c;
    c.width = 1920; c.height = 1080; c.fullscreen = 1; c.vsync = 0;
    c.master = 65; c.sfx = 40; c.mouse = 75; c.director = 1; c.fov = 95;
    c.renderer = 1; c.model_tier = 2; c.rt_scale = 2; c.seed = 123456789ull;
    const Config back = parse(serialize(c));
    require_equal(c, back);

    // Round-trip is idempotent (serialize is canonical).
    REQUIRE(serialize(back) == serialize(c));
}

TEST_CASE("config defaults survive a serialize/parse cycle", "[m16][config]") {
    Config def;
    require_equal(def, parse(serialize(def)));
}

TEST_CASE("parse ignores comments, blanks, and unknown keys; missing keys keep defaults",
          "[m16][config]") {
    const std::string text =
        "# a comment\n"
        "\n"
        "master=33\n"
        "unknown_key=999\n"
        "height=900\n"
        "garbage line without equals\n";
    const Config c = parse(text);
    REQUIRE(c.master == 33);
    REQUIRE(c.height == 900);
    REQUIRE(c.width == 1920);   // untouched -> default (1080p)
    REQUIRE(c.sfx == 90);       // untouched -> default
}

TEST_CASE("sanitize clamps an out-of-range hand-edited config", "[m16][config]") {
    std::string text =
        "master=500\nsfx=-20\nmouse=1000\nfov=10\nwidth=10\nheight=99999\nrt_scale=9\nseed=0\n";
    const Config c = parse(text);  // parse() runs sanitize()
    REQUIRE(c.master == 100);
    REQUIRE(c.sfx == 0);
    REQUIRE(c.mouse == 100);
    REQUIRE(c.fov == 50);          // clamped up to the floor
    REQUIRE(c.width == 320);       // clamped up
    REQUIRE(c.height == 4320);     // clamped down
    REQUIRE(c.rt_scale == 2);      // clamped into -1 (AUTO) .. 2 (Performance)
    REQUIRE(c.seed == 1);          // 0 -> 1 (a valid seed)
}
