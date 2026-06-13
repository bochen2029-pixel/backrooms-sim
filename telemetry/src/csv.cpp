#include "telemetry/csv.h"

namespace br::telemetry {

bool FrameCsv::open(const std::string& path) {
    close();
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) return false;
    std::fprintf(file_, "%s\n", contracts::kFrameCsvHeader);
    return true;
}

void FrameCsv::write(const contracts::FrameMetrics& m) {
    if (!file_) return;
    std::fprintf(file_, "%llu,%.4f,%llu,%llu,%llu\n",
                 static_cast<unsigned long long>(m.frame),
                 m.frame_ms,
                 static_cast<unsigned long long>(m.resident_chunks),
                 static_cast<unsigned long long>(m.generated_total),
                 static_cast<unsigned long long>(m.mem_bytes));
}

void FrameCsv::close() {
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

}  // namespace br::telemetry
