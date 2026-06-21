# 04 — AI Integration & Packaging (Console-Free Unified Package)

> Thread: fourth pivot. Read-only investigation — no edits made.
> Repo state: HEAD = 90410c3 (ADR-076).
>
> Grounded by a 3-agent fan-out (raw findings in 05). The operator reports DOS/console windows
> pop up on startup. Root cause found; tiered integration plan proposed.

## The complaint

Starting Backrooms on Win11 + RTX works, but one or more black DOS/console windows pop up
during startup. Want them completely removed/hidden, and the AI parts (llama/keel/whisper)
embedded/streamlined/consolidated into one cohesive unified package — without reducing
functionality.

## Part 1 — Root cause of the console windows (definitively found)

The popup is **not the sidecars — it's the game exe itself.**

### Primary offender: `Backrooms.exe` is a CONSOLE-subsystem app

Three independent facts, all confirmed by source:

1. **Entry point is `int main(int argc, char** argv)`** — `app/src/main.cpp:5241`. No
   `WinMain`/`wWinMain` anywhere (grep returned zero hits).
2. **No subsystem is ever set.** `app/CMakeLists.txt:19` declares
   `add_executable(backrooms src/main.cpp src/hud.cpp version.rc)` with **no `WIN32_EXECUTABLE`
   property, no `/SUBSYSTEM:` linker flag, no `LINK_FLAGS`**. Repo-wide grep for
   `SUBSYSTEM|WIN32_EXECUTABLE|LINK_FLAGS|link_options` returned no matches.
3. **MSVC default**: `main` + no subsystem flag → `/SUBSYSTEM:CONSOLE`. Windows allocates a
   console window the instant the process starts, before `main()` runs. The console is then
   actively written to (`printf`/`fprintf(stderr,...)` throughout — `main.cpp:406,1055,1207,
   1249,1847` and dozens more), so it stays visible for the process lifetime.

**This is the black window.** It's the game's *own* console, not a sidecar's.

### The sidecars are already correctly hidden

Both `CreateProcess` sites already pass `CREATE_NO_WINDOW`:
- `launch_hidden_in_job` (`main.cpp:622-625`): `CREATE_NO_WINDOW | CREATE_SUSPENDED`, stdio →
  `logs\*.log`, kill-on-close job. Used by llama-server (`:669`) + keel-serve (`:674`).
- `whisper_transcribe` (`main.cpp:3303-3304`): `CREATE_NO_WINDOW`.

No code path launches a child without `CREATE_NO_WINDOW`. No `system()`, no `_popen`, no
`ShellExecute`, no `WinExec`. No runtime `.bat`/`.cmd`/`.ps1` invocations. **The sidecar
windows were already solved in ADR-076; the remaining window is the game's own.**

### Secondary offender (minor): the `RUN.cmd` launcher flash

`dist/Backrooms/RUN.cmd` is `start "" "%~dp0Backrooms.exe"`. The `cmd.exe` host interpreting
`start` can briefly flash a console. Only matters if launching via the `.cmd` rather than the
exe directly.

## Part 2 — The deeper integration gaps

Beyond the console, the current state is **three separate processes talked to over localhost
HTTP, sourced from four `C:\` dev installs, with no version/hash pinning.** That's the opposite
of unified.

- **Gap A:** The game links zero AI code; everything is out-of-process HTTP (ADR-038, deliberate).
  `app/CMakeLists.txt:23-26` links nothing AI; `director` links only `winhttp`.
- **Gap B:** keel-serve is a prebuilt Rust binary, **source not in this repo**. A thin HTTP
  router/proxy in front of llama-server. Loads no model. Staged from
  `C:\keel-sidecar-7071\` (debug snapshot, port-patched). Cannot be "linked in-process" from
  this repo — no source.
- **Gap C:** llama-server + whisper-cli are stock binaries from `C:\llama.cpp` / `C:\whisper.cpp`,
  not built in this repo.
- **Gap D:** No version/hash pinning. `keel.lock` pins versions (llama build b9627, CUDA 12.4)
  but **every `sha256` is `TODO`** (`keel.lock:14,15,20,26,34,39,86`). `package.ps1` does no
  hash verification. Bundle not reproducible from repo alone.
- **Gap E:** Blind `*.dll` copy (`package.ps1:73,81`), not the curated allowlist the proposal
  specified. Ships 13 `ggml-cpu-<arch>.dll` variants, `SDL2.dll`, spare tool DLLs.
  Non-deterministic; could miss a DLL.
- **Gap F:** `keel.lock` ships with vestigial `C:\` paths (`:54,58`). Safe only because keel
  probes `:8080` (already up) and never reads them; latent footgun.
- **Gap G:** No small/no-AI build variant. `package.ps1` has no `-NoAI` flag (`:12`); always
  stages 10.9 GB. But the runtime supports no-AI for free (INV-6: graceful no-op; raster needs
  no `dxcompiler.dll`; CRT statically linked → exe has zero external DLL deps for raster).
- **Gap H:** Models are 91% of the bundle (9.9 GB of 10.9 GB) and ship **both** tiers blindly.
  Every user downloads the tier they won't use. 4B tier ships text-only (no mmproj for it).
- **Gap I:** No installer, no signing, static version `2.0.0.0` (`version.rc:10-11`), no
  git-commit injection. Two bundles indistinguishable.
- **Gap J:** The bundle has **never been run end-to-end** on real hardware (Session 36: dev GPU
  TDR'd, smoke pending). "Structure-verified" = manual eyeball.

## Part 3 — The one decision that gates everything

Two viable end-states:

**Option 1 — Polished sidecar (recommended).** Keep the out-of-process HTTP architecture (it's
correct: deterministic sim, optional model, graceful no-op). Make it *invisible*: kill consoles,
ship one self-contained folder with a unified launcher, pin everything, auto-tier models, add a
real installer. User sees one exe, one window, zero consoles. Respects ADR-038. ~1–2 weeks.

**Option 2 — In-process llama, drop keel.** Link llama.cpp as a C++ lib directly into
`Backrooms.exe`. One process, one binary, zero IPC. But: (a) pulls ggml/llama/CUDA into `app`'s
link line (violates ADR-038's spirit), (b) keel-serve's source isn't here so its routing logic
needs reimplementing in C++, (c) couples model VRAM to the game process (model OOM crashes the
*game*, not a sidecar), (d) reopens the determinism surface. ~3–4 weeks, higher risk.

**Recommendation: Option 1.** It preserves every invariant that makes this build special while
delivering 100% of the *perceptible* integration. Option 2 buys ~5–10 ms IPC latency (imperceptible)
at the cost of re-litigating ADR-038.

## Part 4 — Tiered plan (assumes Option 1)

### Tier 1 — Kill the consoles (~1 day)

1. **Switch the exe to WINDOWS subsystem, keep `main`.** In `app/CMakeLists.txt`:
   ```cmake
   set_target_properties(backrooms PROPERTIES WIN32_EXECUTABLE TRUE)
   target_link_options(backrooms PRIVATE /ENTRY:mainCRTStartup)
   ```
   `/SUBSYSTEM:WINDOWS` + `/ENTRY:mainCRTStartup` → no console allocated, `main()` runs
   unchanged, `printf`/`fprintf(stderr)` become no-ops. **Single highest-leverage fix.** Zero
   code changes to `main.cpp`.
2. **Redirect diagnostic output to `logs\game.log`** in release (`BR_RELEASE`): `freopen`
   stdout/stderr, or `SetStdHandle`. ~5 LOC. Debug build keeps its console.
3. **Kill `RUN.cmd`** — once the exe has no console, users double-click `Backrooms.exe`
   directly. The `.cmd` exists only because the old console exe needed a launcher.

~15 LOC (CMake) + ~5 LOC (stdio redirect) + delete `RUN.cmd`. Gate-green (doesn't touch
sim/render/AI).

### Tier 2 — Consolidate and pin the runtime (~3–5 days)

1. **Pinned manifest + hash verification.** `runtime/MANIFEST.toml` listing every staged
   binary/model with `sha256` + source version. `package.ps1` verifies on copy, **fails on
   mismatch.** Closes Gap D. Fill in the `keel.lock` `sha256: TODO` fields.
2. **Curated DLL allowlist** (implement the proposal's §3 that was skipped). Explicit list per
   runtime; drop 13 `ggml-cpu-<arch>.dll` variants, `SDL2.dll`, tool DLLs. ~1.1 GB → ~1.05 GB
   and deterministic. Closes Gap E.
3. **Trim `keel.lock`** to a relative, probe-only substrate. Remove `C:\` paths and
   `launch_local`/`cloud_tier`/`embedded_tier` branches (never fire in shipped flow). Closes
   Gap F.
4. **Auto-tier the models** (ship one tier per bundle, not both). Three variants:
   - `Backrooms-Full-AI` (9B + mmproj + whisper, ~9 GB, ≥11 GB VRAM)
   - `Backrooms-Lite-AI` (4B + whisper, ~3 GB, 6–10 GB VRAM; optionally fetch Qwen3-VL-4B +
     mmproj to give it vision)
   - `Backrooms-No-AI` (exe + optional DXC pair, ~tens of MB, everyone else / the "control" build)
   Closes Gap G + H. No code change — existing `detect_vram_mb()` + `g_visionAvailable` handles
   whichever tier is present.
5. **`-NoAI` / `-Tier 9B|4B|none` switches** on `package.ps1`. Each bundle self-describing (own
   README/NOTICE variant).

### Tier 3 — The unified launcher experience (~3–5 days)

1. **Loading/state screen during sidecar warmup.** A first-frame overlay "WAKING THE FACILITY…"
   with subtle progress while `service_up(:7071)` polls. Turns invisible background work into
   *intentional atmosphere*. ~60 LOC, in the existing HUD overlay path.
2. **Health monitoring + auto-restart.** Background thread polls `:8080`/`:7071` every few
   seconds; if a sidecar dies (CUDA OOM), restart via `launch_hidden_in_job`. Silent degradation
   becomes self-healing. ~40 LOC. `try_start_sidecar` "fire-once" → "fire-once-then-monitor."
3. **System-tray icon (optional).** `Shell_NotifyIcon` showing "Backrooms + AI runtime", clean
   kill of the whole tree. ~80 LOC. Biggest perceived-quality jump for power users.
4. **Real installer (Inno Setup or MSIX).** `.iss` installing to `%LOCALAPPDATA%\Backrooms`
   (no admin) or `Program Files`; Start Menu + desktop shortcuts; uninstaller; code signing
   (kills SmartScreen); version from `git rev-parse --short HEAD` injected into `version.rc`.
   Closes Gap I. ~1 day for `.iss` + version-injection change.

### Tier 4 — Deep integration (Option 2 divergence, only if chosen)

1. **Vendor llama.cpp** as a CMake subproject; `app` links `llama`+`ggml`+`ggml-cuda`. Game
   calls `llama_model_load` + `llama_decode` directly. No `llama-server`, no `:8080`.
2. **Reimplement keel's routing** in `director` (~200 LOC C++ `LocalTierRouter`). Drop
   `keel-serve.exe`. No `:7071`.
3. **Keep whisper out-of-process** (infrequent, CPU-only, subprocess overhead negligible).
4. **Determinism guard.** Model load on a dedicated thread; all inference behind try/catch +
   CUDA-error-check; model failure flips `g_visionAvailable=false`, game continues (INV-6).
   Real engineering; most likely to introduce a non-determinism bug.

~3–4 weeks. ADR-038 reopened. Model-OOM-crashes-game risk. **Recommended against for v1.**

## Part 5 — The non-negotiable: run the bundle end-to-end

Gap J. Regardless of tier, before anything ships:

1. **Clean-machine test** (VM or fresh Windows, no `C:\llama.cpp`): copy folder, double-click
   `Backrooms.exe`, verify no consoles, game runs, AI warms up, Director speaks, creature
   thinks. The only test that catches "works on dev box, broken on user box."
2. **Apply the RT-crash fixes first** (see 02) — the bundle test hits the same RT path; don't
   debug two things at once.
3. **A `gate.ps1` addition** that stages the bundle and runs a 30s smoke from the staged folder
   (not from `build-release\bin`). The current gate never tests the *packaged* artifact.

## Recommended sequencing

1. **Tier 1 (console fix) — this week.** Smallest diff, kills the symptom, gate-green trivially.
   Independent of everything else.
2. **End-to-end bundle smoke test — alongside Tier 1.** Apply RT-crash fixes first so the smoke
   isn't confounded.
3. **Tier 2 (consolidate + pin) — next.** Manifest + hash pinning + curated DLLs + auto-tier +
   no-AI variant. ~3–5 days.
4. **Tier 3 (unified launcher experience) — after Tier 2.** Loading screen, health monitoring,
   installer, signing. ~3–5 days.
5. **Tier 4 (in-process llama) — only if Option 2 chosen, after Tiers 1–3 stable.** Shelve for v1.

## The headline

The consoles are the game's own CONSOLE-subsystem exe — **one CMake line + an entry-point flag
fixes it**, and the sidecars were already hidden by ADR-076. The deeper "unified integration"
is *not* about linking llama into the exe (that breaks the invariants) — it's about making the
correct sidecar architecture invisible: kill the consoles, pin the runtime to a hashed manifest,
auto-tier the models, add a themed loading screen + real installer. The architecture stays
decoupled (determinism sacred, AI optional, graceful no-op); the *presentation* becomes one
window, one exe, zero popups. That's the higher-caliber package — mostly packaging craft, not
architecture change.

## Key file:line references

- Entry point (CONSOLE offender): `app/src/main.cpp:5241`
- No subsystem set: `app/CMakeLists.txt:19`
- `launch_hidden_in_job` (already correct): `app/src/main.cpp:612-631`
- `whisper_transcribe` (already correct): `app/src/main.cpp:3295-3313`
- `try_start_sidecar`: `app/src/main.cpp:638-676`
- Exe-relative path resolvers: `app/src/main.cpp:559-587`
- VRAM detection + tier: `app/src/main.cpp:590-605`, `:650-653`
- `RUN.cmd` launcher: `scripts/package.ps1:123-126`
- Packager: `scripts/package.ps1`
- `keel.lock` (TODO hashes, vestigial C:\): `dist/Backrooms/runtime/keel/keel.lock`
- keel client (HTTP): `director/src/keel_client.cpp:32-137`
- ADR-038 (deliberate decoupling): `docs/DECISIONS.md`
- ADR-076 (the packaging commit): `docs/DECISIONS.md:445-451`, `docs/SESSION_LOG.md` top entry
- Packaging proposal: `docs/PACKAGING_PROPOSAL.md`
