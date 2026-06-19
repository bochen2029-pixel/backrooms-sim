# CHANGE_AUDIT_LOG.md — append-only change / rollback ledger

Append-only. **Newest entry at the bottom.** One entry per change-set. Each records: WHAT changed,
WHY, the FILES, the COMMIT, the ROLLBACK anchor, and VERIFICATION. This is finer-grained than
`SESSION_LOG.md` (which stays the per-session narrative) and complements `DECISIONS.md` (ADRs). Its
job is audit + fast, unambiguous rollback.

**Rollback quick-reference**
- Undo one change-set, keep history (safe):  `git revert <commit>`
- Reset hard to a known-good anchor (destructive):  `git reset --hard <tag>`  (e.g. `pre-phaseA`)
- Local source snapshots (no git needed):  `C:\backrooms_backups\*.zip`
- Tags are the durable anchors; every change-set below names the anchor to roll back *to*.

---

## E0 — 2026-06-17 — Pre-Phase-A backup + snapshot  ·  anchor created: tag `pre-phaseA`
- **What:** baseline snapshot before the Shoggoth Phase A (feature-aware idle) work begins.
- **Commit:** docs-only — adds `docs/SHOGGOTH_PLAN.md` (the spec) + this ledger. No code touched.
- **Rollback anchor:** `git tag pre-phaseA` at this commit, pushed to `origin`. To abandon all of
  Phase A and return to the shipped crash-fix state: `git reset --hard pre-phaseA`.
- **Local snapshot:** `C:\backrooms_backups\backrooms-src-pre-phaseA-2026-06-17.zip` (source + docs +
  scripts + tests + contracts; excludes build/, build-release/, dist/, extern/, .git, vcpkg_installed).
- **Repo state at anchor:** RT crash fix shipped (`0d73c47`, ADR-077); ctest 100/100; M9 green.
  Shoggoth Phase A not yet started.

## E1 — 2026-06-17 — Phase A: feature-aware idle + Flank fix  ·  anchor: tag `phaseA`
- **What:** the Shoggoth's Lurk idle now ambles toward **features** (junctions / open forward
  cells) via a new pure resolver `resolve_idle_goal` (`app/src/shoggoth.h`), replacing the blind
  `rng % 7` hop. Flank fixed to a real fixed-radius circle-strafe (the old `di/2` rounded to 0 at
  1-cell range → Flank silently collapsed onto Hunt). **LLM-free; no schema / event-log change.**
- **Why:** `SHOGGOTH_PLAN.md` Phase A — highest affect-per-LOC, zero determinism risk, and the
  fallback that keeps the creature feeling intentional even with the brain off / KEEL down.
- **Files:** `app/src/shoggoth.h` (+`resolve_idle_goal`, idle reroute, Flank rewrite),
  `tests/unit/test_shoggoth.cpp` (+3 tests), this ledger.
- **Commit:** see tag `phaseA` (`git rev-parse phaseA`).
- **Rollback:** `git revert <phaseA commit>` (safe — Phase A is self-contained in `shoggoth.h`), or
  `git reset --hard pre-phaseA` to drop Phase A entirely and return to the shipped crash-fix state.
- **Verification:** ctest **103/103** (3 new Phase A tests + all existing shoggoth tests:
  record==replay, lurking, no-tunnel, intent-steers). Shoggoth **record == replay bit-identical**
  (`combined_hash 29310b6befab8895`, model offline) → determinism intact. The sacred gate's
  `valid_intents ≥ 1` non-vacuity clause needs the KEEL sidecar at :7071 (not up this session);
  Phase A is LLM-free, so that clause is structurally unaffected by this change.

## E2 — 2026-06-17 — Adopt per-step self-audit (Externality Principle) + `scripts/audit.ps1`
- **What:** added `scripts/audit.ps1` — the fast non-LLM oracle suite (build /WX · ctest · shoggoth
  record==replay determinism · module inventory · core isolation) in one command, with a paste-ready
  verdict line. Encoded the **pre/post-step audit cadence** in `CLAUDE.md` (Commands + Verification
  etiquette).
- **Why:** operator directive — self-test at each step so the build doesn't drift, errors don't
  accumulate, entropy doesn't compound. Implements the **Externality Principle** from Bo Chen's
  `solo-enterprise-architect` v7 (`C:\ClaudeCode\`): ground truth must originate *outside* the model;
  run the externalized oracles continuously, not only at milestone gates. `gate.ps1` stays the
  milestone gate; `audit.ps1` is the lightweight between-gates check.
- **Files:** `scripts/audit.ps1` (new), `CLAUDE.md` (commands + cadence), this ledger.
- **Rollback:** `git revert <commit>` — pure infra/docs; `audit.ps1` only *reads* existing checks.
- **Verification (pre-audit baseline at this commit):** `AUDIT PASS — build ok | ctest 103 tests,
  0 failed | determ 29310b6befab8895 | inventory ok | isolation ok | tree clean @ phaseA`.
- **Going forward:** each change-set entry below should carry an `audit.ps1` PASS line (pre + post).

## E3 — 2026-06-17 — Phase B: ShoggothIntent/Event schema bump (SHOGLOG2) — determinism-neutral
- **What:** extended the schema for later phases (vision / resolver). `ShoggothIntent` +5 MOTION
  fields (`target_kind, sector, proximity, mood, snap`; all defaulted). `ShoggothEvent` +those 5 +
  `_reserved` (padding-free 40 B, `static_assert`-locked). `SHOGLOG1`→`SHOGLOG2`. Two helpers
  (`event_from_intent` / `apply_event_to_intent`) as the single source of truth across the 4 record
  paths + replay. `shoggoth_hash` folds the 5 new fields. `utterance` deferred to Phase E (voice-only,
  never serialized → no version bump needed then).
- **Why:** `SHOGGOTH_PLAN.md` Phase B — do the one determinism-risky log/hash change ONCE for every
  field the later phases need, behavior-neutral, with the gate re-proven before anything uses them.
- **Files:** `app/src/shoggoth.h`, `app/src/shoggoth_brain.h`, `app/src/main.cpp`.
- **Rollback:** `git revert <phaseB commit>`, or `git reset --hard phaseA`.
- **Verification (Externality Principle — TWO independent oracles + a cold verifier):**
  - `audit.ps1` POST: `build ok | ctest 103/103 | determ record==replay 409129a0236b3084 (lvl 0) | inventory ok | isolation ok`.
  - level-7 record==replay `95945b9087214b14` (the M29 prey-offset path + new schema, model offline).
  - `static_assert(sizeof(ShoggothEvent)==40)` — compile-time padding-free oracle (held → no padding).
  - cold-context verifier subagent (no pass-1 context): **VERDICT CLEAN** — 5×5 field×location matrix
    fully populated (no asymmetry), every event site via the helpers, no string hashed, `shoggoth_step`
    reads no new field (behavior-neutral).
  - Hash changed `29310b6`→`409129a0` BY DESIGN (5 new fields folded); record==replay still holds → determinism intact.
- **Note:** `valid_intents=0` (KEEL :7071 down) — Phase B is LLM-free; the determinism oracle is KEEL-independent.
- **Deferred (tracked, kept Phase B atomic):** a level-7 case added to the `gate.ps1` sacred gate; the
  M29 *vision*-record prey-offset logic fix (`AUDIT_2026-06-15.md:207`) — both separate increments.

## E4 — 2026-06-17 — Fix the M29 prey-offset in ALL shoggoth record paths (vision/hearing/PA)
- **What:** the vision/hearing/PA record paths used the **un-offset** wanderer in `shoggoth_step`,
  while `--shoggoth-record` + `--shoggoth-replay` apply the M29 cross-seam prey-offset → a
  `--level != 0` record on those paths would NOT replay bit-exact. `AUDIT_2026-06-15.md:207` flagged
  *vision*; measure-first found **hearing + PA had the identical bug**. Added a shared `shoggoth_prey()`
  helper (single source of truth) and routed all three paths' summary + step through it.
- **Why:** a latent determinism bug — closes the AUDIT item and the whole class. Pure anti-entropy.
- **Files:** `app/src/main.cpp` (1 helper + 2 replace-all of 3 sites each). Behaviour-neutral at level 0.
- **Rollback:** `git revert <commit>`, or `git reset --hard phaseB`.
- **Verification (oracle):** record==replay **bit-identical at LEVEL 7** for vision/hearing/PA (all
  `95945b9087214b14` = the plain-record level-7 chase) AND level-0 vision **unchanged**
  (`409129a0236b3084` = M22/M23/M24 byte-identical). `audit.ps1` POST: `build ok | ctest 103/103 |
  determ 409129a0 (lvl0) | inventory ok | isolation ok`.

## E5 — 2026-06-17 — Gate the M29 cross-seam determinism of the VISION record path (AUDIT-207 guard)
- **What:** added an `Invoke-GateM29` case that records `--shoggoth-vision-record` across a descent
  (`--level 2`) then replays, asserting record==replay + the creature escapes (Lurk). The M29 gate
  previously tested only the *plain* record path — which is exactly why the vision/hearing/PA
  prey-offset bug (E4) slipped through. The vision path exercises the shared `shoggoth_prey()` helper
  that hearing/PA also call, so one case guards all three. KEEL-independent (escape + determinism).
- **Why:** the gate is the durable guard; without it the E4 fix could silently regress.
- **Files:** `scripts/gate.ps1`. **Rollback:** `git revert <commit>`.
- **Verification:** the case's exact commands run green now — vision-record `--level 2` == replay
  (`95945b9087214b14`), replay final_state=0 (escaped); `gate.ps1` parses clean (PS 5.1 AST); commit
  hook build+ctest green. NOTE: the *rest* of `Invoke-GateM29` still needs KEEL :7071 for its other
  cases' `valid_intents>=1` (pre-existing); this new case does not require `valid_intents`, so it
  greens regardless of KEEL.

## E6 — 2026-06-17 — Phase C (core): pure LLM-request arbitration scheduler + property tests
- **What:** `app/src/keel_scheduler.h` — the THREAD-FREE decision core for the single llama-server
  backend: priority admission (player-speech > shoggoth-vision > director-vision > director-narration
  > shoggoth-brain), a single multimodal slot, latest-wins per class, a concurrency cap, FIFO
  tie-break. `tests/unit/test_keel_scheduler.cpp` — 6 deterministic property tests of those laws (no
  threads → no flakiness). Registered in `tests/CMakeLists.txt`.
- **Why:** `SHOGGOTH_PLAN.md` Phase C — arbitration must land BEFORE live vision (Phase D) or the five
  LLM consumers starve each other on one backend. The pure core is the hard logic; building+testing it
  in isolation (contract-first) is fully verifiable WITHOUT KEEL.
- **Files:** `app/src/keel_scheduler.h` (new), `tests/unit/test_keel_scheduler.cpp` (new),
  `tests/CMakeLists.txt`. **Rollback:** `git revert <commit>` / `git reset --hard phaseC-core` (header
  used only by its test — self-contained, no integration yet).
- **Verification:** `audit.ps1` POST — `build ok | ctest 109/109 (was 103; +6 keel) | determ
  409129a0 | inventory ok | isolation ok`. The 6 scheduler laws each green.
- **DEFERRED = Phase C.2 (best with KEEL :7071 up to soak):** the threaded `KeelBroker` wrapper
  (mutex + condvar around `KeelScheduler`, runs each admitted thunk on the host thread) + route the
  live hosts (DirectorVisionHost / DirectorChatHost / ShoggothBrainHost) through it + the concurrency
  soak gate (frame p99 < 2× median; vision never delays a queued player-speech turn beyond one slot).

## E7 — 2026-06-17 — Fully self-contained AI runtime: no C:\ dependency, ever [ADR-078]
- **What:** closed the dev-tree + packaging C:\ leaks (the *bundle* was already self-contained per
  ADR-076; GLM doc 04 named the rest). `main.cpp` `bundled_w/a` fall back to the in-repo
  `dist\Backrooms` (`..\..\dist\Backrooms`), never C:\; the hearing-record hardcoded model path now
  uses `default_whisper_model()`. New `scripts/keel-up.ps1` + `keel-down.ps1` start/stop the bundled
  sidecar from `dist\Backrooms` (the in-tree replacement for `C:\keel-sidecar-7071\start.cmd`).
  `gate.ps1`'s 7 stale C:\ hints → `keel-up.ps1`. `package.ps1` treats runtime/models/DXC as
  persistent in-repo assets (refresh exe only; verify the rest; never C:\/SDK). `keel.lock` C:\ paths
  → bundle-relative (gitignored artifact).
- **Why:** operator mandate — never need anything outside `C:\backrooms` again.
- **Files:** `app/src/main.cpp`, `scripts/package.ps1`, `scripts/gate.ps1`, `scripts/keel-up.ps1`
  (new), `scripts/keel-down.ps1` (new), `docs/DECISIONS.md` (ADR-078). **Rollback:** `git revert <commit>`.
- **Verification (entirely from C:\backrooms):** `keel-up.ps1` brought up llama :8080 + keel :7071 from
  `dist\Backrooms`; sacred record→replay **`valid_intents=5`** + **record==replay `1ade340c4648b041`**
  (the full sacred-gate assertion, UNBLOCKED, zero external C:\). `audit.ps1` PASS: build ok, ctest
  **109/109**, determinism (real intents) record==replay, inventory + isolation green; main.cpp
  resolver C:\ refs = 0.
- **Deferred (doc 04):** Tier-1 console fix (`WIN32_EXECUTABLE` — must verify it doesn't break the
  gates' stdout capture); exe-relative DXC probe (the dev exe's DXR still finds `dxcompiler.dll` via
  the Windows SDK); manifest hash-pinning (Gap D), DLL allowlist (Gap E), end-to-end bundle smoke (Gap J).

## E8 — 2026-06-18 — No DOS window EVER: release exe is /SUBSYSTEM:WINDOWS (doc 04 Tier 1) [ADR-078]
- **What:** `app/CMakeLists.txt` makes the RELEASE build a Windows-GUI (no-console) exe
  (`WIN32_EXECUTABLE` + `/ENTRY:mainCRTStartup`), keeping `main()` unchanged. The DEBUG build stays
  console-subsystem so the gates keep capturing stdout. Removed `RUN.cmd` (its `start` flashed a
  console) from `package.ps1` + the bundle; users double-click `Backrooms.exe`. Bundle exe refreshed.
- **Why:** operator — no DOS windows popping up, ever. The sidecars were already `CREATE_NO_WINDOW`;
  the game's own console was the last popup.
- **Files:** `app/CMakeLists.txt`, `scripts/package.ps1`. **Rollback:** `git revert <commit>`.
- **Verification:** `dumpbin` → release = **"2 subsystem (Windows GUI)"** (no console), debug = "3
  subsystem (Windows CUI)" (gates OK). Release smoke (Windows-subsystem): **rt_frames=486**, debug-clean,
  exit 0, and stdout STILL reaches a redirected pipe (capture works). `audit.ps1` PASS: ctest 109/109,
  determinism + inventory + isolation green.

## E9 — 2026-06-18 — Shoggoth Phase D/E core: vision-driven motion (resolve_target) + voice field [SHOGGOTH_PLAN]
- **What:** `resolve_target` (shoggoth.h) maps a semantic `target_kind` → a real goal cell (Wanderer →
  the engine's exact cell; Stairs → nearest up-stair; else → the feature-aware wander), wired into
  `shoggoth_step` (target_kind != None overrides the goal). `ShoggothIntent` gains `utterance` (voice-only,
  never hashed/serialized). `parse_shoggoth_intent` now reads the semantic fields + utterance (all optional,
  backward-compatible). `render_shoggoth_vision_prompt` asks for the full semantic schema.
- **Why:** SHOGGOTH_PLAN Phase D (vision drives behaviour) + E (voice) — the immersive core.
- **Determinism-safe:** target_kind=None (the text-brain path + every default) → `resolve_target` skipped →
  M21/M29 byte-unchanged. The vision path carries the fields through the Phase-B event log → record==replay.
- **Files:** `app/src/shoggoth.h`, `shoggoth_brain.h`, `shoggoth_vision.h`. **Rollback:** `git revert <commit>`.
- **Verified (KEEL self-contained :7071):** `audit.ps1` ctest 109/109 + text record==replay (`476a9c07`);
  VISION record==replay `valid_intents=3` (`a91b512d`) — the semantic schema drives `resolve_target`
  deterministically.

## E10 — 2026-06-18 — Shoggoth Phase E LIVE: the creature SPEAKS in the playable game [SHOGGOTH_PLAN]
- **What:** `render_shoggoth_prompt` (the live text brain) now asks for an `utterance`; `run_game` speaks
  it (`speak_pa`) when the wanderer is NEAR (<6 m), it's a NEW line, and a 7 s cooldown has passed
  (audioOn-gated). The creature murmurs impressionistic dread as it hunts — the highest-affect sense, live.
- **Why:** SHOGGOTH_PLAN Phase E — the voice.
- **Determinism-safe:** `utterance` is never hashed/serialized; the voice is live-only (`run_game`), so the
  record/replay gates are untouched.
- **Files:** `app/src/shoggoth_brain.h`, `app/src/main.cpp`. **Rollback:** `git revert <commit>`.
- **Verified (KEEL self-contained):** `audit.ps1` ctest 109/109 + determinism record==replay (`9a6ab59`);
  live `--game` smoke `brain_intents=3`, debug-clean, exit 0 (the brain runs with the utterance prompt;
  the voice path active — fires in-game when the creature closes in).

## E11 — 2026-06-18 — Shoggoth Phase D LIVE: live in-game PIXEL-vision render [SHOGGOTH_PLAN]
- **What:** the creature now reasons from a RENDERED POV live in `run_game` (it previously reasoned from its
  text sense only there; the rendered-POV path was proven only headless in `--shoggoth-vision-record`). New
  `app/src/shoggoth_vision_host.h` (`ShoggothVisionHost`) — an off-thread qwen-VL eye, a faithful merge of
  `DirectorVisionHost` (image→VLM) + `ShoggothBrainHost` (returns a validated `ShoggothIntent`): latest-wins
  `submit`, non-blocking `poll`, graceful no-op when KEEL is down. In `run_game`: a 2nd headless `Renderer`
  (384×216) renders the Shoggoth's vantage (`shoggoth_pov_camera`); every ~25 s the snapshot is encoded
  (`encode_pov_b64`) + submitted; the returned intent (with `target_kind`) is applied to `shog.intent` →
  `resolve_target` → its MOTION follows what it SEES. **Upload-stall solution:** the chunk upload is
  budget-spread across a 24-frame warm window at 16 meshes/frame (`render_chunks`'s existing `upload_budget`),
  so the frame thread never eats the headless path's ~400-iteration / 256-budget stall. The text-brain apply
  now PRESERVES the four perception fields `resolve_target` reads (`target_kind/sector/proximity/snap`) so the
  3 s text brain (which cannot see) can't clobber the seen target between the sparse ~25 s vision frames —
  behaviour-neutral when vision is off (they stay None/default, sacred path unchanged). New counters
  `svision_requests/produced/intents`.
- **Why:** `SHOGGOTH_PLAN.md` Phase D LIVE / REHYDRATE "the single next action" — the last immersive piece: the
  creature SEES live in the playable game (not just at record time).
- **Determinism-safe:** entirely in `run_game` (interactive, non-gated, INV-1 untouched); the intent enters via
  `shog.intent` exactly like the existing live text brain. No contract/schema change. The record path
  (`run_shoggoth_record` / `run_shoggoth_vision_record`) is byte-identical → record==replay green by construction.
- **Files:** `app/src/shoggoth_vision_host.h` (new), `app/src/main.cpp`, `docs/CHANGE_AUDIT_LOG.md`.
  **Rollback:** `git revert <commit>` (additive + gated — new header + a gated block in `run_game`).
- **Verified (KEEL self-contained, 9B+vision tier up):** `audit.ps1` POST — build ok | ctest **109/109** |
  determinism **record==replay** (`20bc9469…`) | inventory ok | isolation ok. Live windowed `--game --auto-play
  --seconds 35` smoke (config had RT+Director on, so all three multimodal consumers ran at once):
  **svision_requests=2 / svision_produced=2 / svision_intents=2** (the creature saw live + its intents drove
  it), **debug_error_count=0** across THREE concurrent D3D12 devices (windowed raster + DXR + the creature-POV
  headless), **frames=3763 in 35 s (~107 fps — no stall)**, `lookcheck: PASS`, clean exit. The budget-spread
  warm window solved the upload-stall concern; ADR-077 (forced validation) made the 3rd device viable.
- **Design notes / watch-items (reviewer-flagged; recorded so a future reader doesn't "tidy" or mis-read them):**
  - **The text-brain field-preservation is `run_game`-ONLY — verified, must stay there.** The preservation
    (`it.target_kind = shog.intent.target_kind; …sector/proximity/snap`) is in the live brain-poll loop in
    `run_game` (main.cpp ~L1676). The gated record/replay apply is entirely separate (`sh.intent = intent` in
    `run_shoggoth_record` / `…_vision_record` / `…_hearing_record` / `…_pa_record` ~L3259+, and
    `apply_event_to_intent` in `run_shoggoth_replay` ~L3672) — NOT touched. Do NOT migrate the preservation into the
    record path: it must never alter the recorded byte stream (that's why `audit.ps1` record==replay stayed green).
  - **The seen `target_kind` persisting between the sparse ~25 s vision frames is BY DESIGN, not a leak** — the
    SHOGGOTH_PLAN "volition across blinks": the creature commits to what it last SAW until the next blink (the text
    brain only updates *how* — action/aggression/voice). Behaviour-neutral when vision is off (the fields stay
    None/default → `resolve_target` skipped → M21/M29 sacred path byte-unchanged).
  - **Warm-window coverage:** 24 frames × 16 meshes = 384 uploads ≥ the ~338 common-case resident set (radius 6 ×
    2 levels @ 169/level). A deep-shaft POV (resident >384) renders a graceful near-field partial (render_chunks
    depth-draws the uploaded near-field — NOT fog); successive 25 s windows upload only the delta. If a deep POV
    ever looks sparse, make the window adaptive (warm until `drawn >= resident_count` OR the cap, like the record path).
  - **OPEN follow-up (not a gate):** VRAM headroom under a *sustained 4K RT* soak with the 3 concurrent D3D12 devices
    + the 9B model (~6 GB) on the 16 GB card — the smoke was ~35 s at the persisted res; confirm before "bulletproof."
  - **Gotcha surfaced:** `run_game`'s persisted `backrooms.cfg` can silently override CLI flags (the smoke ran
    RT+Director+seed1 from a stale cfg despite `--seed 3`) — a future "clean-isolation" smoke must clear/inspect the cfg.
- **DEFERRED (now the single next action):** Phase C.2 — route all 5 live hosts (brain / **creature-vision** /
  director-narration / director-vision / chat) through the threaded `KeelBroker` wrapping `keel_scheduler.h` so the
  single multimodal slot is **arbitrated** rather than best-effort-queued (today they share KEEL on offset sparse
  cadences; the smoke proved they coexist debug-clean but UNarbitrated). **Verify by SOAK, not flaky threaded unit
  tests**; re-confirm `audit.ps1` record==replay after (host-side only). Then Phase F (cheap-tier hearing), G (Escape
  polish). Public-release Agility SDK.

## E12 — 2026-06-18 — Playtest rendering fixes: RT temporal accumulation (GLM 01 Tier 1) + caption word-wrap
- **What (two independent fixes the operator hit while playing the RT build):**
  1. **RTX noise + slowness — temporal accumulation (GLM `_brainstorm/GLM/01` Tier 1).** The interactive PT path
     passed `reset=true` to `render_pt_frame` EVERY frame → each frame was 4-spp-from-scratch (noisy) and slow
     (shooting enough spp to look clean in one frame is expensive). Fix: at the two interactive sites (`run_game`
     ~L1865, `run_play` ~L1041) track the previous camera (`dxrPrevCam`/`dxrHaveCam`) and `reset` ONLY when the
     view moved / the chunk scene rebuilt / first frame (new `pt_view_moved` epsilon helper + the captured
     `sceneRebuilt`); when static, accumulate at **1 spp/frame** (`samples = reset ? 4 : 1`). A static view now
     converges clean instead of re-noising every frame, at ~¼ the rays. Motion keeps 4 spp + denoise (masked by
     movement). KNOWN tradeoff (GLM 1a): the creature ghosts slightly while you stand still and it writhes —
     acceptable for v1; SVGF temporal reproject (GLM Tier 3) is the follow-up if it bothers.
  2. **Caption runoff.** `build_caption_overlay` (hud.cpp) drew the whole line as one centered row with NO width
     check, so long Director/creature lines overflowed both screen edges (worst at 4K, where the RT path
     composites at the 2/3 internal res 2560×1440 and an 86-char line at scale 5 = 2580 px > 2560). Fix:
     word-wrap at spaces to lines that fit `width - 32*scale`, stack them as a lower-third block, size one
     backing bar to span them.
- **Why:** operator playtest feedback ("captions run off the sides; RT is slow and noisy as heck") — the first
  use of the "play it first" pass. GLM doc 01 (previously UNREAD) named the exact root cause + the tiered fix.
- **Determinism-safe:** the RT change is the INTERACTIVE path only (`render_pt_frame` in `run_game`/`run_play`);
  the golden/offline path is the SEPARATE `render_pt` (reset=true, denoise=false) — untouched. The caption is a
  presentation overlay. `audit.ps1` record==replay stayed green (verified).
- **Files:** `app/src/main.cpp` (pt_view_moved + the 2 call sites + state), `app/src/hud.cpp` (caption wrap),
  `docs/CHANGE_AUDIT_LOG.md`. **Rollback:** `git revert <commit>` — the two fixes are independent hunks (hud.cpp
  vs main.cpp); revert one file selectively if needed.
- **Verified:** `audit.ps1` POST — build ok | **ctest 109/109** | **record==replay** (`747aaea9…`) | inventory |
  isolation. Caption: headless `--caption-shot` at 2560×1440 with the operator's exact long line → **wraps to 2
  lines, no edge runoff** (and 1920 fits on one line). RT: live `--game --auto-play --rt --seconds 20` →
  **rt_frames=2008 (~100 fps), debug_error_count=0** (the static camera exercised the new reset=false 1-spp
  accumulation path ~2007/2008 frames — debug-clean, no crash), lookcheck PASS. A/B offscreen render
  (`--dxr-pt`, same pose) — **4 spp grainy vs 64 spp smooth** (what a static view now converges to), both
  debug-clean. Artifacts in `runs\rt_before_4spp.png` / `rt_after_64spp.png` / `caption_wrap_4k.png`.
- **Follow-up DONE (same change-set):** added the missing **'J' glyph** to the 5×7 bitmap font (`hud.cpp`
  `glyph_for`) — it was absent between 'I' and 'K', so every J-word in a caption rendered with a blank ("JUST"
  → " UST"). Verified via `--caption-shot "JUST SIX METERS AWAY..."` → renders correctly. (Captions are now
  fully fixed: word-wrap + the complete glyph set.)
- **Still deferred (optional, measure first):** GLM Tiers 2 (frame-pipeline de-sync — more raw fps), 3 (SVGF
  temporal denoiser — also removes the Tier-1 creature-ghost-while-still), 4 (cheaper stochastic light sampling).

## E13 — 2026-06-18 — RT flashlight (eye-torch) toggled with F [operator request]
- **What:** an interactive flashlight for the ray-traced path — a soft cone of light co-located with the eye,
  aimed along the camera forward, that brightens dark areas. No 3D model (operator: "i dont need to see a 3d
  flashlight, just a cone of light"). Toggle = **F** (in `run_game`; F2/F11 were taken). Added at PRIMARY hits
  only — a primary hit is eye-visible by construction, so NO shadow ray is needed (cheap). Shader: repurposed
  the spare CB pad slot `uPad0` → `float uFlashI` (0 = off), a `flashlight()` spotlight function (cone via
  smoothstep on the axis cosine, gentle distance falloff, near-white color), and a **`[branch]`-guarded** add at
  the primary-hit shade so when off the existing path is provably skipped. C++: `Impl::flashIntensity` (default
  0), `setf(c,25,...)` in `render_pt_frame`, `DxrRenderer::set_flashlight(bool)`. App: F edge-toggle + `flashOn`
  state; the RT block calls `set_flashlight(flashOn)` and **forces a `ptReset` when `flashOn` changes** (the
  lighting changed → the temporal accumulator must re-converge). A `--flashlight` QC flag on `--dxr-pt` renders
  the torch headless for the A/B. Docs: USER_GUIDE controls table + the bundle README control line.
- **Why:** operator request — some areas are too dark; a flashlight to brighten the scene.
- **Regression-proof / determinism:** the flashlight is OFF by default and the offline/golden path (`render_pt`
  → `render_pt_frame`, `d.flashIntensity` never set) leaves `uFlashI == 0`, so the `[branch]` is skipped and the
  PT output is **bit-identical**. PROVEN: `gate.ps1 -Milestone M9` PASSED — converged PT goldens
  **diff_vs_golden = 0.000004 / 0.000677 / 0.000000** at poses 1/3/4 (≪ the 1.0 threshold); also re-greened the
  Tier-1 accumulation gate (169.6 FPS @ 1440p, no-ghost reset clean=0). The sim/replay path is untouched.
- **Files:** `render_dxr/include/render_dxr/dxr.h`, `render_dxr/src/dxr.cpp`, `app/src/main.cpp`,
  `docs/USER_GUIDE.md`, `scripts/package.ps1`, `docs/CHANGE_AUDIT_LOG.md`. **Rollback:** `git revert <commit>`
  (additive + default-off; the shader add is branch-guarded).
- **Verified:** `gate.ps1 M9` PASSED (goldens bit-identical, debug-clean). `audit.ps1` POST — build ok | ctest
  **109/109** | record==replay | inventory | isolation. A/B render (`--dxr-pt --flashlight`, pose 4 dark
  down-view): **luma 31 → 68**, a clean soft-edged cone, debug_error_count 0 (`runs\flash_p4_off/on.png`). Live
  `--game --auto-play --rt` smoke: rt_frames 1143, debug-clean, lookcheck PASS. Cone width/softness/intensity are
  the three shader constants `kFlashCosOut`/`kFlashCosIn`/`kFlashIntensity` — trivial to retune.

## E14 — 2026-06-18 — Settings: manual AI model-tier toggle (AUTO / 9B-vision / 4B-text) [operator request, ADR-079]
- **What:** a new **"AI MODEL"** Settings row to manually pick the LLM tier instead of only the VRAM auto-tier —
  cycles **AUTO / 9B VISION / 4B TEXT** (Left/Right). `Settings.model_tier` + `Config.model_tier` (0/1/2);
  `kSettingsItems` 10→11 with `kSettingsModel=9` inserted BEFORE Back (so Test-Connection/Mic/Subtitles indices
  are unchanged — minimal disruption). `try_start_sidecar()` reads a `g_modelTier` global: 1→force 9B(+mmproj),
  2→force 4B (text-only, `g_visionAvailable=false` → vision hosts no-op), **0=AUTO=the exact prior VRAM logic**.
  Applied on (re)start (the sidecar reloads a ~6 GB model — live restart is the risky path we avoid; a fresh pick
  before first Play still applies this session; hint = "APPLIES ON RESTART - 4B IS TEXT-ONLY (NO VISION)").
- **Why:** operator — "even if i can run 9b, i still want option to run the smaller 4b." Default AUTO = no change.
- **Without breaking anything:** default `model_tier=0` (AUTO) is byte-for-byte the prior behaviour. Config is
  backward/forward compatible (unknown keys ignored, missing → default). The new menu index 9 doesn't touch the
  indices the menu tests assert (3/5/`kSettingsItems-1`). The intentional Settings-render change required the
  **`goldens/m15/settings.png` golden to be regenerated via `goldgen capture`** (the sanctioned sole writer,
  Iron Rule 6) — blessed in THIS commit with ADR-079; the other 3 menu goldens are unchanged.
- **Files:** `app/src/config.h`, `app/src/menu.h`, `app/src/hud.cpp`, `app/src/main.cpp`, `goldens/m15/settings.png`
  (regenerated), `docs/DECISIONS.md` (ADR-079), `docs/USER_GUIDE.md`, `docs/CHANGE_AUDIT_LOG.md`.
  **Rollback:** `git revert <commit>` — restores the 10-item menu + the prior golden (all in one commit).
- **Verified:** `gate.ps1 -Milestone M15` **PASSED** — all 4 menu goldens bit-match (incl. regenerated
  settings.png), state-machine transition tests green, menu compositing debug-clean, shell boots clean.
  `audit.ps1`: build ok | ctest **109/110 (109 run, 0 failed)** — config round-trip + defaults-survive + sanitize
  + menu-wraps all green | record==replay | inventory | isolation. The "AI MODEL AUTO" row + hint render correctly
  (`runs\settings_model.png`). NOTE on live test: the model actually loaded only changes at the next sidecar launch
  (game restart) — verifiable by the operator (pick 4B → restart → Director/creature run text-only, vision no-ops).

## E15 — 2026-06-18 — `--game-shot`: a framed marketing/QC screenshot mode (walk-then-render) [operator request]
- **What:** a new headless mode `run_game_shot` (`--game-shot`) that walks the natural **Stroller** into the maze
  (default 1800 ticks; `--ticks` tunes distance) so the camera sits MID-CORRIDOR, then renders ONE clean frame
  from its POV — ray-traced (`--rt`, converged `--spp`, default 320) or raster. `--seed` varies the world,
  `--out` the PNG. The existing `--shot` / `--dxr-pt` sit at the fixed (16,16) spawn cell (often against a wall);
  this frames naturally (the Stroller looks DOWN corridors — low faceplant, per `--strollcheck`).
- **Why:** operator — "render me some nice screenshots, some raster but most ray tracing, without flashlight."
  Produced the delivered batch (5 RT moody + 3 raster bright, 1920×1080); none enable the flashlight.
- **Without breaking anything:** purely ADDITIVE — a new run mode + one Options bool (`game_shot`) + one parse
  arm + one dispatch line; touches NO existing function, NO gated path, NO golden, NO module inventory (run modes
  aren't inventory). Reuses the proven Stroller + `build_walk_collision` + `render_pt`/`render_chunks` paths.
- **Files:** `app/src/main.cpp` (only). **Rollback:** `git revert <commit>`.
- **Verified:** build clean (/WX); ~12 `--game-shot` renders (RT + raster, varied seed/ticks) all
  **debug_error_count=0**; the audit's ctest/determinism are unaffected (no existing code touched). Aesthetic note:
  raster lights the whole ceiling (bright, classic Level-0), PT leaves it dark between the emissive strips (moody).

## E16 — 2026-06-18 — Green flare "breadcrumbs" (R): analytic emissive point lights in the PT [operator request, ADR-080]
- **What:** `R` drops a green "chemlight" flare at the wanderer's feet — it LIGHTS the ray-traced scene (green
  cast on nearby surfaces + a visible glowing point) and serves as a breadcrumb. **Option A** (analytic point
  lights, no scene geometry). New `app/src/flares.h` (`FlareField` — a pure ring buffer, cap 256, oldest
  recycles → the trail decays behind you; `pack_nearest` pre-culls to the nearest 64 for the GPU). DXR: PT root
  SRV **t2** `StructuredBuffer<float4> g_flares` (mirror of the t1 vertex buffer) + `uFlareN` (repurposed CB pad)
  + `DxrRenderer::set_flares()`; shader `flare_light()` (shadow-rayed green cast, ≤7 m) + `flare_glow()` (the
  visible point), both `[branch]`-guarded. `run_game`: R edge-toggle, per-frame nearest-cull + `set_flares`,
  drop forces a PT accumulator reset. A `--flares` QC flag on `--game-shot` drops a line ahead for the A/B.
- **Why:** operator — green flare breadcrumbs, mapped to R, "perfect for ray tracing." Answered their scaling
  question: memory trivial (256 ≈ 8 KB; GPU buffer fixed 1 KB), cost bounded by the nearest-64 cap + 7 m shader
  cull, so it's flat whether 5 or 999 are dropped (cap is 256; bump trivially).
- **Regression-proof + EASY ROLLBACK (operator's explicit ask):** flares default to NONE; the golden/offline
  path (`render_pt` / `--dxr-pt`) never calls `set_flares` → `uFlareN==0` → the `[branch]` flare code is skipped
  and the (bound but empty) t2 buffer is unread → **PT output bit-identical**. Tagged **`pre-flares`** (pushed)
  BEFORE any work → rollback is `git reset --hard pre-flares`, zero debug; or `git revert` the one change-set.
- **Files:** `app/src/flares.h` (new), `render_dxr/include/render_dxr/dxr.h`, `render_dxr/src/dxr.cpp`,
  `app/src/main.cpp`, `docs/USER_GUIDE.md`, `scripts/package.ps1`, `docs/DECISIONS.md` (ADR-080),
  `docs/CHANGE_AUDIT_LOG.md`. **Rollback:** `git reset --hard pre-flares` OR `git revert <commit>`.
- **Verified:** `gate.ps1 -Milestone M9` **PASSED** — converged PT goldens **bit-identical** (diff
  0.000004/0.000677/0.000000), 178 FPS, no-ghost, 1 km TLAS walk debug-clean (the new root SRV t2 + shader are
  inert with no flares). `audit.ps1` build+ctest 109/109+determinism+inventory+isolation green. Live `--game
  --rt` smoke (flare build, none dropped): rt_frames 863, **debug_error_count 0**, lookcheck PASS. A/B
  (`--game-shot --rt --flares`, 2 scenes): green cast-light + glowing orbs, debug-clean
  (`runs\shots\flares_off/on/on2.png`). **RT-only** for now (raster: no flares — a noted follow-up). Tunables:
  `kFlareColor`/`kFlareReach`/`kFlareGlowR` (dxr.cpp), intensity 2.2 + cap 256 (`FlareField::kCap`).

## E17 — 2026-06-18 — Bundle the D3D12 Agility SDK redist: RT works on a clean Win11 (itch.io-ready) [ADR-081]
- **What:** vendored the **D3D12 Agility SDK redist** (Microsoft.Direct3D.D3D12 **1.619.3** from nuget.org) into the
  bundle — `D3D12Core.dll` + `d3d12SDKLayers.dll` under `dist\Backrooms\D3D12\` — and the RELEASE exe now **exports**
  `D3D12SDKVersion=619` + `D3D12SDKPath=".\\D3D12\\"` (`app/src/main.cpp`, `#ifdef BR_RELEASE`). This makes the OS
  d3d12 loader pull the **bundled** runtime + validation layer, so ADR-077's forced validation (the RT-crash fix)
  works on a clean end-user Win11 that lacks the **Graphics Tools** feature — without it, RT crashes there.
- **Why:** operator is USB-copying a zip to a NEW machine then uploading to itch.io. A clean machine is exactly
  where the ADR-077 blocker bites. This closes it — the zip becomes the real shippable artifact.
- **Without breaking anything (release-gated):** the exports are `#ifdef BR_RELEASE`, so the DEBUG build (every
  gate) has no exports and no `.\D3D12\` beside `build\bin` → it uses the OS D3D12 exactly as before. Zero gate
  impact by construction. `package.ps1` treats the two DLLs as persistent verified bundle assets (never sourced
  from C:\). Additive.
- **Files:** `app/src/main.cpp` (release-only exports), `scripts/package.ps1` (stage + verify the D3D12\ folder),
  `docs/DECISIONS.md` (ADR-081), `docs/CHANGE_AUDIT_LOG.md`; bundle: `dist\Backrooms\D3D12\*.dll` (gitignored, like
  the rest of the bundle; backup in `runs\agility\`). **Rollback:** `git revert <commit>` + delete `dist\Backrooms\D3D12\`.
- **Verified:** release build clean; **`dumpbin /exports`** shows `D3D12SDKVersion` + `D3D12SDKPath` on the staged
  exe; the running **bundle exe LOADS `dist\Backrooms\D3D12\D3D12Core.dll` + `D3D12SDKLayers.dll`** (module-path
  check — the *bundled* redist, not System32 — proving the mechanism), RT smoke **rt_frames=1682, debug_error_count=0,
  lookcheck PASS**. (This box has Graphics Tools so it can't prove the *clean-machine* case directly — that is the
  operator's USB test; the wiring is proven correct.) Debug build/gates unaffected (exports release-only).

## E18 — 2026-06-18 — POC: the world RECOLOURS from what you SAY (LLM-driven dynamic world, step 1) [DYNAMIC_DIRECTOR]
- **What:** the first proof that the Director's perception can MUTATE the world, not just narrate. New headless mode
  `--recolor-shot --say "<phrase>"` (`run_recolor_shot` in main.cpp): build a raster scene (Stroller walk → POV),
  render it, ask the **live LLM** (`keel_complete`) to pick a wall colour from the player's utterance, CPU colour-grade
  the readback toward that hue (brightness-preserving — vivid recolour, not a dim multiply), write
  `<out>_before.png` + `<out>_after.png`. Three helpers: `render_recolor_prompt` / `parse_recolor` (first 3 ints,
  tolerant) / `apply_recolor` (luminance-preserving hue wash).
- **Why:** operator — "implement the simplest thing first as a POC I can test directly: 'I hate this yellow' → red."
  Step 1 of the LLM-driven dynamic-world brainstorm (see the next session note / a future `docs/DYNAMIC_DIRECTOR.md`):
  the scene changes based on what the player says — uncanny because it answers arbitrary natural language.
- **Verified (KEEL up):** `--recolor-shot --say "change the walls to deep red"` → LLM `204,0,0` → vivid red corridor
  (`recolor_red_before/after.png`). After a prompt tune (infer "dislike yellow → change it"): **"I hate this yellow"
  → `40,100,200`** (blue — it changed the hated colour) ✓; "this place is so depressing and cold" → a colour ✓;
  **"is anyone there" → NONE** (no recolour — restraint, not randomness) ✓. The render→LLM→grade→PNG chain works.
- **Without breaking anything:** purely ADDITIVE — a new run mode + 3 static helpers + one Options bool + one parse
  arm + one dispatch line. NO existing function/sim/gate/golden touched; presentation-only (a CPU grade on a
  readback). **Files:** `app/src/main.cpp` only. **Rollback:** `git revert <commit>`.
- **Next (the real thing):** wire it LIVE into `run_game`'s voice loop (mic → whisper → this colour call → a GPU
  wall-tint uniform in the raster shader) so speaking recolours the world in-game. Then the rest of the mutation
  palette (lighting, spawn, doors, rearrange-behind-you). Raster-first (RT too slow — see `docs/RT_PERF_PLAN.md`).

## E19 — 2026-06-18 — Phase H step 1: the apparition sense ("it sees what isn't there"), gate-able [ADR-082]
- **What:** the flagship impossible-before-VLMs mechanic (spec: `docs/APPARITION_SENSE.md`), Phase 1. The creature's
  existing vision call now ALSO reports a **coarse apparition** — emergent pareidolia in the rendered frame
  (a face/figure/word/arrow in the procedural grime that neither the player nor the engine placed). `ShoggothIntent`
  gains `apparition`/`app_kind`/`app_sector`; `parse_shoggoth_intent` reads `"apparition"`/`"app_where"`;
  `event_from_intent`/`apply_event_to_intent` pack/unpack into the spare **`ShoggothEvent::_reserved`** slot;
  `render_shoggoth_vision_prompt` asks for it (conservative — most frames none) and lets it unsettle the creature.
  Reaction is EMERGENT (the brain picks a fearful action/mood/utterance) — no behaviour code, coarse→soft only.
- **Why:** operator — execute the spec'd "it sees what isn't there." The perceiver's fallibility = the horror
  (honest shared uncertainty). Step 1 of `docs/DYNAMIC_DIRECTOR.md`'s VLM-native direction.
- **Determinism / no breakage:** the verdict rides `_reserved` → **`sizeof(ShoggothEvent)` stays 40** (static_assert
  held), **no `SHOGLOG` version bump**, old logs read as "no apparition", defaults "nothing seen" → byte-unchanged
  from M20/M29. The VLM runs at RECORD time; the verdict enters as a logged event → replay model-off reproduces it
  (M22 pattern). `utterance` stays presentation-only.
- **Files:** `app/src/shoggoth.h`, `app/src/shoggoth_brain.h`, `app/src/shoggoth_vision.h`,
  `tests/unit/test_shoggoth.cpp`, `docs/DECISIONS.md` (ADR-082), `docs/CHANGE_AUDIT_LOG.md`.
  **Rollback:** tag **`pre-apparition`** (pushed) → `git reset --hard pre-apparition`, zero debug; or `git revert`.
- **Verified:** `audit.ps1` green — build /WX, ctest **110/110** (new `[apparition]` round-trip + sizeof==40),
  text record==replay, inventory, isolation. **`gate.ps1 -Milestone M29` PASSED** — VISION record→replay
  **bit-identical** (`a560f43…`) with the **live VLM emitting the apparition** (`valid_intents=5`), creature escaped
  (`final_state=0`); descent + M21 + M20 + M5 regressions green. (Manual mismatch was a `--level 2`-omitted-on-replay
  command error — the log stores seed/ticks/events, not level — NOT a determinism bug.)
- **Next (Phase 2):** the apparition read on the PLAYER's POV (not the creature's) + a soft atmosphere gate, so the
  creature reacts to the face the PLAYER is looking at. Live-only, same event-log shape.

## E20 — 2026-06-18 — Apparition Phase 2a: the read on the PLAYER's POV + an audio atmosphere gate [ADR-083]
- **What:** the uncanny front half of the deferred Phase 2 — in the live game the creature + atmosphere now react to a
  face/figure in **what the PLAYER is looking at**, not its own vantage. `run_game` reuses the existing 2nd headless
  device + vision host: **every 3rd vision cycle renders `br::core::wanderer_camera`** (the player's view) instead of
  the creature POV (`svInFlightPlayer`, decided at warm-start). A player cycle's poll takes ONLY the apparition verdict
  (never the seen-target/motion fields) → a present verdict makes the **PA murmur** about it (`speak_pa`, cooldown-gated)
  and **thins the soundscape** (a soft decaying dip to 0.6× on master/sfx volume over ~9 s). Creature cycles unchanged.
- **Why:** operator — "proceed to implementation, you are cleared." Phase 1 (ADR-082) proved the determinism on the
  creature's own POV; this delivers the actual horror thesis — "it reacted to the face *I* was looking at."
- **Why audio-only atmosphere:** (QC-corrected wording) a `FlickerSector` directive *type* exists in the vocabulary and
  `core::light_flicker` is the deterministic per-light baseline, but the `FlickerSector` directive is **parsed-and-never-
  consumed in `run_game`** (only `WandererNote` is acted on) → no *verdict-driven* dim lever today; wiring one is a
  renderer-contract + shader change, split out as **Phase 2b**. The audio levers + `speak_pa` already exist → Phase 2a is
  **zero renderer changes, additive, live-only**.
- **Determinism / no breakage:** `run_game` is presentation-only (INV-1) — same class as the live text brain / flares /
  flashlight. No sim state, nothing hashed/logged; the record/replay path (`run_shoggoth_vision_record`) is untouched.
- **Files:** `app/src/main.cpp` (run_game only: cycle parity + player camera + poll routing + audio dip + 2 telemetry
  counters), `docs/APPARITION_SENSE.md`, `docs/DECISIONS.md` (ADR-083), `docs/CHANGE_AUDIT_LOG.md`.
  **Rollback:** tag **`pre-phaseH2`** (pushed) → `git reset --hard pre-phaseH2`; or drop the `svInFlightPlayer` branch
  (creature path is byte-identical without it).
- **Verified:** `audit.ps1` green — build /WX, ctest **110/110**, shoggoth record==replay, inventory, isolation. Live
  `--game --auto-play --seconds 100` smoke (KEEL up): `svision_player_reads:1`, **`apparition_hits:1 (kind=figure)` on
  the player's own view**, `svision_intents:3` (creature motion intact), `debug_error_count:0`, `lookcheck:PASS`.
  **`gate.ps1 -Milestone M29` PASSED** — VISION record→replay **bit-identical** (`955e0f4b…`), descent + M21 + M20 + M5
  regressions green.
- **Next (Phase 2b):** the **visual** atmosphere cue (a soft lighting dim/flicker on a lingering verdict) — needs a new
  raster brightness uniform (renderer contract + shader). Separate increment.

## E21 — 2026-06-18 — Apparition `app_strength`: close the spec §4 gap a QC found [ADR-083a]
- **What:** an operator QC of `docs/APPARITION_SENSE.md` vs the code caught that the spec's **`strength` 0–3** verdict
  field ("how strongly it reads") was **dropped** when Phase 1 flattened the schema. This adds it. `ShoggothIntent` gains
  `uint8_t app_strength` (0 none / 1 faint / 2 clear / 3 vivid); `parse_shoggoth_intent` reads `"app_strength":<1-3>`
  when a verdict is present (default 2 "clear"; clamp 1..3); it packs into **`ShoggothEvent::_reserved` bits 24–31**
  (free) → `sizeof` stays 40, no SHOGLOG bump, strength-0 byte-identical. The prompt asks for it. **The reaction now
  scales with strength** (live `run_game`): the audio dip deepens (0.33×→0.59×) + lengthens (7→11 s), and the PA murmurs
  **only for clear/vivid (≥2)** — a faint hint shifts the atmosphere alone.
- **Why:** faithful to the documented ask — the spec listed strength so the dread could scale with how vividly the shape
  reads; the binary version couldn't. Also fixed two doc-wording points the QC surfaced (the "no brightness lever"
  overstatement → the `FlickerSector`-parsed-but-unconsumed precise statement, in ADR-083 + E20).
- **Determinism / no breakage:** same M22 pattern — VLM emits strength at RECORD time, it rides `_reserved` into the
  combined hash via `fold_bytes`, replay reconstructs it. The no-apparition case stays `_reserved==0` (test-asserted).
  The determinism hash *value* shifted (a present apparition now carries strength) but **record==replay holds**.
- **Files:** `app/src/shoggoth.h`, `shoggoth_brain.h` (parse + pack/unpack), `shoggoth_vision.h` (prompt),
  `app/src/main.cpp` (strength-scaled dip/murmur + telemetry), `tests/unit/test_shoggoth.cpp`, `docs/*`.
  **Rollback:** tag **`pre-strength`** (pushed) → `git reset --hard pre-strength`.
- **Verified:** `audit.ps1` green — build /WX, **ctest 110/110** (the `[apparition]` test now covers strength round-trip
  via bits 24–31, clamp 9→3, default absent→2, sizeof==40). **`gate.ps1 -Milestone M29` PASSED** — VISION record→replay
  **bit-identical** with the live VLM now emitting `app_strength`; M21 + M20 + M5 regressions green. Live `--game` smoke
  debug-clean, `strength=%u` wired into telemetry.
