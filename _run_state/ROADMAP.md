# BACKROOMS SIM — ROADMAP (the single forward plan: NOW → DONE → perpetual polish)

> **What this is.** The durable, machine-followable blueprint from the current state to completion,
> then a self-improvement loop that never "stops." **Any autonomous session reads this (after the
> `rehydrate.ps1` brief + the newest `SESSION_LOG.md` entry) and knows the exact next action — zero
> operator re-explanation.** Forward complement to `SESSION_LOG.md` (the chronological trail) +
> `PROGRESS.md` (the you-are-here cursor). **Trust `git` + the gate over this doc for *state*; trust
> this doc for the *plan + completion criteria*.** `docs/MILESTONES.md` holds the detailed milestone
> specs; this ROADMAP holds the loop + the slice checklist + the ISSUES queue.

---

## 0 · THE AUTONOMY CONTRACT (how a session uses this file — the loop)
You run under the operator's standing grant (`_run_state/AUTOSTART.md`). Run this loop without asking:
1. **Reconstitute + verify by artifact** (AUTOSTART §1): newest `SESSION_LOG.md` → this ROADMAP →
   active `MILESTONES.md` section → memory; then `build.ps1` + `ctest` green, `git status` clean, HEAD
   at the last `m<N>-green`. Drift → the artifact wins. A red prior gate → revert to last green.
2. **Pick the next slice:** the first `[ ]` below whose deps are `[x]` and which is **not** `[G]`/`[!]`.
   Respect dependency order.
3. **Build it** to a **change manifest** (≤400 LOC), against the canon + the target MODULE.md. Never
   bend a contract/golden to ease an impl — if a slice seems to need that, the boundary is wrong: log
   an ISSUE and skip.
4. **Gate, bank, push:** `gate.ps1 -Milestone M<N>` exits 0 → secret-scan the staged diff → one commit
   (one-line intent + the Co-Authored-By trailer) → `git tag m<N>-green` → **push branch + tags**.
5. **Record:** mark the slice `[x]` here, update `SESSION_LOG.md` + `PROGRESS.md` + the memo (**same
   commit** as the slice).
6. **Loop** to step 2 until **~90% context**, then checkpoint + exit (the supervisor respawns). Never
   continue through a forced compaction.
7. **Decide-and-document on EVERYTHING except the operator-only acts** (AUTOSTART §INVIOLABLE) — those
   → §5 ISSUES; **route around, never block.**

**Operator-touch is required exactly once** (turn the loop on — see `_run_state/AUTOSTART_SETUP.md`).
After that the loop self-perpetuates; the operator reviews §5 ISSUES whenever he chooses, never to keep
it moving. **Why it's safe:** the gate is a non-model oracle (bit-exact determinism + goldens +
debug-clean + pacing/connectivity/memory bounds) — a wrong step **cannot** tag green; every increment
is committed+tagged+pushed (auditable + revertible); the supervisor is the temporary external driver.

## Status legend
`[ ]` todo · `[x]` done · `[~]` in progress · `[!]` blocked (see ISSUES) · `[G]` operator-gated · `[?]`
unknown (needs a falsifier/decision)

---

## 1 · DONE (the foundation — do not redo; verify by `git log` + the `m<N>-green` tags)
**v1.0** headless visualization (M0–M12) · **v2.0** the playable, portable game (M13–M17) · **v2.1**
procedural app-icon polish. **Phase III "the Backrooms come alive" — COMPLETE through `m25-green`:**
M18 head-bob+run · M19 ray-tracing toggle · M20/M20b Shoggoth body + deterministic nav · M21 KEEL brain
· M21b live async brain · M22 vision · M23 hearing · M24 procedural-TTS PA voice · M25 Shoggoth body in
the DXR path. Determinism stayed sacred throughout (AI → event log → bit-exact replay, models offline).

---

## 2 · THE PLAN (NOW → DONE) — Phase IV: the Vertical Backrooms (infinite Z)
*Design locked (see the `project-phase-IV-vertical` memo + the SESSION_LOG brainstorm): infinite stacked
floors, non-repeating by construction (generation is already per-`level`), **hybrid** stair connectivity
**K=4 ≈ 128 m** (density scatter + 4×4-superblock hard backstop), **effectively-infinite near origin**
(float horizon; floating-origin deferred), **open shafts** — deep hashed **5–10 floors**, **accidental
but telegraphed**, **very rare ≈ 1.3 km**, soft-catch (no fail-state). Every slice headless-first +
gate-green. M7 already left the bones: `ChunkKey.level`, `level_base_y`, per-level `chunk_seed`/`biome_at`,
`build_stairwell`, `--descend`. The gap is integration (de-hardcode level 0) + in-world stairs/shafts.*

### Slice 0 — the autopilot rig (this) · `[x]` DONE
`_run_state/AUTOSTART.md` + this ROADMAP + `scripts/autoloop.ps1` + `_run_state/AUTOSTART_SETUP.md` +
`.brstate/` sentinels. Turns "a human starts each session" into a respawn supervisor. Reuses the gate
as the oracle, `m<N>-green` as the revert anchor, SESSION_LOG/PROGRESS/memory as the checkpoint.

### Phase IV milestones (dependency-ordered)
- `[x] M26` · **Live multi-level** (`m26-green`). De-hardcode `level` in run_play/run_game/run_walkbot/run_soak;
  wanderer tracks its current floor; **level-aware collision** + real per-chunk floor collision
  (retire the fake `{-1e6,-1,-1e6}..{1e6,0,1e6}` ground plane); per-level light Y (`level_base_y(L)+
  kCeilingHeight`, currently pinned to 3.0). **Gate:** M5 raster golden **bit-identical** (no
  regression — the load-bearing constraint: level-0-only stays byte-identical) + spawn-on-level-N
  renders/collides correctly + replay hash stable + M0–M25 regression. **Deps:** Slice 0.
- `[x] M27` · **Procedural stairs (hybrid, K=4)** (`m27-green`). `stair_at(seed,L,cx,cz)` (density + 4×4
  backstop) read identically from both floors (the vertical-seam shared hash, mirroring `door_index`);
  aligned ceiling/floor holes cut in `GenerateChunk`; a climbable thin riser-slab stairwell + collision;
  **stair-aware carve** (opens the stair cell, perimeter/seams untouched); **step-up locomotion** in
  `move_and_collide` (inert for full-height walls → prior collision bit-identical); a **vertical-
  connectivity validator** (Z flood-fill). **Gate PASSED:** every 4×4 superblock links up+down (`[m27]`);
  live ascent climbs to level 1, bit-identical ×2 (`--ascend`); `--descend` still reaches level -1;
  2-D connectivity holds with holes; walk-bot 1 km × 0 stuck; blast radius = only the 2 ceiling-facing
  top-down goldens, re-baselined via goldgen (**ADR-053**). **Deps:** M26.
- `[x] M28` · **Vertical streaming + see-through** (`m28-green`). `StreamManager::update(center, extra_level)`
  keeps a second concentric ring at one adjacent level (chosen by stance: climbing → `level+1`, else
  `level-1` to see down); the 1-arg path delegates to single-level so run_game/soak/tests stay bit-identical.
  See-through is automatic (each chunk renders at its baked world-Y; cache keyed by full `ChunkKey`).
  **Gate PASSED:** `[m28]` two-level residency (exactly `2x` ring, bounded, evicts on switch); `--vstream`
  proves both floors resident + rendered debug-clean at a stairwell with the floor above visible through the
  hole (`see_through_diff ~57-62`); M5 golden bit-identical + M27 ascent intact (presentation-only). ADR-054.
  **Deps:** M27.
- `[ ] M29` · **Per-floor Shoggoth.** Confined to its level; seeded per `(seed, level)`; descending (stairs
  or a shaft) **escapes** the current one; each floor has its own. **Gate:** the shoggoth never leaves its
  level; record→replay across a descent **bit-exact, model offline**. **Deps:** M27. **Needs the KEEL
  sidecar (:7071) for the brain gate** — else graceful-no-op.
- `[x] M30` · **Open shafts & the abyss** (`m30-green`; first pass — done before M29 finishes since M30 is
  model-free and M29-Inc2 needs the sidecar). `gen::shaft_at(seed,cx,cz)` — pure per-column, very rare
  (`~1/kShaftDensityN=1500`), hashed cell + top level + depth **5–10**; `shaft_passes` cuts each floor's
  void slice locally (additive to the M27 stair holes); the **soft-catch fall** needs no new physics — the
  void has no floor, gravity drops you, the bottom level's solid floor stops you via swept collision (no
  fail-state). **Gate PASSED:** `[m30]` placement (rare/deep/deterministic/per-seed) + `--shaftfall` (full-
  depth fall, lands, bounded, bit-identical ×2) + debug-clean shaft renders + M5 golden bit-identical (voids
  miss the views) + M27/M28 regressions. ADR-055. **Polish status:** ✅ live in-game descent (ADR-057, `gate
  M30` green — the interactive walks build a HOLED per-cell floor via `build_walk_collision`, so you fall
  through real down-stair holes + shaft voids in-game; + a pillar-over-void generation fix; all level-0 goldens
  bit-identical) · ✅ multi-floor fog-to-black abyss render (`1a067bb`) · ✅ deep-descent soak (`--descentsoak`,
  ADR-058, `gate M30` green — repeated deep falls hold determinism + bounded residency/memory over the long
  haul; the ROADMAP §3 DONE criterion) · ⏳ remaining: the draft-audio telegraph (decision 6, off the sim hash).
  **Deps:** M27, M28.

### Deferred / leftover (decide-and-document, or operator-gate)
- `[?] M31` · **floating-origin rebase** for *true*-unbounded XYZ (fixes the float horizon in 2-D too).
  Deferred this phase by operator choice (effectively-infinite-near-origin). Revisit only if a real need
  appears; record the decision. **Deps:** M26.

---

## 3 · DONE definition
Phase IV is **complete** when **all** hold: M26–M30 `[x]` (each `m<N>-green`, tagged, pushed) · the
vertical-connectivity validator green over a large sample · a deep-descent soak holds determinism +
bounded memory · `M31` decided (ON as its own phase, or OFF-with-rationale) · the §5 ISSUES resolved or
explicitly accepted · the full M0–M25 regression sweep still green. **Then the loop does not stop — it
enters perpetual-polish (§4).** Write `.brstate/DONE` only when all of the above holds.

## 4 · Perpetual-polish mode (post-DONE; until quota/power)
When §2 is exhausted: (1) `/code-review` the tree → fix findings; (2) raise gate/test/golden coverage
where thin; (3) re-check determinism + soak invariants with fresh seeds; (4) reconcile doc drift;
(5) a **completeness-critic** pass — "what's unverified, missing, or stale?" → new polish slices;
(6) harden + simplify (smaller, never larger). Each polish item is a gated/banked/pushed slice. Honest
about diminishing returns — bounded by the gate, not a promise of literal perfection. (Run a
completeness-critic pass every ~N build slices too, to catch drift early.)

## 5 · ISSUES / BLOCKERS register (operator-only + unknown — route AROUND; never block the rest)
- **ISSUE-1 [operator design-review]** — M30 open-shaft **soft-resolution UX**: the exact deceleration
  profile + audio/visual telegraph + whether a shaft ever lands you somewhere "special." Build the
  deterministic bounded-fall + catch first (decide-and-document the physics); reserve the *feel* tuning
  for the operator if it becomes load-bearing.
- **ISSUE-2 [decide-and-document]** — M27 stair density `N` (~12–16) + the 4×4 backstop trigger rate:
  tuned empirically against the vertical-connectivity validator. Not operator-only; pick, justify in
  SESSION_LOG, proceed.
- **ISSUE-3 [unknown/decision]** — M31 floating-origin ON/OFF (see `[?] M31`). Needs a real
  precision-horizon trigger to justify the work; default OFF this phase.
- **ISSUE-4 [operator-only]** — anything outward-facing (publishing / Steam / store assets). Never
  self-authorize.
- **ISSUE-5 [environment/blocked]** — the **M29 sacred gate** (`gate.ps1 -Milestone M29`) needs the KEEL
  model stack UP: `llama-server :8080` (the model backend) → `keel-serve.exe :7071` (the sidecar,
  `C:\keel-sidecar-7071\start.cmd`). Both were DOWN this session (operator away; launching :7071 found no
  :8080 backend). The gate CODE + the descent determinism are DONE + committed (`56df9e2`):
  `--shoggoth-record/replay --level 2` give an IDENTICAL `combined_hash` (`5652627d017bb899`) + `final_state
  0` (Lurk = escaped) model-free, so per-floor confinement + escape + cross-descent replay are proven; only
  `valid_intents>=1` (the live brain) is unmet. **Unblocks:** start `:8080` (operator's normal setup) →
  autoloop `Ensure-Sidecar` starts `:7071` → `gate.ps1 -Milestone M29` → `m29-green`. Do NOT relax the
  valid-intents assertion (that would gut the sacred gate). M29-Inc1 (core+escape) is already `[m29]`-green.
- **ISSUE-6 [polish/follow-on]** — the 2026-06-15 multi-agent QC audit (`docs/AUDIT_2026-06-15.md`; 82
  findings; `core`/`gen` clean, 0 confirmed criticals). This run FIXED the real ones (DXR InstanceID
  overflow guard, ARCHITECTURE §5 doc-drift, the stair+shaft same-cell geometry guard) and OVERTURNED the
  3 "stream race" false-positives (`in_flight_` is main-thread-only). Open, low-impact, NOT blocking:
  propagate the M29 `--level` descent-escape to the vision/hearing/PA record paths (level-0-only today;
  model-dependent), plus the catalogued mediums/lows (dead `build_stairwell`, `ChunkContentHash` omits
  collision, assorted doc-drift). Pull from the audit doc during perpetual-polish (§4).
- *(Append new issues as discovered: `ISSUE-N [type] — description · what unblocks it`. If the loop
  STALLS — only `[G]`/`[!]`/`[?]` remain and none can advance — write `.brstate/STALLED` with the reason
  so the supervisor stops respawning; the operator clears the queue on his next look.)*

## 6 · The cursor
`SESSION_LOG.md` newest entry + `PROGRESS.md` are the live you-are-here. **This ROADMAP is the map;
they are the pin.** A session: reconstitute → find the next actionable `[ ]` here → go.
