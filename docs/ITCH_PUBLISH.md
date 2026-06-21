# Publishing to itch.io — steps + the two decisions to make first

The page copy is in [`ITCH_PAGE.md`](ITCH_PAGE.md). This file is the *how* — and two things you
should decide **before** you upload, because this is a public release.

---

## Decision 1 — what to ship: the full 11 GB bundle, or a slim build?

`dist\Backrooms\` is **~11 GB**, and **~10.4 GB of that is the bundled AI models** (the Qwen
language/vision models + Whisper). The game *plays fully without them* — on a weaker or
non-NVIDIA GPU the AI just stays quiet.

| | **Full bundle (~11 GB)** | **Slim build (~0.6 GB, no models)** |
|---|---|---|
| Download size | Large (fine via butler; heavy for a free grab) | Small, friendly |
| The AI presence / Director | ✅ works | ❌ silent (core game still plays) |
| Model-license question | Must verify first (see Decision 2) | Sidestepped entirely |

**My recommendation:** the AI *is* the hook in the page copy ("it watches you back"), so the
full bundle is the real experience — **ship it once the licenses are confirmed (Decision 2).**
If you'd rather a small, friction-free free download, ship the slim build and link the models
as an optional separate download. (I can produce a slim `package.ps1 -NoModels` variant on
request — it's not built yet.)

## Decision 2 — confirm the bundled models are OK to redistribute publicly  ✅ mostly done

`dist\Backrooms\models\` ships the two Qwen models + the Qwen vision projector + Whisper base.en.
I checked the GGUF metadata directly, and it's good news:
- **Both Qwen models declare `general.license = apache-2`** (Apache-2.0) in their own GGUF
  headers → **free to redistribute publicly with attribution.** `mmproj-F16` is the matching
  Qwen vision projector (same family/license).
- **Whisper base.en** (`ggml-base.en.bin`) — MIT. **llama.cpp / whisper.cpp / ggml** — MIT.
  **CUDA** runtime DLLs — redistributable under the CUDA EULA. All fine.

**Done for you:** I added the full license texts (the Apache-2.0 license + the MIT notice + the
CUDA note) to **`dist\Backrooms\licenses\THIRD-PARTY-LICENSES.txt`**, and wired `package.ps1` to
always stage it. The bundle is now license-complete for a public release.

**The one thing left to you (5 min, belt-and-suspenders):** open the Hugging Face page you
downloaded the Qwen GGUFs from and eyeball the model card says Apache-2.0 too (the GGUF metadata
already says so — this just double-confirms the source wasn't a restricted re-upload). If it's
clean, you're clear to ship the full bundle. If anything looks off, ship the slim build instead.

---

## The build is current; the old zip is NOT

- `dist\Backrooms\` was just rebuilt — it has the latest RT sampling + the F3/V perf knobs. **Use the folder.**
- `dist\Backrooms-portable.zip` is **stale** (dated before these changes). Don't upload it as-is.
  Either ignore it (butler doesn't need it) or regenerate it (below).

## Recommended: push the FOLDER with butler (itch's CLI)

For an 11 GB game the web uploader won't cut it — use **butler**. It also does cheap delta
patches, so future updates only upload what changed.

```powershell
# 1) Stop the bundled AI servers first, or they lock their own files:
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\keel-down.ps1

# 2) Install butler (once): https://itch.io/docs/butler/installing.html
#    (or: get it from itch.io, unzip, add to PATH)

# 3) Log in (opens a browser to authorize YOUR itch.io account):
butler login

# 4) Push the folder. Replace <your-handle> and the game slug:
butler push dist\Backrooms <your-handle>/backrooms:windows

#    "windows" is the channel name; itch tags it as a Windows download automatically.

# 5) Check status any time:
butler status <your-handle>/backrooms:windows
```

Then on itch.io: **Create a new project** → paste the page copy from `ITCH_PAGE.md` → set
**Pricing: No payment (free)** → check **Windows** → set the uploaded build as the Windows
download → mark it **playable / this file will be played on Windows** → Save & view → set the
visibility to **Public** when you're ready.

### If you prefer a downloadable zip instead of butler
Regenerate a *fresh* zip (the old one is stale), then upload via butler (web upload struggles
past ~1 GB):
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\keel-down.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\package.ps1 -SkipBuild   # reuses the current build, re-zips
```
…but for 11 GB, pushing the folder with butler is simpler and gives free patches. Prefer it.

---

## Pre-publish checklist

- [ ] **Decision 2 done** — model licenses confirmed + `LICENSE` files added to `licenses\`.
- [ ] **Clean-machine test** — copy `dist\Backrooms\` to a USB stick / another PC, unzip, and
      run `Backrooms.exe` to confirm it plays with nothing else installed (you've been playing
      it locally, so it works on *your* box; this just proves it's truly self-contained).
- [ ] **Page copy** pasted from `ITCH_PAGE.md`; title + tags chosen.
- [ ] **A few screenshots / a short clip** for the page (the RT mode in an open room reads well;
      itch shows the first image as the cover). The hover-preview on itch uses your screenshots.
- [ ] **Pricing set to free** (or name-your-own-price, $0 minimum).
- [ ] **Visibility** flipped to Public.

That's everything. The game is built, current, and self-contained; the page copy is written;
the only blockers are the two decisions above — both yours to make.
