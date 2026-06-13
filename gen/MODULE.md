# MODULE: gen (L1)

**Purpose.** Procedural chunk generation: layout solver, biome field, set
pieces, connectivity + geometry validators.

**Depends on:** `core` (types only).

**Invariants owned.** INV-2 Generation purity — `GenerateChunk(WorldSeed,
ChunkKey)` is pure and total; no neighbor queries; cross-seam agreement only
via shared edge hashes. INV-3 Connectivity — no sealed regions. Compiled
`/fp:strict`.

**Public surface.**
- `contracts/chunk_gen_v1.h` (M3) — `GenerateChunk(seed, ChunkKey)` (pure/total,
  INV-2) + `ChunkContentHash`. M3 geometry is placeholder: a world-coordinate
  grid floor (per-chunk tint) + interior posts, seam-correct by construction.
- `gen/gen.h` — identity stub.

**Planned.** Level-0 rooms/doorways + connectivity & geometry validators (M4),
biomes/set pieces/verticality (M7).

**Status:** M3 — `GenerateChunk` (placeholder geometry). Regen-identical across
1000 chunks; adjacent-chunk seams match exactly.
