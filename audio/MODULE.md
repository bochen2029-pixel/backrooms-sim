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
- `audio/engine.h` (M6/M14) — `AudioEngine`: real-time mixer thread. `start()` is
  the M6 headless path: a prebuffered producer renders ahead of a *virtual* read
  cursor (~170 ms headroom) into a null sink, counting underruns. `start_device()`
  is the M14 path: the producer fills a lock-free ring that a real miniaudio device
  callback drains (or the null backend, for gates). Fed lock-free from the sim via
  `post()`; `set_master_volume`/`set_sfx_volume` (playback mix). Exposes
  `underruns()` / `blocks_rendered()` / `device_open()` / `backend()`. Wall-clock
  lives here (real-time path, not goldens).
- `audio/ring.h` (M14) — `FloatRing`: single-producer/single-consumer lock-free ring
  of interleaved float frames; the mixer→device hand-off. Wait-free, allocation-free
  after `reset()`. Unit-tested (wrap + 200 k-frame threaded round-trip).
- `audio/device.h` (M14) — `AudioDevice`: opaque PIMPL over a miniaudio `ma_device`
  (default playback endpoint, or the hardware-free null backend). `<miniaudio.h>` is
  compiled into `device.cpp` only, so no other TU inherits it (ADR-040).
- `audio/audio.h` — identity stub (module banner).

**Contracts consumed:** `contracts/audio_events_v1.h` (M6: `AudioListener`,
`FootstepEvent`, `kAudioSampleRate/Channels`, `kStrideLength`),
`contracts/geometry_v1.h` (wall AABBs for the probe).

**Depends on (M14):** miniaudio (header-only, `vcpkg.json`, ADR-040) — real-time
playback backend, compiled into `device.cpp`, consumed as `<miniaudio.h>`.

**Status:** M14 — real-time audio **output** to speakers via miniaudio. The mixer
feeds a real device (WASAPI) through a lock-free ring; audible in `app --play`;
`app --audiodev [--null]` is the headless gate (device opens + zero underruns).
M6 procedural synth + room-probe reverb + offline WAV (`app --render-wav`, still
**bit-identical** — determinism untouched) + headless soak (`app --audiosoak`)
remain. Gated by the WAV spectrum (`tools/wavcheck`), footstep 1:1 alignment,
zero-underrun soak (M6) + null/real device zero-underrun output + the SPSC ring
tests (M14).
