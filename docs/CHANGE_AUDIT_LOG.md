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
