// test_texgen — procedural texture determinism (M5): per (kind, seed) hashes are
// stable, distinct kinds/seeds differ.
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "render_d3d12/texgen.h"

using namespace br::render_d3d12;

TEST_CASE("texture generation is deterministic per (kind, seed)", "[m5][texgen]") {
    const TexKind kinds[] = { TexKind::Wallpaper, TexKind::Carpet, TexKind::CeilingTile,
                              TexKind::Fluorescent, TexKind::Baseboard };
    for (TexKind k : kinds) {
        std::vector<uint8_t> a, b;
        generate_texture(k, 4242u, a);
        generate_texture(k, 4242u, b);
        REQUIRE(a.size() == static_cast<size_t>(kTexSize) * kTexSize * 4u);
        REQUIRE(a == b);
        REQUIRE(texture_hash(a) == texture_hash(b));
    }
}

TEST_CASE("distinct kinds and seeds produce distinct textures", "[m5][texgen]") {
    std::vector<uint8_t> wall, carpet, wall2;
    generate_texture(TexKind::Wallpaper, 1u, wall);
    generate_texture(TexKind::Carpet, 1u, carpet);
    generate_texture(TexKind::Wallpaper, 2u, wall2);
    REQUIRE(texture_hash(wall) != texture_hash(carpet));   // different material
    REQUIRE(texture_hash(wall) != texture_hash(wall2));    // different seed
}
