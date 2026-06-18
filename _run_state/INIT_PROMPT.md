# BACKROOMS SIM — NEW-SESSION INITIALIZATION PROMPT
(Paste this whole block into a fresh session. It is intentionally exhaustive — better too much than too little.
A companion, always-on-disk version lives at `C:\backrooms\_run_state\INIT_PROMPT.md`; the canonical
cold-start pointer is `C:\backrooms\_run_state\REHYDRATE.md`.)

---

You are **Claude Code**, resuming an autonomous, long-running, milestone-by-milestone build of **Backrooms
Sim** at **`C:\backrooms`** — a native Windows, C++20, **Direct3D 12 + DXR** procedurally-generated, infinite,
never-repeating "Backrooms" walking simulation with a local-LLM/VLM-driven AI creature ("**the Shoggoth**").
It is a *demonstration/visualization, not a game* (no win state, no combat, **no asset files — everything is
procedural**). A prior session (37–38) did large amounts of verified work. This prompt plus the on-disk
artifacts carry the full state so you start with **zero re-explanation tax and make no incorrect assumptions.**

You are powered by Opus 4.8 (1M context). The operator is **Bo Chen**. Default to **autonomous, decisive
execution** — see WORKING MODE below.

═══════════════════════════════════════════════════════════════════════════════
## STEP 0 — MANDATORY BOOTSTRAP (do ALL of this before writing a single line of code)
═══════════════════════════════════════════════════════════════════════════════
1. **Read `C:\backrooms\_run_state\REHYDRATE.md` IN FULL.** It is the reconstitution pointer: read-order,
   verify-commands, the single next action, the don't-assume list, REEL-ringed state, verbatim operator intent.
2. **Read** `C:\backrooms\docs\SESSION_LOG.md` (newest entry) and `C:\backrooms\docs\CHANGE_AUDIT_LOG.md`
   (entries **E0–E10** — the append-only per-change ledger: what changed, why, files, rollback, verification).
3. **Read the harness memory** (auto-loaded, but read deliberately):
   `C:\Users\user\.claude\projects\C--backrooms\memory\project-rt-crash-fix.md` (the master thread) and
   `feedback-working-mode.md` (how to operate here).
4. **VERIFY REALITY — never trust this prompt or any recalled summary blindly; disk + git are the source of
   truth (the Externality Principle):**
   ```
   git -C C:\backrooms log --oneline -15
   git -C C:\backrooms status --short
   powershell -NoProfile -ExecutionPolicy Bypass -File C:\backrooms\scripts\audit.ps1
   ```
   - HEAD should be **`577eef1`** or later (tag `phaseD-live`). Working tree clean except `?? _brainstorm/` (+ `_run_state/` notes).
   - `audit.ps1` MUST print: **build ok · ctest 109/109 · shoggoth record==replay · inventory ok · isolation ok.**
     If it is NOT green, STOP — do not "debug forward." Reconcile to the last green tag (Iron Rule 2) and re-audit.
5. **Before ANY LLM / vision / sacred-gate work**, bring up the SELF-CONTAINED sidecar:
   ```
   powershell -NoProfile -ExecutionPolicy Bypass -File C:\backrooms\scripts\keel-up.ps1   # llama :8080 + keel :7071 from dist\Backrooms
   ```
   Stop it cleanly (once, never loop-kill) with `scripts\keel-down.ps1`.
6. Only after the above is green do you proceed. If anything conflicts with this prompt, **the files/git win.**
7. **Raw-transcript backstop** (for any detail the distillation dropped — grep, don't re-read whole):
   the prior session `.jsonl` under `C:\Users\user\.claude\projects\C--backrooms\`, plus the archive viewer
   `C:\TRANSPORTER\claude_archive_viewer_v4.html` (Ctrl-K concept search).

═══════════════════════════════════════════════════════════════════════════════
## CURRENT STATE (where we are — all verified GREEN at HEAD `577eef1`, tag `phaseD-live`)
═══════════════════════════════════════════════════════════════════════════════
- **Build/tests:** clean build (warnings-as-errors), **ctest 109/109**, D3D12 debug layer clean, determinism
  record==replay, module inventory matches ARCHITECTURE.md, core isolation (INV-5) clean.
- **RT/ray-tracing instant-crash: FIXED** (ADR-077). Root cause: a D3D12 device created *without* the validation
  layer is device-REMOVED the instant a windowed FLIP swapchain is in play; release builds had no validation.
  Fix: force `EnableDebugLayer()` + DXGI debug factory + DRED on in **all** builds (no-op without the SDK
  layers). Release renders RT default + 4K + raster, debug-clean. A live-RT smoke guards M30.
- **NO DOS WINDOWS** (ADR-078 / doc-04 Tier 1): the **release** exe is `/SUBSYSTEM:WINDOWS`
  (`WIN32_EXECUTABLE` + `/ENTRY:mainCRTStartup`, **release-only** so the DEBUG build stays console-subsystem and
  the gates keep capturing stdout). Sidecars already launch `CREATE_NO_WINDOW`. `RUN.cmd` removed.
- **FULLY SELF-CONTAINED AI RUNTIME — nothing outside `C:\backrooms`, ever** (ADR-078). The bundle was
  self-contained (ADR-076); this closed the dev-tree + packager + lock leaks: `main.cpp` `bundled_w/a` fall back
  to the in-repo `dist\Backrooms` (`..\..\dist\Backrooms`), never C:\; `scripts\keel-up.ps1`/`keel-down.ps1` run
  the bundled sidecar; `package.ps1` treats runtime/models/DXC as persistent in-repo assets (refresh exe only,
  never C:\/SDK); `keel.lock` C:\ paths → bundle-relative. PROVEN: sacred record→replay `valid_intents=5` +
  record==replay, entirely from C:\backrooms.
- **The Shoggoth's immersive arc is COMPLETE + LIVE in the playable game:** it **THINKS** (live LLM brain ~every 3 s),
  **SEES** (live rendered POV — Phase D LIVE, `577eef1`/`phaseD-live`/E11: a 2nd headless `Renderer` renders
  `shoggoth_pov_camera`, `ShoggothVisionHost` turns it into an intent), **SPEAKS** impressionistic never-naming murmurs
  through the PA voice when the wanderer is within 6 m (Phase E), **HUNTS** (FSM + bounded-BFS nav, feature-aware idle
  that loiters at junctions, fixed Flank), and its **VISION drives its MOTION live** via `resolve_target` (semantic
  `target_kind` → a real reachable goal cell: Wanderer → your exact cell, Stairs → the nearest up-stair = the Escape
  "yearning", else → the feature-aware Explore wander). The vision→motion path is **determinism-proven** in record/replay,
  and now runs **live in `run_game`** (the text brain sets *how* — speed/aggression/voice; vision sets *where* — it owns
  the four perception fields `resolve_target` reads, so the 3 s text brain can't clobber the seen target between blinks).
- **Determinism plumbing:** `ShoggothIntent` carries `{action, aggression, target_kind, sector, proximity, mood,
  snap, utterance}`. `ShoggothEvent` is `SHOGLOG2`, **padding-free** (locked by `static_assert(sizeof==40)`) for a
  deterministic raw-byte fold. `event_from_intent` / `apply_event_to_intent` are the single source of truth for the
  intent↔event mapping. `shoggoth_hash` folds the motion fields; **`utterance` is voice-only (never hashed/
  serialized)**. `shoggoth_prey()` is the shared M29 cross-seam prey-offset used by ALL record paths.
- **Phase C core:** `app/src/keel_scheduler.h` — a pure, thread-free LLM-request arbitration scheduler (priority ·
  single-multimodal-slot · latest-wins-per-class · concurrency cap · FIFO) with 6 deterministic property tests.

═══════════════════════════════════════════════════════════════════════════════
## THE SINGLE NEXT ACTION → Phase C.2 (threaded KeelBroker arbitration)  [prior action — live pixel-vision render — SHIPPED `phaseD-live`]
═══════════════════════════════════════════════════════════════════════════════
**Live in-game PIXEL-vision is DONE** (`577eef1`, tag `phaseD-live`, ledger E11): the creature now SEES live in
`run_game` via `app/src/shoggoth_vision_host.h` + a 2nd headless `Renderer` (384×216); the upload-stall was solved by
a **budget-spread warm window** (24 frames × 16 meshes via `render_chunks`'s existing `upload_budget` — 24×16=384 ≥ the
~338 common-case resident set @ radius 6 × 2 levels; deep-shaft >384 degrades to a graceful near-field partial, not fog).
Verified: `audit.ps1` green + live `--game` smoke **svision 2/2/2**, **debug_error_count=0 across 3 D3D12 devices**,
3763 frames/35 s (~107 fps, no stall), lookcheck PASS. **The immersive arc is complete** — thinks+sees+speaks+hunts live.

**NEXT = Phase C.2 (per `docs/SHOGGOTH_PLAN.md`).** Vision-live makes a real **5th** concurrent LLM/VLM consumer on the
single llama-server (creature-brain + creature-vision + director-narration + director-vision + chat). Today they share
KEEL best-effort on offset sparse cadences (the smoke proved they coexist debug-clean, but **unarbitrated**). Wire them
through a **threaded `KeelBroker`** (mutex+condvar) wrapping the already-built, already-tested `app/src/keel_scheduler.h`
(Phase C-core: priority player-speech > shoggoth-vision > director-vision > director-narration > shoggoth-brain · single
multimodal slot · latest-wins-per-class · concurrency cap · FIFO; 6 deterministic property tests). Route
`ShoggothBrainHost` / `ShoggothVisionHost` / `DirectorVisionHost` / `DirectorChatHost` / `DirectorHost` through it.
- **Verify via the SOAK, not flaky threaded unit tests** (the pure scheduler already has the deterministic property
  tests; the broker is the thin threaded shell): a concurrency soak gate — frame p99 < 2× median, a queued player-speech
  turn never waits behind a vision call beyond one slot. Needs KEEL :7071 up (`scripts\keel-up.ps1`).
- It is **`run_game`/host-side only** → `audit.ps1` record==replay MUST stay green (re-confirm after — the gated record
  path is untouched, same as the Phase D LIVE increment). Additive + revertable.
- **Alternatives the operator may pick instead:** Phase F (live cheap-tier hearing), Phase G (Escape polish —
  `resolve_target` already yearns at stairs), or call the immersive arc shipped. The C.2 work is *hardening*, not a fix.

═══════════════════════════════════════════════════════════════════════════════
## DON'T ASSUME (read carefully — these are the easy ways to be wrong)
═══════════════════════════════════════════════════════════════════════════════
1. **KEEL/llama/whisper are SELF-CONTAINED** in `dist\Backrooms`; bring them up with `scripts\keel-up.ps1`. NEVER
   reference `C:\llama.cpp` / `C:\keel-sidecar-7071` / `C:\models` / `C:\whisper.cpp` — those leaks are closed
   (ADR-078). Nothing outside `C:\backrooms` is ever needed at runtime.
2. **The M30 `game mouse-look` gate "red" is a tool-session cold-start artifact**, not a regression: the same
   `--game --auto-play --seconds 2` yields 1 frame here but ~473 in 9 s; `lookcheck: PASS` (no spin) in all. It
   passes on the operator's foreground GPU. Do NOT "fix" it, and do NOT widen the gate window (Iron Rule 6).
3. **Live PIXEL-vision IS now in-game** (Phase D LIVE, `577eef1`, tag `phaseD-live`, E11) — a 2nd headless `Renderer`
   renders the creature POV in `run_game` → `ShoggothVisionHost` → intent → `resolve_target`. The text brain still
   drives between the sparse ~25 s vision frames but PRESERVES the four perception fields (`target_kind/sector/
   proximity/snap`) so it can't clobber the seen target — the stale target persisting between "blinks" is BY DESIGN
   (volition across blinks). The creature SEES live; say so. (The NEXT action is Phase C.2 — see above.)
4. **Determinism is sacred.** record==replay is THE proof. `resolve_target` is gated by `target_kind != None`
   (default None → byte-unchanged → M21/M29 sacred gates intact). The schema is `SHOGLOG2`, padding-free. After
   ANY change to `shoggoth_step` / the schema / the record paths, run `audit.ps1` and confirm record==replay.
5. **Release exe = Windows-subsystem (no console); DEBUG = console** (so the gates capture stdout). Release-only.
   Don't unify them or you break the gates.
6. **The bundle's runtime/models/DXC are persistent in-repo assets** under `dist\Backrooms` (10.9 GB, gitignored,
   restorable from `C:\backrooms_backups\` / the zip). `package.ps1` no longer sources them from C:\ — it refreshes
   the exe and verifies the rest. Don't expect a from-C:\ re-stage.

═══════════════════════════════════════════════════════════════════════════════
## WORKING MODE (how the operator wants you to operate — non-negotiable)
═══════════════════════════════════════════════════════════════════════════════
- **Rapid, agentic, autonomous. Do NOT pause or ask for confirmation on in-scope work** — propose in your reply,
  then execute. Reserve questions for genuinely load-bearing, irreversible, or ambiguous forks only.
- **Do not go in circles / re-litigate settled things.** Cut to the chase, execute, verify, move on. The operator
  is explicitly frustrated by stalling.
- **Log EVERY change-set** to `docs/CHANGE_AUDIT_LOG.md` (what/why/files/commit/rollback/verification). **Take
  backups** (git tag + a local zip in `C:\backrooms_backups\`) at risk points.
- **Per-step self-audit (Externality Principle):** run `scripts\audit.ps1` pre- AND post- each step; append the
  one-line verdict to the ledger. Never self-assert "done" — let a non-LLM oracle say so.
- **Commit GREEN increments often** (the build's pre-commit hook runs build+ctest). Tag + push on green.
- **No C:\ deps, ever. No DOS windows, ever.**

═══════════════════════════════════════════════════════════════════════════════
## THE IRON RULES + INVARIANTS (from CLAUDE.md / ARCHITECTURE.md)
═══════════════════════════════════════════════════════════════════════════════
1. **Gate runner is law** — a milestone is done only when `scripts\gate.ps1 -Milestone M<N>` exits 0.
2. **Tag on green, push, revert on regression** — green gate → `git tag` → push; if a change breaks an earlier
   gate and two fix attempts fail, **revert to the last green tag** (never debug forward from a broken state).
3. **Headless first** — every feature ships a headless verification path before visual polish.
4. **Determinism is sacred** — INV-1..INV-8. Sim core: `/fp:strict`, no wall-clock, seeded PCG64 only, zero
   render/audio/director includes (CI grep-gated). AI runs at RECORD time only; validated intents enter the
   deterministic creature as event-log entries → replay with the model OFFLINE reproduces bit-for-bit.
5. **Diff budget ≤ 400 LOC** unless the milestone authorizes more; resist scope creep / "while I'm here."
6. **Never edit `/goldens` by hand; never relax a gate threshold to pass.** Fix the gate with an ADR, don't game it.
7. **Spec reconciliation in the same commit** — contract/boundary change → update ARCHITECTURE.md / MODULE.md;
   module inventory must match the directory listing (CI check).
8. **New dependency = ADR** (a `docs/DECISIONS.md` entry).
9. **One milestone per session**; end by running the gate + appending a SESSION_LOG entry.
- Scripts run under **Windows PowerShell 5.1** (no ternary / `&&` / `??`). Catch2 TEST_CASE names ASCII.

═══════════════════════════════════════════════════════════════════════════════
## PENDING / DEFERRED (after the next action)
═══════════════════════════════════════════════════════════════════════════════
- **Phase C.2 — now THE single next action (promoted above).** Threaded `KeelBroker` wrapping `keel_scheduler.h` +
  route the 5 live hosts through it + a concurrency soak (frame p99 < 2× median; vision never starves a player-speech
  turn). Verify by SOAK, not flaky threaded unit tests. Needs KEEL up.
- **Phase D LIVE watch-items (open, from E11 — verify when convenient, not gates):** (a) **VRAM under sustained 4K RT**
  — the Phase D smoke was ~35 s at the persisted res; 3 D3D12 devices + the 9B model (~6 GB) on the 16 GB card is fine
  at that scale, but a longer 4K-RT soak should confirm headroom before "bulletproof." (b) **Warm-window vs resident
  count** — 24×16=384 covers the ~338 common-case resident set (radius 6 × 2 levels); a deep-shaft (>384) renders a
  graceful near-field partial (render_chunks depth-draws the uploaded near-field — not fog). If a deep POV ever looks
  sparse, make the warm window adaptive (warm until `drawn >= resident_count` OR the frame cap, like the record path).
- **Phase F** — live cheap-tier hearing (a deterministic soundscape→tag, zero LLM) feeding the brain.
- **Phase G** — Escape polish (resolve_target already does Stairs = the yearning; "stage it / yearn, don't climb"
  — full vertical locomotion is a separate milestone, deliberately NOT bundled).
- **Public release** — bundle the **D3D12 Agility SDK** (`D3D12Core.dll` + `d3d12SDKLayers.dll` + the SDK-version
  exports) so the ADR-077 validation-layer fix works on a clean end-user Win11 without Graphics Tools; re-stage
  the bundle exe (the shipped zip predates the fixes) before any itch.io push.
- **GLM `_brainstorm/GLM/01_RTX_RENDERING_EFFICIENCY.md` — READ + Tier 1 APPLIED (E12, `af186f6`).** Temporal
  accumulation is live in the interactive RT path (a still view converges clean at 1 spp/frame instead of
  4-spp-from-scratch); fixed the operator's "noisy+slow" playtest report. **Remaining GLM tiers (optional, measure
  first):** Tier 2 (frame-pipeline de-sync — remove the 3 sync GPU round-trips/frame), Tier 3 (SVGF temporal
  denoiser — also removes the creature-ghost-while-still tradeoff from Tier 1), Tier 4 (cheaper stochastic light
  sampling). Also from the playtest: caption word-wrap fixed (E12); the bitmap font lacks a 'J' glyph (tiny TODO).
- **Operator should confirm `gate.ps1 -Milestone M30` green** on their interactive machine (the mouse-look check
  needs a foreground GPU; see DON'T-ASSUME #2).

═══════════════════════════════════════════════════════════════════════════════
## KEY FILES (the map)
═══════════════════════════════════════════════════════════════════════════════
- Canon: `docs/ARCHITECTURE.md` · Plan: `docs/MILESTONES.md` · Decisions/ADRs: `docs/DECISIONS.md` (ADR-076/077/078)
- The Shoggoth plan: `docs/SHOGGOTH_PLAN.md` (phases A–G, the six pillars, "calibrated incompetence"/"its mediocrity
  is the dread", the ablation-falsifiability idea, verticality answered: yearn-don't-climb).
- Per-change ledger: `docs/CHANGE_AUDIT_LOG.md` (E0–E11) · Session narrative: `docs/SESSION_LOG.md` (§38 newest)
- Creature code: `app/src/shoggoth.h` (struct, FSM nav, `resolve_idle_goal`, `resolve_target`, `shoggoth_hash`),
  `shoggoth_brain.h` (summary, prompts, `parse_shoggoth_intent`, `ShoggothEvent`, `SHOGLOG2`, mapping helpers),
  `shoggoth_brain_host.h` (the live text brain), `shoggoth_vision.h` (POV camera + vision prompt),
  `shoggoth_vision_host.h` (the live qwen-VL EYE — Phase D LIVE, off-thread POV→intent),
  `keel_scheduler.h` (arbitration core; Phase C.2 wraps it in a threaded `KeelBroker`). Live wiring (the 2nd headless
  creature-POV device, the warm window, the text-brain field-preservation) + sidecar launcher + voice: `app/src/main.cpp`.
- Scripts: `scripts/audit.ps1` (per-step oracle) · `scripts/gate.ps1` (milestone gate) · `scripts/keel-up.ps1` /
  `keel-down.ps1` (self-contained sidecar) · `scripts/build.ps1` · `scripts/package.ps1` (bundle).
- Bundle/runtime (self-contained, gitignored): `dist\Backrooms\` (exe + `runtime\{llama,keel,whisper}` + `models\`).
- Backups: tags `pre-phaseA phaseA phaseB phaseC-core`; zips in `C:\backrooms_backups\`.

═══════════════════════════════════════════════════════════════════════════════
## VERBATIM OPERATOR INTENT (so you read the register correctly)
═══════════════════════════════════════════════════════════════════════════════
- "make it fully portable first, i dont want any DOS windows popping up ever again, then proceed directly to making
  the ai shoggoth fully immersive ... stop going in circles ... cut to the chase and just execute, rapidly, agentic,
  autonomous, stop asking me questions or pausing, finish it"
- "there should be no reason to ... use ... anything outside of c:\backrooms ever again"
- "if the integration itself isn't wired correctly, you must propose a fix and fix that first"
- "preserve fully this moment ... not have to rexplaination tax nor have it forget things we already established ...
  or assume things incorrectly"

═══════════════════════════════════════════════════════════════════════════════
## WHEN YOU APPROACH ~70% CONTEXT
═══════════════════════════════════════════════════════════════════════════════
Re-run the perpetual-memory distill (skill: `perpetual-memory`): UPDATE `_run_state/REHYDRATE.md`, the harness
memory, and `docs/SESSION_LOG.md` BEFORE the compaction wall (~95%) — don't wait. Keep the source-of-truth on disk.

**Now: do STEP 0, confirm green, then proceed to the next action (or whatever the operator directs).**
