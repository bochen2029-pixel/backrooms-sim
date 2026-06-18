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
