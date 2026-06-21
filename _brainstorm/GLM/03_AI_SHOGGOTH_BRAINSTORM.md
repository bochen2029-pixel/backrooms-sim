# 03 — The Fully-Embodied Emergent AI Shoggoth (Brainstorm)

> Thread: third pivot. Brainstorm only — no code proposed, no edits made.
> Repo state: HEAD = 90410c3 (ADR-076).
>
> Grounded by a 4-agent fan-out that mapped the entire codebase (raw findings in 05). Every
> load-bearing claim below is cited to file:line.

## The governing principle: narrow-channel embodiment

The project's invariants (INV-1/5/6 + the `ShoggothEvent`/`shoggoth_hash` machinery)
*structurally force* the LLM to be a discrete-event nudge. There is no place to put a
load-bearing judgment. The design law that falls out:

> **Any feature that requires the model to be *right* is simultaneously an engineering risk
> (non-determinism) and an aesthetic failure (legibility kills dread). Deploy the model only
> through channels narrow enough that its failures stop being checkable; the engine supplies
> all checkable truth.**

The creature has six organs. Each is a *channel* the model speaks through, disciplined to a
narrow band where the 9B's errors read as a mind, not a malfunction:

| Organ | Model's job (narrow) | Engine's job (perfect truth) | Narrow band |
|---|---|---|---|
| Eyes | classify what's in frame | knows exact cells of every doorway/stair/wanderer | `{target_kind, sector, proximity}` — no geometry |
| Ears | classify a sound tag | knows wanderer distance/bearing | one word: footsteps/voice/silence/hum |
| Voice | emit a sensory gesture | TTS shapes it into PA register | ≤12 words, no object-naming |
| Brain | pick a goal + mood | resolves goal to a real cell + routes | semantic intent only |
| Body | — | writhe + idle motor program | deterministic, always alive |
| Locomotion | — | BFS routes around walls | snap-fidelity per mode |

Emergence is what you get when a fuzzy perceiver is coupled to a precise world through narrow
channels that can't be caught wrong.

## What's fully built and battle-tested (do not re-invent)

| Organ | What exists | Where |
|---|---|---|
| Eyes (image→brain) | `shoggoth_pov_camera` + `encode_pov_b64` (384×216 box-average→PNG→base64) + `keel_complete_vision` (multimodal, 30s). Proven by M22 (record) + Director's live `DirectorVisionHost`. | `shoggoth_vision.h:26`, `main.cpp:1302`, `keel_client.cpp:108` |
| Ears (audio→brain) | `MicCapture` (16kHz mono waveIn + energy VAD + echo gate) + `whisper_transcribe` (shell-out, 120s cap) + `clean_transcript` + `plausible_utterance`. | `mic_capture.h:30`, `main.cpp:3295`, `shoggoth_hearing.h:25` |
| Voice (brain→audio) | `synthesize_speech` (deterministic formant synth, 105Hz f0, declining pitch, jitter) + `speak_pa` (whole-WAV→`PlaySoundW` async, cuts prior line). | `tts.h:382`, `main.cpp:531` |
| Brain (text→intent) | `ShoggothBrainHost` — the *only* live host. Latest-wins submit, non-blocking poll, KEEL graceful no-op, off-thread. | `shoggoth_brain_host.h:37` |
| Body | `build_shoggoth_mesh` — 13 tentacles × 96 verts = 1248 verts, stable topology, writhe-animated, material-7 salmon in DXR. | `shoggoth_body.h:49` |
| Locomotion | `shoggoth_step` FSM (Lurk/Hunt/Chase/Retreat) + `next_step_dir` BFS (R=22 cells). Deterministic, per-floor-locked. | `shoggoth.h:134` |
| Geometry oracle | `maze_open` + `ChunkLayout.{door_*, stair_at, shaft_at, floor_hole_at}` + `Stroller::plan` (bearing-scored 104m BFS) + `Stroller::steer` (24-dir clearance). | `shoggoth.h:63`, `layout.h:22`, `main.cpp:2314` |
| Determinism shell | `ShoggothEvent` log + `shoggoth_hash` + record/replay sacred gate. The LLM can only nudge `sh.intent` as a discrete tick-boundary event. | `shoggoth_brain.h:96`, `shoggoth.h:225` |

## The critical gaps the agents exposed

1. **No live vision host for the Shoggoth.** Only the *text* brain is live. `shoggoth_vision_host.h` does not exist. The Director has `DirectorVisionHost`; the Shoggoth doesn't. ~120 LOC of mirroring.
2. **No live hearing host for the Shoggoth.** `shoggoth_hearing.h` is pure (cleaner + prompt only). Record path exists; no live host.
3. **The Shoggoth never speaks.** `synthesize_speech`/`speak_pa` are the *Director's* voice. The creature has no `utterance` field and no voice path.
4. **`ShoggothIntent` is `{action, aggression}` only.** No target kind, sector, mood, utterance. **Tightest determinism constraint:** any field affecting motion must be added to `ShoggothEvent`, serialized, replay-reconstructed, and hashed — and `SHOGLOG1` has no version field, so a schema change needs a magic bump + ADR.
5. **The summary is sensor-poor.** `ShoggothSummary` = `{sgi,sgj, wgi,wgj, distance_m, state, tick}`. No yaw, writhe, what's-ahead, memory.
6. **No line-of-sight query.** The engine knows wall adjacency + doorway/stair cells but no "is A visible from B past walls" raycast. Distance + bearing only. (This is also what makes the engine the geometry oracle — it doesn't *need* LOS; it knows the truth.)
7. **No vertical locomotion.** The creature is hard-locked to one floor (`pos.y` never written). `stair_at` resolves a stair cell but the creature can't climb it. Escape's "climb out" is resolvable, not executable.
8. **No cross-host scheduling.** Three hosts hit one KEEL serially, no priority/preemption. A slow vision call starves chat/brain.
9. **`Stroller` is not the Shoggoth's navigator.** The sophisticated path-follower exists but the creature uses simpler `next_step_dir`. The "resolver substrate" exists as a class but isn't wired to the creature.
10. **Flank is half-built** (integer `/2` on 1-cell deltas degenerates to Hunt). Idle amble is random (`rng % 7`), not feature-aware.

## The six organs, concretely

### 1. Eyes — live vision host (the missing ~120 LOC)

`ShoggothVisionHost` as a direct mirror of `DirectorVisionHost` (`director_vision.h:89`). Same
latest-wins submit/poll, same off-thread `keel_complete_vision`, same graceful no-op. POV from
`shoggoth_pov_camera` (exists, `shoggoth_vision.h:26`). Prompt asks for the **semantic
classifier schema**, not a narration line:
```
{ target_kind: wanderer|doorway|stairs|shaft|dark|light|none,
  sector: ahead|ahead_left|left|behind_left|behind|behind_right|right|ahead_right,
  proximity: near|mid|far,
  confidence: 0..1 }
```
This is "classifier not localizer" — the 9B is reliable at "doorway, ahead-left, near" and
hopeless at "30°, 8m." `parse_shoggoth_intent` extends to validate this (same fence-stripping,
same safe default).

**Hallucination policy:** when `target_kind=doorway` but the engine finds *no* doorway candidate
in that sector, *don't silently correct* (that makes a legible robot). Instead **enact the
hallucination with mode-gated probability**: walk the sector bearing for N cells with no target,
bump the wall, recover via the existing swept-AABB + BFS, let the next blink correct. Gate to
Explore/Escape (curiosity/desperation), not Hunt (wanderer is real ground-truth).

### 2. Ears — live hearing host + a sound-tag shortcut

`ShoggothHearingHost`, same pattern. Two tiers:
- **Cheap tier (no whisper):** the engine already renders audio at the creature's ears
  (`shoggoth_listen_wav`, `main.cpp:3266`). Derive a sound tag deterministically —
  `silence | hum | footsteps_near | footsteps_far | voice` — feed to brain as text. Zero LLM
  cost, zero latency, deterministic. 90% of "the creature heard something."
- **Full tier (whisper):** when the player speaks (VAD fires), shell out to whisper, clean it,
  feed the transcript as "the wanderer said: …". The bilateral-conversation path.

### 3. Voice — the creature speaks (the real new craft)

Reuse `synthesize_speech` + `speak_pa` with two craft layers:
- **Style cage (prompt):** utterance prompt forbids object-adjudication, locks to sensory
  gestures. "Something pale… *give it to me*." Never "you're holding a red key."
- **Structural cage (validator):** extend `plausible_utterance` — ≤12 words, n-gram-loop
  detector across the memory window (suppress if last 2 utterances share >60% n-grams — silence
  is creepier than an echo), register check (must contain a sensory verb or imperative).

**Latency craft:** "loom before it answers" — body on frame thread acting on old intent, voice
arrives whenever. Body must not freeze during voice call (already async). Latency must be
*asymmetric and inconsistent* (variance is the character). Streaming TTS (phoneme→formant as
tokens generate) is highest-affect-per-LOC but a separate session; v1 ships render-then-play.

**Trigger:** speak only when `dist < 6m` AND `utterance != ""` AND not echo-suppressed. Modal
exchange — the creature *takes the floor* when close.

### 4. Brain — the propose/dispose resolver (the genuine intelligence)

New `resolve_target` on the existing `Stroller::plan` substrate:
```
resolve_target(seed, level, shog_pos, facing, target_kind, sector, proximity, snap)
  -> (goal_cell, snap_mode)
```
- Enumerate real candidates of `target_kind` in `sector` near the creature: doorways via
  `ChunkLayout.door_*`, stairs via `stair_at`, shafts via `shaft_at`, wanderer via
  `world_to_cell`. All pure, all exist.
- Score by bearing match (`cos(rel_bearing)`, the exact `Stroller::plan` formula) + proximity
  match + jitter.
- **`snap` is the per-mode resolution-fidelity knob:** Hunt snaps tight (fixation correct),
  Explore wanders loose (head the sector, don't lock a cell), Escape snaps to stairs but
  erratically, `void` = go still somewhere weird.

The brain emits the *resolved goal cell* into `shoggoth_step`, *replacing* the FSM goal
selection. The deterministic navigator (`next_step_dir` BFS) still does wall-routing — the LLM
never touches motion directly, only the goal. INV-6-clean.

### 5. Body — the idle motor program (alive without the API)

The idle *exists* (`Lurk` ambles random ±3 cell, `writhe` pulses) but is **feature-unaware**.
Fix: route the Lurk amble target through the *same* `resolve_target` resolver, so the creature
*loiters near doorways, patrols junctions, drifts toward open space* instead of `rng % 7`.
Unifies perception and idle under one mechanism.

Three idle modes (deterministic, mode-conditioned):
- **Prowl** (Hunt/Explore): wall-follow the maze-runner's right-hand rule, slow scanning turns
  at junctions.
- **Lurk** (Lurk state): near-still in corners, occasional slow turn. Dormant but aware.
- **Restless** (Escape): heading-biased toward verticality, anxious pacing.

The idle *generates the events perception responds to* — prowling around a corner into LOS fires
an opportunistic blink. The cadence isn't a timer; it's "something changed because I moved."

### 6. Locomotion — three modes, one resolver

- **Hunt:** `target_kind=wanderer`, `snap=1.0` (engine knows exactly where you are; fixation
  correct). Falls back to existing BFS-to-wanderer — never lose current behavior.
- **Explore:** `target_kind ∈ {doorway, dark, light}`, `snap≈0.5`, hallucination-enactment on.
  **The proof mode** — the only one where vision is irreplacable.
- **Escape:** `target_kind=stairs`, `snap≈0.3` with enactment, engine knows `stair_at` even
  when vision is vague. The narrative-peak mode — *it might actually climb out*. (Execution
  needs the vertical-locomotion gap; see open question 1.)

## Emergent behaviors that fall out for free

1. **An interior you can't model.** Imperfect `snap` + hallucination enactment → sometimes
   inexplicable motion, stops at blank walls, doubles back. Unpredictable. That's the soul.
2. **Volition across blinks.** Memory = rolling ring of last 3 `(target_kind, sector,
   proximity, mood)` tuples, textualized to ~120 tokens (you *can't* stuff 3 prior images into
   a 9B VLM context — memory is forced impressionistic, which is the character). Commits to a
   plan across blinks instead of re-deciding.
3. **The watcher/watched asymmetry.** Director sees through *your* eyes; Shoggoth through *its
   own* (situated, fallible, occluded). They share KEEL but not context. Director can narrate
   "the wanderer is alone" while the Shoggoth is 8m behind you. **Preserve this isolation as an
   explicit invariant** — load-bearing for horror, costs nothing.
4. **Bilateral voice.** Creature hears your mic (whisper tier), replies (voice organ). Body
   doesn't freeze for voice; latency inconsistent → reads as a creature with priorities.
5. **Arrhythmic perception.** Event-driven blinks (LOS gain, heard voice, waypoint reached) +
   endogenous clocks (curiosity/boredom that blink *independent of you*) + 15-20s floor
   heartbeat. "Thinks in bursts," not a metronome. A purely-reactive creature is a mirror with
   teeth; endogenous clocks break the mirror.
6. **Mode-transition as drama.** Pottering in Explore, noticing you, the shift to directed —
   the beat where it *commits* is the dramatic unit.

## The determinism-safe sequencing

The one hard constraint: **any new `ShoggothIntent` field affecting motion must be added to
`ShoggothEvent`, serialized, replay-reconstructed, and hashed — and `SHOGLOG1` needs a version
bump.** Build order:

1. **Schema change as an atomic increment (first).** Extend `ShoggothIntent` → `{action,
   target_kind, sector, proximity, mood, utterance, snap}`. Bump `SHOGLOG1`→`SHOGLOG2`. Extend
   `ShoggothEvent`, `write/read_shoggoth_log`, replay reconstruction (`main.cpp:3576`),
   `shoggoth_hash`. Re-prove the sacred gate. *Load-bearing-risk work; do before anything live.*
2. **The resolver + ablation harness.** `resolve_target` on `Stroller::plan` substrate (~40
   LOC) + ablation (~50 LOC, reusing record/replay): same seed, same wanderer path, (a) live
   vision vs (b) random-picker emitting the same schema. If a metric can't distinguish them,
   vision is a mirror — pull it. **The creature is allowed to be unfalsifiable in-character;
   the vision contribution must be falsifiable to you.**
3. **`ShoggothVisionHost` + Explore mode only.** ~120 LOC mirror. Verify via ablation. Explore
   is the proof mode.
4. **The dial + feature-aware idle + `void` target.** Character tuning (a week, not a day).
   Route Lurk amble through the resolver.
5. **The voice organ** — `utterance` field (already in step 1) + style cage + structural cage +
   trigger. Separate session from the VLA loop.
6. **`ShoggothHearingHost`** — cheap-tier sound tag first (deterministic, zero cost), full-tier
   whisper for player speech.
7. **Escape mode** — the narrative peak, polished last. **Requires the vertical-locomotion gap.**
8. **Cross-host scheduling** — priority enum around `keel_complete`: player-speech >
   shoggoth-vision > director-narration, latest-wins cancellation of stale lower-priority work.

## Two genuine open questions

**1. Verticality — build it or stage it?** The engine *knows* every stair (`stair_at`) and the
vertical graph is connected (`validate_vertical_connectivity`), but the creature's locomotion
has no Y integration (`pos.y` never written, `shoggoth.h:220-221`). Escape's full payoff — "it
climbs out and you lose it, or descends toward you" — requires building vertical locomotion (a
real milestone: stair-mounting, Y-integration, cross-level sensing). The cheaper alternative is
*atmosphere only*: resolve to a stair cell, walk to it, gaze up the stairwell, never climb.
Thematically pure (the glimpsed impossible exit), far less code. **Which Escape — the one that
can leave, or the one that yearns toward leaving?**

**2. The ablation metric.** "Vision-influenced motion" vs "vision-correctly-imitated motion"
aren't the same. A creature that *avoids* the doorway it saw (curiosity's twin is aversion)
would fail a "did it head toward the perceived feature" metric while vision is doing real work.
The metric must measure *divergence from the random-picker*, not *agreement with the
perception*. **What are you actually proving — that vision moves the creature, or that it moves
it toward what it saw?** The first is honest and shippable; the second bakes in an assumption
about what "seeing" means.

## The headline

*The creature is emergent because no organ is load-bearing for correctness — the engine's truth
is load-bearing, the model's contribution is flavor that steers the truth through narrow
channels that can't be caught wrong.* Six organs, each disciplined to a band where the 9B's
errors read as a mind. The determinism shell is untouched — the LLM still only nudges
`sh.intent` as a discrete tick-boundary event; the headless record/replay gate stays the proof.
Everything load-bearing exists. The missing pieces are a vision host (mirror), a hearing host
(mirror), a voice path (cages), a resolver (on an existing substrate), and the schema bump (the
one risky increment). Ship Explore first (it's the proof), the voice second (it's the affect),
Escape last (it's the peak, and it forces the verticality decision).

## Key file:line references

- `ShoggothIntent` (today): `shoggoth.h:32-35`
- `ShoggothSummary` (sensor-poor): `shoggoth_brain.h:24-30`
- `shoggoth_step` (locomotion FSM): `shoggoth.h:134-222`
- `next_step_dir` BFS: `shoggoth.h:84-113`
- `maze_open`: `shoggoth.h:63-79`
- `shoggoth_hash` (what gets hashed): `shoggoth.h:225-236`
- `ShoggothEvent` + log format: `shoggoth_brain.h:96-130`
- `ShoggothBrainHost` (the only live host, the template): `shoggoth_brain_host.h:37-116`
- `shoggoth_pov_camera` + vision prompt: `shoggoth_vision.h:26-60`
- `DirectorVisionHost` (mirror this): `director_vision.h:89-168`
- `keel_complete_vision`: `director/src/keel_client.cpp:108-120`
- `MicCapture` (ears): `mic_capture.h:30-142`
- `synthesize_speech` + `speak_pa`: `tts.h:382`, `main.cpp:531-552`
- `build_shoggoth_mesh` (body): `shoggoth_body.h:49-95`
- `Stroller::plan` (the resolver substrate): `main.cpp:2314-2363`
- `ChunkLayout` doorways/stairs/shafts: `gen/include/gen/layout.h:22-135`
- `draft_intensity_near_shaft` (semantic→metric precedent): `main.cpp:844-861`
- Record/replay sacred gate: `scripts/gate.ps1:2031-2043`
- INV-1..INV-8: `docs/ARCHITECTURE.md:46-53`
- ADR-048 (the live-presentation seam): `docs/DECISIONS.md:252-256`
