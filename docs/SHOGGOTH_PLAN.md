# SHOGGOTH_PLAN.md — The Embodied Shoggoth: vision, hearing, speech, a brain, and *running well*

> Definitive plan (Session 37, 2026-06-17). Synthesises GLM-5.2's brainstorm
> (`_brainstorm/GLM/03`, grounded by the raw fan-out in `05`) with a direct re-verification of the
> live code and my own engineering judgement. **This is a spec, not code** — nothing here is built
> yet. Numbers and file:line anchors verified against HEAD `0d73c47`.
>
> The arc M20–M25 already shipped the Shoggoth's whole sensory *stack* (body, BFS nav, live text
> brain, record-time vision/hearing/voice). This plan makes those senses **live, coupled, and
> performant** — and, above all, makes the creature *feel like a mind* without ever requiring the
> model to be *right*.

---

## 0. The north star

Two sentences govern every decision below.

**Aesthetic law — "its mediocrity is the dread."** The Shoggoth is not a clever boss-AI. It is a
dim, persistent, slightly-wrong presence. It hunts a little incompetently, mutters things that are
*almost* about you, takes pointless detours, stops at blank walls. The horror is that it is as
trapped and as limited as you are — a fellow prisoner of the Backrooms, not a predator that wins.
**We must actively resist making it tactically smart.** A creature that out-thinks you is a game
boss; a creature that *almost* understands is a nightmare.

**Engineering law — narrow-channel embodiment (GLM's frame, and it's correct).** *Any feature that
needs the model to be right is both an engineering risk (non-determinism) and an aesthetic failure
(legibility kills dread). Deploy the model only through channels narrow enough that its errors stop
being checkable; the engine supplies all checkable truth.* The model proposes in **semantic** space
("a doorway, ahead-left, near"); the engine disposes in **metric** space (the exact cell, the
BFS route, the collision). The model can never be caught wrong because it never makes a claim the
player can verify against geometry.

These two laws are the same law from two sides: the model's job is *flavour that steers truth*, and
flavour is allowed — required — to be fallible.

---

## 1. What already exists (do not re-invent)

Verified live, header-only in `app/src/`, all `namespace br::app`:

| Organ | Exists today | Anchor |
|---|---|---|
| Body | `build_shoggoth_mesh` — 13 tentacles × 96 = **1248 verts**, writhe-animated, salmon material-7 in DXR | `shoggoth_body.h:49` |
| Locomotion | `shoggoth_step` FSM (Lurk/Hunt/Chase/Retreat) + `next_step_dir` bounded BFS (R=22, ~88 m) | `shoggoth.h:134`, `:84` |
| Brain (live) | `ShoggothBrainHost` — the **only** live host; latest-wins submit, non-blocking poll, off-thread, KEEL graceful no-op | `shoggoth_brain_host.h:37` |
| Eyes (record-only) | `shoggoth_pov_camera` + `encode_pov_b64` (384×216) + `keel_complete_vision` (qwen-VL + mmproj) | `shoggoth_vision.h:26`, `keel_client.cpp:108` |
| Ears (record-only) | soundscape render + `whisper_transcribe` + `clean_transcript` | `main.cpp:3266`, `shoggoth_hearing.h:25` |
| Voice (Director's) | `synthesize_speech` (deterministic formant TTS) + `speak_pa` | `tts.h:382`, `main.cpp:531` |
| Geometry oracle | `maze_open`, `ChunkLayout.{door_*, stair_at, shaft_at, floor_hole_at}`, `Stroller::plan` (bearing-scored 104 m BFS) | `shoggoth.h:63`, `layout.h:22`, `main.cpp:2314` |
| Determinism shell | `ShoggothEvent` log (`SHOGLOG1`) + `shoggoth_hash` + the record/replay sacred gate | `shoggoth_brain.h:96`, `shoggoth.h:225`, `gate.ps1:2031` |

**The honest current state of the *brain*:** the intent the model can set is just
`{action ∈ Hunt/Stalk/Lurk/Flank/Flee, aggression 0..1}` (`shoggoth.h:32`). The live path drops
the polled intent straight onto `sh.intent` every ~3 s (`main.cpp:986/1649`). Vision and hearing
are **record-time only** — there is no `ShoggothVisionHost`/`ShoggothHearingHost`, and **the
creature has no voice of its own** (the PA is the Director's). The idle wander is `rng % 7`
(`shoggoth.h:174`) — feature-blind. Flank is half-built (`di/2` rounds to 0 at close range,
`:189`).

---

## 2. The six design pillars (my synthesis)

1. **Narrow-channel embodiment.** (§0.) Semantic in, metric out. The model nudges a *goal+mood*;
   the deterministic navigator owns motion. Non-negotiable — it's also what keeps INV-1/5/6 intact.

2. **Calibrated incompetence.** (§0.) The engine *guarantees* the floor (never sealed, never stuck,
   never tunnels a wall, never crashes — all already true and tested). Inside that floor, the
   model's steering is *allowed to be dumb*, and we lean into it: **hallucination-enactment** (walk
   the bearing of a doorway the model saw even when none exists; bump a wall; recover via the
   existing swept-AABB + BFS; let the next blink correct). A confidently-wrong creature reads as a
   mind with bad eyes — exactly the register.

3. **The oracle is the workhorse; the model is garnish.** The geometry API is so complete that the
   *majority* of compelling behaviour needs **no LLM at all** — deterministic, free, every tick,
   and alive even when KEEL is down (INV-6). The model blinks every few seconds to pick a goal and a
   mood; between blinks (95% of the time) the creature runs on the oracle. **Corollary: build the
   LLM-free affect first** (the feature-aware idle), because it's the cheapest, lowest-risk, and
   most-of-the-time-visible win, and it's the fallback that must always hold.

4. **Runs-well-first arbitration.** *This is the operator's explicit ask and the real risk.* There
   is **one** llama-server backend (`:8080`, `-c 8192`), serialized, fronted by KEEL `:7071`. Today
   three Director hosts already share it with **no priority and no preemption** (`05` §B.10) — a
   28 s vision call blocks everything behind it. Adding the Shoggoth's *own* vision + hearing makes
   **five** LLM consumers on one serial backend. Without arbitration, "give the creature eyes"
   means "make the whole game stutter." So arbitration is **co-equal with the schema change** and
   lands **before** any new expensive host (§5, Phase 2). Most of the budget is bought back by
   pillar 3: the Shoggoth's routine hearing is the **deterministic cheap tier (zero LLM)**, and its
   vision is low-cadence, lowest-priority, and 9B-tier-only.

5. **Falsifiable contribution (the ablation).** GLM's sharpest idea, kept verbatim in spirit: *the
   creature is allowed to be unfalsifiable in-character, but each sense's contribution must be
   falsifiable to us.* Every live sense ships with a headless ablation harness (reusing
   record/replay): same seed, same wanderer path, **live sense vs a random-picker emitting the same
   schema**. If a metric can't distinguish them, that sense is a mirror — cut it. We measure
   **divergence from the random baseline**, never *agreement with the perception* (see §6, Q2).

6. **The watcher/watched asymmetry (an invariant, not a feature).** The Director sees through *your*
   eyes (your POV, narrated to you); the Shoggoth sees through *its own* (situated, occluded,
   fallible). They share the KEEL backend but **never share context**. The Director can serenely
   narrate "the subject is alone" while the Shoggoth is 8 m behind you in the dark. Preserve this
   isolation explicitly — it is load-bearing for horror and costs nothing.

---

## 3. The schema (the one genuinely risky increment)

Everything live flows through `ShoggothIntent`. To carry vision/hearing/voice it must grow — and
**any field that affects motion must be added to `ShoggothEvent`, serialized, replay-reconstructed,
and hashed, or record ≠ replay** (`05` §D.9 — the tightest constraint in the codebase). The binary
log has no version field beyond its magic, so this is a `SHOGLOG1` → `SHOGLOG2` bump + an ADR.

```c++
struct ShoggothIntent {            // extends shoggoth.h:32
    ShoggothAction action  = Hunt; // kept: back-comp default = M20 behaviour
    float aggression       = 0.5f; // kept
    // NEW — semantic perception (classifier space, never metric):
    TargetKind  target_kind = None; // wanderer|doorway|stairs|shaft|dark|light|none
    Sector      sector      = Ahead;// 8-way relative bearing bucket
    Proximity   proximity   = Far;  // near|mid|far
    Mood        mood        = Idle; // curious|fixated|afraid|idle  (flavours snap + idle program)
    float       snap        = 0.5f; // per-mode resolution fidelity: 1=lock the cell, 0=head the sector
    // utterance is voice-only and does NOT affect motion -> NOT hashed, NOT in ShoggothEvent:
    std::string utterance;          // <=12 words; spoken, never drives the creature
};
```

**Determinism plumbing (atomic, all in one commit):**
- Extend `ShoggothEvent` with the **motion-affecting** new fields (`target_kind, sector, proximity,
  mood, snap`) — *not* `utterance` (voice is presentation-only, never replayed into motion).
- Bump magic `SHOGLOG1` → `SHOGLOG2`; update `write/read_shoggoth_log`.
- Extend replay reconstruction (`main.cpp:3580`) to rebuild every new field.
- Extend `shoggoth_hash` (`shoggoth.h:225`) to fold the new motion fields.
- **Fold in the known AUDIT gap in the same pass:** `run_shoggoth_vision_record` does not apply the
  M29 prey-offset that replay applies, so a `--level≠0` vision record would not replay bit-exact
  (`05` §A.9, `AUDIT_2026-06-15.md:207`). Fix it here while the determinism surface is already open.
- Re-prove the sacred gate (`gate.ps1:2031`) and add a `--level 7` record/replay case so the AUDIT
  gap can't return.

This phase ships **no behaviour change** (every new field defaults to the M20 values). It is pure,
risky plumbing — do it alone, prove the gate, tag it, before anything live touches the new fields.

---

## 4. The brain: propose / dispose (the genuine intelligence, on an existing substrate)

The model emits the semantic schema. A new **pure** resolver turns it into a real goal cell, reusing
the *already-built* `Stroller::plan` machinery (bearing-scored BFS, the exact "semantic→metric"
pattern `draft_intensity_near_shaft` already uses in production for audio):

```c++
// pure, deterministic, no LLM, no I/O — lives next to maze_open in shoggoth.h
CellGoal resolve_target(seed, level, shog_cell, facing,
                        target_kind, sector, proximity, snap, rng);
```

- **Enumerate** real candidates of `target_kind` in `sector` near the creature — doorways via
  `ChunkLayout.door_*`, stairs via `stair_at`, shafts via `shaft_at`, the wanderer via
  `world_to_cell`. All pure, all exist.
- **Score** by bearing match (`cos(rel_bearing)` — the `Stroller::plan` formula), proximity match,
  and a little jitter.
- **`snap` is the fidelity knob:** Hunt snaps tight (the engine knows exactly where you are —
  fixation is *correct*, and Hunt must never regress below today's BFS-to-wanderer); Explore heads
  the sector loosely; Escape snaps to a stair cell but erratically.
- **Hallucination policy (pillar 2):** if the model says `doorway` but no doorway exists in that
  sector, *do not silently correct it* — enact it (walk the bearing N cells, bump, recover). Gate
  enactment to Explore/Escape (curiosity/desperation), **never** Hunt (the wanderer is real ground
  truth — a hallucinated player would be a bug, not a vibe).

The resolved goal cell replaces the FSM's goal selection inside `shoggoth_step`; `next_step_dir`
still does all wall-routing. **The model never touches motion — only the goal.** INV-6-clean.

This same resolver also fixes the **idle** (pillar 3): route the Lurk amble through `resolve_target`
so the creature loiters near doorways, patrols junctions, drifts toward open space — instead of
`rng % 7`. One mechanism unifies perception-driven and idle motion, and the idle works with the
model off.

---

## 5. The sequenced plan

Ordered for **risk-down, affect-up, runs-well-before-features**. Each phase is a gate-able
increment. (Milestone numbers are proposals continuing from M30; the operator sets the final
numbering.)

### Phase A — Feature-aware idle + the resolver  *(no LLM, no schema, no determinism risk)*
**Why first:** highest affect-per-LOC, zero risk, and it's the LLM-off fallback that must always
hold. Transforms "mediocre random wander" into "something with intention" before we touch a model.
- Build `resolve_target` (§4) on the `Stroller::plan` substrate (~40 LOC, pure).
- Route the Lurk amble + fix Flank (real circle-strafe, not `di/2`) through it.
- Three deterministic idle programs, mode-conditioned: **Prowl** (wall-follow + slow scans at
  junctions), **Lurk** (near-still in corners), **Restless** (heading-biased toward verticality).
- **Gate:** record/replay still bit-exact (idle is deterministic); a headless metric showing the
  creature now spends more time near features than a random walker. ~90 LOC. **Risk: low.**

### Phase B — The schema bump + AUDIT fix  *(the one risky increment; §3)*
- Extend `ShoggothIntent`/`ShoggothEvent`/`shoggoth_hash`/log/replay; `SHOGLOG1`→`SHOGLOG2`; fix the
  M29 vision-record gap; re-prove the gate incl. a `--level 7` case. **No behaviour change.**
- **Gate:** the sacred record/replay gate, green, on both level 0 and level 7. ~120 LOC. **Risk:
  high (determinism) — but contained, because nothing yet *uses* the new fields.** Tag before Phase C.

### Phase C — Minimum-viable arbitration  *(runs-well; before any new expensive host)*
**Why before vision:** adding an expensive multimodal consumer without this *is* the stutter the
operator wants gone.
- A single **request broker** in front of `keel_complete*`: one queue, a priority enum
  (`player-speech > shoggoth-vision > director-vision > director-narration > shoggoth-brain`),
  latest-wins per class, and **cancellation of stale lower-priority work** when a higher-priority
  request arrives.
- A **single multimodal slot** (semaphore): at most one expensive vision call in flight
  process-wide; text brain/cheap-tier hearing never wait on it.
- **Cadence budgeting** (§7) so the aggregate request rate fits the backend.
- **Gate:** a headless concurrency soak — all five consumers active, assert frame-time p99 < 2×
  median (the existing M21b async-isolation bar) and that a vision call never delays a queued
  player-speech turn beyond one slot. ~150 LOC. **Risk: medium.**

### Phase D — Live vision host + Explore mode + the ablation  *(the proof mode)*
- `ShoggothVisionHost` — a direct mirror of `DirectorVisionHost` (the canonical template), POV from
  `shoggoth_pov_camera`, prompting the **classifier schema** (§3), through the broker at
  shoggoth-vision priority, 9B-tier-only, graceful no-op on 4B / KEEL-down.
- Wire Explore mode: `target_kind ∈ {doorway, dark, light}`, `snap≈0.5`, hallucination-enactment on.
- **The ablation harness (pillar 5):** headless, same seed + wanderer path, live-vision vs
  schema-matched random-picker; report the divergence metric. If indistinguishable → vision is a
  mirror → cut or rethink.
- **Gate:** record/replay bit-exact (vision still record-time for the gate); ablation shows
  measurable divergence; live concurrency soak still green. ~140 LOC (host is a mirror). **Risk:
  medium.**

### Phase E — The voice organ  *(the affect peak)*
- The creature speaks: reuse `synthesize_speech`/`speak_pa` with two cages — **style cage** (prompt
  forbids object-naming; sensory gestures only: *"Something pale… give it to me"*, never "you have a
  red key") and **structural cage** (extend `plausible_utterance`: ≤12 words, n-gram-loop suppressor
  across the memory window — *silence is creepier than an echo* — register check for a sensory verb
  or imperative).
- **Trigger:** speak only when `dist < 6 m` AND `utterance ≠ ""` AND not echo-suppressed. The body
  must **not** freeze during the voice call (already async); latency is deliberately *asymmetric and
  inconsistent* — variance reads as a creature with priorities. Coexist with the Director's PA via
  the existing `speak_pa` purge + the broker's priority.
- **Gate:** TTS determinism test (the M24 `thread_local` lowpass bug must stay fixed); a headless
  check that utterances obey the cages. ~80 LOC. **Risk: low** (presentation-only; `utterance`
  never touches motion or the hash).

### Phase F — Hearing: cheap tier (default) + whisper tier (player only)
- **Cheap tier (default, zero LLM, deterministic):** derive a sound tag —
  `silence | hum | footsteps_near | footsteps_far | voice` — directly from the soundscape already
  rendered at the creature's ears (`shoggoth_listen_wav`). Feed to the brain as text. This carries
  ~90% of "the creature heard something" at zero cost/latency and *stays deterministic*.
- **Whisper tier (rare, player-initiated only):** when the player speaks, the existing whisper path
  feeds "the wanderer said: …" — the bilateral exchange. Whisper stays **lowest priority** in the
  broker (it's a 120 s-capped blocking shell-out; it must never block the creature's perception).
- **Gate:** sound-tag determinism (record/replay); whisper graceful no-op. ~110 LOC. **Risk: low.**

### Phase G — Escape mode + the verticality decision  *(the narrative peak; last)*
- Escape: `target_kind = stairs`, `snap≈0.3` + enactment; the engine knows `stair_at` even when
  vision is vague. **Recommended scope: atmosphere-only** (see §6, Q1) — resolve to a stair cell,
  walk to it, gaze up the stairwell, *yearn*; do **not** climb. Full vertical locomotion is a
  separate milestone if ever desired.
- **Gate:** record/replay bit-exact; a headless check that an Escape-mood creature converges on the
  nearest `stair_at` cell. ~70 LOC (atmosphere-only). **Risk: low** as scoped; **high** if vertical
  locomotion is bundled (don't).

**Cross-cutting, build alongside (pillar 3 tunability):** a **live decision-trace** — a small ring
buffer of `(perception → intent → resolved goal → mode)` dumpable via a flag or a debug HUD overlay.
*Not* for determinism (live play isn't replayed) but for **character tuning** — you cannot tune a
mind you cannot watch think. ~50 LOC, no risk.

---

## 6. The two open questions — answered

**Q1. Verticality: build it, or stage it?**  **→ Stage it (atmosphere-only).** Two reasons. (a)
*Engineering:* vertical locomotion is a real milestone — Y-integration, stair-mounting, cross-level
sensing, a whole new determinism surface — and bundling it into the aliveness work multiplies risk
for the one phase that's already the narrative peak. (b) *Aesthetic, and decisive:* a creature that
actually escapes is **gone** — that's relief, the opposite of dread. A creature that paces at the
stairs it cannot take is **shared damnation** — it is as trapped as you. The operator's own framing
("its mediocrity is the dread") points straight here: the glimpsed-but-impossible exit is more
horrifying than the exit taken. Ship the yearning; leave the climb as an optional future milestone.

**Q2. The ablation metric: prove vision *moves* the creature, or moves it *toward what it saw*?**
**→ Measure divergence from the random baseline, never agreement with the perception.** A creature
that *avoids* a doorway it saw (aversion is curiosity's twin) is vision doing real work, yet it
would *fail* a "did it head toward the feature" test. Agreement bakes in an assumption about what
"seeing" means and is exactly the legibility that kills dread. Divergence-from-random is honest,
shippable, and in-character: it proves vision *changes* behaviour without demanding the change be
"correct."

---

## 7. The "runs well" budget (concrete)

The whole system must fit one serialized llama-server. Target steady-state load:

| Consumer | Tier | Cadence | Cost | Broker priority |
|---|---|---|---|---|
| Player speech (chat) | any | on-demand (rare) | whisper 120 s cap + 1 LLM turn | **highest** |
| Shoggoth vision | 9B only | ~20–25 s, **offset** from Director's 28 s | expensive (mmproj) | high |
| Director vision | 9B only | 28 s | expensive (mmproj) | medium |
| Director narration | any | 18 s | cheap (text) | low |
| Shoggoth brain | any | 3 s | cheap (text) | lowest-but-frequent |
| Shoggoth hearing (cheap tier) | any | every blink | **zero LLM** (deterministic tag) | — |

Rules: **one multimodal slot** (the two vision consumers never overlap; stagger their phases so they
don't even queue together); text calls (brain/narration) never wait behind the slot; **the cheap
hearing tier carries routine perception** so the expensive channel is reserved for the rare,
high-affect moments. **Graceful degradation by tier:** on the 4B tier (no vision), the Shoggoth runs
brain + cheap-hearing + voice + the LLM-free feature-aware idle — *still fully alive*, just blind. On
KEEL-down, it runs Phase A's deterministic idle — *still moving with intention*. Nothing in this
plan can make the game stutter or fail to launch (INV-6).

---

## 8. Risks, non-goals, and the headline

**Risks:** (1) the schema bump (Phase B) is the determinism cliff — contained by shipping it as
behaviour-free plumbing with the gate re-proven before use. (2) Arbitration (Phase C) is new
concurrency — contained by the existing async-isolation soak bar. (3) Five LLM consumers could still
saturate a slow machine — contained by tier-gating, cadence budgets, and the cheap hearing tier.

**Non-goals (explicit):** no tactical-genius hunting (pillar 1+2 forbid it); no multi-floor chase in
v1 (Q1); no streaming TTS in v1 (render-then-play is enough; streaming is a separate session); no
LOS/raycast (the engine *is* the oracle — it knows the truth, it doesn't need to simulate seeing);
no new model, no new dependency.

**The headline.** Everything load-bearing already exists. The missing pieces are small and mostly
*mirrors* of code that's already battle-tested: a resolver on an existing substrate, two async hosts
mirroring `DirectorVisionHost`, a voice path that's two validators around the existing TTS, a request
broker, and one risky schema bump. The determinism shell is never touched — the model still only
nudges `sh.intent` as a discrete tick-boundary event, and the headless record/replay gate stays the
proof. **Build the LLM-free aliveness first, make it run well before you make it see, prove every
sense isn't a mirror, and keep the creature dumb on purpose.** Ship Explore first (the proof), the
voice second (the affect), Escape last (the peak) — and let it yearn at the stairs rather than climb
them.

---

## Appendix — key anchors (verified @ `0d73c47`)

`ShoggothIntent` `shoggoth.h:32` · `shoggoth_step` `:134` · idle `rng%7` `:174` · Flank `di/2`
`:189` · `shoggoth_hash` `:225` · `ShoggothSummary` `shoggoth_brain.h:24` · `parse_shoggoth_intent`
`:64` · `ShoggothEvent`/`SHOGLOG1` `:96` · `ShoggothBrainHost` `shoggoth_brain_host.h:37` ·
`shoggoth_pov_camera` `shoggoth_vision.h:26` · `DirectorVisionHost` (the template) `director_vision.h:89`
· `keel_complete_vision` `keel_client.cpp:108` · `Stroller::plan` (resolver substrate) `main.cpp:2314`
· `draft_intensity_near_shaft` (semantic→metric precedent) `main.cpp:844` · live intent assignment
`main.cpp:986/1649/5209` · `ChunkLayout`/`stair_at`/`shaft_at` `gen/include/gen/layout.h:22` · sacred
gate `gate.ps1:2031` · AUDIT determinism gap `AUDIT_2026-06-15.md:207` · INV-1..8 `ARCHITECTURE.md:46`
· GLM brainstorm `_brainstorm/GLM/03`, raw fan-out `05`.
