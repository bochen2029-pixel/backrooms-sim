# MODULE: telemetry (L2)

**Purpose.** Counters, timers, CSV/JSON run output (`/runs/<timestamp>/`),
minidump capture. Parsed by the gate scripts to enforce NFRs.

**Depends on:** nothing (interface defined in `contracts/telemetry_v1.h`).

**Public surface.**
- `telemetry/csv.h` (M3) — `FrameCsv`: writes `contracts::FrameMetrics` rows
  (frame, frame_ms, resident_chunks, generated_total, mem_bytes) parsed by the
  M3 hitch + soak gates.
- `telemetry/crash.h` (M10) — `install_crash_handler(dir)` installs an
  unhandled-exception filter that, on a fatal fault, writes `<dir>/minidump.dmp`
  (dbghelp `MiniDumpWriteDump`) + a `<dir>/crash.log` marker and exits with
  `kCrashExitCode` (70); `force_crash()` is the forced-crash drill. `app` installs
  it at startup so any soak fault is captured; `scripts/soak.ps1` auto-restarts.
- `telemetry/telemetry.h` — identity stub.

**Planned.** Tick/chunk-gen counters (later).

**Status:** M10 — frame-telemetry CSV (M3) + minidump capture (`dbghelp`).
