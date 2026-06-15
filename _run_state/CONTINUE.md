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
- 🔨 **M27 (procedural stairs) IN PROGRESS — geometry DONE, validator+gate NEXT.** DONE so far:
  `stair_at` placement (`a8d4b12`, `[m27]` green); **floor/ceiling holes** cut in `gen/src/chunk.cpp`
  (floor opens where `stair_at(seed,L-1,..)` fires, ceiling where `stair_at(seed,L,..)` fires — aligned
  by the same fn); **thin grounded riser-slab stairwell** + collision in `GenerateChunk` (8×0.5 m risers,
  inset, abutting — passes `ValidateChunkGeometry`; pillar pass skips the stair cell); **stair-aware
  carve** in `generate_layout` (opens the stair cell's interior walls, perimeter/seams untouched). All
  94 ctest green. **Goldens re-baselined** via `goldgen capture` — only `goldens/m4/topdown_seed{1,7}`
  changed (top-down sees the ceiling holes); all m5 shots / m1 / m2 / m7 biomes byte-identical, walk-bot
  1 km × 0 stuck on all 5 seeds. ADR-053 written. **NEXT, in order:** (d) a **vertical-connectivity
  validator** (Z flood-fill / superblock reachability — make "no floor ever sealed" machine-checkable) +
  an `[m27]` test; (e) `Invoke-GateM27` + dispatch in `scripts/gate.ps1` → run `gate.ps1 -Milestone M27`
  to exit 0 → `git tag m27-green` → push branch+tags → mark ROADMAP §2 M27 `[x]` + SESSION_LOG. Then
  **M28** (vertical streaming + see-through), **M29** (per-floor Shoggoth; needs KEEL sidecar :7071),
  **M30** (open shafts).

## THE LOOP (per ROADMAP §0)
pick next step → tight change manifest (≤400 LOC; sim core `/fp:strict`, seeded PCG64, no wall-clock;
**level-0 byte-identical** — keep `world_state_hash` unchanged, derive level from `pos.y`) → implement →
`scripts/gate.ps1 -Milestone M<N>` until exit 0 → **commit green** (trailer `Co-Authored-By: Claude
Opus 4.8 (1M context) <noreply@anthropic.com>`) → `git tag m<N>-green` → **push branch+tags** → mark
the slice `[x]` in ROADMAP + append SESSION_LOG + update the memo's current-position line → **repeat,
do not stop.** Halt only at ~90% context (checkpoint + exit; the supervisor/next session resumes) or
`.brstate\DONE`. Keep the KEEL sidecar (`C:\keel-sidecar-7071\start.cmd`, `:7071`) up for brain gates.
`C:\KEEL` is READ-ONLY. Secret-scan diffs. Never hand-edit a golden or relax a gate.
