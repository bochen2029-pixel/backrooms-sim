# Backrooms Sim

Infinite, never-repeating, procedurally generated **Backrooms walking
simulation**. Native Windows, C++20, D3D12 + DXR, with a local-LLM Director.
A demonstration/visualization — no win state, no combat, no asset files
(everything is procedural).

Built autonomously, milestone by milestone, with machine-checkable gates.
Canon: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). Build sequence:
[`docs/MILESTONES.md`](docs/MILESTONES.md). Where the build stands:
[`docs/SESSION_LOG.md`](docs/SESSION_LOG.md).

## Prerequisites

- Windows 10/11 x64
- Visual Studio 2022 (Desktop development with C++) + Windows 11 SDK (DXR)
- CMake ≥ 3.28, Ninja (the VS-bundled copies are fine)
- Git (for vcpkg bootstrap). NVIDIA RTX GPU for the path-traced mode (M9+).

`scripts/build.ps1` bootstraps **vcpkg** automatically (to `C:\vcpkg` or
`$env:VCPKG_ROOT`) and imports the MSVC dev environment, so a fresh clone needs
no manual setup beyond the toolchain above.

## Build, test, gate

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1            # incremental build
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -Clean     # clean build
ctest --test-dir build --output-on-failure                                       # run tests
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gate.ps1 -Milestone M0   # milestone gate
```

A milestone is done only when `gate.ps1 -Milestone M<N>` exits 0.

## Run

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run.ps1          # build + a lit smoke render -> runs/run-smoke.png
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run.ps1 -Window  # the windowed walking sim
```

Everything is procedural — no asset files; geometry, materials, audio, and lighting
are all generated from a seed at runtime. Highlights:

- **Path-traced mode (DXR):** `backrooms --dxr-pt --pose P --spp N --out shot.png`
- **The noclip intro:** `backrooms --intro` (mundane room → fall-through → Level 0)
- **8 h walk-bot soak:** `scripts/soak.ps1 -Hours 8`
- **Framed captures ("photo mode"):** `--shot` / `--dxr-pt` / `--topdown` write
  deterministic PNGs. Configuration is the CLI flag surface (see `app/MODULE.md`).

### The Director (optional local LLM)

The ambient **Director** routes to a local **KEEL** sidecar (OpenAI-compatible HTTP)
for inference — no model is bundled. Start the sidecar, then add `--director`:

```powershell
scripts/soak.ps1 -Hours 8 -Director     # acceptance soak with the Director ON
backrooms --director-probe              # one-shot: a WandererSummary -> a directive
```

The Director is **enhancement-only** (INV-6): `--no-director` (the default) runs the
full sim with no LLM. Determinism is preserved — the model's output enters the sim
only as a recorded event log, so a replay is **bit-identical with the model offline**.

## Layout

`core` `gen` `stream` `render_d3d12` `render_dxr` `audio` `telemetry`
`director` `app` `tools` — see each module's `MODULE.md`. Dependency arrows
point downward only; `core` depends on nothing.
