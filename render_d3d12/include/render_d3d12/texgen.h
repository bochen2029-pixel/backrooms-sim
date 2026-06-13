#pragma once
//
// render_d3d12/texgen.h — procedural material textures (M5). Generated at
// startup, no asset files (ADR-004). Deterministic per (kind, seed): the
// determinism gate hashes each texture. D3D12-free so it is unit-testable.
//
#include <cstdint>
#include <vector>

namespace br::render_d3d12 {

enum class TexKind : uint32_t {
    Wallpaper = 0,    // yellow backrooms wallpaper
    Carpet = 1,       // muted damp carpet
    CeilingTile = 2,  // off-white tile grid
    Fluorescent = 3,  // bright emissive panel
    Baseboard = 4,    // scuffed dark baseboard
};
constexpr uint32_t kTexCount = 5;
constexpr int kTexSize = 256;  // square, RGBA8

// Fill `rgba` with kTexSize*kTexSize*4 bytes for the given material + seed.
void generate_texture(TexKind kind, uint64_t seed, std::vector<uint8_t>& rgba);

// FNV-1a over the pixel bytes (determinism gate).
uint64_t texture_hash(const std::vector<uint8_t>& rgba);

}  // namespace br::render_d3d12
