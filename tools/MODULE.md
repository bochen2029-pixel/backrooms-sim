# MODULE: tools (dev)

**Purpose.** Developer/gate executables. Not part of the shipping app.

| Tool | Purpose | Milestone |
|---|---|---|
| `hashdiff` | image hash + perceptual diff CLI | M0 |
| `goldgen` | deterministic synth + golden capture (sole /goldens writer, INV-8) | M0 |
| `walkbot` | autonomous wanderer for soak/connectivity tests | M4 |
| `contactsheet` | tile periodic screenshots for visual review | M10 |
| `wavcheck` | FFT/RMS analysis of offline WAV renders | M6 |

**Depends on:** varies. `hashdiff`/`goldgen` use `core` + isolated `br_stb`
(stb_image/stb_image_write) for image I/O. `wavcheck` links `audio` for the
header-only `audio/wav.h` reader; its FFT is self-contained.

**Status:** M10 — `hashdiff`, `goldgen`, `wavcheck`, `contactsheet` implemented
(`contactsheet <shots_dir> <out.png>`: tiles `shot_*.png` into a grid + flags
all-black/all-white frames by mean luma; exit 1 on a degenerate frame). The
`walkbot` wanderer lives inline in `app` (`--walkbot`/`--soak`), not a separate tool.
