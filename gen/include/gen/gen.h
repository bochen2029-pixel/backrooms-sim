#pragma once
// gen/gen.h — chunk layout solver, biome field, set pieces, connectivity +
// geometry validators. M0 stub: identity only. GenerateChunk(WorldSeed,
// ChunkKey) (INV-2 pure/total) arrives in M3/M4 via contracts/chunk_gen_v1.h.
namespace br::gen {
const char* module_name() noexcept;
}  // namespace br::gen
