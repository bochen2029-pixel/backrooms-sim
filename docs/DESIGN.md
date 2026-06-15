# Backrooms Sim — Design & Architecture

*A readable companion to the terse canon in [`ARCHITECTURE.md`](ARCHITECTURE.md), the
build plan in [`MILESTONES.md`](MILESTONES.md), and the decision log in
[`DECISIONS.md`](DECISIONS.md). This document explains **what the thing is, why it is the
way it is, and how the pieces fit** — for a reader meeting the project for the first time.*

---

## 1. Vision

Backrooms Sim is an **infinite, never-repeating, fully procedural walk through the
Backrooms** — the internet legend of endless mono-yellow rooms, buzzing fluorescent
lights, and damp carpet that you fall into when you "noclip out of reality." It is a
**demonstration / visualization, not a game in the win/lose sense**: there is no goal, no
combat, no score. You walk; the world unfolds ahead of you forever and closes behind you;
the lights hum.

The whole project is built around one uncompromising idea: **everything is generated from
a seed.** Every wall, doorway, light fixture, carpet stain, footstep sound, reverb tail,
and rendered photon comes from deterministic math. There are **no asset files** — no
textures, no meshes, no audio clips, no models. A 7 MB folder contains an entire infinite
world.

### Design pillars
1. **Procedural everything.** If it can be generated, it is. No assets, ever.
2. **Determinism is sacred.** The same seed + the same inputs always produce the same
   world, the same pixels, and the same sound — bit-for-bit, across machines and runs.
   This is the foundation that makes everything else (replay, testing, photo mode, the
   LLM Director) possible.
3. **Liminal, not threatening.** The mood is loneliness and unease, not horror. No jump
   scares, no monsters. The dread is architectural.
4. **Headless-first.** Every feature ships with a machine-checkable, often GPU-free
   verification path *before* any visual polish. The build is validated by code, not by
   eyeballing.

---

## 2. The experience

You start in a mundane, ordinary room. The floor gives way — you **noclip** — and free-fall
into **Level 0**: the canonical Backrooms, an endless office-like maze of yellow wallpaper,
worn carpet, and a ceiling grid of fluorescent lights, some flickering. You walk with
`WASD` and the mouse. The maze never ends and never repeats. Occasionally the architecture
shifts biome — a tighter cubicle warren, a cavernous hall, a pillared expanse — and rarely
you find a **stairwell down** to a darker sublevel.

A **VHS post-process** layer (grain, scanlines, chromatic aberration, a soft vignette)
wraps everything in a found-footage haze. With a ray-tracing GPU you can switch to a
**path-traced** mode where the yellow walls bleed warm light into the corners and the
fluorescents cast soft, true shadows.

There is, optionally, a **Director**: a local language model that quietly shapes the
*ambience* of your walk. It never controls the world — only the flavor.

---

## 3. The world

### Level 0 — the maze
The world is an infinite grid of **chunks**, each a fixed-size cell of the maze generated
on demand from `(WorldSeed, chunkCoord)`. Generation guarantees the maze is always
**walkable**: adjacent chunks agree on their shared-edge doorways (a contract checked
against 10,000 random chunks every build), and a **walk-bot** can traverse 1 km across
five seeds with zero "stuck" events as a gate. Sealed rooms, seam cracks, or a stuck bot
are treated as **generator bugs, never tuned around**.

### Biomes & set pieces
Beyond the default office maze, the generator produces **biomes** (e.g. a Cubicle Farm, a
larger hall) selected deterministically by location, and **set-piece pillars** — columns
that break up the space — all validated by the same geometry checker.

### Verticality
Some locations contain a **stairwell** descending to **Level −1**, a stacked sublevel with
its own base height and ambience. Gravity + capsule collision carry the wanderer down the
steps; the descent is fully deterministic.

### The wanderer
You are the **wanderer** — a capsule with an eye height, walking at a fixed speed, subject
to gravity and AABB collision against the chunk geometry. (The glossary is strict: it's the
*wanderer*, never a "player"; chunks are *chunks*, never "tiles" — consistent vocabulary is
enforced in review.)

---

## 4. Determinism — the heart of it

Everything depends on the simulation being a **pure function of `(WorldSeed, input
stream)`**. This is encoded as a set of non-negotiable invariants (INV-1…INV-8 in the
canon), the important ones being:

- The **sim core** computes ticks at a fixed **120 Hz**, uses only a seeded **PCG64** RNG,
  and **never reads the wall clock**. It is compiled `/fp:strict` (no fast-math reordering)
  and contains **zero** render / audio / Director / OS includes — enforced by a CI grep
  gate. The core depends on *nothing*.
- All input enters the sim as an abstract **`InputCommand`** (move, look-delta, buttons).
  Keyboard, mouse, **gamepad**, the walk-bot, and replay all funnel through the same struct
  — so a recorded input stream **replays bit-identically**, and a controller is
  indistinguishable from a keyboard to the simulation.
- Audio is *also* a pure function of the sim: footsteps fire on the exact tick the integer
  step counter increments, so the offline `.wav` render is reproducible.

Why go to this trouble? Because determinism is what makes the rest **cheap and trustworthy**:
- **Replay** is a tiny input log that reconstructs an entire session.
- **Testing** can assert exact hashes of world state, frames, and audio — no fuzzy
  tolerances.
- **Photo mode** is perfectly reproducible.
- The **LLM Director** can be bolted on *without* compromising any of it (§8).

---

## 5. Engine architecture

The codebase is a strict **downward-only dependency DAG** of modules; `core` sits at the
bottom and depends on nothing. Errors cross module boundaries as **typed results, never
exceptions**; there are no globals outside the `app` composition root.

```
            app   (Win32 shell, CLI, game state machine — the composition root)
   ┌────┬────┬────┴───┬──────────┬──────────┬─────────┐
 render render audio director telemetry  stream      …
 _d3d12  _dxr    │       │         │         │
   └──────┴──────┴───────┴────┬────┴─────────┘
                            gen  (chunk generation + validators)
                              │
                            core (tick, RNG, collision, WorldState hash) — depends on nothing
```

- **`core`** — the deterministic simulation: tick loop, PCG64, collision, the hashable
  `WorldState`. The oracle of truth.
- **`gen`** — chunk generation, biomes, pillars, stairwells, and the validators that prove
  every chunk is well-formed and connected.
- **`stream`** — the chunk **ring**: a background worker pool that keeps a window of chunks
  resident around the wanderer (generate ahead, evict behind), bounding memory and smoothing
  hitches. Hardened by an 8-hour walk-bot soak.
- **`render_d3d12`** — the rasterizer: D3D12 device/swapchain, **procedural material
  textures**, forward **fluorescent lighting**, the **VHS post** pass + HUD, the windowed
  present, and (M16) swapchain resize / fullscreen.
- **`render_dxr`** — the **path-traced** mode: a self-contained DXR renderer (its own
  device) that consumes the same chunk geometry, builds BLAS/TLAS, and traces primary +
  shadow + GI rays with accumulation. Enhancement-only; raster is always the fallback.
- **`audio`** — procedural synthesis (fluorescent hum, HVAC drone, footsteps), **room-probe
  reverb** (raycast the walls to set RT60), offline WAV, and (M14) **real-time output** via
  miniaudio.
- **`director`** — the optional LLM host (§8).
- **`telemetry`** — metrics, frame-pacing CSVs, crash minidumps.
- **`app`** — the Win32 window, the CLI flag surface, the **game state machine** (§7), and
  the wiring that composes all of the above.
- **`tools`** — headless utilities: `hashdiff`, `goldgen` (the *only* sanctioned writer of
  golden artifacts), `walkbot`, `contactsheet`, `wavcheck`.

---

## 6. Rendering

**Raster (default).** Chunk geometry is drawn with a lit pipeline sampling a procedurally
generated **Texture2DArray** of materials (wallpaper, carpet, ceiling, trim). The ceiling's
fluorescent grid contributes **forward lighting**, each light scaled by a deterministic
`light_flicker(seed, id, tick)` so flicker reproduces exactly. A highlight knee keeps the
yellow wallpaper from clipping to white under stacked lights. A **VHS post pass** then adds
seeded film grain, chromatic aberration, barrel distortion, scanlines, interlace, and a
vignette, and composites the HUD — the found-footage look.

**Path-traced (DXR).** On an RTX-class GPU, `render_dxr` builds an acceleration structure
from the resident chunks and traces the scene: primary rays, next-event estimation to the
fluorescent lights, shadow rays, and one bounce of global illumination, accumulated over
many samples into a converged still. The result is verified two ways — a cross-renderer
**depth gate** (DXR vs raster depth must agree at a fixed pose) and a **converged golden
image**.

Both renderers are held to a hard rule: **zero D3D12 debug-layer messages, always.** The
debug layer is a gate; it is never disabled to pass.

---

## 7. The game shell (front end)

What turns the visualization into a *game* is a small, deliberately **dependency-free**
front end (no UI framework — it would violate the no-deps / CRT-aesthetic rules):

- A **game-state machine** (`app/menu.h`) — splash → main menu → play → pause → settings →
  quit — written as a **pure function** `menu_step(model, action) → command`. Because it has
  no rendering, no wall-clock, and no globals, the *entire front end* is **unit-tested
  headlessly** by feeding it synthetic button presses.
- An **immediate-mode UI** drawn on the existing **CPU 5×7 bitmap font** (the same one the
  HUD uses), composited to the window through a small fullscreen-blit primitive that's kept
  isolated from the post pass.
- **Persistent settings** (`app/config.h`) — a `key=value` file saved next to the exe;
  resolution, fullscreen, volumes, mouse sensitivity, the Director toggle, and the last seed
  all survive across launches. Serialize/parse is pure, so the round-trip is a unit test.
- **Input** — keyboard + mouse, **XInput gamepad** (mapped by a pure function to the same
  `InputCommand`), and **F11** borderless fullscreen.

---

## 8. The Director (optional local LLM)

The Director is the project's most unusual idea: a **stochastic language model living inside
a bit-exact engine** — without breaking it.

- It runs a **local** model (Qwen-class) through the **KEEL sidecar**, an OpenAI-compatible
  HTTP endpoint on `127.0.0.1:7071`. Everything stays on the machine; it costs nothing and
  never touches the cloud.
- It is **presentation-layer only.** The Director emits schema-validated *Directives*
  (ambience, biome bias, occasional textual notes) that affect *how the walk looks and
  feels* — never the world geometry or the simulation. Invalid output is rejected by a
  validator, never trusted.
- **It cannot break replay.** Directives enter the simulation only as entries in a recorded
  **Director Event Log**, applied at deterministic ticks. Replaying that log **with the model
  completely offline reproduces the run bit-for-bit.** The LLM is the *record-time generator*
  of a flavor track; the engine remains the source of truth. This is the sacred invariant the
  M11 gate proves.
- If the sidecar is unreachable, the Director is a graceful **no-op** — the game plays
  normally.

This is why the Director could be added as a *dependency removal* (HTTP to a sidecar) rather
than embedding a multi-gigabyte model and a CUDA build into the engine.

---

## 9. How it was built — the gate-driven method

The entire project was built **autonomously, milestone by milestone**, under a strict
contract: **a milestone is "done" only when its machine-checkable gate (`gate.ps1
-Milestone M<N>`) exits 0.** No green, no merge. On green: tag `m<N>-green`, push (the
remote is the backup). On a regression that two fixes can't resolve: **revert to the last
green tag and re-approach** — never debug forward from a broken state. Goldens change *only*
through `goldgen` plus a recorded decision; a gate is never relaxed to pass (if a gate is
wrong, it's fixed with an ADR).

This discipline is why the build is trustworthy end-to-end. Every feature — generation,
streaming, lighting, audio, the path tracer, the Director, the menus, packaging — entered
behind a gate that still runs today.

### The milestones
| | |
|---|---|
| **M0** | Scaffold + the verification harness (the harness comes before the product) |
| **M1** | Win32 window, D3D12 device, headless PNG |
| **M2** | Sim core: tick, camera, collision, **replay** |
| **M3** | Infinite chunk streaming + telemetry |
| **M4** | Level-0 generator: maze, doorways, walk-bot |
| **M5** | Procedural materials + raster fluorescent lighting |
| **M6** | Procedural audio: synth, room-probe reverb, offline WAV |
| **M7** | Biomes, set-piece pillars, verticality (a stairwell to Level −1) |
| **M8** | VHS post-processing stack + HUD / timestamp |
| **M9** | DXR path-traced mode (BLAS/TLAS, ray query, accumulation) |
| **M10** | Walk-bot **soak** + hardening (telemetry, contact sheet, minidumps) |
| **M11** | The **Director** + the Voice (local LLM via the KEEL sidecar) |
| **M12** | Integration · the noclip intro · one-command run · acceptance soak → **`v1.0`** |
| **M13** | Playable real-time windowed walk (`--play`, mouse-look, pacing gate) |
| **M14** | Sound on: real-time audio output (miniaudio, lock-free ring, WASAPI) |
| **M15** | Menus + the game-state machine |
| **M16** | Settings persistence + windowing/fullscreen + gamepad |
| **M17** | Portable packaging (bundled DXC, release build, `.zip`) → **`v2.0`** |

---

## 10. Packaging

The shipped artifact is a **portable folder** (`backrooms-portable.zip`, ~7 MB): the
optimized release exe (with the debug layer compiled out), the two Microsoft DXC
redistributable DLLs (so the path tracer works on a machine with no Windows SDK), and
plain-text README / CREDITS / `RUN.cmd`. No installer, no registry, no assets. The
**clean-env gate** proves it: unzip to a temp dir, scrub the SDK out of `PATH`, and the
path-traced mode still renders — purely from the bundled compiler.

---

## 11. Dependencies (the whole list)

Three third-party libraries, all small:
- **Catch2** — the test framework.
- **stb** — PNG writing.
- **miniaudio** — the real-time audio backend (header-only).

Everything else is the Windows SDK / system import libraries (D3D12, DXGI, WinHTTP for the
Director, XInput for the gamepad, dbghelp for minidumps) — nothing to ship beyond the DXC
pair. **No assets. No engine. No framework.** Just math, from a seed.

---

## 12. The future (post-v2.0)

- **Steam** — the engineering (Steamworks SDK, depot upload) is straightforward; the
  partner account, fee, store page, and *Publish* are operator-only steps.
- **Polish** — an in-game credits screen, far-chunk camera-relative rendering (the one known
  floating-point deferral, visible only kilometers from the origin), and GBNF-constrained
  Director decoding once the sidecar exposes it. *(The procedurally-generated app icon —
  drawn from code by `tools/icongen` at build time, no committed asset — landed in v2.1.)*

None of it is required. What exists is complete: an endless, deterministic, fully
procedural Backrooms you can unzip and walk into. There is no exit — that's the point.
