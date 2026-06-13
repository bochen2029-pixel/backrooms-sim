#pragma once
//
// contracts/telemetry_v1.h — all -> telemetry boundary (v1.0).
//
// Frame telemetry rows written to /runs/<timestamp>/frames.csv and parsed by
// the gate scripts (e.g. the M3 hitch gate and memory-soak gate).
//
#include <cstdint>

namespace br::contracts {

struct FrameMetrics {
    uint64_t frame;
    double frame_ms;
    uint64_t resident_chunks;
    uint64_t generated_total;
    uint64_t mem_bytes;
};

constexpr const char* kFrameCsvHeader =
    "frame,frame_ms,resident_chunks,generated_total,mem_bytes";

}  // namespace br::contracts
