# ⏸ REHYDRATE — read this FIRST (reconstitution pointer)

> Written by the session that lived the context (Bo's perpetual-memory doctrine, Procedure A), at ~89%
> context before a planned fresh-session handoff. **Trust the files + git over this summary. Verify with
> the commands below. Never recall blind.** Date stamped: 2026-06-18.

## 0. Read order, then VERIFY (do this before any work)
1. This file.
2. `docs/SESSION_LOG.md` — newest entry (the auto-rehydration hook also injects it).
3. `docs/CHANGE_AUDIT_LOG.md` — the per-change ledger **E0–E10** (what changed, why, rollback, verification).
4. Memory: `C:\Users\user\.claude\projects\C--backrooms\memory\project-rt-crash-fix.md` (master thread) +
   `feedback-working-mode.md` (how to work here).
5. As needed: `docs/SHOGGOTH_PLAN.md` (the plan), `docs/DECISIONS.md` ADR-077/078, `docs/ARCHITECTURE.md`.

**VERIFY reality (disk wins over this doc):**
```
git -C C:\backrooms log --oneline -12          # HEAD should be 1ac1fcf (or later)
git -C C:\backrooms status --short             # clean except ?? _brainstorm/ (+ now _run_state files)
powershell -NoProfile -ExecutionPolicy Bypass -File C:\backrooms\scripts\audit.ps1   # expect: ctest 109/109, record==replay, inventory+isolation green
powershell -NoProfile -ExecutionPolicy Bypass -File C:\backrooms\scripts\keel-up.ps1 # self-contained sidecar :8080+:7071 BEFORE any LLM-gated work
```
Raw-transcript backstop (grep, don't re-read whole): `C:\Users\user\.claude\projects\C--backrooms\159009d0-ac22-4528-bdb0-53a845d3463b.jsonl` — and the archive viewer `C:\TRANSPORTER\claude_archive_viewer_v4.html` (Ctrl-K concept-search).

## 1. CORE — where we are + the single next action
**Project:** C:\backrooms — native Win32 C++20 D3D12+DXR Backrooms sim; local-LLM AI "Shoggoth" creature.
**State:** all green @ HEAD `1ac1fcf`, pushed. ctest **109/109**, determinism record==replay, no D3D12 errors,
inventory+isolation clean. **No DOS windows** (release exe = /SUBSYSTEM:WINDOWS). **KEEL fully self-contained**
(runs from `dist\Backrooms` via `scripts\keel-up.ps1`; nothing outside C:\backrooms is ever needed).
**The Shoggoth is immersive + LIVE in-game:** it THINKS (live LLM brain ~3 s), SPEAKS (impressionistic
murmurs via `speak_pa` when <6 m, Phase E), HUNTS, and its VISION drives motion (`resolve_target`,
determinism-proven in record/replay).

**THE SINGLE NEXT ACTION → live in-game PIXEL-vision render.** Make the creature reason from a *rendered*
POV live (vs its current live text-sense). It needs: a **2nd headless D3D12 device** in `run_game` (note
`render_chunks`/`readback` are headless-only — see `render_d3d12/include/.../renderer.h`), feeding a new
`ShoggothVisionHost` (mirror `app/src/director_vision.h`; the prompt+parse already exist:
`render_shoggoth_vision_prompt` in `shoggoth_vision.h` + `parse_shoggoth_intent` in `shoggoth_brain.h`).
**The hard part = chunk-upload management:** the headless record path uploads in a ~400-iteration loop
(fine offscreen, a STALL in the live game). Do it with **incremental/budget-spread uploads + a frame-time
check** — do NOT bolt on a hitchy render. ADR-077 (validation layer forced on) makes the 2nd device viable
(the RT path already runs dual-device). Verify with a windowed `--game` smoke (no crash + the host produces
intents) + `audit.ps1`. It's additive/revertable.

## 2. RING 1 — DON'T ASSUME (avoid these wrong moves)
1. **KEEL is self-contained.** `scripts\keel-up.ps1` runs llama :8080 + keel :7071 from `dist\Backrooms`.
   NEVER reference `C:\llama.cpp` / `C:\keel-sidecar-7071` / `C:\models` / `C:\whisper.cpp` — closed (ADR-078).
2. **M30 `game mouse-look` gate red is tool-session cold-start** (1 frame @2s, 473 @9s; `lookcheck: PASS`),
   NOT a regression — passes on the operator's foreground GPU. Do NOT "fix" it or widen the gate window.
3. **Live pixel-vision is NOT in-game yet** (that's the next action). Vision *drives motion* via
   `resolve_target` (proven record/replay); the creature reasons from its text-sense live. Don't claim it sees pixels live.
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
GLM brainstorm `_brainstorm/GLM/` (00,02,03,04,05 read; **01 RT-perf UNREAD**). Also pending: Phase C.2 (threaded
`KeelBroker` + host integration + soak — needs KEEL up); live cheap-tier hearing (Phase F); Escape polish (Phase G —
`resolve_target` already does Stairs = the yearning); public-release D3D12 Agility SDK (so the validation-layer fix
works on a clean end-user Win11 without Graphics Tools); re-stage the bundle exe before any itch.io push.

## 5. Working mode (full detail in feedback-working-mode.md)
Rapid · agentic · autonomous · **don't pause/ask for confirmation** on in-scope work · cut to the chase + execute +
verify · log EVERY change to `CHANGE_AUDIT_LOG.md` · take backups (tags/zip) when needed · NO C:\ deps · NO DOS
windows · per-step self-audit. The operator gets frustrated with "going in circles" — be decisive, finish things.

## 6. Anticipated questions a fresh agent will ask → answers
- *"Is the live vision render done?"* No — it's THE next action (§1). Vision drives motion (proven); the live render is pending.
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
