# MODULE: app (L2)

**Purpose.** Win32 shell, config (`config.toml`), composition root, CLI flags
(`--headless`, `--replay`, `--no-director`, `--render-wav`). Maps typed module
errors to process exit codes for the gate scripts (ARCHITECTURE.md §7).

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

**Planned.** `config.toml` + flag/config mirroring (M12), noclip intro + photo
mode (M12), `--no-director` (M11).

**Status:** M8 — adds `--post` (VHS post + HUD/timestamp; `app/hud.*` bitmap
font). Earlier: M7 `--biomeat`/`--descend`, M6 `--render-wav`/`--footsteps`/
`--audiosoak`, over the M5 render/sim/stream/walkbot/topdown/shot CLI.
