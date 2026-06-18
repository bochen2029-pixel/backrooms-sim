# Backrooms Sim — User Guide

*Version 2.0 — playable & packaged.*

Backrooms Sim is an infinite, never-repeating walk through the **Backrooms**: endless
mono-yellow rooms, buzzing fluorescent lights, damp carpet, and the hum of empty space.
There is no win condition, no enemies, no inventory. It is a place to *be* — a liminal
visualization where **every wall, light, sound, and frame is generated from a number**.
There are no asset files anywhere in the game.

---

## 1. Installing & launching

### The portable build (recommended)
1. Unzip `backrooms-portable.zip` anywhere (Desktop, a USB stick — it's self-contained).
2. Double-click **`RUN.cmd`** (or run `backrooms.exe --game`).

That's it — no installer, no admin rights, no Windows SDK. The DirectX shader compiler
(`dxcompiler.dll` / `dxil.dll`) is bundled in the folder, so the path-traced mode works
on a clean machine. Your settings are saved to `backrooms.cfg` next to the executable.

### Building from source
See the project [`README.md`](../README.md). In short: Visual Studio 2022 + the Windows
11 SDK, then `scripts/build.ps1`, then `scripts/run.ps1 -Window`.

---

## 2. Controls

| Action | Keyboard / Mouse | Gamepad (XInput) |
|---|---|---|
| Move | `W A S D` | Left stick |
| **Run** | Hold `Shift` | Hold left trigger / LB |
| Look | Mouse | Right stick |
| Jump | `Space` | `A` |
| Pause / Back | `Esc` | `Start` |
| Menu navigate | Arrow keys / `W S` | (Start to open) |
| Menu select | `Enter` / `Space` | `A` |
| Adjust a setting | `← →` | — |
| Toggle fullscreen | `F11` | — |
| Toggle ray tracing | `F2` | — |
| Toggle flashlight (ray tracing only) | `F` | — |
| Quit | Quit from the menu, or close the window | — |

Mouse-look is captured while you walk and released in menus. Mouse sensitivity is a
setting (below).

---

## 3. The menus

- **Splash** → press any key to enter the **Main Menu**.
- **Main Menu**
  - **New Game** — start a fresh descent. `← →` here cycles the **world seed** (the seed
    determines the entire infinite maze — same seed, same world, forever).
  - **Continue** — resume the session you were last in (greyed out until you've started one).
  - **Settings** — see below.
  - **Quit** — exit.
- **Pause** (`Esc` mid-walk) — Resume, Settings, or Quit to Menu.
- **Settings** — `↑ ↓` to choose a row, `← →` to change it, `Esc`/Back to return:
  - **Master** — overall volume (0–100).
  - **SFX** — footstep / effect volume (0–100).
  - **Mouse** — look sensitivity (0–100).
  - **Director** — turn the optional local-LLM "Director" on/off (see §6).
  - **Ray Tracing** — toggle real-time **path-traced lighting** (DXR). Off by default;
    needs an RTX / DXR GPU. Softer, more physically realistic light + shadows. Also toggled
    in-game with **F2**; press **F** for a flashlight (a cone of light, ray tracing only).
  - **AI Model** — choose the local LLM tier: **AUTO** (picked from your VRAM — the default),
    **9B VISION** (the larger model; the Director/creature can *see* your screen), or **4B TEXT**
    (the smaller, lighter model — text only, no vision). **Applies on restart** (the model loads
    when the game next launches its AI sidecar).
  - **Back** — return.

All settings (plus resolution, fullscreen, and the last seed) **persist** across launches
in `backrooms.cfg`.

---

## 4. Modes (command line)

`backrooms.exe` is one executable with several modes. The default experience is `--game`,
but the same engine powers a set of deterministic capture/utility modes:

| Mode | What it does |
|---|---|
| `--game` | The windowed game shell (menus → walk). **The default.** |
| `--play` | Jump straight into the walk (skip the menu). |
| `--intro` | The iconic **noclip into the Backrooms** — stand in a mundane room, the floor gives way, free-fall, land in Level 0. |
| `--dxr-pt --out p.png` | **Photo mode (path-traced):** a converged, ray-traced still with soft global illumination. Needs a DXR-capable GPU (RTX). |
| `--shot --out p.png` | Photo mode (raster): a fast lit still from a fixed pose. |
| `--topdown --out p.png` | A top-down debug map of the maze. |
| `--render-wav --out a.wav` | Render the procedural audio of a walk to an offline `.wav`. |
| `--credits` | Print the credits / about text. |
| `--version` | Print the version. |

Common options: `--seed S` (the world seed), `--width W --height H` (resolution),
`--seconds N` (auto-exit after N seconds), `--no-audio`, `--master 0.5 --sfx 0.8`,
`--config path.cfg`.

**Examples**
```
backrooms.exe --game --seed 1234
backrooms.exe --dxr-pt --seed 7 --spp 512 --width 1920 --height 1080 --out room.png
backrooms.exe --intro --seed 42
backrooms.exe --render-wav --seed 9 --ticks 7200 --out walk.wav
```

---

## 5. Photo mode

Because the world is **fully deterministic**, any capture is perfectly reproducible: the
same `--seed`, pose, and tick always produce the same pixels. `--dxr-pt` gives the
prettiest results (true ray-traced lighting, soft shadows, color bleed from the yellow
walls); `--shot` is the fast rasterized look with the VHS post-processing (grain,
scanlines, chromatic aberration, vignette) that gives the game its found-footage feel.

The five canonical poses (`--pose 0..4`) frame a room forward, right, back, up at the
ceiling lights, and down at the carpet.

---

## 6. The Director (optional)

Backrooms Sim can host a **local Large Language Model** as a "Director" that subtly shapes
the *presentation* of your walk (ambience, biome bias, the occasional textual note). It is
**off by default** and entirely optional.

- It runs **locally** through the **KEEL sidecar** (an OpenAI-compatible endpoint on
  `http://127.0.0.1:7071`) — nothing leaves your machine, and it costs nothing.
- It is **presentation-only**: the Director can never change the world geometry or the
  deterministic simulation. A recorded walk **replays bit-for-bit identically with the
  model switched off** — the LLM is a flavor layer, never the source of truth.
- If the sidecar isn't running, the Director is simply a no-op; the game plays normally.

Enable it via the **Settings → Director** toggle, or `--director` on the command line.

---

## 7. System requirements

- **OS:** Windows 10 or 11, 64-bit.
- **GPU:** any Direct3D 12 GPU for the game and raster modes; an **RTX / DXR Tier 1.1**
  GPU for the path-traced `--dxr-pt` mode.
- **Disk:** a few MB (no assets).
- **No** internet, account, or installer required.

---

## 8. Troubleshooting

- **The path-traced mode says it can't find `dxcompiler.dll`.** Make sure you're running
  the exe from inside the unzipped folder (the bundled `dxcompiler.dll` / `dxil.dll` must
  sit next to `backrooms.exe`). `RUN.cmd` handles this for you.
- **No sound.** Check the Master/SFX settings aren't at 0; try `--no-audio` to confirm the
  rest runs, then re-enable. The game falls back silently if no audio device is present.
- **The window is the wrong size / stuck fullscreen.** Press `F11` to toggle fullscreen, or
  delete `backrooms.cfg` to reset all settings to defaults.
- **It launched into a menu but the mouse is gone.** That's expected in the walk —
  mouse-look captures the cursor. Press `Esc` to pause and free the cursor.

---

## 9. What *are* the Backrooms?

The Backrooms are an internet legend: if you "noclip" out of reality in the wrong place,
you fall into a place of endless, randomly segmented empty rooms — the smell of old moist
carpet, the buzz of fluorescent lights at maximum hum, and the feeling that you are very
alone and not quite alone. This simulation renders that idea literally and without end:
**Level 0**, generated forever, never repeating, deterministic to the last photon.

## 10. …and you are not alone

Something else walks the Backrooms with you. A **Shoggoth** — a warm, writhing, radial mass of
tentacles — lurks in the maze, and if you wander too close it will **hunt you**, routing the
corridors to close the distance. It's driven by its own deterministic navigation (and, in time, an
AI brain of its own). You can't fight it. You can only keep moving.

There is no exit. That's the point. Walk a while — but listen for what's behind you.
