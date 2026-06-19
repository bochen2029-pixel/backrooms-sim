# Spec — "It sees what isn't there": the apparition sense (Well B), execution-ready

> Status: **Phase 1 + Phase 2a SHIPPED** (ADR-082 / ADR-083; tags `phaseH-apparition`, `phaseH2a-player`). Phase 2b
> (the *visual* lighting dim) remains — see Phase 2 below. (Originally: "spec it end-to-end, prep for execution.")
> The flagship "impossible-before-VLMs" mechanic (see `docs/DYNAMIC_DIRECTOR.md` / the brainstorm): the creature's
> dread is driven by **emergent apparitions in the rendered frame that neither the player nor the engine placed** —
> faces/figures/words/arrows that arise only as pareidolia in the 2D projected image. There is no `f(scene-graph) →
> "a face is here"`; detecting it requires rendering + a human-like perceiver (the VLM). Builds on `SHOGGOTH_PLAN.md`
> (the creature's sensory arc) as **Phase H**. Raster-compatible (RT is deferred — see `RT_PERF_PLAN.md`).

## 0. Why it passes the impossibility test (the three criteria)
Open-vocabulary (any emergent shape, not an enumerable set) × semantic (it's a *meaning* a human reads into noise)
× **absent from the engine's own state** (the engine knows the texture params + light angle, never the gestalt). The
perceiver's own **fallibility is the feature**: a VLM sometimes sees a face that isn't there, so *neither the player
nor the AI has ground truth* — "is it really there?" becomes a genuine, shared, unanswerable question. No pre-VLM
game could host honest shared uncertainty about whether something exists.

## 1. The mechanic (player-facing)
On a sparse cadence, a VLM reads a rendered frame and returns a **coarse** verdict: does any patch of stain / shadow
/ grime happen to read as a **face, figure, word, or arrow** (and roughly where, how strongly). When one is present:
- the **creature reacts** — the LLM (which is already emitting the creature's intent from the same frame) may
  freeze ("transfixed"), be drawn toward it, or grow agitated, and **murmur** about it (the existing voice);
- the **atmosphere tightens** — lights dim a touch, the soundscape thins (presentation-only, like the flashlight).

Coarse verdict → **soft, ambient** outcomes (mood/behaviour/atmosphere), **never** a hard fail-state gate. Slow dread,
not a twitch — enforced for free by the ~1–2 s VLM latency + the sparse off-thread vision cadence.

## 2. Architecture it builds on (all already wired)
- **The creature already sees.** `app/src/shoggoth_vision.h::render_shoggoth_vision_prompt` + `shoggoth_pov_camera`
  → `ShoggothVisionHost` (`app/src/shoggoth_vision_host.h`, off-thread Qwen-VL, latest-wins submit / non-blocking
  poll / graceful no-op) → `parse_shoggoth_intent` (`shoggoth_brain.h`). The apparition read is the **same POV→VLM
  call** with an extended prompt + parse — not a new pipeline.
- **The event-log carries it for free.** `ShoggothEvent` (`shoggoth_brain.h:142`) is padding-free 40 B with a spare
  **`int32_t _reserved`** (line 155, explicitly reserved for "one more field … without another version bump"); the
  intent↔event mapping is the single pair `event_from_intent`/`apply_event_to_intent` (lines 164/176). The verdict
  packs into `_reserved` → record==replay holds, **`SHOGLOG2` magic unchanged**, old logs read back as "no
  apparition" (backward-compatible).
- **The determinism pattern is M22's.** The VLM runs at RECORD time; the validated verdict enters the deterministic
  creature ONLY as a logged event at its tick; `--shoggoth-replay` (model offline) reproduces it bit-for-bit.

## 3. Phased build
**Phase 1 — gate-able foundation (the creature's own POV).** Extend the creature's existing vision call so the VLM
also reports apparitions in *its* view; the verdict flows through the intent → the event-log → the creature's
reaction. This reuses `run_shoggoth_vision_record` / `run_shoggoth_replay`, so **the existing sacred gate (record ==
replay) PROVES the apparition is deterministic on replay.** This is the foundation + the determinism proof. ~1 day.

**Phase 2 — the full uncanny (the player's POV).** Render the *player's* view (`wanderer_camera`) on the 2nd
headless device the live vision host already owns, run the apparition read on **what the player sees**, and have the
creature + atmosphere react to *that* ("it reacted to the face I was looking at"). Live-only (like the live brain),
using the same event-log shape. Built after Phase 1 greens.
> **Phase 2a — DONE** (ADR-083 + ADR-083a, ledger E20/E21, tags `phaseH2a-player` + `pre-strength`). `run_game` renders
> `br::core::wanderer_camera` every 3rd vision cycle (`svInFlightPlayer`, on the same 2nd device); a player cycle's poll
> takes ONLY the apparition verdict (never motion) → the **PA murmurs** about it + the **soundscape thins** (a decaying
> dip that **scales with `app_strength`** — 0.33×→0.59× deep, 7→11 s; the murmur fires only for a clear/vivid one ≥2).
> Live smoke fired it end-to-end: `apparition_hits:1 (kind=figure)` on the player's own view, `debug_error_count:0`,
> gate M29 still bit-identical (now with the live VLM emitting `app_strength`). **Phase 2b (remaining):** the *visual*
> lighting dim — wire the parsed-but-unconsumed `FlickerSector` directive (or a new brightness uniform) into the raster
> lighting path (renderer contract + shader), so it's a separate, more invasive increment.

## 4. Schema changes (minimal, version-stable)
> **As built (reconciled after a QC, ADR-082/083/083a).** The proposal below sketched a nested `Apparition` struct +
> object JSON; the shipped form is **flattened** (easier for the VLM to emit reliably): `ShoggothIntent` carries flat
> fields `bool apparition; uint8_t app_kind; uint8_t app_sector; uint8_t app_strength;`, and `parse_shoggoth_intent`
> reads a **string** `"apparition":"none|face|figure|word|arrow"` + optional `"app_where"` + `"app_strength":<1-3>`
> (present-but-unspecified → 2 "clear"; clamp 1..3). All four pack into `ShoggothEvent::_reserved` (bit0 present /
> bits8–15 kind / bits16–23 sector / bits24–31 strength) → `sizeof` stays 40, no SHOGLOG bump. `strength` was the one
> field originally dropped; the QC caught it and it was added in **ADR-083a / E21**.
`app/src/shoggoth_brain.h` (the original proposal):
- `struct ShoggothIntent` gains a presentation+motion field group: `Apparition apparition{}` where
  `struct Apparition { bool present; uint8_t kind; uint8_t strength; uint8_t sector; }` (kind ∈ none/face/figure/
  word/arrow; strength 0–3; sector reuses the existing `Sector` codes). Packs into one `int32_t`.
- `parse_shoggoth_intent` reads an optional `"apparition":{"present":bool,"kind":"face|figure|word|arrow|none",
  "strength":0-3,"where":"<sector>"}` object (all optional → default absent → backward-compatible; bad/missing →
  present=false).
- `event_from_intent` / `apply_event_to_intent`: pack/unpack the apparition into **`ShoggothEvent::_reserved`** (one
  line each). `sizeof(ShoggothEvent)` stays 40 — the `static_assert` is the non-LLM oracle that it stayed padding-free.
- Determinism hashing (RESOLVED by code scan): `shoggoth_hash` (`shoggoth.h:342`) folds **named fields**, NOT the raw
  struct — so it would not pick up the apparition by itself. BUT the record paths ALSO fold each event via
  `fold_bytes(&event, sizeof(ShoggothEvent))`, which **does** include `_reserved` → **the logged apparition is already
  in the combined record/replay hash via the event fold** (no extra code for the gate). Separately, if the apparition
  changes the creature's *runtime* intent/behaviour, add one `mix(packed_apparition)` line to `shoggoth_hash` so the
  per-tick state hash reflects it too.

## 5. The VLM call boundary (the only new prompt/parse)
Extend `render_shoggoth_vision_prompt` (`shoggoth_vision.h`) with a second instruction block, appended to the intent
request:
> "ALSO: ignoring the obvious walls, doors, lights and the floor, does any patch of stain, watermark, mould, grime
> or shadow in THIS image happen to read as a FACE, a human FIGURE, a readable WORD, or an ARROW — the way an eye
> finds shapes in noise? Be CONSERVATIVE: only if it genuinely reads that way; most frames have none. Add to your
> JSON: \"apparition\":{\"present\":true|false,\"kind\":\"face|figure|word|arrow|none\",\"strength\":0-3,\"where\":\"ahead|left|...\"}."

The reply stays one JSON object (intent + apparition); `parse_shoggoth_intent` already tolerates fenced/prose-wrapped
JSON. The creature's *reaction* to a present apparition is the LLM's existing intent choice (it sees the apparition
and picks `lurk`/`flee`/`hunt` + an `utterance`) — emergent, not hard-coded.

## 6. The behaviour + atmosphere response
- **Creature:** when `apparition.present`, the intent the VLM returns already encodes the reaction (e.g.
  `action:lurk, mood:afraid, utterance:"...something in the wall..."`). Optionally add a `ShoggothAction::Freeze`
  (int32-backed; `shoggoth_step` holds position when `action==Freeze`) for an explicit "transfixed" beat — a 4-line
  addition to the `switch (sh.intent.action)` in `shoggoth.h:283`. The murmur uses the existing Phase-E `speak_pa`.
- **Atmosphere (presentation-only, `run_game`, like the flashlight/flares):** while a recent verdict is `present`,
  nudge the lighting down (a `flicker`/dim already exists in the directive vocabulary) and thin the soundscape for a
  few seconds. Gated, soft, decaying. Never touches the sim/replay.

## 7. Cadence, latency, failure-modes-as-texture
- **Cadence:** ride the existing ~20–28 s vision cycle (the `ShoggothVisionHost` warm-window + latest-wins). One
  multimodal call at a time (the `keel_scheduler.h` arbitration is the eventual home). Off the frame thread (INV-6).
- **Latency:** ~1–2 s/call → the dread is slow by construction; perfect.
- **False positives = texture, by design:** a hallucinated face → the creature reacts to nothing → the player can't
  tell if it was real → the intended shared uncertainty. **No hard outcome (no damage, no fail) is ever gated on the
  verdict** — only mood, a murmur, soft light/sound. False negatives are invisible (ambient).

## 8. Determinism (the sacred gate, intact)
- Record (`--shoggoth-vision-record`): the POV renders deterministically; the VLM is called at record time; the
  validated intent **including the apparition** → `event_from_intent` → packed into `_reserved` → folded into the
  combined hash → logged.
- Replay (`--shoggoth-replay`, model OFF): `apply_event_to_intent` reconstructs the apparition from `_reserved`; the
  creature reacts identically → **bit-identical combined hash.** The `utterance` stays presentation-only (never
  serialised). No `SHOGLOG` version bump (size unchanged); pre-apparition logs replay as `present=false`.

## 9. Verification plan (let oracles say done)
1. `audit.ps1` green (build /WX, ctest, **record==replay**, inventory, isolation) after each step.
2. **The sacred gate with the new sense:** `--shoggoth-vision-record` (KEEL up) → ≥1 apparition verdict logged →
   `--shoggoth-replay` (model off) → **record == replay bit-identical**; extend `Invoke-GateM29`/the vision case to
   assert an apparition round-trips through `_reserved`.
2b. A unit test (`[apparition]`): `parse_shoggoth_intent` reads the apparition block (valid/missing/bad → absent);
    `event_from_intent`→`apply_event_to_intent` round-trips it; `sizeof(ShoggothEvent)==40` (the static_assert).
3. A **forced-verdict** headless test (no KEEL): inject `apparition.present=true` → confirm the creature reacts +
   the hash changes vs. absent (proves the wiring drives behaviour deterministically).
4. Live `--game` smoke (KEEL up): no crash, debug-clean, the creature murmurs/reacts when a verdict fires.

## 10. Manifest / rollback / budget
- **Files:** `app/src/shoggoth_brain.h` (Apparition + parse + event pack/unpack), `app/src/shoggoth_vision.h` (prompt),
  `app/src/shoggoth.h` (optional `Freeze` in `shoggoth_step`), `app/src/main.cpp` (atmosphere gate in `run_game`;
  Phase 2: the player-POV render), `tests/unit/test_shoggoth.cpp` (the `[apparition]` case), `scripts/gate.ps1`
  (round-trip assertion), `docs/DECISIONS.md` (ADR), `docs/CHANGE_AUDIT_LOG.md`.
- **Rollback:** tag `pre-apparition` (push) before starting → `git reset --hard pre-apparition`, zero debug. The
  apparition defaults absent (present=false) everywhere → byte-unchanged when the VLM reports nothing → M21/M29 gates
  intact by construction (same guard discipline as `resolve_target`/the flashlight).
- **Diff budget:** Phase 1 ~150–250 LOC (milestone-authorised). Additive; `_reserved` reuse means no schema churn.

## 11. Open decisions (resolve before/while building)
- **Whose frame for the live mechanic — creature POV (Phase 1, gate-able) vs. player POV (Phase 2, the real
  uncanny).** Build Phase 1 first (it's the determinism proof), then Phase 2.
- ~~Does `shoggoth_hash` fold the raw struct or named fields?~~ **RESOLVED** (§4): named fields, but the event
  `fold_bytes` already covers `_reserved` for the gate; add one `mix()` to `shoggoth_hash` only if the apparition
  alters runtime behaviour.
- **Explicit `Freeze` action vs. let the LLM express the reaction through the existing actions.** Start with the
  latter (zero behaviour code, fully emergent); add `Freeze` only if a distinct transfixed beat is wanted.
- **Verdict cadence vs. the player-speech / director-vision slot** — one multimodal call at a time; this is the
  concrete motivation to land Phase C.2 (`KeelBroker` arbitration) around the same time.
