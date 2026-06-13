#pragma once
// stream/stream.h — chunk ring management, worker pool, residency events.
// M0 stub: identity only. Ring + background generation + GPU-upload handoff
// arrive in M3 via contracts/stream_events_v1.h.
namespace br::stream {
const char* module_name() noexcept;
}  // namespace br::stream
