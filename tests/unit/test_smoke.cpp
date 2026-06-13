// test_smoke — proves the full module DAG links and reports stable identity.
#include <catch2/catch_test_macros.hpp>

#include <cstring>

#include "core/version.h"
#include "gen/gen.h"
#include "stream/stream.h"
#include "telemetry/telemetry.h"
#include "audio/audio.h"
#include "render_d3d12/render_d3d12.h"
#include "render_dxr/render_dxr.h"
#include "director/director.h"

TEST_CASE("core version banner is stable", "[smoke]") {
    REQUIRE(std::strcmp(br::core::core_version(), "0.0.0") == 0);
    REQUIRE(br::core::kVersionMajor == 0u);
    REQUIRE(br::core::kVersionMinor == 0u);
    REQUIRE(br::core::kVersionPatch == 0u);
}

TEST_CASE("every module links and reports its canonical identity", "[smoke][modules]") {
    REQUIRE(std::strcmp(br::gen::module_name(), "gen") == 0);
    REQUIRE(std::strcmp(br::stream::module_name(), "stream") == 0);
    REQUIRE(std::strcmp(br::telemetry::module_name(), "telemetry") == 0);
    REQUIRE(std::strcmp(br::audio::module_name(), "audio") == 0);
    REQUIRE(std::strcmp(br::render_d3d12::module_name(), "render_d3d12") == 0);
    REQUIRE(std::strcmp(br::render_dxr::module_name(), "render_dxr") == 0);
    REQUIRE(std::strcmp(br::director::module_name(), "director") == 0);
}
