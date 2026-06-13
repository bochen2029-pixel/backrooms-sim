# MODULE: gen (L1)

**Purpose.** Procedural chunk generation: layout solver, biome field, set
pieces, connectivity + geometry validators.

**Depends on:** `core` (types only).

**Invariants owned.** INV-2 Generation purity — `GenerateChunk(WorldSeed,
ChunkKey)` is pure and total; no neighbor queries; cross-seam agreement only
via shared edge hashes. INV-3 Connectivity — no sealed regions. Compiled
`/fp:strict`.

**Public surface.**
- `contracts/chunk_gen_v1.h` — `GenerateChunk` (pure/total, INV-2) +
  `ChunkContentHash` + `ValidateChunkGeometry`. M4 geometry is a real **Level-0
  maze**: world-coord floor + walls (render verts + collision AABBs). M5 extends
  each vertex with `uv` + `material` (kMat*), assigns materials (floor=Carpet,
  walls=Wallpaper), and emits a **ceiling** grid carrying a regular
  **fluorescent-tile pattern** (`is_fluorescent_cell` / `fluorescent_light_pos`,
  shared verbatim with the renderer's lights). The ceiling is render-only — it is
  **not** added to collision, so the wanderer/walk-bot are unaffected.
- `gen/layout.h` (M4) — `generate_layout` (G=8 spanning-tree maze + extra carves
  + 4 edge-hash doorways that neighbours agree on) + `validate_connectivity`
  (flood-fill, zero sealed). M7: the carve ratio is biome-driven via
  `generate_layout_carve` (connectivity holds for any ratio).
- `gen/biome.h` (M7) — the **biome field**: pure low-frequency
  `biome_at(seed, level, cx, cz)` over a coarse K=3 lattice (contiguous regions,
  designed proportions) + `BiomeParams` (carve ratio, pillar density, tint). 5
  biomes (ClassicYellow/CubicleFarm/PipeCorridors/ParkingGarage/Poolrooms).
  Selects internal layout + materials; never the edge-doorway protocol, so
  cross-biome seams stay connected (INV-3).
- `gen/gen.h` — identity stub.

**Planned.** Set pieces (pillar halls, flooded) + the geometry-validator
extension for pillars; verticality (level −1 + stairwells) — M7 phases 2b–3.

**Status:** M7 (in progress) — biome field wired into generation (per-biome carve
ratio + tint); distribution within ±2 % over 102,400 chunks; per-biome 10k-chunk
connectivity green; cross-biome seams connect; M4/M5 goldens re-captured (ADR-031).
Earlier (M5): per-vertex uv/material + fluorescent ceiling. Regen bit-identical.
