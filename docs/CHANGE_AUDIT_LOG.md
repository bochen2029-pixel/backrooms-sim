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
