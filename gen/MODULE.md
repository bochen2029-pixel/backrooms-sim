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
  maze**: world-coord floor + walls (render verts + collision AABBs).
- `gen/layout.h` (M4) — `generate_layout` (G=8 spanning-tree maze + ~25% extra
  carves + 4 edge-hash doorways that neighbours agree on) + `validate_connectivity`
  (flood-fill, zero sealed).
- `gen/gen.h` — identity stub.

**Planned.** Biomes / set pieces / verticality (M7), set-piece injection.

**Status:** M4 — Level-0 maze. 10,000-chunk connectivity (zero sealed) + geometry
validators; adjacent-chunk seam doorways agree; regen bit-identical.
