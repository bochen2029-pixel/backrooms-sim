#include "stream/stream_manager.h"

#include <chrono>
#include <utility>

namespace br::stream {

using contracts::ChunkData;
using contracts::ChunkKey;

StreamManager::StreamManager(uint64_t world_seed, int radius, unsigned worker_count)
    : seed_(world_seed), radius_(radius) {
    if (worker_count == 0) worker_count = 1;
    for (unsigned i = 0; i < worker_count; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

StreamManager::~StreamManager() {
    stop_.store(true);
    req_cv_.notify_all();
    for (std::thread& t : workers_) {
        if (t.joinable()) t.join();
    }
}

bool StreamManager::in_ring(ChunkKey c, ChunkKey center) const {
    if (c.level != center.level) return false;
    int64_t dx = c.cx - center.cx;
    int64_t dz = c.cz - center.cz;
    if (dx < 0) dx = -dx;
    if (dz < 0) dz = -dz;
    return dx <= radius_ && dz <= radius_;
}

void StreamManager::enqueue(ChunkKey k) {
    {
        std::lock_guard<std::mutex> lk(req_mtx_);
        requests_.push_back(k);
    }
    in_flight_.insert(k);
    req_cv_.notify_one();
}

void StreamManager::worker_loop() {
    for (;;) {
        ChunkKey k;
        {
            std::unique_lock<std::mutex> lk(req_mtx_);
            req_cv_.wait(lk, [this] { return stop_.load() || !requests_.empty(); });
            if (stop_.load() && requests_.empty()) return;
            k = requests_.front();
            requests_.pop_front();
        }
        ChunkData data = contracts::GenerateChunk(seed_, k);
        {
            std::lock_guard<std::mutex> lk(ready_mtx_);
            ready_.push_back(std::move(data));
        }
        ready_cv_.notify_one();
    }
}

void StreamManager::collect() {
    std::vector<ChunkData> got;
    {
        std::lock_guard<std::mutex> lk(ready_mtx_);
        got.swap(ready_);
    }
    for (ChunkData& d : got) {
        in_flight_.erase(d.key);
        ++generated_total_;
        resident_[d.key] = std::move(d);
    }
}

void StreamManager::update(ChunkKey center) {
    for (int64_t dx = -radius_; dx <= radius_; ++dx) {
        for (int64_t dz = -radius_; dz <= radius_; ++dz) {
            const ChunkKey k{center.level, center.cx + dx, center.cz + dz};
            if (resident_.find(k) == resident_.end() &&
                in_flight_.find(k) == in_flight_.end()) {
                enqueue(k);
            }
        }
    }
    collect();
    for (auto it = resident_.begin(); it != resident_.end();) {
        if (!in_ring(it->first, center)) {
            it = resident_.erase(it);
        } else {
            ++it;
        }
    }
}

void StreamManager::wait_idle() {
    while (!in_flight_.empty()) {
        collect();
        if (in_flight_.empty()) break;
        std::unique_lock<std::mutex> lk(ready_mtx_);
        ready_cv_.wait_for(lk, std::chrono::milliseconds(5),
                           [this] { return !ready_.empty(); });
    }
}

std::vector<contracts::ResidentChunk> StreamManager::resident() const {
    std::vector<contracts::ResidentChunk> out;
    out.reserve(resident_.size());
    for (const auto& kv : resident_) {
        contracts::ResidentChunk rc;
        rc.key = kv.first;
        rc.vertices = kv.second.vertices.data();
        rc.vertex_count = static_cast<uint32_t>(kv.second.vertices.size());
        out.push_back(rc);
    }
    return out;
}

}  // namespace br::stream
