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
- ✅ **M28 (vertical streaming + see-through) DONE — `m28-green`, gate exits 0, tagged + pushed.**
  `StreamManager::update(center, extra_level)` keeps a 2nd ring at one adjacent level (climbing → above,
  else below); 1-arg path delegates → single-level bit-identical. See-through automatic (verts baked at
  `level_base_y`, cache keyed by full `ChunkKey`). Proven via `--vstream` (both floors resident `2x` ring
  bounded + rendered debug-clean, floor above visible through the hole `see_through_diff ~57-62`); M5 golden
  + M27 ascent intact. `[m28]` test. ADR-054. Commits `050d08c` → `--vstream`+gate → gate commit.
- 🔨 **NEXT = M29 (per-floor Shoggoth).** Confine the Shoggoth to its level, seed it per `(seed, level)`,
  and let descending a stair escape it (it can't follow across the seam); record→replay across a descent
  **bit-exact with the model OFFLINE** (the M21 sacred gate, extended to verticality). **NEEDS the KEEL
  sidecar at :7071** (`C:\keel-sidecar-7071\start.cmd`; NEVER :7070) for the live-brain path — but the
  determinism gate runs model-offline. Study M20/M21's shoggoth (`app/src/main.cpp` run_shoggoth*, the
  event-log→effective-tick design) before changing it. **Deps:** M27. See `_run_state/ROADMAP.md` §2. Then
  **M30** (open shafts & the abyss — telegraphed bounded soft-catch fall + fog render + deep-descent soak).

## THE LOOP (per ROADMAP §0)
pick next step → tight change manifest (≤400 LOC; sim core `/fp:strict`, seeded PCG64, no wall-clock;
**level-0 byte-identical** — keep `world_state_hash` unchanged, derive level from `pos.y`) → implement →
`scripts/gate.ps1 -Milestone M<N>` until exit 0 → **commit green** (trailer `Co-Authored-By: Claude
Opus 4.8 (1M context) <noreply@anthropic.com>`) → `git tag m<N>-green` → **push branch+tags** → mark
the slice `[x]` in ROADMAP + append SESSION_LOG + update the memo's current-position line → **repeat,
do not stop.** Halt only at ~90% context (checkpoint + exit; the supervisor/next session resumes) or
`.brstate\DONE`. Keep the KEEL sidecar (`C:\keel-sidecar-7071\start.cmd`, `:7071`) up for brain gates.
`C:\KEEL` is READ-ONLY. Secret-scan diffs. Never hand-edit a golden or relax a gate.
