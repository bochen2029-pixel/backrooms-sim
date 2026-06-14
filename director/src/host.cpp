#include "director/host.h"

#include "director/director.h"
#include "director/keel_client.h"

#include "contracts/replay_v1.h"

#include <cstdio>
#include <utility>

namespace br::director {

std::optional<contracts::Directive> request_directive(const std::string& host, int port,
                                                      const contracts::WandererSummary& summary,
                                                      uint32_t timeout_ms) {
    const std::string prompt = render_prompt(summary);
    const KeelResponse resp = keel_complete(host, port, prompt, timeout_ms);
    if (!resp.ok) return std::nullopt;
    const DirectiveResult vr = validate_directive(resp.content);
    if (!vr.ok) return std::nullopt;
    return vr.directive;
}

bool write_director_log(const std::string& path, uint64_t world_seed, uint64_t run_ticks,
                        const std::vector<contracts::DirectorEvent>& events) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    contracts::DirectorLogHeader h{};
    h.magic = contracts::kDirectorLogMagic;
    h.version = contracts::kDirectorLogVersion;
    h.world_seed = world_seed;
    h.run_ticks = run_ticks;
    h.event_count = static_cast<uint64_t>(events.size());
    bool ok = std::fwrite(&h, sizeof(h), 1, f) == 1;
    if (ok && !events.empty()) {
        ok = std::fwrite(events.data(), sizeof(contracts::DirectorEvent), events.size(), f) == events.size();
    }
    std::fclose(f);
    return ok;
}

bool read_director_log(const std::string& path, uint64_t& world_seed, uint64_t& run_ticks,
                       std::vector<contracts::DirectorEvent>& events) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    contracts::DirectorLogHeader h{};
    bool ok = std::fread(&h, sizeof(h), 1, f) == 1;
    if (ok && (h.magic != contracts::kDirectorLogMagic || h.version != contracts::kDirectorLogVersion)) ok = false;
    if (ok) {
        world_seed = h.world_seed;
        run_ticks = h.run_ticks;
        events.resize(static_cast<size_t>(h.event_count));
        if (h.event_count > 0) {
            ok = std::fread(events.data(), sizeof(contracts::DirectorEvent), events.size(), f) == events.size();
        }
    }
    std::fclose(f);
    return ok;
}

DirectorHost::DirectorHost(std::string host, int port, uint32_t timeout_ms)
    : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms) {
    thread_ = std::thread(&DirectorHost::worker_loop, this);
}

DirectorHost::~DirectorHost() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stop_ = true;
    }
    cv_.notify_one();
    if (thread_.joinable()) thread_.join();
}

void DirectorHost::submit(const contracts::WandererSummary& summary) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        pending_ = summary;     // latest-wins: a newer summary supersedes a queued one
        have_pending_ = true;
    }
    cv_.notify_one();
}

std::vector<contracts::Directive> DirectorHost::poll() {
    std::vector<contracts::Directive> out;
    std::lock_guard<std::mutex> lk(mtx_);
    out.swap(ready_);
    return out;
}

void DirectorHost::worker_loop() {
    for (;;) {
        contracts::WandererSummary s;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [&] { return stop_ || have_pending_; });
            if (stop_) return;
            s = pending_;
            have_pending_ = false;
        }
        requests_.fetch_add(1);
        const std::optional<contracts::Directive> d = request_directive(host_, port_, s, timeout_ms_);
        if (d) {
            produced_.fetch_add(1);
            std::lock_guard<std::mutex> lk(mtx_);
            ready_.push_back(*d);
        }
    }
}

}  // namespace br::director
