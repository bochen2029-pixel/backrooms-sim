# CLAUDE.md — Backrooms Sim (C:\backrooms)

Infinite, never-repeating, procedurally generated Backrooms walking simulation. Native Windows, C++20, D3D12 + DXR, local LLM Director via llama.cpp. Built autonomously, milestone by milestone, with machine-checkable gates. **This is a demonstration/visualization, not a game** — no win state, no combat, no asset files.

## Session start ritual (every session)

1. Read `docs/ARCHITECTURE.md` (canon) and the **active milestone section only** of `docs/MILESTONES.md`.
2. Read the latest entry of `docs/SESSION_LOG.md` — it is the source of truth for where the build stands.
3. Load the target module's MODULE.md + its contracts. Do **not** load the whole repo.
4. Produce a change manifest (files to touch, contract changes y/n, tests to add, rollback plan, diff budget) **before writing code**.

## Commands

```bash
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1            # incremental build
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -Clean     # clean build
ctest --test-dir build --output-on-failure                                           # all tests
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/gate.ps1 -Milestone M3   # milestone gate
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/soak.ps1 -Hours 8        # walk-bot soak
```

(These scripts are created in M0. Until then, bootstrap them as part of M0's scope.)

## Iron Rules

1. **Gate runner is law.** A milestone is done only when `gate.ps1 M<N>` exits 0. No green, no merge.
2. **Tag on green, push, revert on regression.** Green gate → `git tag m<N>-green` → push branch + tags to remote (this is the project's backup). If a change breaks an earlier gate and two fix attempts fail, revert to the last green tag and re-approach. Never debug forward from a broken state.
3. **Headless first.** Every feature ships with a headless verification path (offscreen render, replay hash, telemetry, offline WAV) before visual polish.
4. **Determinism is sacred.** INV-1..INV-8 in ARCHITECTURE.md §3.1 override convenience. Sim core: `/fp:strict`, no wall-clock, seeded PCG64 only, zero render/audio/director includes (CI grep-gated).
5. **Diff budget ≤ 400 LOC** per change unless the milestone scope authorizes more. Resist scope creep — no new features, dependencies, or "while I'm here" refactors outside the manifest.
6. **Never edit `/goldens` by hand** and never relax a gate threshold to pass. Goldens change only via `tools/goldgen` plus a DECISIONS.md entry in the same commit. If a gate seems wrong, fix the gate with an ADR — don't game it.
7. **Spec reconciliation in the same commit.** Contract or boundary change → update ARCHITECTURE.md / MODULE.md in that commit. Module inventory must match the directory listing (CI check).
8. **New dependency = ADR.** No library enters vcpkg.json without a DECISIONS.md entry.
9. **One milestone per session.** End every session by running the gate, then writing a SESSION_LOG.md entry: done / pending / open questions / gotchas.

## Module map (details in each MODULE.md)

`core` deterministic sim (tick, RNG, collision, WorldState hash) · `gen` chunk generation + validators · `stream` chunk ring + workers · `render_d3d12` raster + procedural textures + VHS post · `render_dxr` path-traced mode · `audio` procedural synth + room-probe reverb · `director` llama.cpp host + schema-validated Directives · `telemetry` metrics + minidumps · `app` Win32 shell + flags (`--headless`, `--replay`, `--no-director`, `--render-wav`) · `tools` hashdiff/goldgen/walkbot/contactsheet/wavcheck.

Dependency arrows point downward only; `core` depends on nothing.

## Code standards

C++20, MSVC, warnings-as-errors. Errors cross module boundaries as typed results, never exceptions. No globals outside `app` composition root. Glossary terms from ARCHITECTURE.md §2 are mandatory — forbidden synonyms (player, tile, theme…) fail review. Headers self-contained; includes ordered local→project→system.

## Verification etiquette

- Hooks auto-run a quick build+test after edits (`.claude/settings.json`, activated in M0). If the hook fails, fix before doing anything else.
- Long operations (soaks, converged PT goldens) run via `scripts/soak.ps1` — start them, then verify telemetry output, don't sit idle.
- D3D12 debug layer output is a gate: zero errors or warnings, always. Never disable the debug layer to pass.
- Walk-bot getting stuck, sealed rooms, seam cracks, or determinism hash drift are **always** generator/sim bugs — never "tune around" them.

## What NOT to do

No game mechanics, no networking, no asset files (everything procedural), no browser tech, no Vulkan/OpenGL paths, no second windowing framework, no skipping the manifest step, no loading the entire repo into context.
