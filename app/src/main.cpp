// app/main.cpp — composition root. M0 stub: prints the build banner, proves the
// whole module DAG links, and exits 0. The Win32 window, D3D12 device, and the
// real CLI flags (--headless / --replay / --no-director / --render-wav) arrive
// in M1+. No globals outside this composition root (CLAUDE.md Code standards).
#include <cstdio>
#include <cstring>

#include "core/version.h"
#include "core/rng.h"
#include "gen/gen.h"
#include "stream/stream.h"
#include "telemetry/telemetry.h"
#include "audio/audio.h"
#include "render_d3d12/render_d3d12.h"
#include "render_dxr/render_dxr.h"
#include "director/director.h"

int main(int argc, char** argv) {
    std::printf("Backrooms Sim v%s\n", br::core::core_version());

    // Prove the deterministic core RNG is wired and seedable (INV-1).
    br::core::Pcg64 rng(0x00B4C1200DULL);
    std::printf("seed-probe: %016llx\n",
                static_cast<unsigned long long>(rng.next_u64()));

    // Prove every module in the DAG is linked (ARCHITECTURE.md §4).
    std::printf("modules: %s %s %s %s %s %s %s\n",
                br::gen::module_name(), br::stream::module_name(),
                br::telemetry::module_name(), br::audio::module_name(),
                br::render_d3d12::module_name(), br::render_dxr::module_name(),
                br::director::module_name());

    // M0 has no behavioural flags yet; echo for visibility/headless plumbing.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("%s\n", br::core::core_version());
            return 0;
        }
    }
    return 0;
}
