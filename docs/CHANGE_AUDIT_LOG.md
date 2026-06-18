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
