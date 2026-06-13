# MODULE: stream (L2)

**Purpose.** Chunk ring management, background worker pool, residency events
(ChunkGenerated / ChunkEvicted). Bounds memory (INV-4).

**Depends on:** `core`, `gen`.

**Public surface.**
- `stream/stream_manager.h` (M3) — `StreamManager`: a `(2r+1)^2` chunk ring
  around a moving center; background worker pool generates missing chunks
  (`gen::GenerateChunk`), main thread collects + evicts. Bounded residency
  (INV-4); decoupled from the sim (INV-1). Exposes a `ResidentChunk` snapshot
  (contracts/stream_events_v1.h).
- `stream/stream.h` — identity stub.

**Planned.** TLAS-refit residency events (M9), prefetch/priority by heading.

**Status:** M3 — ring + worker pool, bounded + recentering verified.
