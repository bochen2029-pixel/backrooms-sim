#pragma once
//
// contracts/world_view_v1.h — core -> renderers read-only snapshot (v1.0).
//
// Immutable per frame. The renderer consumes a WorldView and never reaches back
// into sim state (INV-5). M2 carries the camera pose and the hardcoded test-room
// geometry; resident chunks and lights are added (additively) as streaming and
// lighting come online (M3, M5).
//
#include <cstdint>
#include <span>

#include "contracts/geometry_v1.h"

namespace br::contracts {

struct CameraPose {
    float pos[3];     // world-space eye position
    float yaw;        // radians; 0 looks toward +Z
    float pitch;      // radians; + looks up, clamped by the sim
    float fov_y;      // vertical field of view, radians
    float aspect;     // width / height
};

struct WorldView {
    CameraPose camera;
    std::span<const BoxInstance> boxes;
    uint64_t tick = 0;
};

}  // namespace br::contracts
