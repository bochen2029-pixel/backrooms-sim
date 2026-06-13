# MODULE: telemetry (L2)

**Purpose.** Counters, timers, CSV/JSON run output (`/runs/<timestamp>/`),
minidump capture. Parsed by the gate scripts to enforce NFRs.

**Depends on:** nothing (interface defined in `contracts/telemetry_v1.h`).

**Public surface.**
- `telemetry/csv.h` (M3) — `FrameCsv`: writes `contracts::FrameMetrics` rows
  (frame, frame_ms, resident_chunks, generated_total, mem_bytes) parsed by the
  M3 hitch + soak gates.
- `telemetry/telemetry.h` — identity stub.

**Planned.** Tick/chunk-gen counters (M3+), minidump + auto-restart logging (M10).

**Status:** M3 — frame-telemetry CSV.
