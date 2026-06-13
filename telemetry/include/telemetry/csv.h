#pragma once
//
// telemetry/csv.h — frame-telemetry CSV writer (telemetry_v1). Parsed by gates.
//
#include <cstdio>
#include <string>

#include "contracts/telemetry_v1.h"

namespace br::telemetry {

class FrameCsv {
public:
    FrameCsv() = default;
    ~FrameCsv() { close(); }
    FrameCsv(const FrameCsv&) = delete;
    FrameCsv& operator=(const FrameCsv&) = delete;

    bool open(const std::string& path);
    void write(const contracts::FrameMetrics& m);
    void close();

private:
    std::FILE* file_ = nullptr;
};

}  // namespace br::telemetry
