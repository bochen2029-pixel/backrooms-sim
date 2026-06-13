# MODULE: audio (L2)

**Purpose.** Procedural fluorescent hum + HVAC drone synthesis, footsteps
fired by sim ticks, raycast room-probe reverb, offline `--render-wav` mode.

**Depends on:** `core` (deterministic events + RNG).

**Public surface.**
- `audio/synth.h` (M6) — `Synth`: deterministic block-based DSP. 60 Hz mains hum
  + harmonics, lowpassed-noise HVAC bed, footstep transients, Freeverb-style
  reverb whose RT60 is set by the room probe. Pure given (seed, call sequence) —
  no wall-clock — so the offline render is reproducible (INV-1).
- `audio/room_probe.h` (M6) — `probe_reverb_seconds` / `probe_mean_free_path`:
  raycast the wall AABBs (16 horizontal rays, slab method) and map the mean free
  path to a reverb time. Pure.
- `audio/wav.h` (M6) — header-only PCM16 RIFF/WAVE read/write (also used by the
  `wavcheck` tool); `to_pcm16`.
- `audio/engine.h` (M6) — `AudioEngine`: headless real-time mixer thread. A
  prebuffered producer (renders ahead of a virtual real-time read cursor, ~170 ms
  headroom) fed lock-free from the sim thread via `post()`; exposes `underruns()`
  / `blocks_rendered()`. Wall-clock lives here (real-time path, not goldens).
- `audio/audio.h` — identity stub (module banner).

**Planned.** miniaudio real-time speaker backend (ADR + `vcpkg.json` per Iron
Rule 8) when audible playback ships; Director-driven cues (M11).

**Contracts consumed:** `contracts/audio_events_v1.h` (M6: `AudioListener`,
`FootstepEvent`, `kAudioSampleRate/Channels`, `kStrideLength`),
`contracts/geometry_v1.h` (wall AABBs for the probe).

**Status:** M6 — deterministic procedural synth + room-probe reverb + offline WAV
(`app --render-wav`) + headless real-time soak (`app --audiosoak`). Gated by the
WAV spectrum (`tools/wavcheck`), footstep 1:1 alignment, and zero-underrun soak.
No device dependency yet (headless-first, ADR-028).
