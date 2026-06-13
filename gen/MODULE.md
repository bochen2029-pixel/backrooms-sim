# MODULE: gen (L1)

**Purpose.** Procedural chunk generation: layout solver, biome field, set
pieces, connectivity + geometry validators.

**Depends on:** `core` (types only).

**Invariants owned.** INV-2 Generation purity — `GenerateChunk(WorldSeed,
ChunkKey)` is pure and total; no neighbor queries; cross-seam agreement only
via shared edge hashes. INV-3 Connectivity — no sealed regions. Compiled
`/fp:strict`.

**Public surface (M0).** `gen/gen.h` — identity stub.

**Planned.** `GenerateChunk` + `ChunkContentHash` (M3), Level 0 rooms/doorways
+ connectivity & geometry validators (M4), biomes/set pieces/verticality (M7).

**Contracts produced:** `contracts/chunk_gen_v1.h` (M3).

**Status:** M0 stub.
