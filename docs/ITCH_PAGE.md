# itch.io page — paste-ready copy

> Paste the sections below into itch.io's page editor. itch.io supports a rich-text editor
> (bold/headings/lists) — paste as plain text and apply formatting, or use the Markdown
> toggle. Replace `<your-handle>` and pick a title. Suggested tags + classification are at
> the bottom (those go in itch.io's metadata fields, not the description body).

---

## TITLE (pick one)

- **BACKROOMS ∞**  ← recommended (keeps the searchable keyword + signals "infinite")
- **BACKROOMS: NOCLIP**
- **NOCLIP — An Infinite Backrooms**
- **THE HUM** (mono-yellow, the fluorescent buzz — more arthouse, less searchable)

## TAGLINE (the short one-liner under the title)

> An infinite, never-repeating Backrooms — with a local AI that watches you back.

---

## SHORT DESCRIPTION (itch.io's ~140-char summary field)

> Fall into an endless, procedurally-generated Backrooms. Real-time ray tracing. A local AI presence that sees, narrates, and hunts. No two walks alike.

---

## PAGE BODY (paste into the main description)

You noclip out of reality and fall into **Level 0**: endless mono-yellow rooms, damp carpet,
and a ceiling grid of buzzing fluorescent lights. The maze unfolds ahead of you forever and
closes behind you. It **never ends, and it never repeats.**

Every wall, doorway, stain, footstep, and reverb tail is generated from math — **there are no
asset files.** No textures, no meshes, no audio clips. A tiny seed contains an entire infinite
world, so no two walks are ever the same.

### It watches you back

This isn't a static maze. Bundled inside is a **local AI** — running entirely on your own GPU,
fully offline, nothing ever leaving your machine:

- **A Director that sees your screen.** A vision-capable language model literally looks at what
  you're looking at and murmurs over the PA — and you can **speak back into your mic** and it
  answers, aloud and subtitled.
- **A presence that thinks.** Something shares the levels with you. It has a real, live AI
  brain — it **sees** through its own eyes, **hears** the soundscape, **speaks** in an
  impressionistic, half-formed voice, and **hunts** by what it perceives, not by a script.
- **It sees what isn't there.** The AI reads faces, figures, and words in the procedural grime
  that *neither you nor the engine placed* — and reacts to them. Sometimes you'll both be
  unsure whether something was really there.

Because it's all emergent — driven by perception, not scripted triggers — it behaves a little
differently every single time.

### Built from light

With a ray-tracing GPU you can switch to a **path-traced mode**: the yellow walls bleed warm
light into the corners, the fluorescents cast soft true shadows, and the whole place feels
lit by real photons. Drop **green chemlight flares** as breadcrumbs; click on a **flashlight**
for the dark sublevels. A **VHS layer** — grain, scanlines, chromatic aberration, a soft
vignette — wraps everything in a found-footage haze.

### What this is (and isn't)

This is an **experience**, not a win/lose game. There's no score, no objective, no inventory.
You walk; the world unfolds; the lights hum; something else is down here with you. The dread
is architectural — and lately, a little more personal.

### Features

- ♾️ **Infinite, never-repeating procedural world** — every wall, light, and sound from a seed
- 🧠 **A local AI presence** — it sees your screen, narrates, talks back to your voice, and hunts
- 👁️ **Emergent "apparition sense"** — the AI perceives faces/figures in the grime that no one placed
- 💡 **Real-time DXR ray tracing** (optional) — path-traced light, soft shadows, flares, flashlight
- 🔊 **100% procedural audio** — the hum, footsteps, room reverb, even the PA voice are synthesized
- 🔒 **Fully offline & private** — all AI runs locally on your GPU; no internet, no telemetry, no accounts
- 📦 **Portable, no install** — unzip and double-click; everything it needs is in the folder

### Controls

| | |
|---|---|
| **WASD / Arrows** | Move |
| **Mouse** | Look |
| **Shift** | Run · **Space** Jump · **Esc** Pause |
| **F11** | Fullscreen |
| **F2** | Toggle ray tracing |
| **F3** | Ray-tracing quality (Quality / Balanced / Performance) |
| **V** | Vsync on/off |
| **F** | Flashlight (in RT) · **R** Drop a flare (in RT) |

> **Running slow with ray tracing on?** Press **V** to uncap the frame rate and **F3** to lower
> the ray-tracing resolution. Together they're a big speed-up, especially at 4K. The image
> sharpens as you hold still.

### System requirements

- **OS:** Windows 10 / 11 (64-bit)
- **GPU:** any Direct3D 12 card to play; an **NVIDIA RTX** card for ray tracing
- **For the local AI** (Director & the creature's mind): an NVIDIA RTX GPU + a recent driver.
  The AI auto-selects by VRAM — **12 GB+** loads the larger vision model, less loads a smaller
  one. **With a weaker or non-NVIDIA GPU the game still plays fully — the AI simply stays quiet.**
- First launch spends ~20–40 s loading the AI in the background before it speaks.

### How to run

1. Download and **unzip** anywhere.
2. Double-click **`Backrooms.exe`**. That's it — no installer, no setup, no dependencies.
3. Everything (the renderer, the audio, the local AI models) lives in the folder.

### A note on the AI

All of the intelligence runs **locally, on your own machine.** Nothing you see, say, or do is
ever sent anywhere — there's no server, no account, no tracking. The bundled models are why
the download is large; they're what let the thing in the dark actually think.

---

## CREDITS (put at the bottom of the page, or in a devlog)

Built from scratch in C++ / Direct3D 12 + DXR. The world, textures, audio, and the PA voice are
100% procedural — no asset files.

Bundled open technology that makes the offline AI possible:
- **Qwen** language/vision models — Alibaba/Qwen *(verify the exact model + license before release)*
- **llama.cpp** + **ggml** — MIT (ggerganov)
- **whisper.cpp** + base.en model — MIT (ggerganov; OpenAI Whisper, MIT)
- **DirectX Shader Compiler** — Microsoft, MIT
- **NVIDIA CUDA** runtime redistributables — per the CUDA EULA

See `licenses/NOTICE.txt` and `CREDITS.txt` in the build.

---

## itch.io METADATA (the fields, not the body)

- **Classification:** Games
- **Kind of project:** Downloadable
- **Pricing:** **No payment (free)** — or "Name your own price" with a $0 minimum if you want optional tips
- **Platform:** Windows (check the Windows box; upload the build as the Windows download)
- **Genre:** Adventure (closest fit) — or Simulation
- **Suggested tags** (itch allows up to ~10): `backrooms`, `horror`, `atmospheric`, `liminal`,
  `procedural-generation`, `walking-simulator`, `ray-tracing`, `singleplayer`, `3d`, `ai`
- **Inputs:** Keyboard, Mouse (and Gamepad)
- **Average session:** A few minutes — about an hour
- **Release status:** Released (or "Prototype"/"In development" if you want to set expectations)
- **Community:** Comments on (recommended for a free release)
