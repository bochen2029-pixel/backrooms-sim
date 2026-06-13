#pragma once
//
// contracts/geometry_v1.h — shared primitive geometry types used across
// boundaries (world_view, chunk_gen). Header-only, dependency-free.
//
namespace br::contracts {

// A solid axis-aligned box in world space (render decoration or collision wall).
struct BoxInstance {
    float mn[3];
    float mx[3];
};

}  // namespace br::contracts
