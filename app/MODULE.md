# MODULE: app (L2)

**Purpose.** Win32 shell + composition root + the CLI flag surface (the de-facto
settings interface: `--headless`, `--replay`, `--director`/`--no-director`,
`--render-wav`, `--intro`, …; `scripts/run.ps1` is the one-command entry). Maps
typed module errors to process exit codes for the gate scripts (ARCHITECTURE.md §7).

**Depends on:** all modules. Nothing depends on `app`.

**Public surface.** `backrooms` console executable. Modes:
- `--headless` / `--window` (M1) — clear-color frame; PNG / swapchain.
- `--scene` (M2) — render the test room from a fixed pose → PNG.
- `--sim --ticks N [--record/--replay f] [--hashlog f]` (M2) — drive the sim,
  per-tick WorldState hash log.
- `--stream [--frames N|--seconds S] [--csv f] [--radius R] [--workers W]` (M3) —
  infinite chunk-streaming walk on open ground; frame-time telemetry CSV.
- `--walkbot --km K [--seed S]` (M4) — wander-bot through the generated maze
  (collides with walls), reports distance / stuck events / determinism hash.
- `--topdown --out p.png [--seed S]` (M4) — orthographic top-down debug render
  of a 3×3 chunk block.
- `--shot --pose P [--seed S] [--ticks T] --out p.png` (M5) — deterministic
  fixed-pose textured+lit render from one of 5 canonical camera poses at flicker
  tick T (bit-exact per seed/pose/tick/GPU). Prints a luminance histogram
  (`luma_mean`, `luma_p01/p50/p99`, `frac_black`, `frac_white`) so the gate can
  assert the frame is neither all-black nor blown-out.
- `--render-wav --out a.wav [--seed S] [--ticks N] [--audiolog f]` (M6) — offline
  audio: drive the deterministic maze walk, synth the mix (400 frames/tick),
  write a PCM16 WAV (+ footstep-tick log). Bit-identical across runs.
- `--footsteps --out f [--seed S] [--ticks N]` (M6) — the independent footstep
  reference: the same walk, footstep ticks only (gate aligns it with `--audiolog`).
- `--audiosoak [--audio] [--seconds S | --ticks N] [--seed S]` (M6) — drive the
  sim flat-out while the real-time mixer thread runs; reports mean tick time +
  `underruns` (proves the audio thread never blocks the sim).
- `--audiodev [--null] [--seconds S] [--seed S] [--master V] [--sfx V]` (M14) —
  real-time audio **output**: the mixer feeds a real miniaudio playback device (or
  the hardware-free null backend with `--null`, the gated path) through a lock-free
  ring. Reports `device_open` / `backend` / `audio_blocks` / `underruns` (must be 0).
- `--biomeat [--seed S]` (M7) — print the biome at the spawn chunk (0,0); used to
  target a known biome for per-biome goldens / gate captures.
- `--descend [--seed S] [--ticks N]` (M7) — scripted stairwell descent to level
  −1: builds the approach floor + stairwell set piece + landing, walks the
  wanderer down (gravity/collision), reports drop / level reached / sublevel
  connectivity / determinism hash.
- `--post` (M8) — adds the VHS post stack + HUD/timestamp to `--shot` (composites
  a CPU-rasterised HUD; echoes `timestamp:` to telemetry) and to `--stream`
  (VHS-only, for the post-pass perf measure). `app/hud.*` is the 5×7 bitmap font.
- `--dxr-probe` (M9) — print the DXR capability of the box (adapter, Device5,
  raytracing tier, DXC availability + a trial compile); exit 0 iff DXR-ready.
- `--dxr-test` (M9) — DispatchRays smoke test (raygen UV gradient → PNG).
- `--dxr --pose P [--seed S]` (M9) — BLAS/TLAS of the resident chunks, primary-ray
  render from the same poses as `--shot` (distance-shaded); reports debug errors.
- `--dxr-depth --pose P [--seed S]` (M9) — renders the same pose with the raster
  renderer and the DXR path tracer, reads back both depth buffers, linearizes NDC
  depth to eye-space and compares per pixel (exit gate #1). Prints co-foreground
  pixel count, depth mismatch/edge fractions, mean/max rel-err, both debug counts.
- `--dxr-pt --pose P --spp N [--seed S]` (M9) — path-traced render (emissive
  fluorescents + shadow + GI, accumulated over N spp), `--out` PNG; prints a luma
  band + debug count (exit gate #2 reference).
- `--dxr-fps --pose P [--spp N]` (M9) — times N reduced-sample interactive frames
  (resetting accumulation each frame, the moving path); prints median/p99 ms + FPS
  (exit gate #3a).
- `--dxr-ghost [--seed S]` (M9) — converges pose A, then renders pose B with and
  without an accumulation reset; prints clean-vs-fresh (≈0) and ghost-vs-fresh
  (large) mean-abs diffs, proving reset-on-move clears the accumulator (gate #3b).
- `--dxr-walk --km K [--seed S]` (M9) — walk-bot covers K km in PT mode, rebuilding
  the BLAS/TLAS as chunks stream; prints distance, TLAS rebuilds, PT frames, and
  the debug count (exit gate #4).
- `--soak [--seconds S | --ticks N] [--csv f] [--out dir] [--shot-every N]` (M10) —
  long-haul walk-bot soak over the streaming raster renderer: writes the frame
  telemetry CSV (FPS percentiles + memory slope), runs periodic connectivity
  audits, and dumps periodic screenshots for `contactsheet`. Prints frames, audits,
  audit_failures, stuck_events, screenshots, mem first/last, debug count.

- `--director-probe [--seed S] [--director-url host:port]` (M11) — renders a
  representative WandererSummary → KEEL sidecar (default `127.0.0.1:7071`) → prints
  the KEEL routing (tier/cost/route), the raw directive JSON, and the validator
  verdict. The end-to-end Director wire; live-validates the sidecar.
- `--director-record --director-log f [--seed S] [--ticks N] [--director-url u]`
  (M11) — walks the maze with the Director ON (live KEEL), recording each validated
  directive into the event log at its tick; prints the combined run hash.
- `--director-replay --director-log f` (M11) — re-walks the SAME run (seed + ticks
  from the log) with KEEL offline, applying the recorded directives; prints the
  combined run hash. Record == replay proves Gate 4 (replay bit-identical, model off).
- `--director-eval [--eval-count N] [--seed S]` (M11) — runs N varied WandererSummary
  scenarios through KEEL, validating each; prints schema-valid rate + latency
  p50/p95/max + a few sample directives (Gates 1 + 3).
- `--soak --director [--director-interval S]` (M11) — runs the async `DirectorHost`
  during the soak (generation off the frame thread; ambient wall-clock pacing, ~15 s);
  prints director request/produced/applied counts, notes cached, and the latest Voice
  line. `--no-director` is the explicit kill switch (INV-6; off by default, so the M10
  soak path is byte-unchanged).
- `--intro [--seed S] [--ticks N]` (M12) — the noclip intro: stand in a mundane room,
  the floor gives way, free-fall, land in the Level-0 maze (pure scripted core sim →
  deterministic; prints noclipped/landed/final_y/final_hash). The visual fall is the
  windowed experience; the headless run is the determinism check.
- `--play [--seed S] [--seconds N] [--width W --height H] [--csv f] [--director]` (M13) —
  the **real-time playable windowed walk**: a fixed-120 Hz tick accumulator decoupled from
  render, WASD + jump + mouse-look, 3×3 collision rebuild as you cross chunks, Esc/close to
  exit (spawns at the proven-open (2,2) cell). Renders via `render_chunks_windowed`, with
  **real-time audio** (M14: WASAPI mixer; `--no-audio` / `--master` / `--sfx` to control).
  `--csv` logs per-frame pacing (frame_ms + residency/mem) for the gate; `--seconds N`
  auto-exits (headless-friendly). `scripts/run.ps1 -Window` launches this.
- `--game [--seed S] [--seconds N] [--width W --height H] [--no-audio] [--config f]` (M15/M16)
  — the **windowed game shell**: boots to the main menu and runs the `app/menu.h` state machine
  (splash → main menu → play → pause → settings → quit). Menu screens present a CPU overlay
  via `render_d3d12::present_overlay_windowed`; New Game enters the live `--play` walk; Esc
  pauses. Keyboard nav (arrows/WASD + Enter/Esc), **XInput gamepad**, **F11 fullscreen**. M16:
  loads/saves a **config** (`--config`, default `backrooms.cfg`) — resolution, fullscreen,
  volumes, mouse sensitivity, seed all persist. `--seconds N` is the debug-clean gate smoke.
- `--menu-shot --screen <splash|mainmenu|pause|settings> [--sel N] --out p.png` (M15) — render
  one menu screen to a PNG (deterministic, CPU-only → the menu-render golden).
- `--menu-smoke` (M15) — composite every menu screen through the GPU (post + HUD path) across
  state changes; reports `debug_error_count` (the "no debug-layer messages across state changes" gate).
- `--config-check --config f [--width W --height H --master V --sfx V --seed S]` (M16) — write a
  config from the flags, read it back, and **apply** it to a headless render; prints the
  round-tripped values + `rendered_width/height` (proves the config drives the engine).
- `--resize-smoke` (M16) — resize the swapchain across resolutions + a borderless-fullscreen
  toggle, presenting each; reports `debug_error_count` (the windowing-change smoke).

**The menu / game-state machine (M15).** `app/menu.h` is a pure, header-only
`menu_step(MenuModel, UiAction) → UiCommand` (no rendering / wall-clock / globals), so the
front end is unit-tested headlessly with synthetic input. `hud.cpp build_menu_overlay` draws it
on the existing 5×7 bitmap font.

**Settings persistence + input (M16).** `app/config.h` (pure `serialize`/`parse`/`sanitize`,
header-only) is a `key=value` config saved/loaded next to the exe; `app/gamepad.h` (pure
`gamepad_to_input`) maps an XInput pad to the same `InputCommand` as keyboard/replay. Both are
unit-tested; XInput is a Windows system import lib (no vcpkg). Fullscreen is borderless (no
exclusive mode); `render_d3d12::resize` handles the swapchain.

**Realistic walking (M18).** `app/head_bob.h` (pure, header-only) — `head_bob(distance, speed,
walk, run)` returns a camera offset (two vertical dips/stride + half-freq lateral sway, eased by
speed); `apply_head_bob` adds it to the `CameraPose` after `wanderer_camera` — **view-only, never
WorldState** (the M5 golden stays bit-identical). Shift / gamepad trigger sets `kButtonRun`, which
`core::tick` turns into `kRunSpeed` (deterministic through the input contract).

**The Shoggoth (M20).** `app/shoggoth.h` (pure, header-only) — a deterministic chase creature that
lives **outside WorldState** (existing hashes untouched). `shoggoth_step(sh, wanderer, seed,
pathfind)` BFS-navigates the maze (`gen` layouts) toward the wanderer with a lurk→hunt→chase→retreat
state machine + organic ooze; `shoggoth_hash` for replay. `--shoggoth` is the headless gate driver
(determinism + chase metrics + a CPU top-down chase-map PNG via `--out`). The KEEL brain is M21.
`app/shoggoth_body.h` (M20b) generates its procedural **warm-orange radial-tentacle body** (no
assets) in world space, injected each frame as a synthetic `ResidentChunk` and drawn through the lit
pipeline — visible first-person in `--play`/`--game`; `--shoggoth-shot` renders it to a PNG.

**Settings & photo mode (M12).** Configuration is the **CLI flag surface** above
(the de-facto settings interface; `scripts/run.ps1` is the one-command entry).
**Photo mode** = the deterministic framed-capture modes already shipped: `--shot`
/ `--dxr-pt` at any of the 5 canonical poses (or a seed), and `--topdown` for the
debug map — each writes a reproducible PNG.

**Status:** M12 — adds `--intro` (noclip fall into Level 0). M11 added the Director
modes (`--director-probe`/`--director-eval`/`--director-record`/`--director-replay`,
`--soak --director`, `--no-director`). Earlier: M10 `--soak`/`--crash-test`, M9 the
`--dxr*` path-traced modes, M8 `--post`, over the M2–M7 sim/stream/walk/render CLI.
