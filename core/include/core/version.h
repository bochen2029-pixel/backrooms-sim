#pragma once
//
// core/version.h — build/version identity for the sim core. Kept in the
// dependency-free L1 core so every module and tool can report a consistent
// version banner without reaching upward.
//
#include <cstdint>

namespace br::core {

// Semantic version of the simulation, mirroring the CMake project() version.
const char* core_version() noexcept;

// Compile-time version triple.
constexpr uint32_t kVersionMajor = 0;
constexpr uint32_t kVersionMinor = 0;
constexpr uint32_t kVersionPatch = 0;

}  // namespace br::core
