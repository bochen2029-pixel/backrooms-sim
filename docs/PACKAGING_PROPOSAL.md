# Proposal — Self-Contained, Portable Backrooms (single ZIP, plug-and-play on Win11 + RTX)

**Goal (operator):** one self-contained download that unzips and plays on any Windows 11 + NVIDIA RTX machine
with **zero setup** — no `C:\models\`, `C:\whisper.cpp\`, `C:\KEEL\`, `C:\llama.cpp\`; no HuggingFace downloads;
no DOS/console windows popping up; the whole LLM/VLM/whisper stack bundled and abstracted away inside the game.
Host it on itch.io. The game must still **play even with no GPU / old driver** — the AI is an enhancement, not
a requirement (it already degrades to deterministic AI when the sidecar is down).

**Acceptance criteria.** (1) Copy the folder to a fresh Win11+RTX box, double-click → menu → play, and the
Director/creature-brain/hearing all work with no manual steps. (2) No console windows ever appear. (3) Process
Explorer shows `llama-server`/`keel-serve` as children that **die when the game exits** (no orphans). (4) No
absolute `C:\…` path anywhere in the running system. (5) `git grep "C:\\\\"` in `app/` returns nothing.

---

## 1. What's blocking portability today (all confirmed in code/disk)

| Blocker | Where | Fix (see §4) |
|---|---|---|
| Sidecar launcher is an external `.cmd` at a hardcoded path | `main.cpp:546` `C:\keel-sidecar-7071\start-all.cmd` | §4a launch servers directly, exe-relative |
| `start-all.cmd` hardcodes `C:\llama.cpp` + `C:\models` | the .cmd file | §4a — eliminate the .cmd entirely |
| whisper exe/model defaults are absolute | `main.cpp:2968/3070/3294` `C:\whisper.cpp\whisper-cli.exe`, `:2969/3073/3295` `C:\models\ggml-*.bin` | §4a exe-relative defaults |
| `keel.lock` pins `C:\llama.cpp`, `C:\whisper.cpp`, `C:\models` | `runtime/keel/keel.lock` | §4c probe-and-use :8080 (paths become vestigial) |
| Servers spawn **visible** (minimized) windows via `Start-Process` | `start-all.cmd` | §4b `CREATE_NO_WINDOW` + Job Object |
| Orphaned servers survive a game crash | `try_start_sidecar` fire-and-forget | §4b Job Object `KILL_ON_JOB_CLOSE` |
| DXR shader compile needs `dxcompiler.dll`/`dxil.dll` (not bundled — works only because they're on the dev box) | not in `build/bin` | §4d bundle them |
| Model/whisper/llama/keel live **outside** the game | `C:\…` | §2 bundle under the exe |

---

## 2. Target bundle layout (the portable root — this is what ships)

```
Backrooms/                         <- zip this folder, or `butler push` it
  Backrooms.exe                    <- no-arg double-click = play
  dxcompiler.dll  dxil.dll         <- DXR shader compiler (redistributable; see §4d)
  runtime/
    llama/      llama-server.exe + the ~12 needed DLLs (see §3)        (~1.1 GB)
    keel/       keel-serve.exe + keel.lock (portable) + .keelstate/    (~13 MB)
    whisper/    whisper-cli.exe + whisper.dll + ggml-base.dll +
                ggml-cpu.dll + libopenblas.dll                          (~51 MB)
  models/
    Qwen3.5-9B-Q5_K_M.gguf   (6.13 GB)   <- LLM (vision-capable pair below)
    mmproj-F16.gguf          (0.86 GB)   <- Qwen-VL vision projector
    ggml-base.en.bin         (0.14 GB)   <- whisper (PA-voice transcription)
  licenses/   Qwen, llama.cpp(MIT), whisper.cpp(MIT), OpenAI-Whisper(MIT), CUDA-EULA, KEEL
  logs/       <- hidden servers' stdout/stderr go here (not to a window)
```

Everything is resolved **relative to `GetModuleFileNameW(exe)`** at runtime. No registry, no env vars, no `C:\`.

---

## 3. Component inventory + sizes (measured on disk)

**llama runtime — bundle ~1.1 GB of `C:\llama.cpp` (NOT the whole 1.75 GB):** `llama-server.exe`,
`ggml-cuda.dll` (539 MB — the CUDA kernels), `cublasLt64_12.dll` (452 MB), `cublas64_12.dll` (95 MB),
`cudart64_12.dll`, `llama-server-impl.dll`, `llama-common.dll`, `llama.dll`, `mtmd.dll` (multimodal — needed for
vision), `ggml-base.dll`, `ggml.dll`, `libomp140.x86_64.dll`, and **one** CPU fallback (`ggml-cpu-haswell.dll`).
Skip the other `ggml-cpu-*` arch variants and the bench/quantize/cli tools.

**whisper runtime ~51 MB:** `whisper-cli.exe`, `whisper.dll`, `ggml-base.dll`, `ggml-cpu.dll`, `libopenblas.dll`
(48.8 MB). Whisper runs on CPU — no CUDA needed. Model `ggml-base.en.bin` (140 MB; already on disk) is the
research-recommended sweet spot (vs the 1.5 GB large-v3-turbo the dev box uses — overkill for a PA-word tag).

**keel ~13 MB:** `keel-serve.exe` (12.9 MB, a self-contained Rust binary) + a portable `keel.lock`.

**models (the bulk):** vision-capable **Qwen3.5-9B-Q5_K_M (6.13 GB) + mmproj-F16 (0.86 GB)**, or text-only
**Qwen3.5-4B-Q4_K_M (2.55 GB, no mmproj on disk → no vision)**.

**Total ZIP:** **~8.3 GB** (9B + vision) or **~3.8 GB** (4B text-only). itch.io + `butler` handle both (see §6).

---

## 4. Engineering work-items

### 4a. Exe-relative path resolution (kill every `C:\`)
Add a one-liner root resolver (`app`): `GetModuleFileNameW` → strip filename → the bundle root. Derive all
paths from it: `runtime/llama/llama-server.exe`, `models/Qwen3.5-9B-Q5_K_M.gguf`, `models/mmproj-F16.gguf`,
`runtime/keel/keel-serve.exe`, `runtime/whisper/whisper-cli.exe`, `models/ggml-base.en.bin`. Replace the three
hardcoded whisper defaults (`main.cpp:2968/2969/3070/3073/3294/3295`) and the launcher path (`:546`). The
`--director-url http://127.0.0.1:7071` default is a localhost port — already portable.

### 4b. Hidden, job-managed server launch (no windows, no orphans) — replaces `start-all.cmd`
Rewrite `try_start_sidecar` into an in-process launcher (no `.cmd`, no PowerShell, no `Start-Process`):
1. **Create a Job Object** with `JOBOBJECT_EXTENDED_LIMIT_INFORMATION.BasicLimitInformation.LimitFlags |=
   JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`; keep the handle for the game's lifetime → the servers die on exit/crash.
2. **Probe `:8080`**; if down, `CreateProcessW(runtime/llama/llama-server.exe -m models/… --mmproj models/… --host
   127.0.0.1 --port 8080 -ngl <auto> -c 8192)` with **`CREATE_NO_WINDOW | CREATE_SUSPENDED`**, stdout/stderr →
   `logs/llama.log` (pipes, `STARTF_USESTDHANDLES`), `AssignProcessToJobObject`, then `ResumeThread`.
3. **Probe `:7071`**; if down, same for `runtime/keel/keel-serve.exe keel.lock` (cwd `runtime/keel/`). Start llama
   FIRST so keel only **probes-and-uses** :8080 (never managed-launches with a stale path — see §4c).
4. The existing per-request retry already makes the LLM a graceful no-op until the model finishes loading (~20–40 s).

Microsoft pattern (create-suspended → assign → resume) prevents the child escaping the job. `CREATE_NO_WINDOW`
is the correct flag for a console exe (no console host window at all).

### 4c. keel.lock portability
keel `probe_running` already lists `http://127.0.0.1:8080/v1`. Because §4b starts llama **first**, keel finds and
reuses :8080 and **never reads `llama_cpp.path`/`models_dir`** — so the `C:\…` lines in `keel.lock` are vestigial
in the shipped flow. To be safe + clean: ship a trimmed `keel.lock` whose substrate is **probe-only** (drop
`launch_local`, or rewrite the paths relative). Verify keel-serve starts + serves a directive with the trimmed
lock. (KEEL is the operator's own project, so the `keel.lock` schema is theirs to adjust.)

### 4d. Bundle the DXC shader compiler
The DXR path `LoadLibrary`s `dxcompiler.dll` (+ `dxil.dll` for signing). They're absent from `build/bin` →
the game only path-traces today because the dev box has them on PATH. Copy both next to `Backrooms.exe`
(redistributable from the DirectX Shader Compiler / Windows SDK). Raster mode doesn't need them; without them the
DXR toggle should fail gracefully to raster (it already has a fallback).

### 4e. Graceful degradation + a friendly driver check
On launch, if `llama-server` can't init CUDA (driver < **551.61**, no RTX, or VRAM OOM), the game must still play
on deterministic AI (it already does — the sidecar being down is a no-op). Add a one-time, non-blocking notice
("Local AI needs an NVIDIA driver ≥ 551.61; running without narration") rather than a crash. No `nvcuda.dll`
bundling (it's the driver's, non-redistributable).

---

## 5. Model strategy (the real tradeoff — needs an operator decision)

VRAM = weights + mmproj + KV-cache + the game's own D3D12/DXR (~1–2 GB on the SAME GPU).

| Tier | Files | VRAM | 8 GB cards (3070/4060) | 12 GB (4070+) | Vision? |
|---|---|---|---|---|---|
| **9B-Q5 + mmproj** | 6.13 + 0.86 GB | ~8–8.5 GB | ⚠️ tight/OOM with the game | ✅ comfortable | ✅ yes |
| **4B-Q4** (no mmproj on disk) | 2.55 GB | ~4 GB | ✅ easily | ✅ | ❌ no vision |

- The **VLM Director feature you just asked for needs a Qwen-**VL** model + its mmproj** → effectively the 9B
  pair, which sets a **~12 GB VRAM floor** (RTX 4070 and up). On 8 GB cards the 9B+vision spills to system RAM and
  crawls.
- **Recommended:** ship the **9B-Q5 + mmproj** as the headline (your 4070 + the vision feature both want it), and
  **auto-pick `-ngl`/tier by detected VRAM** — full offload on ≥12 GB, and on ≤8 GB either reduce offload or fall
  back to the 4B-text tier (download a 4B-**VL** GGUF if vision-on-8GB matters). A two-tier bundle (4B-VL + 9B-VL,
  auto-select at launch) maximizes "any RTX" reach at +2.5 GB download.
- Quantizing mmproj to Q8_0 (Qwen-VL ships it) shaves ~0.4 GB for the 8 GB tier.

---

## 6. Licensing (clearable — bundle the texts)
- **Qwen weights:** Qwen3/Qwen3-VL dense + VL GGUFs are **Apache-2.0** (redistribution OK, incl. commercial).
  **Action:** confirm the exact file's HF `license:` tag reads `apache-2.0` (some Qwen2.5 SKUs use the custom Qwen
  license); ship its `LICENSE`+`NOTICE`. (The dev box's `Qwen3.5-9B` — verify its card before shipping.)
- **whisper.cpp (MIT)** + **ggml whisper model** (OpenAI Whisper is MIT) — redistributable; ship MIT texts.
- **llama.cpp (MIT)** — redistributable; ship MIT.
- **CUDA cudart/cublas/cublasLt DLLs** — CUDA EULA Attachment A permits redistribution **embedded in an app** (not
  as a standalone lib); ship the CUDA EULA in `licenses/`. Do **not** bundle `nvcuda.dll` (driver-owned).
- **KEEL** — operator's own project; per its sidecar README, ship a **clean release build** of keel-serve (not the
  byte-patched debug snapshot) + whatever license the operator chooses.

## 7. Distribution (itch.io)
- The web uploader caps ~2 GB → **use `butler`** (no practical size cap; "will let you upload a 32 GB game").
- **Push the unzipped folder**, not a monolithic ZIP — butler diffs file-by-file, so code-only patches re-upload
  ~nothing of the 7 GB of models. One `windows` channel; itch builds the patches.
- Put the **download size + "RTX, 12 GB VRAM recommended, driver ≥ 551.61"** on the store page.

## 8. Packaging script (`scripts/package.ps1`, new)
`build.ps1 -Clean -Release` → stage `dist/Backrooms/`: copy `Backrooms.exe` + `dxcompiler.dll`/`dxil.dll`; copy
the §3 llama DLL allowlist → `runtime/llama/`; `keel-serve.exe`+trimmed `keel.lock` → `runtime/keel/`; the whisper
allowlist → `runtime/whisper/`; the chosen GGUFs + `ggml-base.en.bin` → `models/`; license texts → `licenses/`;
create `logs/`. Optionally `Compress-Archive` or invoke `butler push dist/Backrooms you/backrooms:windows`. The
source models/llama/keel/whisper still live at `C:\…` on the dev box; the script *copies* from there — nothing in
the shipped tree points back.

---

## 9. Phased plan
- **P0 — prove the runtime model locally (no bundle yet).** Implement §4a (exe-relative paths) + §4b (hidden
  Job-Object launch), but pointed at a local `runtime/`+`models/` you populate by copying the C:\ stuff into
  `build/bin/`. Verify: no windows, servers die on exit, Director works, `git grep C:\\` clean. *This is the core
  code change and is independently shippable.*
- **P1 — `package.ps1` + the bundle** (§3, §8): produce `dist/Backrooms/`, run it from a *different* folder to
  prove no `C:\` dependency.
- **P2 — robustness:** DXC bundling (§4d), VRAM auto-tier + driver check (§4e, §5), `licenses/` (§6).
- **P3 — ship:** `butler` channel + store page (§7). Test on a second RTX machine (ideally an 8 GB card to find
  the VRAM cliff).

## 10. Open decisions for the operator
1. **Model:** 9B-Q5+vision only (headline, ~8.3 GB, 12 GB-VRAM floor) · 4B text-only (~3.8 GB, runs on 8 GB, **no
   vision**) · **both tiers auto-selected** (~10.8 GB, best reach)? *(Vision Director needs a VL model — see the
   TODO_director_vision note.)*
2. **whisper model:** `base.en` (140 MB, recommended) vs `small.en` (466 MB, more accurate) vs the current
   large-v3-turbo (1.5 GB, overkill)?
3. **keel.lock:** confirm a probe-only/relative lock works, or have the operator regenerate a clean release
   keel-serve + lock for shipping.
4. Ship-time: confirm the **Qwen license tag** on the exact GGUF before public distribution.
