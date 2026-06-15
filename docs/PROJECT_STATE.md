# PROJECT STATE — Backrooms Sim (snapshot 2026-06-15)

A current-reality snapshot of the project + codebase. Canon design lives in `docs/ARCHITECTURE.md`;
the plan in `docs/MILESTONES.md`; the decision log in `docs/DECISIONS.md`; the running narrative in
`docs/SESSION_LOG.md`; QC findings in `docs/AUDIT_2026-06-15.md`. This doc is the "you are here."

---

## 1 · What it is
An **infinite, never-repeating, procedurally generated Backrooms walking simulation** — native Windows,
C++20, D3D12 + DXR, with an optional local-LLM "Director" / AI "Shoggoth" via llama.cpp/KEEL. It is a
**demonstration/visualization, not a game** in the mechanics sense: no win state, no combat, no asset
files — *everything* is procedural (geometry, textures, audio, even the PA voice). Built autonomously,
milestone by milestone, behind machine-checkable gates.

**Release lineage:**
- **v1.0** — the headless visualization (M0–M12): infinite procedural Backrooms, raster + DXR path tracer,
  procedural audio, 8 h-soak-hardened streaming, optional KEEL Director, bit-exact replay.
- **v2.0** — the playable, portable game (M13–M17): real-time windowed walk, real-time audio, menus,
  persistent settings/fullscreen/gamepad, portable no-SDK `.zip`.
- **v2.1** — procedural app-icon polish.
- **Phase III** (M18–M25) — the living Backrooms: head-bob/run, a ray-tracing toggle, and the full
  Shoggoth sensory arc (body, BFS navigation, live KEEL brain, vision, hearing, procedural-TTS PA voice,
  body in the ray-traced path).
- **Phase IV** (M26–M30) — **the Vertical Backrooms** (this phase): infinite stacked floors in Z, joined
  by stairs and open shafts. **M26/M27/M28/M30 gate-green + tagged; M29 code-complete + determinism-proven,
  its gate model-blocked** (needs the KEEL `llama-server :8080`; ROADMAP §5 ISSUE-5).

## 2 · Snapshot stats
| Metric | Value |
|---|---|
| Total commits | 112 |
| `m<N>-green` tags | 30 (M0–M30; M29 pending; M12/M17 = the v1.0/v2.0 caps) |
| Source (LOC, .h/.cpp) | ~12,600 across 11 modules + ~1,600 in tests |
| Unit test cases (Catch2) | 100 (99 runnable + a disabled gate canary), all green |
| Goldens (PNG) | 34 (m1,m2,m4,m5,m7,m8,m9,m15) |
| ADRs (DECISIONS.md) | 48 entries (latest ADR-056) |
| Backup remote | `github.com/bochen2029-pixel/backrooms-sim` (branch + tags pushed) |

## 3 · Architecture (modules + dependency)
Dependency arrows point **downward only**; `core` depends on nothing. Errors cross module boundaries as
**typed results, never exceptions**. No globals outside the `app` composition root.

| Module | LOC | Role |
|---|---|---|
| `core` | 592 | deterministic sim: tick, PCG64 RNG, swept-AABB collision (+ M27 step-up), WorldState + `world_state_hash`, level math |
| `gen` | 669 | chunk generation + validators: maze layout, biome, geometry, `stair_at`/`shaft_at`, connectivity validators |
| `stream` | 219 | chunk ring + worker pool: `StreamManager` (single / 2-level / range residency), eviction |
| `render_d3d12` | 2203 | raster + procedural textures + VHS post + HUD compositing |
| `render_dxr` | 1293 | DXR path-traced mode: BLAS/TLAS, dynamic creature BLAS |
| `audio` | 740 | procedural synth + room-probe reverb + footsteps + formant TTS |
| `director` | 680 | llama.cpp/KEEL host + schema-validated Directives + shoggoth brain |
| `telemetry` | 134 | metrics + minidumps + frame CSV (observational only) |
| `app` | 4946 | Win32 shell + flags + run_* modes + menu/config/hud + the Shoggoth + Phase IV modes |
| `tools` | 554 | hashdiff / goldgen / walkbot / contactsheet / wavcheck / icongen |
| `contracts` | 300 | cross-module typed boundaries (versioned headers: `chunk_gen_v1.h`, `stream_events_v1.h`, …) |

## 4 · The determinism model (the load-bearing invariant)
INV-1..8 (ARCHITECTURE.md §3.1) are sacred. In practice:
- The **sim core** is `/fp:strict`, uses a **seeded PCG64** only, has **no wall-clock**, and a **CI grep
  gate** (`scripts/checks/check_core_isolation.ps1`) enforces **zero render/audio/director includes**.
- `world_state_hash` is a **pure fold of WorldState** — a function of (seed, input, event-log) alone.
- Generation is **pure/total per `ChunkKey`** — same (seed, key) → byte-identical chunk.
- **The AI never breaks determinism.** The LLM brain runs only at *record* time; its validated intent is
  written to an **event log at a deterministic effective-tick**; *replay* re-applies the log with the
  **model fully offline** and reproduces the run **bit-for-bit** (the M21 "sacred gate", extended through
  vision/hearing/voice/verticality). This is the single most-tested property in the project.

## 5 · Phase IV — the Vertical Backrooms (M26–M30)
The infinite 2-D plane became infinite **stacked floors** in Z, kept **level-implicit** (the floor is
derived from `pos.y` via `level_from_y`; `world_state_hash` is unchanged → every pre-existing replay +
the level-0 goldens stay byte-identical).
- **M26** — live multi-level (per-level ground/chunks/lights; `--shot --level N`).
- **M27** — procedural stairs: `stair_at` (density + 4×4 backstop), aligned floor/ceiling holes read from
  the same shared-seam hash, a climbable riser-slab stairwell, **step-up locomotion**, a vertical-
  connectivity validator. `--ascend` climbs to level 1. (ADR-053)
- **M28** — vertical streaming + see-through: a 2nd resident ring at one adjacent floor; you see through
  holes in **both** the raster and DXR paths (verts baked at world-Y, cache keyed by full `ChunkKey`).
  (ADR-054)
- **M29** — per-floor Shoggoth: level-confined nav, per-`(seed,level)`, escape across a seam; the sacred
  record→replay holds across a descent **model-free**. **Gate model-blocked** on `llama-server :8080`
  (ISSUE-5) — the only Phase IV gate not yet green. (per SESSION_LOG/ROADMAP)
- **M30** — open shafts & the abyss: rare deep `shaft_at`, multi-floor void holes, a **soft-catch fall**
  (no new physics — gravity + the swept collision; no fail-state), and a **fog-to-black abyss render**
  (range residency + `--abyss` + an in-game band over shafts). (ADR-055)

**The despair gradient** (the emergent core loop): stairs climb +1 slowly; shafts drop you 5–10 floors
fast — you sink faster than you climb.

## 6 · Build · test · gate · run
```
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1            # incremental build
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -Clean     # clean build
ctest --test-dir build --output-on-failure                                       # 99 unit tests
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gate.ps1 -Milestone M<N>   # a milestone gate
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/soak.ps1 -Hours 8          # walk-bot soak
```
**A milestone is done only when `gate.ps1 -Milestone M<N>` exits 0** → tag `m<N>-green` → push. Regression
→ revert to the last green tag (Iron Rule 2). Goldens change only via `tools/goldgen` + a DECISIONS entry.
Headless proof vehicles: `--shot/--topdown/--scene` (render goldens), `--walkbot`, `--replay`,
`--render-wav`, `--shoggoth-record/replay`, and the Phase IV `--ascend/--descend/--shaftfall/--vstream/--abyss`.

## 7 · Pending / blocked (full detail in `_run_state/CONTINUE.md` + ROADMAP §5)
1. **M29 → `m29-green`** — code + determinism done; needs `llama-server :8080` up, then one gate run.
2. **Live in-game descent** — the run_play ground plane still catches you, so you can *see* the abyss but
   can't fall into it while playing; hole the ground plane at open-floor cells (not determinism-gated).
3. **M30 polish** — the draft-audio telegraph (locked design decision 6) + a deep-descent soak.
4. **ROADMAP §3 DONE checks** — the full M0–M25 regression sweep + the soak, then perpetual-polish (§4).
5. `M31` floating-origin — deferred (default OFF; needs a real precision-horizon trigger).

## 8 · Repo layout
```
core/ gen/ stream/ render_d3d12/ render_dxr/ audio/ director/ telemetry/ app/ tools/ contracts/   modules
tests/unit/        Catch2 unit tests              goldens/m*/    committed render goldens
scripts/           build/gate/soak + checks/      docs/          ARCHITECTURE, MILESTONES, DECISIONS, SESSION_LOG, this
_run_state/        the autonomous-build rig (AUTOSTART, ROADMAP, CONTINUE) + the autoloop supervisor
build/             generated (CMake+ninja, MSVC; not committed)
```
