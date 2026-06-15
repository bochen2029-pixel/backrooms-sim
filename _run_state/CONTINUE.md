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
- ✅ **M30 (open shafts & the abyss) DONE — `m30-green`, gate exits 0, tagged + pushed.** `shaft_at` (pure
  per-column, ~1/1500, depth 5–10) + per-level void hole-cutting + a soft-catch fall (`--shaftfall`:
  full-depth fall + land, bounded, bit-id ×2 — no new physics, the bottom floor catches you). ADR-055.
  Polish deferred (telegraph audio, fog-to-black, deep soak). Commits `07f7d26`→`7c263ff`→`30b6508`→gate.
  **Phase-IV milestones M26–M30 are all green or (M29) model-blocked.**
- ⏸ **M29 (per-floor Shoggoth) — CODE COMPLETE; gate BLOCKED on the model server (ROADMAP §5 ISSUE-5).**
  Inc1 ✅ (core+escape, `[m29]` green) + Inc2 ✅ CODE (`56df9e2`: descent record/replay gated by `--level`,
  `Invoke-GateM29` + dispatch). PROVEN MODEL-FREE: `--shoggoth-record/replay --level 2` → identical hash
  `5652627d017bb899` + `final_state 0` (Lurk = escaped). Only `valid_intents>=1` needs the brain → needs
  `llama-server :8080` UP (the :7071 sidecar reuses it; both were down). To reach `m29-green`: start :8080
  → autoloop `Ensure-Sidecar` :7071 → `gate.ps1 -Milestone M29`. Do NOT relax valid-intents. (Design ref below.)
- 🔨 **NEXT (all MODEL-FREE; round-robin, kill-timer each):** this is polish/closing now. (1) **live descent**
  — run_play's fake ground plane catches you, so you can't fall through shafts/down-stairs in-game; give it
  holes (or real per-cell floor) at shaft + down-stair cells so the despair-gradient works live (ascent
  already works). (2) **M30 telegraph** — draft/wind audio near a shaft (decision 6; off the sim hash).
  (3) ✅ **M30 fog-to-black DONE** (`1a067bb`) — StreamManager range update (`[m30]` test) + `--abyss` proof
  (depths show through the void, bounded, debug-clean, gated `abyss_ok`) + run_play/run_game open a downward
  band over a shaft. NOTE: this is the SEE-down; the FALL-through-in-game is item (1) live-descent (still
  open — the fake ground plane still catches you, so you see the abyss but can't yet fall into it live).
  (4) ✅ **M28-DXR see-through
  CONFIRMED** (read `render_dxr/src/dxr.cpp:738` — `build_scene` builds a BLAS per resident chunk at its
  world-Y, no `(cx,cz)` dedup, so 2-level residency renders both floors in the ray-traced path too; no DXR
  change needed). (5) **deep-descent soak** (blocked on live descent (1)). (6) the **M0–M25 regression
  sweep** — done this session: 27 level-0 goldens (m1/m2/m4/m5/m7/m8) bit-identical, 98 ctest green,
  10-seed ascend/fall robustness all pass. Then Phase-IV DONE checks (ROADMAP §3) + perpetual-polish (§4).
  The Shoggoth lives
  OUTSIDE WorldState (`app/src/shoggoth.h`), pos is 2-D (X/Z; pos.y = spawn height, never moves vertically),
  nav already takes `seed` + 2-D cells but **hardcodes `ChunkKey{0,cx,cz}`** in `maze_open` (~L62-78).
  No frozen shoggoth-replay golden exists (the M21 gate records→replays FRESH + compares), so folding `level`
  into `shoggoth_hash` is SAFE. **INCREMENT 1 (~15 lines, low-risk, do first):** (a) add `int32_t level=0;`
  to `struct Shoggoth` (`shoggoth.h` ~L37-46); (b) in `shoggoth_step` (~L133-217) set `sh.level =
  contracts::level_from_y(sh.pos.y);` near the top; (c) thread `level` into `next_step_dir`(~L83-112) +
  `maze_open`(~L62-78) → use `ChunkKey{level,cx,cz}` (confines nav to the floor; per-level maze → per-(seed,
  level) DISTINCT behaviour for free — no rng re-seed needed); (d) fold `sh.level` into `shoggoth_hash`
  (~L220-230); (e) `[m29]` test in `tests/unit/test_shoggoth.cpp` (~L31-77 pattern): same seed, levels 0 vs 1
  → DIFFERENT hashes; deterministic (run twice == ); still navigates. Build + `[m29]` green → commit.
  **INCREMENT 2 (escape + the sacred gate):** (f) make hunt/sense level-aware in `shoggoth_step` — if
  `level_from_y(wanderer.y) != sh.level`, the Shoggoth can't sense the wanderer (stay Lurk) → **descending a
  stair ESCAPES it**; (g) extend `run_shoggoth_record`/`run_shoggoth_replay` (`main.cpp` ~L1996/L2344) so the
  MazeWalker descends mid-run (or add a scripted level change) and assert **record==replay bit-exact MODEL
  OFFLINE across the level change** (the M21 sacred gate, extended); (h) `Invoke-GateM29` + dispatch:
  `[m29]` + confinement (sh.level constant w/o a stair) + per-(seed,level) distinct + escape (wanderer on a
  different level → Shoggoth loses the hunt) + **sacred record==replay-offline across a descent** + M20
  brain-off determinism + M5/INV-5/inventory regression → `m27`…`m29-green`. **Live-brain path needs KEEL
  sidecar :7071** (`C:\keel-sidecar-7071\start.cmd`, NEVER :7070; currently DOWN — launch via
  `scripts/autoloop.ps1` Ensure-Sidecar or the .cmd); the determinism gate itself runs OFFLINE. **Deps:** M27.
  Then **M30** (open shafts & the abyss — telegraphed bounded soft-catch fall + fog render + deep-descent soak).

## THE LOOP (per ROADMAP §0)
pick next step → tight change manifest (≤400 LOC; sim core `/fp:strict`, seeded PCG64, no wall-clock;
**level-0 byte-identical** — keep `world_state_hash` unchanged, derive level from `pos.y`) → implement →
`scripts/gate.ps1 -Milestone M<N>` until exit 0 → **commit green** (trailer `Co-Authored-By: Claude
Opus 4.8 (1M context) <noreply@anthropic.com>`) → `git tag m<N>-green` → **push branch+tags** → mark
the slice `[x]` in ROADMAP + append SESSION_LOG + update the memo's current-position line → **repeat,
do not stop.** Halt only at ~90% context (checkpoint + exit; the supervisor/next session resumes) or
`.brstate\DONE`. Keep the KEEL sidecar (`C:\keel-sidecar-7071\start.cmd`, `:7071`) up for brain gates.
`C:\KEEL` is READ-ONLY. Secret-scan diffs. Never hand-edit a golden or relax a gate.
