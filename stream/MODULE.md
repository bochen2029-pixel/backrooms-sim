# MODULE: stream (L2)

**Purpose.** Chunk ring management, background worker pool, residency events
(ChunkGenerated / ChunkEvicted). Bounds memory (INV-4).

**Depends on:** `core`, `gen`.

**Public surface (M0).** `stream/stream.h` — identity stub.

**Planned.** Load ring + background generation + main-thread GPU upload (M3),
TLAS-refit residency events (M9).

**Contracts produced:** `contracts/stream_events_v1.h` (M3).

**Status:** M0 stub.
