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

**Planned.** `config.toml` + flag/config mirroring (M12), noclip intro + photo
mode (M12), `--no-director` (M11).

**Status:** M6 — adds `--render-wav`, `--footsteps`, `--audiosoak` (procedural
audio: offline WAV + headless real-time soak) to the
render/sim/stream/walkbot/topdown/shot CLI modes.
