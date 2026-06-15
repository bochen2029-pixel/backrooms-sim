# BACKROOMS SIM — Autonomous Build Milestone Map

Infinite, never-repeating, procedurally generated Backrooms walking simulation.
Native Windows, C++20, DirectX 12 (raster) + DXR (path-traced mode), CUDA-class RTX GPU, local LLM via llama.cpp.
Built end-to-end by Claude Code with **zero human verification until final acceptance**.

---

## Iron Rules (the agent's standing orders)

1. **Gate runner is law.** Every milestone has one entry point: `scripts/gate.ps1 M<N>`. A milestone is complete only when its gate script exits 0. No green, no merge.
2. **Tag on green, revert on regression.** Every green gate → `git tag m<N>-green`. If a later change breaks an earlier gate, revert to the last green tag and re-approach. Never debug forward from a broken state for more than 2 attempts.
3. **Headless first.** Every feature must have a headless verification path (offscreen render, replay, telemetry, or hash) before any visual polish.
4. **Determinism is sacred.** The sim core is a pure library: fixed timestep, seeded RNG, no graphics/audio/LLM dependencies. Same seed + same input stream = bit-identical state hash, always. Any PR that breaks this fails the gate.
5. **Diff budget.** ≤ 400 LOC per change unless the milestone scope explicitly authorizes more. Scope creep fails review.
6. **Spec reconciliation in the same commit.** Any change to a module boundary updates ARCHITECTURE.md / MODULE.md in that commit. CI checks module inventory vs. directory listing.
7. **One milestone per session.** Load ARCHITECTURE.md + this file's relevant milestone + the target module's context pack. Do not load the whole repo.

## Repository Layout

```
/core        # deterministic sim: time, RNG, player kinematics, collision, world state (NO gfx deps)
/gen         # procedural generation: chunks, biomes, set pieces, connectivity validator
/render_d3d12# raster renderer
/render_dxr  # path-traced renderer (added M9)
/audio       # procedural audio engine
/director    # LLM module: llama.cpp host, schema validation, event log (added M11)
/tools       # hashdiff, goldgen, walkbot, contactsheet, wavcheck
/tests       # unit, contract, characterization (Catch2/CTest)
/goldens     # reference images, state hashes, converged PT renders, WAV spectra
/scripts     # gate.ps1, build.ps1, soak.ps1, ci checks
/docs        # ARCHITECTURE.md, DECISIONS.md, SESSION_LOG.md, MODULE.md per module
```

Toolchain pins (record exact versions in ARCHITECTURE.md §Technology Decisions): MSVC + Windows SDK with DXR support, CMake + Ninja, vcpkg manifest mode, Catch2, stb (image write/perceptual tools), miniaudio, llama.cpp, one small instruct model file (e.g., 7–8B class, quantized).

---

## M0 — Scaffold + Verification Harness (1–2 sessions)

The harness comes before the product. Nothing else can be autonomous without it.

**Scope:** CMake/Ninja/vcpkg project skeleton matching layout above. Catch2 wired into CTest. `scripts/build.ps1` (clean + incremental), `scripts/gate.ps1 <milestone>`. Tools: `hashdiff` (image hash + perceptual diff CLI), `goldgen` (golden capture/update). Claude Code hooks: post-edit incremental build + affected tests; pre-commit runs current milestone gate. CLAUDE.md (build/gate commands + Iron Rules), ARCHITECTURE.md skeleton, DECISIONS.md, SESSION_LOG.md.

**Exit gates:**
- [ ] Fresh clone → `build.ps1` → all targets compile, zero warnings-as-errors violations
- [ ] `ctest` discovers and passes a seed unit test
- [ ] `hashdiff` round-trips a known image pair (identical → 0 diff; perturbed → nonzero)
- [ ] Deliberately failing test blocks commit via hook (test the gate itself)
- [ ] `gate.ps1 M0` exits 0; tag `m0-green`

## M1 — Window, D3D12 Device, Headless Mode (1 session)

**Scope:** Win32 window, D3D12 device/swapchain/queue/fence, debug layer + DRED enabled in dev builds. Clear-color frame. `--headless --frames N --out path` mode: offscreen render target, PNG dump, clean exit.

**Exit gates:**
- [ ] 10 s windowed run, exit 0, **zero D3D12 debug-layer errors or warnings** (parse debug output — this gate stays on for every later milestone)
- [ ] Headless frame 0 PNG bit-identical to golden across 3 consecutive runs
- [ ] 60 s soak: stable memory (CRT debug heap delta ≈ 0), no fence timeouts

## M2 — Sim Core: Camera, Input, Collision, Replay (1–2 sessions)

**Scope:** `/core` as standalone lib. Fixed 120 Hz tick, seeded RNG. First-person walk camera, capsule-vs-AABB collision with wall sliding, gravity. Hardcoded test room. **Input replay system now** (record/playback streams) — the enabler for every future automated movement test. Per-tick state hash (position, orientation, RNG state).

**Exit gates:**
- [ ] Replay of recorded stream → bit-identical per-tick state hash across runs
- [ ] Collision unit tests: no wall penetration at any speed, sliding preserves tangential velocity, no floor tunneling at low FPS (sim ticks decoupled from render)
- [ ] Golden screenshot from fixed pose in test room
- [ ] `/core` compiles with zero graphics includes (CI grep gate)

## M3 — Infinite Chunk Streaming, Placeholder Geometry (1–2 sessions)

**Scope:** Unbounded chunk grid (e.g., 32×32 m). `GenerateChunk(worldSeed, cx, cz)` = pure function. Load ring around player, unload behind, background-thread generation, main-thread GPU upload. Placeholder geometry (floor + numbered grid walls) to expose seams. Frame-time telemetry to CSV.

**Exit gates:**
- [ ] Generate → free → regenerate any chunk: bit-identical geometry hash (1,000 random chunks)
- [ ] Replay walking 100+ chunks: no frame > 2× median (hitch gate from telemetry CSV)
- [ ] 10 min soak: memory plateaus after ring fill (regression slope ≈ 0)
- [ ] Seam assert: boundary vertices of adjacent chunks match exactly

## M4 — Level 0 Generator: Rooms, Doorways, Connectivity (2 sessions)

**Scope:** Real layout algorithm per chunk — grid rooms, walls, doorways, pillars. Edge-constrained generation: doorway positions on each chunk edge derive from `hash(seed, sharedEdge)` so neighbors agree without communication. `/gen` connectivity validator (flood fill within and across chunks). Geometry validator (no overlapping/zero-thickness/floating walls). Walk-bot v1: random walker on replay infra with stuck detection (position variance ≈ 0 over 10 s = fail).

**Exit gates:**
- [ ] 10,000 random chunks: flood fill proves every walkable cell reaches every open edge — **zero sealed boxes**
- [ ] Geometry validator: zero overlaps/degenerates across the same 10,000
- [ ] Walk-bot completes 1 km without stuck events (5 seeds)
- [ ] World-state determinism hash unchanged across runs
- [ ] Headless top-down debug render of 3×3 chunks matches golden per seed

## M5 — Procedural Materials + Raster Lighting v1 (2 sessions)

**Scope:** All textures generated at startup (no asset files): yellow wallpaper noise, carpet, ceiling-tile grid, emissive fluorescent panels, scuffed baseboards. Lighting v1: regular fluorescent grid as clustered/forward lights with falloff + flat ambient; per-light flicker driven by seeded RNG in sim core (deterministic flicker!).

**Exit gates:**
- [ ] Texture generation deterministic (hash per texture per seed)
- [ ] Goldens: 5 fixed poses × 3 seeds, perceptual diff < threshold
- [ ] **Luminance histogram gate:** scene mean/percentile luminance within band (catches all-black / blown-out regressions mechanically)
- [ ] ≥ 120 FPS at 1440p on dev GPU (telemetry gate); zero debug-layer messages

## M6 — Procedural Audio (1–2 sessions)

**Scope:** miniaudio (or XAudio2) backend. Synthesized fluorescent hum (60 Hz + harmonics, per-light spatialized), HVAC drone bed, footsteps fired by sim ticks, reverb sized by raycast room-volume probe. `--render-wav` offline mode: run a replay, write the mix to WAV.

**Exit gates:**
- [ ] Offline WAV from fixed replay: FFT shows expected 60 Hz fundamental + harmonic peaks (`wavcheck` tool); silence check fails if RMS ≈ 0
- [ ] Footstep events in audio log align 1:1 with step ticks in replay log
- [ ] 10 min soak: zero buffer underruns; audio thread never blocks sim thread (tick-time telemetry unchanged audio on/off)

## M7 — Biomes, Set Pieces, Verticality (2 sessions)

**Scope:** Low-frequency biome field over chunk space: classic yellow, cubicle farm, parking garage, poolrooms, pipe corridors. Rare set pieces (vast pillar halls, flooded sections, stairwells descending to level −1 with altered generator params). Biome-blended seams.

**Exit gates:**
- [ ] Each biome generator passes the full M4 validator suite (connectivity + geometry, 10,000 chunks each)
- [ ] Biome distribution over 100,000 chunks within expected proportions ±2% (statistical gate)
- [ ] Scripted replay descends a stairwell to level −1; connectivity and determinism hold across levels
- [ ] Goldens per biome (fixed pose × seed)

## M8 — VHS Post-Processing + HUD (1 session)

**Scope:** Post stack: film grain, chromatic aberration, slight barrel distortion, scanline/interlace flicker, timestamp overlay, periodic autofocus hunt (blur pulse), vignette. All toggleable via config. HUD: odometer, world seed, chunk coords, perf overlay.

**Exit gates:**
- [ ] Goldens with post ON and OFF (clean A/B)
- [ ] Timestamp overlay renders correct sim time during replay (OCR-free: render time also written to telemetry, golden compares pixels)
- [ ] Post pass budget < 1.5 ms at 1440p
- [ ] Grain is seeded (deterministic goldens still pass)

## M9 — DXR: Path-Traced Mode (2–3 sessions)

**Scope:** DXR 1.1. BLAS per chunk, TLAS refit on stream events. Path-traced lighting mode with temporal accumulation (full quality when stationary, reduced bounces while moving). Raster remains default + fallback. Emissive fluorescents become the actual light sources.

**Exit gates:**
- [ ] **Cross-renderer depth compare:** same pose rendered raster vs. DXR primary hits → depth buffers within epsilon (proves acceleration structures contain exactly the streamed geometry)
- [ ] Converged golden: 1,000+ spp accumulation at 3 fixed poses vs. stored reference, RMSE < threshold
- [ ] Interactive PT mode ≥ 60 FPS while walking; accumulation resets correctly on movement (no ghost gate: histogram delta after teleport)
- [ ] TLAS rebuild under streaming: walk-bot 1 km in PT mode, zero debug-layer/DRED errors

## M10 — Walk-Bot Soak + Long-Haul Hardening (1–2 sessions)

**Scope:** Walk-bot v2: corridor-following + doorway-seeking wanderer. 8-hour headless soak: telemetry (FPS percentiles, memory slope, chunk-gen times), periodic connectivity audits, screenshot every 5 min → `contactsheet` tool tiles them for agent visual review. Minidump capture + auto-restart-and-log on crash.

**Exit gates:**
- [ ] 8 h soak: zero crashes, memory slope < epsilon, 1% low FPS above floor, all connectivity audits pass
- [ ] Contact sheet passes mechanical screening (no all-black/all-white frames via histogram) **and** agent visual review of the sheet
- [ ] Forced-crash drill: injected crash produces a minidump and a clean restart log

## M11 — The Director + The Voice (local LLM) (2–3 sessions)

**Scope:** `/director` hosts llama.cpp (CUDA offload) on its own thread/process with a message queue. Contract: input = wanderer summary JSON (time walked, route loops, dwell points, biome history); output = directive JSON validated against schema (flicker sector, distant sound cue, biome bias, intercom line, wanderer note). Invalid output → rejected and logged; sim never consumes raw text. The Voice renders intercom/radio captions. Wanderer notes generated once per location hash, then cached. **Determinism preserved:** LLM outputs enter the sim only as a logged event stream; replays consume the log, never the model. Kill switch: `--no-director` runs the full sim cleanly.

**Exit gates:**
- [ ] Eval suite (100+ scenario inputs): 100% schema-valid outputs; zero contradictory directives (lint rules); tone rubric scored by Claude during build ≥ threshold
- [ ] Frame-time telemetry identical (± noise) with generation running vs. idle (proves async isolation)
- [ ] p95 directive latency < 5 s on dev GPU
- [ ] Replay with recorded event log is bit-identical with the model fully offline
- [ ] `--no-director` passes the entire M10 soak

## M12 — Integration, Polish, Acceptance (1–2 sessions)

**Scope:** Noclip intro sequence (mundane room → fall-through → Level 0), photo mode, settings file, README with one-command build/run, final spec reconciliation audit.

**Exit gates:**
- [ ] Full CTest suite green; every prior milestone's `gate.ps1` still exits 0 (regression sweep)
- [ ] Fresh clone → one script → running exe (repo is self-contained, including model download step)
- [ ] **Acceptance run: 12 h unattended soak with Director ON** passing all M10 gates
- [ ] CI doc checks: module inventory ↔ directory listing match, every boundary has a contract file
- [ ] Tag `v1.0`

---

# Phase II — Shippable Game (M13–M17)

v1.0 is a deterministic, headless-first visualization. Phase II turns it into a
**playable, portable game** (stop before Steam; that's a later phase). The enabling
fact: input is already an abstracted `InputCommand` stream (the walk-bot/replay feed
it), so real keyboard/mouse drive the *same* `InputCommand → tick → render` path —
which keeps interactive features gate-testable by **replaying canned input through the
live loop**. Scope target: **through M17** (playable + settings + portable zip).
Verification: automated gates + captured-frame review (no hands-on playtest required).

## M13 — Playable: real-time loop + KB/mouse (the keystone)
**Scope:** A windowed game loop (`app --play`) that **draws the streamed maze in real
time** (closing the clear-frame gap), a **fixed 120 Hz sim tick decoupled from render**
(accumulator), and real input — WASD + jump/sprint + **raw mouse-look** → `InputCommand`
→ `core::tick`. The renderer gains a windowed maze path (chunks → back buffer → present,
depth-tested). `--window` (M1 clear-frame) stays for the M1 gate; `run.ps1 -Window` →
`--play`.
**Exit gates:** [ ] Replay a canned `InputCommand` stream through the LIVE loop →
deterministic WorldState hash (live path == replay path). [ ] Windowed run (`--play
--seconds N`) is debug-layer clean + frame-pacing telemetry bounded (p99 < 2× median).
[ ] A maze-render golden frame from the windowed path (proves the world renders, not a
clear screen). [ ] `--no-director`/raster default unaffected (M0–M12 regression).

## M14 — Sound on: real-time audio output
**Scope:** Wire the existing real-time `AudioEngine` (built + headless-tested in M6) to
the speakers via **miniaudio** (the dep M6 deliberately deferred; header-only → ADR +
vcpkg). Master/SFX volume hooks.
**Exit gates:** [ ] Real-time playback run: **zero underruns** over N s (existing
counters), device opens/closes clean. [ ] The offline `--render-wav` path stays
**bit-identical** (determinism intact). [ ] miniaudio ADR + vcpkg entry.

## M15 — Menus + game flow
**Scope:** A game **state machine** (splash → main menu → play → pause → settings →
quit) and an **immediate-mode UI on the existing CPU bitmap-font HUD** (no new
framework — fits the no-deps rule + the CRT aesthetic). Main menu (New/seed, Continue,
Settings, Quit), pause menu; mouse + keyboard navigation.
**Exit gates:** [ ] State-transition tests driven by synthetic input (menu → play →
pause → menu) — deterministic. [ ] Menu-render goldens. [ ] No debug-layer messages
across state changes.

## M16 — Settings + persistence + windowing + gamepad
**Scope:** A settings screen (resolution, fullscreen/borderless, vsync, FOV, mouse
sensitivity, master/SFX volume, renderer raster↔DXR, quality, Director on/off, **key
rebinding**), a **config file** that saves/loads/applies on launch, fullscreen +
resolution handling, and **XInput** gamepad → `InputCommand`.
**Exit gates:** [ ] Config write→read **round-trips** identically; apply-settings
headless checks (the renderer/sim reflect them). [ ] Resolution/fullscreen-change
smoke (debug-clean). [ ] Synthetic-gamepad → `InputCommand` mapping tests.

## M17 — Ship-ready packaging (portable)
**Scope:** **Bundle `dxcompiler.dll`/`dxil.dll`** beside the exe so end-users need no
Windows SDK; a **release build config** (no debug layer/DRED, optimized); app icon +
version info + a splash + a credits/EULA screen; a **portable .zip** (self-contained
folder). No installer (per decision).
**Exit gates:** [ ] **Clean-env test:** unzip to a temp dir with a scrubbed PATH (no
SDK) → `--play`/`--dxr-pt` run → uses the **bundled** DXC, debug-clean. [ ] Fresh-unzip
→ launch smoke. [ ] Full M0–M16 regression sweep. [ ] Tag `v2.0` (playable, packaged).

*(Post-M17, a later phase: Steam integration — Steamworks SDK + depot/SteamPipe upload.
The engineering is autonomous; the partner account, $100 fee, store page, and Publish
are operator-only.)*

---

# Phase III — The Backrooms come alive (M18–M23)

*Operator-directed after `v2.1`. Make the walk feel alive, add a ray-tracing toggle, and put a
living, AI-driven **Shoggoth** in the maze. Determinism stays sacred: every AI decision (Director
**and** Shoggoth) enters the sim only as recorded **event-log** entries at a deterministic tick, so
replay is **bit-identical with all models offline**. Head-bob and ray-tracing are presentation-only;
the shoggoth's body + navigation live in WorldState (seeded, replayable), its intent in the log.
**Do NOT touch `C:\KEEL`** — reuse the `:7071` sidecar; for vision, stand up a backrooms-local KEEL
copy if needed. See the durable plan in memory `project-phase-III-living-backrooms.md`.*

## M18 — Realistic walking (head-bob + run)
**Scope:** A **run** modifier (`kButtonRun` — Shift / gamepad) → a deterministic higher move speed
in the sim core. A humanlike **head-bob**: a pure function of the walk-phase (from the deterministic
odometer), speed, and run-state — vertical `A·sin(2πf·phase)` + lateral sway at **half frequency**
(figure-8), footfall at the bob trough; running raises `f` and `A`. Applied to the **render camera
only** (INV-1 intact — never perturbs WorldState).
**Exit gates:** [ ] Unit-test the pure bob curve (amplitude/frequency, walk vs run, continuity) +
the run input (deterministic odometer-rate increase, replayable hash). [ ] `--game --seconds N`
debug-clean. [ ] M0–M17 regression (the raster/sim goldens are unchanged — bob is view-only).

## M19 — Ray-tracing toggle (real-time DXR in the window)
**Scope:** Bring `render_dxr` into the live windowed loop as a **toggleable renderer** (Settings →
"Ray Tracing", persisted in config, **default OFF → no regression**). Add a **windowed present** for
DXR (it currently does headless readback): render → accumulate → blit to the swapchain, reset on
movement. "Not too fancy" is fine — few-spp RT lighting, or RT shadows + AO on the raster path.
**Exit gates:** [ ] RT-on and RT-off both windowed debug-clean. [ ] RT lighting is physically
plausible (luma band / soft shadow present). [ ] The toggle persists in `backrooms.cfg`. [ ]
**Default-off leaves the M5 raster golden bit-identical** (no regression). [ ] M0–M18 regression.

## M20 — Shoggoth: body + deterministic movement
**Scope:** A procedural amorphous **3D blob** (metaball / SDF-noise sphere cluster that writhes — no
assets), as a **WorldState entity** with a seeded position. **Grid pathfinding** (A* / flow-field on
the chunk collision grid) navigates it to a target cell; **organic locomotion** oozes it along
("walks like a shoggoth"). An intermittent chase state machine (lurk → hunt → chase → retreat) on
distance (and later AI intent).
**Exit gates:** [ ] Deterministic shoggoth movement (seeded → replayable hash, ×2 identical).
[ ] Pathfinding reaches targets across seeds with no stuck. [ ] Renders (raster + DXR) debug-clean.
[ ] M0–M19 regression.

## M21 — Shoggoth brain (KEEL intent + cascade)
**Scope:** A "Shoggoth Director" reusing the KEEL HTTP client with a **shoggoth system prompt**:
input = a summary (shoggoth/wanderer positions, distance, recent events) → a **schema-validated
INTENT** (`{action: stalk|lurk|flee, target_hint, mood}`). The intent → a **deterministic
navigator** picks a concrete target cell → M20 locomotion walks it. Expensive LLM every N s; cheap
motion every tick — cascading, delegated embodiment.
**Exit gates:** [ ] 100% schema-valid (post-hoc validator). [ ] Intent → navigation mapping tests.
[ ] **Replay bit-identical with the model offline** (the sacred gate — intent via the event log).
[ ] `--no-shoggoth` kill switch. [ ] M0–M20 regression.

## M21b — Live async brain (the Shoggoth thinks while you play) ✅ DONE (`m21b-green`)
**Scope:** Make the M21 brain run in the actual game. `app/shoggoth_brain_host.h` — a `ShoggothBrainHost`
mirroring the Director's async `DirectorHost` (worker thread, latest-wins `submit`, non-blocking `poll`,
atomic counters, graceful no-op when KEEL is down). Wired into `run_play` + `run_game`: submit a
`ShoggothSummary` every ~3 s (ambient wall-clock), apply the returned intent to the live Shoggoth at a tick
boundary. On by default; **`--no-shoggoth-brain`** kill switch. KEEL stays OFF the 120 Hz frame thread (the
M11/Gate-2 async-isolation lesson); the headless record/replay sacred gate is untouched.
**Exit gates:** [x] `--play` brain-ON thinks live (≥1 intent applied). [x] Async-isolated — p99 frame < 2×
median best-of-2 (1440p) AND no new hitches vs brain-off (measured on **1.79×** vs off **1.44×** — ON ≈ OFF).
[x] Graceful no-op with KEEL down. [x] `--no-shoggoth-brain` kill switch. [x] The M21 sacred gate (record ==
replay, model off) + M0–M21 regression. → `m21b-green`.

## M22 — Shoggoth sees (KEEL vision / qwen-VL + mmproj) ✅ DONE (`m22-green`)
**Scope:** A virtual camera renders the shoggoth's **POV snapshot** (offscreen → image) → a **vision
model via KEEL** (qwen-VL + `mmproj-F16.gguf`) → richer intent. **Investigation found the `:7071`
sidecar can do vision AS-IS** (its `keel-serve` parses `image_url` → forced-local vision tier; the
shared `llama-server :8080` runs `--mmproj`; a live probe described a real POV shot in ~1.3 s) — so **no
backrooms-local KEEL copy was needed**. `director::keel_complete_vision` carries a `[text, image_url]`
turn; `run_shoggoth_vision_record` renders the POV (`app/shoggoth_vision.h` + `app/base64.h`) → vision →
the same `parse_shoggoth_intent` → `ShoggothEvent` log. Snapshot + vision happen at **record time only**;
the intent enters via the event log → `--shoggoth-replay` is bit-identical model-offline.
**Exit gates:** [x] POV snapshot → vision call → parsed intent (≥1 real vision intent). [x] Determinism
preserved — record == replay bit-identical, model off, snapshot never re-rendered. [x] Graceful no-op if
the vision endpoint is down. [x] M0–M21b regression (incl. the M21 text sacred gate). → `m22-green`.

## M23 — Shoggoth hears (whisper.cpp) *(later / speculative)*
**Scope:** Nearby procedural audio → `C:\whisper.cpp` → text → the shoggoth's LLM context, so it
"knows its true surroundings" (vision + hearing). Defer until M22 lands; then the sys-prompt drives
"act like a real shoggoth" with the cheap deterministic navigation/locomotion doing the embodiment.
**Exit gates:** [ ] Audio → transcript → context. [ ] Determinism preserved. [ ] Kill switch + no-op.

---

## Session protocol per milestone

1. Load ARCHITECTURE.md + this milestone's section + target module context pack
2. Produce change manifest (files, contract changes, tests, rollback, diff budget) before code
3. Implement → hooks auto-build/test → iterate
4. Run `gate.ps1 M<N>` until exit 0
5. Spec reconciliation in same commit → tag → SESSION_LOG.md entry

Estimated total: 18–24 sessions. M0 and M2 are the highest-leverage milestones — the harness and the replay system are what make every later gate possible.
