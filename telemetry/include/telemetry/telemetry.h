#pragma once
// telemetry/telemetry.h — counters, timers, CSV/JSON output, minidump capture.
// M0 stub: identity only. Real interface arrives with the contracts/telemetry_v1.h
// boundary in M3 (frame-time CSV) and M10 (minidumps).
namespace br::telemetry {
const char* module_name() noexcept;
}  // namespace br::telemetry
