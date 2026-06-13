# MODULE: telemetry (L2)

**Purpose.** Counters, timers, CSV/JSON run output (`/runs/<timestamp>/`),
minidump capture. Parsed by the gate scripts to enforce NFRs.

**Depends on:** nothing (interface defined in `contracts/telemetry_v1.h`).

**Public surface (M0).** `telemetry/telemetry.h` — identity stub.

**Planned.** Frame/tick/memory/chunk-gen counters + CSV (M3), minidump +
auto-restart logging (M10).

**Status:** M0 stub.
