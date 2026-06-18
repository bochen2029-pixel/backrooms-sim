# Bringing KEEL Inside Backrooms — how the local LLM went from an external C:\ install to a fully self-contained, deterministic, no-window part of the game

> A narrative + technical record of how the local-LLM "Director/creature brain" substrate (KEEL) was
> integrated into Backrooms and then pulled *completely internal* — no `C:\` dependency, no DOS windows,
> plug-and-play, and without ever breaking the engine's bit-exact replay. Grounds: ADR-038, 039, 047, 048,
> 049, 076, 078; `scripts/keel-up.ps1`; `app/src/main.cpp::try_start_sidecar`; `director/keel_client.h`.

---

## 1. What KEEL is, and the purpose

**KEEL** is a *sovereign, single-operator LLM substrate* — the operator's own project that runs a local model
(Qwen3.5 via llama.cpp/CUDA) on the same box/GPU and exposes an **OpenAI-compatible HTTP egress**. Backrooms is
designated KEEL's **first "cell"** (its first real consumer / dogfood).

The purpose of routing Backrooms' AI through KEEL — rather than embedding an LLM directly — was threefold:

1. **Sovereignty + $0 + privacy.** All inference is local. No cloud egress, no API keys, no per-token cost, no
   data leaving the machine. The game's "Director" narrator and the AI "Shoggoth" creature think on the
   player's own GPU, offline, forever.
2. **Dependency *removal*, not addition.** The original M11 plan embedded llama.cpp as a vcpkg dependency (a
   multi-GB GGUF fetch, a CUDA build coupling, an in-process model thread, model-selection logic). Routing over
   a process boundary to KEEL deleted all of that from the build. **Catch2 + stb remain the only third-party
   libraries in the whole project**, and the "minimal deps / no asset files" rule is honored better than
   embedding ever could.
3. **A stable boundary that insulates the game from churn.** KEEL talks `serve_openai` over localhost HTTP; KEEL
   can refactor its own internals freely behind that boundary without touching Backrooms.

---

## 2. The integration: a thin HTTP client over a process boundary (ADR-038, M11)

The game does **not** link an LLM. Instead, the `director/` module is:

- a **thin async HTTP client** (`director/keel_client.h`) built on **WinHTTP** — a Windows *system* import lib,
  so **no new vcpkg dependency** (the same way the project already used `dxc`/`dbghelp`);
- a **dependency-free JSON reader** (hand-written, unit-tested);
- a **schema validator** (rejects + logs malformed model output).

It reaches the model over localhost HTTP at **`http://127.0.0.1:7071/v1/chat/completions`** — a collision-proof,
local-only copy of KEEL's `keel-serve` (deliberately *not* KEEL's volatile dev endpoint on `:7070`). Requests
carry `sovereign:true, kind:"scaffolding", think:false`, which makes KEEL's router **force the local Qwen tier**
(no cloud, $0, single-shot, p95 well under 5 s).

Two calls back the whole AI surface (a shared private `keel_post` underneath both):

- **`keel_complete(host, port, prompt, timeout)`** — text completion (the Director's `WandererSummary →
  Directive`; the Shoggoth's `ShoggothSummary → ShoggothIntent`).
- **`keel_complete_vision(host, port, prompt, image_base64, timeout)`** — the same call with the user turn
  carrying a `[text, image_url(data:image/png;base64,…)]` array, routed by KEEL to the **local vision tier**
  (qwen-VL + `mmproj-F16`).

If KEEL is unreachable, every consumer is a **graceful no-op** (identical to `--no-director` / the kill switch).

---

## 3. The hard part: determinism stayed sacred (INV-4)

The engine's defining invariant is **bit-exact replay** — a recorded session must reproduce frame-for-frame.
An LLM is *stochastic*. Reconciling the two is the crux of the whole integration, and the rule is:

> **KEEL runs at RECORD time only.** Its validated output enters the deterministic simulation **as a logged
> event-log entry at a fixed effective-tick** — never as a continuous coupling. A **replay consumes the
> recorded log with the model fully offline → bit-identical.** The model lives *only* in the log.

Concretely, for the creature (ADR-047, M21): the brain builds a `ShoggothSummary` → `keel_complete` → a
**validated** `ShoggothIntent` → that intent is appended to the event log as a `ShoggothEvent` at its tick →
the cheap, deterministic BFS navigator does the actual per-tick walking. `--shoggoth-record` (brain on, live
KEEL) and `--shoggoth-replay` (model **off**, replaying the logged intents) produce the **identical combined
hash** — the project's "sacred gate." Vision (ADR-049) and hearing (M23) work the same way: the POV snapshot /
audio transcript is taken at record time, fed to KEEL, and only the resulting validated intent is logged; the
snapshot/transcript is never re-derived on replay. For the Director (M11) it's even simpler: directives are
presentation-layer only, so the sim hash is Director-independent by construction.

This is why the LLM — the least deterministic thing imaginable — sits inside a strictly deterministic engine
without ever threatening a single gate.

---

## 4. Async isolation: off the frame thread (INV-6, ADR-039/048)

A blocking multi-second local inference on the 120 Hz frame thread would freeze the game. So every live consumer
(`DirectorHost`, `ShoggothBrainHost`, `ShoggothVisionHost`, `DirectorChatHost`) is a header-only host with a
**background worker thread**, a **non-blocking latest-wins `submit()`** (≤ 1 inference in flight, always
reasoning about the present), a **non-blocking `poll()`** that drains validated results, and a graceful no-op
when KEEL is down. The frame loop submits a fresh summary on an ambient wall-clock cadence (~3 s) and applies
any returned intent **at a tick boundary** — the same "intent is a discrete, timestamped event" shape the
headless sacred gate proves bit-exact.

ADR-039 recorded a subtle truth this surfaced: KEEL's inference and the D3D12 renderer **share one GPU**, so
live generation shifts the *render* frame-time baseline (GPU contention) — but the async-isolation *invariant*
(the sim/frame thread never blocks) holds: p99/median stays ≈ 1.0× ON vs OFF, and with inference unreachable
the baseline returns. It's resource contention, not a stall.

---

## 5. One client, many "cells"

The same `keel_complete` / `keel_complete_vision` pair, reused verbatim, powers the entire AI surface — proof
that the thin-client boundary was the right shape:

- **The Director** — narrates the level (text, M11; later vision-grounded narration of the player's actual
  rendered frame).
- **The Shoggoth brain** — chooses the creature's intent (M21), live while you play (M21b).
- **The Shoggoth's eyes** — a rendered POV → qwen-VL → intent (M22, and live in-game).
- **The Shoggoth's ears** — soundscape → whisper.cpp → tag → intent (M23).
- **Two-way voice** — your mic → whisper → KEEL → the procedural PA voice answers.

No consumer added a dependency; each is a few dozen lines over the shared client.

---

## 6. Bringing it fully internal — the packaging journey (ADR-076 → ADR-078)

Originally KEEL lived **outside** the repo: `C:\keel-sidecar-7071\start.cmd` launched it, llama.cpp was at
`C:\llama.cpp`, models at `C:\models`, whisper at `C:\whisper.cpp`. That's fine for the operator's dev box but
useless for a portable, shippable game. Pulling it internal happened in two ADRs:

### ADR-076 — the self-contained bundle
1. **Exe-relative resolution.** `bundled_w/a(rel, fallback)` returns `<exe-dir>\rel` if it exists. The shipped
   bundle has `runtime\{llama,keel,whisper}\` + `models\` sitting beside the exe, so it resolves *everything to
   itself*.
2. **A hidden, job-managed launcher** replaced the `.cmd`/PowerShell start scripts: `CreateProcessW` with
   **`CREATE_NO_WINDOW`** under a **`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`** job held for the game's lifetime — so
   **no console windows ever pop up**, and llama-server/keel-serve **die when the game exits or crashes**. stdio
   is redirected to `logs\`.
3. **Topology + idempotency.** **`llama-server :8080` starts FIRST** (it serves the raw model); **`keel-serve
   :7071` starts second and reuses `:8080` read-only** (its `keel.lock`'s `resolver_order` probes :8080). Each
   launch is skipped if the port already answers (`director::service_up`, a WinHTTP `/health` GET) — so the dev
   gates, a manually-started sidecar, and the game never double-launch.
4. **VRAM auto-tier.** `detect_vram_mb()` (DXGI dedicated VRAM): `≥ 11 GB` → 9B-Q5 + `mmproj` (vision tier);
   else 4B-Q4 (text tier); unknown → 9B. A `g_visionAvailable` flag gates the vision features; both tiers are
   fully functional.
5. **`scripts/package.ps1`** stages `dist\Backrooms\` (exe + DXC + all llama/whisper CUDA DLLs + keel + the four
   GGUF models + licenses) and zips it store-mode (~10.9 GB; the GGUFs are incompressible). KEEL itself isn't
   forked — a **clean release `keel-serve.exe` + a backrooms-tuned `keel.lock` (config, not code)** are vendored
   in; nothing to backport.

### ADR-078 — closing the last `C:\` leaks ("nothing outside C:\backrooms, ever")
The bundle was self-contained, but the *dev tree*, the *packager*, and the *lock* still reached `C:\`:
- `bundled_w/a`'s fallback pointed at the `C:\` installs when no runtime sat beside the exe (the dev exe in
  `build[-release]\bin` has none) → re-pointed to the **in-repo `dist\Backrooms`** bundle (`..\..\dist\Backrooms`),
  never `C:\`. A hardcoded `C:\models\…` whisper path → `default_whisper_model()`.
- **`scripts/keel-up.ps1` + `keel-down.ps1`** were added as the in-tree replacement for
  `C:\keel-sidecar-7071\start.cmd`: they bring the **bundled** sidecar up from `dist\Backrooms\runtime`, so the
  dev gates/tests connect to an already-running `:7071` and never touch `C:\`. (`gate.ps1`'s seven stale `C:\`
  hints were re-pointed here too.)
- `package.ps1` now treats `runtime\` + `models\` + DXC as **persistent in-repo assets** (refresh only the exe,
  verify the rest is present, never source from `C:\` or the Windows SDK).
- `keel.lock`'s vestigial `C:\` paths → bundle-relative.

Proven entirely from `C:\backrooms`: `keel-up.ps1` brought up llama + keel from the bundle, and the sacred
record→replay produced **`valid_intents=5`** (the LLM really ran) with **record == replay bit-identical** — the
full determinism gate, with zero external `C:\`.

---

## 7. How it runs today

```
  ┌─────────────────────────────── dist\Backrooms\  (or build\bin via keel-up.ps1) ──────────────────────────┐
  │                                                                                                           │
  │  Backrooms.exe ──WinHTTP POST──▶  keel-serve.exe  :7071  ──reuses read-only──▶  llama-server.exe  :8080   │
  │   (director/keel_client.h)        (/v1/chat/completions,    (keel.lock router,    (Qwen3.5 9B-Q5 +mmproj  │
  │   keel_complete[_vision]          OpenAI-compatible)         local-tier, vision)    OR 4B-Q4, CUDA/VRAM)   │
  │                                                                                                           │
  │  launched hidden in a kill-on-close Job Object; servers die with the game; logs -> logs\                  │
  └───────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

- **In the shipped game**, `try_start_sidecar()` fires once, idempotently, on first AI need — VRAM picks the
  tier (overridable now via the **Settings → AI MODEL** toggle: AUTO / 9B-vision / 4B-text), launches
  llama-server then keel-serve hidden + job-managed, and is a graceful no-op if the runtime is missing
  (deterministic AI still works; only live generation is skipped).
- **On the dev box / for gates**, `scripts\keel-up.ps1` brings the *same bundled* sidecar up by hand (the dev
  exe has no co-located `runtime\`), and `keel-down.ps1` stops it cleanly. Never loop-kill it — repeatedly
  killing the CUDA `llama-server` has TDR'd the dev GPU.

---

## 8. The payoff

- **Sovereign, offline, $0, private** AI — Director narration + a thinking/seeing/hearing/speaking creature, all
  on the player's GPU.
- **Fully portable**: one folder, plug-and-play on Win10/11 + RTX; **no `C:\` dependency, ever**; **no DOS
  windows, ever**; servers self-start hidden and die on exit.
- **Determinism never broke**: the LLM is a record-time generator behind an event-log; replays are bit-exact
  with the model offline — the sacred gate is green throughout.
- **The build stayed minimal**: routing through a process boundary *removed* a heavy dependency rather than
  adding one; Catch2 + stb are still the only third-party libraries.

In short: the AI is as local, private, and self-contained as the procedurally-generated world it inhabits — and
just as deterministic on replay.
