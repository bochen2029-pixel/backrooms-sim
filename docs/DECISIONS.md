# DECISIONS.md

Full ADRs. Summaries live in ARCHITECTURE.md \xc2\xa78. Format: ADR-NNN / Status / Context / Decision / Consequences.

## ADR-001 .. ADR-009
See ARCHITECTURE.md §8 for the accepted set; expand each here as the build touches it. New dependencies, golden updates, and gate-threshold changes REQUIRE a new ADR entry in the same commit (Iron Rules 6 and 8).

## ADR-010 — vcpkg acquisition: pinned baseline + self-bootstrapping location
- **Status:** Accepted (M0).
- **Context:** ADR-007 chose vcpkg manifest mode. A fresh clone must resolve deps (Catch2, stb) with no manual setup, reproducibly.
- **Decision:** `vcpkg.json` pins `builtin-baseline` to `d592849579fb1fb22f87406b2184522ea21a8783`. `scripts/lib/common.ps1::Ensure-Vcpkg` discovers vcpkg in order `$env:VCPKG_ROOT` → `C:\vcpkg` → repo `extern/vcpkg`, cloning+bootstrapping (shallow) into `extern/vcpkg` (gitignored) when none exists. The dev machine uses `C:\vcpkg`.
- **Consequences:** Fresh clone self-bootstraps; vcpkg is never vendored in git; the baseline pin makes dependency versions reproducible (bumping it is a deliberate edit + ADR).

## ADR-011 — x64-windows-static triplet and static CRT
- **Status:** Accepted (M0).
- **Context:** The gate scripts run `app`, `tools`, and `tests` executables directly from `build/bin` and must not depend on DLL deployment.
- **Decision:** `VCPKG_TARGET_TRIPLET = x64-windows-static`; `CMAKE_MSVC_RUNTIME_LIBRARY = MultiThreaded$<$<CONFIG:Debug>:Debug>` (`/MT`). Third-party (Catch2, stb) and our code all link the static CRT.
- **Consequences:** Self-contained executables, no DLL-copy step, simpler gates; slightly larger binaries; CRT-mismatch is structurally impossible.

## ADR-012 — "test-the-gate" canary mechanism
- **Status:** Accepted (M0).
- **Context:** The M0 exit gate must prove a deliberately failing test is detected (would block a commit) without making the normal `ctest` run red.
- **Decision:** `gate_canary` is a Catch2 executable containing a `FAIL`, always compiled, registered in CTest as `DISABLED`. `scripts/gate.ps1` runs the binary directly and asserts a nonzero exit; the git `pre-commit` hook runs `quickcheck` (build + tests) so any real failing test blocks commits.
- **Consequences:** The normal suite stays green (canary disabled); the gate still mechanically proves failure detection; no reconfigure/second build dir needed.

## ADR-013 — warnings-as-errors scoping via external-header flags
- **Status:** Accepted (M0).
- **Context:** `/W4 /WX` on third-party headers (Catch2, stb) would fail the build on warnings we do not own.
- **Decision:** Strict flags live on the `backrooms_flags` INTERFACE target (our code only). Angle-bracket/`SYSTEM` includes are demoted with `/external:anglebrackets /external:W0`; the stb implementation TU is isolated in `br_stb` and compiled `/w`. The deterministic sim core additionally gets `/fp:strict` via `backrooms_sim_flags`.
- **Consequences:** `/WX` protects our code without third-party noise; "zero warnings-as-errors violations" is enforced by a clean build, not a grep.

## ADR-014 — M1 memory-soak metric: process private bytes, not the CRT debug heap
- **Status:** Accepted (M1).
- **Context:** The M1 gate wording is "60 s soak: stable memory (CRT debug heap delta ≈ 0)". We ship the **release** static CRT (`/MT`, ADR-011), which has no CRT debug heap, so that exact metric is unavailable without a separate debug-CRT build.
- **Decision:** Measure process **PrivateUsage** via `GetProcessMemoryInfo` (`PROCESS_MEMORY_COUNTERS_EX`) at soak start vs end and assert the delta `< 16 MiB` over the 60 s headless soak. "No fence timeouts" is enforced structurally: the renderer returns failure (nonzero process exit) if any fence wait times out.
- **Consequences:** Build-config-independent leak detection that preserves the gate's intent (flat memory, zero slope). Empirically ~1.5 MB over 62k frames in 10 s — warmup-dominated, no per-frame growth. A real leak in the tight render loop would be hundreds of MB and trip the gate immediately.

## ADR-015 — M1 frame-0 golden
- **Status:** Accepted (M1).
- **Context:** The M1 gate requires a committed headless frame-0 golden, bit-identical across runs.
- **Decision:** `goldens/m1/frame0_320x180.png`, captured via `goldgen capture` from the headless renderer at 320×180. The frame is the deterministic clear color RGBA (46, 43, 33, 255); FNV-1a content hash `65e8578815ec303c`; verified bit-identical across 3 consecutive runs on the dev GPU (RTX 4070 Ti SUPER).
- **Consequences:** Golden committed alongside this ADR (INV-8). Any future renderer change that alters the clear output requires a deliberate `goldgen` update plus a new ADR in the same commit.
