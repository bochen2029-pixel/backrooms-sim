# BACKROOMS SIM — CONTINUATION PROMPT (paste to resume; the autoloop/SessionStart also feeds this)

You are continuing the **autonomous Backrooms Phase IV (the Vertical Backrooms) build** at
`C:\backrooms`. **DO NOT STOP, DO NOT ASK, DO NOT PAUSE TO REPORT.** Build first-order milestones to
green, bank commits, and keep going until the roadmap is exhausted (`.brstate\DONE`). The operator is
away/working; a blocking question or a "want me to continue?" HALTS everything (a sibling KEEL instance
blocked all night — never repeat that). Decide-and-document every fork yourself; route only true
operator-only acts (hand-edit a golden / relax a gate / publish) to `_run_state/ROADMAP.md` §5 ISSUES
and press on. Minimal meta — no scaffolding-on-scaffolding; the bulk of effort goes into real milestone code.

## RECONSTITUTE (verify by artifact, never recall)
1. Read memory **`project-phase-IV-vertical.md`** (the locked Phase IV design + the EXACT current
   position + the standing directives) → **`_run_state/ROADMAP.md`** §0 loop + §2 slice list →
   newest **`docs/SESSION_LOG.md`** entry → **`_run_state/AUTOSTART.md`**.
2. Verify: `git -C C:\backrooms log --oneline -6` (HEAD ≈ the latest M27 WIP / a green tag) ·
   `scripts/build.ps1` + `ctest --test-dir build --output-on-failure` green · `git status` clean.
   Drift → the artifact wins. A red prior gate → revert to the last `m<N>-green` (Iron Rule 2).

## CURRENT POSITION (update this line as you go)
- ✅ Autopilot rig + perpetual memory + M26 (`m26-green`, live multi-level) shipped.
- ✅ **M27 (procedural stairs) DONE — `m27-green`, gate exits 0, tagged + pushed.** `stair_at` hybrid
  placement; aligned floor/ceiling holes; a climbable thin riser-slab stairwell + collision; stair-aware
  carve; **step-up locomotion** in `move_and_collide` (inert for walls → prior collision bit-identical);
  `validate_vertical_connectivity` (Z flood-fill). Proven: live ascent to level 1 (`--ascend`, bit-id ×2),
  `--descend` still ↓ to -1, walk-bot 1 km × 0 stuck, only the 2 m4 top-down goldens re-baselined
  (ADR-053). Commits `e7543bf` → `70f0eef` → the gate commit.
- 🔨 **NEXT = M28 (vertical streaming + see-through).** Two-floor residency at a stairwell so the live
  ascent crosses the seam seamlessly (currently the live walk streams a single level via
  `level_from_y(pos.y)` — the ascent works headlessly but a live climb needs both floors resident at the
  seam) + render down through a floor hole. **Gate:** stand at a stairwell, both floors resident +
  rendered, debug-clean, memory-slope ~0. **Deps:** M27 (done). See `_run_state/ROADMAP.md` §2. Then
  **M29** (per-floor Shoggoth; needs KEEL sidecar :7071), **M30** (open shafts).

## THE LOOP (per ROADMAP §0)
pick next step → tight change manifest (≤400 LOC; sim core `/fp:strict`, seeded PCG64, no wall-clock;
**level-0 byte-identical** — keep `world_state_hash` unchanged, derive level from `pos.y`) → implement →
`scripts/gate.ps1 -Milestone M<N>` until exit 0 → **commit green** (trailer `Co-Authored-By: Claude
Opus 4.8 (1M context) <noreply@anthropic.com>`) → `git tag m<N>-green` → **push branch+tags** → mark
the slice `[x]` in ROADMAP + append SESSION_LOG + update the memo's current-position line → **repeat,
do not stop.** Halt only at ~90% context (checkpoint + exit; the supervisor/next session resumes) or
`.brstate\DONE`. Keep the KEEL sidecar (`C:\keel-sidecar-7071\start.cmd`, `:7071`) up for brain gates.
`C:\KEEL` is READ-ONLY. Secret-scan diffs. Never hand-edit a golden or relax a gate.
