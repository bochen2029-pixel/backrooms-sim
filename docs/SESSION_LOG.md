# SESSION_LOG.md

Newest entry first. Every session appends: done / pending / open questions / gotchas.

---

## Session 2 — M1: Window, D3D12 Device, Headless Mode  ✅ gate green (`m1-green`)

**Done.**
- `render_d3d12/renderer.h` + `renderer.cpp`: opaque-PIMPL `Renderer`. DXGI
  factory, adapter selection (prefers the RTX 4070 Ti SUPER via
  `EnumAdapterByGpuPreference`, WARP fallback), D3D12 device, **debug layer +
  InfoQueue + DRED** (auto-breadcrumbs + page-fault), command queue/allocator/
  list, fence sync. Headless: offscreen R8G8B8A8 RT (optimized clear value
  matches the issued clear), copy → readback buffer with `GetCopyableFootprints`
  row-pitch handling → tight CPU RGBA. Windowed: flip-discard swapchain, present.
  No D3D12/DXGI/`<windows.h>` leaks through the header (INV-5 holds).
- `app`: CLI `--headless/--window`, `--frames N | --seconds S`, `--out PNG`,
  `--width/--height`, `--version`. Creates the Win32 window (no focus-steal),
  writes PNG via the shared `br_stb`, reports `debug_error_count` + memory.
- Gate `Invoke-GateM1`: clean build (zero warnings); ctest regression; **frame-0
  PNG bit-identical across 3 runs**; matches committed golden; **zero D3D12
  debug-layer messages** (headless + 10 s windowed run); **60 s memory soak**
  (372,750 frames, +1.6 MB private bytes → flat, no fence timeouts). Plus the
  standing INV-5 + inventory checks. `gate.ps1 -Milestone M1` exits 0; M0
  regression sweep still green.
- Golden `goldens/m1/frame0_320x180.png` (clear RGBA 46,43,33,255; hash
  `65e8578815ec303c`) via `goldgen capture`. ADR-014 (private-bytes soak metric)
  + ADR-015 (golden), reconciled into ARCHITECTURE.md §8.

**Pending.** M2 — `/core` standalone lib: fixed 120 Hz tick, seeded RNG already
present, first-person walk camera, capsule-vs-AABB collision + sliding, **input
replay** (record/playback), per-tick WorldState hash. The replay system is the
enabler for every later automated movement test.

**Open questions.** None blocking. PT/DXR (M9) will reuse this device; the
renderer is structured to add a second (DXR) path without touching the sim.

**Gotchas / notes for the next session.**
- The active PostToolUse hook rebuilds+tests after every Edit/Write; during
  multi-file features intermediate states fail it harmlessly — push through, the
  final state is green. (Real verification is the explicit `build.ps1`/gate runs.)
- PowerShell **StrictMode** is on in `common.ps1`: `.Count` on a scalar throws —
  wrap pipeline results in `@(...)` before `.Count` (bit us once in the gate).
- D3D12 readback **must** honor the 256-byte row-pitch alignment
  (`GetCopyableFootprints`); the renderer copies row-by-row into tight RGBA.
- Windowed gate run opens a 10 s window (`SW_SHOWNOACTIVATE`, no focus steal).
- Clear color is fixed/deterministic; changing it = new golden + ADR (INV-8).

---

## Session 1 — M0: Scaffold + Verification Harness  ✅ gate green (`m0-green`)

**Done.**
- CMake/Ninja/vcpkg skeleton (`CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`)
  matching the full 10-module inventory with correct dependency arrows; static
  `x64-windows-static` triplet, `/W4 /WX /permissive-`, `/fp:strict` on `core`+`gen`.
- `core`: real **PCG64** (XSL-RR 128/64) seeded RNG — the determinism oracle
  (INV-1). Portable 128-bit math, locked output-vector regression test.
- Stub libs for `gen, stream, telemetry, audio, render_d3d12, render_dxr,
  director` + `app` console exe (links the whole DAG). `MODULE.md` for all 10.
- Tools: `hashdiff` (image hash + mean-abs-diff, stb) and `goldgen`
  (deterministic synth via core RNG + golden capture; sole `/goldens` writer).
- Catch2 under CTest: seed/determinism/statistical tests + smoke link-check;
  `gate_canary` (deliberately failing, DISABLED) for the test-the-gate proof.
- Scripts: `lib/common.ps1` (VS dev-env import + Ninja-on-PATH + vcpkg discovery),
  `build.ps1`, `quickcheck.ps1` (exit 2 on fail), `precommit.ps1`,
  `install-hooks.ps1`, `gate.ps1` (M0 dispatch), `soak.ps1` (stub), and
  invariant checks `check_core_isolation.ps1` (INV-5) + `check_inventory.ps1`.
- Activated `.claude/settings.json` (PostToolUse → quickcheck). git pre-commit
  hook installed. ADR-010..013 recorded + reconciled into ARCHITECTURE.md §8.
- **`scripts/gate.ps1 -Milestone M0` exits 0.** Clean build, 10/10 tests,
  hashdiff round-trip, canary-nonzero, hook present, INV-5, inventory — all green.

**Pending.** M1 — Win32 window, D3D12 device/swapchain, debug layer + DRED,
`--headless --frames N --out` offscreen PNG dump, frame-0 golden.

**Open questions.** Remote backup: no `origin` was configured in the starter
(`<your-remote-url>` placeholder). Tagged `m0-green` locally; push deferred until
a remote exists (see below).

**Gotchas / notes for the next session.**
- Scripts run under **Windows PowerShell 5.1** (`powershell.exe`), not pwsh 7 —
  no ternary/`??`/`&&` in `scripts/*.ps1`.
- Ninja is **not** globally installed; `common.ps1` prepends the VS-bundled Ninja
  to PATH. The MSVC env is imported via `vcvars64.bat` inside `Enter-VsDevEnv`.
- vcpkg lives at `C:\vcpkg` (baseline pinned in `vcpkg.json`); a fresh clone
  self-bootstraps into `extern/vcpkg`.
- `files/` and `*.zip` (the redundant starter archives) are gitignored.
- The PostToolUse hook now builds+tests after every Edit/Write; expect a few
  seconds per edit. Intermediate multi-file states may transiently fail it.

---

## Session 0 — (starter created)
- Done: canon documents (ARCHITECTURE.md, MILESTONES.md), CLAUDE.md, kickoff guide, hooks template.
- Pending: M0 — scaffold + verification harness.
- Gotchas: activate .claude/settings.json only after scripts/quickcheck.ps1 exists (see KICKOFF_PROMPT.md).
