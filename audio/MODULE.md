# MODULE: audio (L2)

**Purpose.** Procedural fluorescent hum + HVAC drone synthesis, footsteps
fired by sim ticks, raycast room-probe reverb, offline `--render-wav` mode.

**Depends on:** `core` (events).

**Public surface (M0).** `audio/audio.h` — identity stub.

**Planned.** miniaudio backend + synthesis + offline WAV (M6).

**Contracts consumed:** `contracts/audio_events_v1.h` (M6).

**Status:** M0 stub. (miniaudio dependency = future ADR per Iron Rule 8.)
