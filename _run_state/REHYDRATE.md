# ⏸ REHYDRATE — read this FIRST (reconstitution pointer)

> Written by the session that lived the context (Bo's perpetual-memory doctrine, Procedure A), at ~89%
> context before a planned fresh-session handoff. **Trust the files + git over this summary. Verify with
> the commands below. Never recall blind.** Date stamped: 2026-06-18.

## 0. Read order, then VERIFY (do this before any work)
1. This file.
2. `docs/SESSION_LOG.md` — newest entry (the auto-rehydration hook also injects it).
3. `docs/CHANGE_AUDIT_LOG.md` — the per-change ledger **E0–E25** (what changed, why, rollback, verification). E22–E25 = the RT-perf bundle (ghost/A/B/C) + the stop-here decision.
4. Memory: `C:\Users\user\.claude\projects\C--backrooms\memory\project-rt-crash-fix.md` (master thread) +
   `feedback-working-mode.md` (how to work here).
5. As needed: `docs/SHOGGOTH_PLAN.md` (the plan), `docs/DECISIONS.md` ADR-077/078, `docs/ARCHITECTURE.md`.

**VERIFY reality (disk wins over this doc):**
```
git -C C:\backrooms log --oneline -12          # HEAD should be ad6683b (RT sampling step 2) or later; newest tag rt-sampling-green
git -C C:\backrooms status --short             # clean except ?? _brainstorm/ + ?? _staged_* backup dirs (all applied+committed; removable)
powershell -NoProfile -ExecutionPolicy Bypass -File C:\backrooms\scripts\audit.ps1   # expect: ctest 116/116, record==replay, inventory+isolation green
powershell -NoProfile -ExecutionPolicy Bypass -File C:\backrooms\scripts\keel-up.ps1 # self-contained sidecar :8080+:7071 BEFORE any LLM-gated work
```
Raw-transcript backstop (grep, don't re-read whole): `C:\Users\user\.claude\projects\C--backrooms\159009d0-ac22-4528-bdb0-53a845d3463b.jsonl` — and the archive viewer `C:\TRANSPORTER\claude_archive_viewer_v4.html` (Ctrl-K concept-search).

## 1. CORE — where we are + the single next action
**Project:** C:\backrooms — native Win32 C++20 D3D12+DXR Backrooms sim; local-LLM AI "Shoggoth" creature.
**State:** all green @ HEAD `ad6683b` (newest tag `rt-sampling-green`; apparition arc + Phase C.2 KeelBroker + RT sampling steps 1-2 complete), pushed. ctest **116/116**, determinism record==replay,
no D3D12 errors, inventory+isolation clean. **No DOS windows** (release exe = /SUBSYSTEM:WINDOWS). **KEEL fully
self-contained** (runs from `dist\Backrooms` via `scripts\keel-up.ps1`; nothing outside C:\backrooms is ever needed).

**RT SAMPLING shipped (Session 43, tag `rt-sampling-green`, ledger E30-E31).** From the operator's Coding-Adventure
brainstorm. **Step 1** (`6eca645`): free temporal AA (sub-pixel primary-ray jitter, accumulation resolves edges) +
a NaN/Inf accumulator-poison guard; gated by a new `render_pt_frame(aa=false)` arg (uFrame high bit), golden-safe.
**Step 2** (`ad6683b`): **stochastic single-light NEE (RIS)** — pick ONE ceiling light by a weighted reservoir + one
shadow ray instead of the full 5x5 grid (~6-9 rays), at the primary hit + GI bounce; **unbiased, proven by the new
`--dxr-stoch` oracle wired into gate M9** (err excess 0.0898 vs the noise floor). INTERACTIVE-ONLY (uFrame bit 30);
offline/gate paths use full NEE → goldens within epsilon. The shadow-ray cut is largest in light-dense OPEN rooms (the
operator's pain point — to feel at their fullscreen settings). Rollback: tag `pre-rt-sampling` `cdb28f1`. Optional next
RT levers (GI-NEE cut, SVGF) in `docs/RT_PERF_PLAN.md`. **Release exe is stale** — rebuild via `package.ps1 -StageOnly`.

**RT PERFORMANCE FIXED (Session 40, tag `rtperf-green`, ledger E22–E25).** The operator's "RT unplayable — too slow +
the Shoggoth ghosts into a quantum-superposition blend" is resolved: **ghost** (material-7 history reject, `0c8e0b3`) +
**A** the per-frame cross-device readback killed (single-device `present_pt_texture`, `0f3da13`) + **B** the frame
pipelined (denoise folded into the accumulate list + per-frame AS → `PREFER_FAST_BUILD`, `e072e8a`, tag `rtperf-green`) +
**C** the creature BLAS refit in place (`ALLOW_UPDATE`+`PERFORM_UPDATE`, mesh is writhe-stable, `df97807`). Each
gate-M9-green (goldens bit-identical, denoiser 0.362, interactive PT ~173 FPS), `audit.ps1` PASS, live `--game --rt`
~116–123 fps debug-clean, ghosting gone. **DECISION: stopped here — D/E/SVGF deferred as measured-optional** (Step-0
diagnosis was ~80% stalls [fixed by A+B+C] / ~20% ray cost; the remaining items are ray-cost cuts that change the
converged lighting → need interactive-only two-path + an unbiasedness oracle, integrator surgery not justified on an
already-playable scene; E's skip-denoise-when-converged was prototyped + reverted — creature-always-1-spp noise). Next
lever IF still slow at the operator's real settings: interactive-only stochastic direct lighting (RIS) + convergence
test. See `docs/RT_PERF_PLAN.md` + ledger E25. Rollback: tag `pre-rtperf` `0644ef8`.
**The Shoggoth's immersive arc is COMPLETE + LIVE in-game:** it THINKS (live LLM brain ~3 s), SEES (live rendered POV,
Phase D LIVE), SPEAKS (murmurs via `speak_pa`, Phase E), HUNTS (vision→`resolve_target` motion), and now **SEES WHAT
ISN'T THERE** — the apparition sense (Phase H): emergent pareidolia (a face/figure/word/arrow in the procedural grime
that neither player nor engine placed), read by the VLM on the creature's POV (Phase 1, gate-proven) AND on **the
PLAYER's own POV** (Phase 2a) → when YOU see a face the **PA murmurs**, the **soundscape thins**, and the **lights SAG**
(Phase 2b visual dim, raster + RT, both gate-green), all scaled by `app_strength`. The apparition arc is COMPLETE.

**Phase D LIVE shipped (the prior "single next action" — DONE).** `app/src/shoggoth_vision_host.h` (`ShoggothVisionHost`,
off-thread qwen-VL eye → validated intent) + a **2nd headless `Renderer` (384×216)** in `run_game` rendering
`shoggoth_pov_camera`; the upload-stall solved by a **budget-spread warm window** (24 frames × 16 meshes via
`render_chunks`'s `upload_budget` — no hitch). Text-brain apply preserves the 4 perception fields `resolve_target` reads
so the 3 s text brain can't clobber the seen target. Verified: `audit.ps1` green + live `--game` smoke svision **2/2/2**,
**debug_error_count=0 across 3 concurrent D3D12 devices**, 3763 frames/35 s (~107 fps, no stall), lookcheck PASS. Ledger E11.

**THE OPEN NEXT PICKS (operator's call).** Two big arcs just COMPLETED:
- **Apparition (Phase 1 · 2a · `app_strength` · 2b raster+RT; ADR-082/083/083a/084):** when you see a face the PA
  murmurs, the room thins, and the lights sag, on both render paths, all gate-green.
- **Phase C.2 — the KeelBroker (ADR-085, E28/E29, tags `phaseC2a-broker` / `phaseC2b-hosts`):** the 4 app-level LLM
  hosts (brain/shoggoth-vision/director-vision/chat) now route through a threaded `KeelBroker` wrapping
  `keel_scheduler.h` (priority · single multimodal slot · cap · latest-wins · `try_admit`). 150 s live soak: none
  starved, debug-clean, ~127 fps, clean exit (no deadlock). The text `DirectorHost` (br::director) stays unarbitrated
  (module-dependency direction) — a deferred dependency-free-gate follow-up.

Strong candidate now: **PLAYTEST** — RT is fast AND the full immersive arc is in (apparition sight/sound/voice/light +
the arbitrated LLM backend). Tunables: apparition `maxDim`/`apparitionWindowS` in `main.cpp` (two sites raster+RT, keep
in sync). Other open threads: the rest of the **LLM-mutation palette** (wire recolour LIVE into the voice loop +
lighting/spawn/doors), **live hearing** (Phase F), the **itch.io push** (`butler push dist\Backrooms`, confirm the Qwen
GGUF HF license), the deferred C.2 follow-ups (route the text DirectorHost; a broker telemetry counter), or call it done.

## 2. RING 1 — DON'T ASSUME (avoid these wrong moves)
1. **KEEL is self-contained.** `scripts\keel-up.ps1` runs llama :8080 + keel :7071 from `dist\Backrooms`.
   NEVER reference `C:\llama.cpp` / `C:\keel-sidecar-7071` / `C:\models` / `C:\whisper.cpp` — closed (ADR-078).
2. **M30 `game mouse-look` gate red is tool-session cold-start** (1 frame @2s, 473 @9s; `lookcheck: PASS`),
   NOT a regression — passes on the operator's foreground GPU. Do NOT "fix" it or widen the gate window.
3. **Live pixel-vision IS now in-game** (Phase D LIVE, tag `phaseD-live`, `577eef1`). A 2nd headless device renders the
   creature POV in `run_game` → `ShoggothVisionHost` → intent → `resolve_target`. Verified svision 2/2/2, 3 devices
   debug-clean, no stall. The text brain still drives between the sparse ~25 s vision frames (it preserves the seen target).
4. **Determinism is sacred:** record==replay is THE proof; `ShoggothEvent` = `SHOGLOG2`, padding-free
   (`static_assert(sizeof==40)`); `resolve_target` gated by `target_kind != None` (default None → byte-unchanged →
   M21/M29 gates intact); `utterance` is voice-only (never hashed/serialized). `shoggoth_prey()` is the shared
   M29 prey-offset across ALL record paths.
5. **Release exe = Windows-subsystem (no console); DEBUG stays console** so the gates capture stdout. Release-only
   `WIN32_EXECUTABLE`. Don't unify them.
6. **Per-step cadence:** run `scripts\audit.ps1` pre+post each step; append a verdict to `CHANGE_AUDIT_LOG.md`.
   `gate.ps1 -Milestone M<N>` is the milestone gate; `audit.ps1` is the fast between-gates oracle. Never self-assert
   "done" — let an oracle say so (Externality Principle).

## 3. RING 2 — completed this arc (terse; verify from git/ADRs)
RT instant-crash FIXED (ADR-077: force the D3D12 validation layer on in all builds). Shoggoth **Phase A** (feature-aware
idle + Flank fix), **B** (semantic-schema bump SHOGLOG2, behaviour-neutral), **C-core** (pure `keel_scheduler.h`
arbitration + 6 property tests), **D-core** (`resolve_target` vision→cell), **E LIVE** (the voice). M29 prey-offset
fixed in all record paths + gated. **ADR-078** self-contained AI runtime (no C:\) + console fix. Adopted `scripts\audit.ps1`
+ the per-step audit cadence. Tags: `pre-phaseA phaseA phaseB phaseC-core`; backups `C:\backrooms_backups\`.

## 4. RING 3 — pointers / pending
GLM brainstorm `_brainstorm/GLM/` (00–05 all read; **01 RT-perf READ + Tier 1 temporal accumulation APPLIED E12 `af186f6`** — fixed the operator's noisy+slow playtest report; Tiers 2–4 optional, measure first). Also pending: Phase C.2 (threaded
`KeelBroker` + host integration + soak — needs KEEL up); live cheap-tier hearing (Phase F); Escape polish (Phase G —
`resolve_target` already does Stairs = the yearning); public-release D3D12 Agility SDK (so the validation-layer fix
works on a clean end-user Win11 without Graphics Tools); re-stage the bundle exe before any itch.io push.

## 5. Working mode (full detail in feedback-working-mode.md)
Rapid · agentic · autonomous · **don't pause/ask for confirmation** on in-scope work · cut to the chase + execute +
verify · log EVERY change to `CHANGE_AUDIT_LOG.md` · take backups (tags/zip) when needed · NO C:\ deps · NO DOS
windows · per-step self-audit. The operator gets frustrated with "going in circles" — be decisive, finish things.

## 6. Anticipated questions a fresh agent will ask → answers
- *"Is the live vision render done?"* YES — Phase D LIVE shipped (`577eef1`, tag `phaseD-live`, ledger E11). The creature
  SEES live in `run_game` (svision 2/2/2, 3 devices debug-clean, no stall). The next action is now Phase C.2 (§1).
- *"How do I get KEEL/the LLM up?"* `scripts\keel-up.ps1` (self-contained). `keel-down.ps1` to stop. Never C:\.
- *"Why is M30 red?"* Tool-session cold-start, not a regression (§2.2). Don't touch it.
- *"Can I just summarize git to catch up?"* Read §1–§4 + run the §0 verify commands; then act. The CHANGE_AUDIT_LOG E0–E10 has the why.
- *"Is it safe to change shoggoth_step / the schema?"* Only with determinism care — run `audit.ps1` (record==replay) after; the schema is SHOGLOG2 padding-free (§2.4).

## 7. Verbatim tail (operator intent, near word-for-word)
- "make it fully portable first, i dont want any DOS windows popping up ever again, then proceed directly to making
  the ai shoggoth fully immersive ... stop going in circles ... cut to the chase and just execute, rapidly, agentic,
  autonomous, stop asking me questions or pausing, finish it"
- "earlier we packaged KEEL and llamacpp and whisper all self contained within backrooms, did you forget? ... there
  should be no reason to keep to use C:\keel-sidecar-7071 ... or anything outside of c:\backrooms ever again"
- (doc 04) "if the integration itself isn't wired correctly, you must propose a fix and fix that first, i don't ever
  want to continue by ever needing ANYTHING outside of C:\backrooms EVER again"
- (this handoff) "preserve fully this moment in time, so that i can start new session and not have to rexplaination
  tax nor have it forget things we already established etc etc etc or assume things incorrectly"
