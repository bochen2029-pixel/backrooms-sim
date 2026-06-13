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

**Planned.** `config.toml` + flag/config mirroring (M12), noclip intro + photo
mode (M12), `--no-director` (M11).

**Status:** M5 — adds `--shot` (fixed-pose lit goldens + luminance histogram) to
the render/sim/stream/walkbot/topdown CLI modes.
