#pragma once
//
// stream/stream_manager.h — chunk ring with a background worker pool.
//
// A square ring of (2*radius+1)^2 chunks around a moving center. Missing chunks
// are generated on worker threads (gen::GenerateChunk is pure, INV-2); the main
// thread collects results and evicts chunks that leave the ring. Memory is
// bounded by the ring (INV-4); streaming is fully decoupled from the sim so it
// cannot affect determinism (INV-1).
//
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "contracts/chunk_gen_v1.h"
#include "contracts/stream_events_v1.h"

namespace br::stream {

class StreamManager {
public:
    StreamManager(uint64_t world_seed, int radius, unsigned worker_count);
    ~StreamManager();
    StreamManager(const StreamManager&) = delete;
    StreamManager& operator=(const StreamManager&) = delete;

    // Per frame (main thread): enqueue missing ring chunks, collect generated
    // ones, evict chunks outside the ring around `center`.
    void update(contracts::ChunkKey center);

    // M28: also keep one ADJACENT level resident -- the floor reached through stair
    // holes -- so a vertical climb crosses the seam and holes see through to the next
    // floor. Two concentric rings at (center.level) and (extra_level). Presentation
    // only (INV-1); residency stays bounded by <= 2*(2r+1)^2 (INV-4). Passing
    // extra_level == center.level is exactly the single-ring behaviour above.
    void update(contracts::ChunkKey center, int32_t extra_level);

    // Block until all in-flight generation has completed and been collected.
    void wait_idle();

    // Read-only resident snapshot (valid until the next update()).
    std::vector<contracts::ResidentChunk> resident() const;

    size_t resident_count() const { return resident_.size(); }
    size_t pending_count() const { return in_flight_.size(); }
    uint64_t generated_total() const { return generated_total_; }

private:
    void worker_loop();
    void enqueue(contracts::ChunkKey k);
    void collect();
    bool in_ring(contracts::ChunkKey c, contracts::ChunkKey center, int32_t extra_level) const;

    uint64_t seed_;
    int radius_;

    std::vector<std::thread> workers_;
    std::atomic<bool> stop_{false};

    std::mutex req_mtx_;
    std::condition_variable req_cv_;
    std::deque<contracts::ChunkKey> requests_;

    std::mutex ready_mtx_;
    std::condition_variable ready_cv_;
    std::vector<contracts::ChunkData> ready_;

    // Main-thread-only state.
    std::map<contracts::ChunkKey, contracts::ChunkData> resident_;
    std::set<contracts::ChunkKey> in_flight_;
    uint64_t generated_total_ = 0;
};

}  // namespace br::stream
