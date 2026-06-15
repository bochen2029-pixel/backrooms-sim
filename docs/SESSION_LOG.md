# SESSION_LOG.md

Newest entry first. Every session appends: done / pending / open questions / gotchas.

---

## Session 32 — M30 polish ×3: live descent + deep-descent soak + draft telegraph  ✅ — `gate M30` green; model-free Phase IV EXHAUSTED

**Three gated/pushed slices this session** (`04767fb` live descent · `30c5f77` deep-descent soak · `337ae21`
draft telegraph), each green + committed + pushed. With them, **all model-free Phase-IV work is done** — the
despair gradient is fully playable and the §3 DONE criteria hold **except M29** (`m29-green` is model-blocked
on `llama-server :8080`, ISSUE-5; both servers were down again all session). Detail per slice below.

`gate.ps1 -Milestone M30` exits 0 (with a NEW live-descent proof). **You can now fall through the
world's real openings while playing** — not just look down them. This closes ROADMAP §2 M30 item (1)
"live descent," the last big model-free Phase-IV item. (M29 stays model-blocked on the KEEL sidecar :8080,
ISSUE-5 — both servers were down again this session; routed around per the standing directive.)

**The problem.** M26–M30 made the world vertical, but the interactive walks (`run_play`/`run_game`/
`run_screensaver`) still substituted a single solid `{-1e6..1e6}` ground plane at the wanderer's floor — a
hold-over that **sealed every floor opening**. So in-game you could *see* down a shaft / through a down-stair
hole (M28/M30 see-through + abyss) but never **fall** through it: ascent worked, descent didn't. The headless
`--shaftfall`/`--descend` already proved the fall physics; what was missing were holes in the LIVE floor.

**Done (ADR-057).** (1) **Single source of truth in `gen`:** inline `shaft_floor_open`/`shaft_ceil_open` +
`floor_open_in_cell`/`ceil_open_in_cell`, and public `floor_hole_at`/`ceiling_hole_at(seed,level,cx,cz,i,j)`.
`GenerateChunk` is **refactored to use them** — the stair/shaft specs are still computed once per chunk, so it
is a **pure extraction: the floor/ceiling mesh is bit-identical** (no per-cell perf cost, no drift). (2)
`app::build_walk_collision()` replaces the giant plane in the three **interactive** walks: a per-cell solid
floor slab (`baseY-1..baseY`, top surface identical to the old plane) over the 3×3 ring, **skipping cells where
`floor_hole_at` is true** (down-stair holes + shaft voids). You fall through; the level below soft-catches you
on the next per-level rebuild — exactly like `--shaftfall`. The two **gated** level-0 walk-bot paths
(`run_walkbot` soak, `run_dxr_walk` PT) keep the flat sealed plane **on purpose** (a bot falling would perturb
their pacing/PT gate; they never descend). (3) **Generation fix:** the pillar pass placed full-height columns at
*any* cell, incl. floor-hole/shaft cells — a pillar **floating over a void** that also blocked a fall. It now
skips floor-/ceiling-hole cells (shared predicate; the skip is after the `rng.next_double()` consume, so
non-hole cells stay **bit-identical**). A column over a void is a generator bug to fix, not tune around.

**Gate.** clean build + full ctest (99/99) + the **NEW `--livedescent` proof** (finds a clean level-0
down-stair hole — not co-located with a level-0 up-stairwell — then asserts a SOLID cell HOLDS the wanderer up
AND the open DOWN-STAIR hole DROPS him a floor + soft-catches him, bit-identical ×2, model-free; seeds 1/7/42) +
the M30 soft-catch fall + abyss render + **M5 golden bit-identical** + M27 ascent + M28 see-through + INV-5/
inventory. Hand-verified beyond the gate: live descent robust across **15 seeds**, and **all** level-0 goldens
bit-identical — m4 top-downs, m5 shots (seeds 1/7/42), m7 biomes **including parking_garage (the pillar biome)**
— so the pillar fix touched no golden view; **zero re-baseline**.

**Also this session — the deep-descent SOAK (`--descentsoak`, ROADMAP §3 DONE criterion).** A long-haul
headless soak that repeatedly falls the wanderer down a deep shaft using the FULL live machinery (holed
`build_walk_collision` + abyss band-residency + a headless render each frame), teleporting back to the top
on each landing to churn level transitions / streaming load-evict / collision rebuilds. Gate-proven (seeds
1/42, `--ticks 12000`): ~50–66 descent cycles each (every one reaches the bottom — no stuck), residency
**bounded** (245 ≤ 294 = (kBand+2)×ring), process memory **flat post-warmup** (the baseline is sampled at
frame 64; growth ~3 MB, even negative on seed 7 — no leak; <32 MB threshold), and **bit-identical ×2** under
`--ticks` (determinism holds under async streaming). So the vertical paths hold **determinism + bounded
memory over the long haul** — a Phase-IV DONE criterion checked off. ADR-058.

**Also this session — the open-shaft draft TELEGRAPH (decision 6; M30 polish now COMPLETE).** Locked design
decision 6 says shaft entry is *accidental but ALWAYS telegraphed* — a draft warns you ~1 cell out (no
fail-state, so the cue is the only safeguard). Added a low "draft" wind voice to the `Synth` (`set_draft`,
a ~480 Hz lowpassed whoosh, click-free-smoothed) on a **dedicated `wind_rng_`** so the bed's `rng_` — and
the deterministic offline `--render-wav` — are byte-for-byte untouched (the wind term is `×draft_amp`, exactly
0 when no draft is set). `AudioEngine::set_draft` plumbs it lock-free; the shell computes
`draft_intensity_near_shaft` (scan the 3×3 for a shaft whose floor is open at this level, ramp 0→1 from ~8 m
to ~1 m) and feeds the mixer each frame in `run_play`/`run_game`/`run_screensaver` (the gated offline audio
paths never call it). Proven: a new `[m30][audio]` test (wind swells RMS >1.2×, 60 Hz bed survives, deterministic
×2, **draft=0 byte-identical to baseline**) + `--render-wav` still bit-identical ×2 + wavcheck OK (the m6
invariant holds); ctest 100/100. ADR-059. The despair gradient is now whole — you hear the void before you reach it.

**Gotchas.** (1) A down-hole co-located with a level-0 up-stair is a stacked stair-junction: the up-stairwell
steps catch you on level 0, so that particular cell isn't a clean fall (traversable, nothing stuck). The proof
picks a clean hole (fixture choice, like `--ascend` picks an interior up-stair); in-game these are a small
fraction of down-holes. (2) The SESSION_LOG's own `$seed:` PowerShell parse trap bit the new gate block — use
`${seed}:`. (Files: `gen/layout.h` + `layout.cpp` + `chunk.cpp`, `app/src/main.cpp`, `scripts/gate.ps1`.)

**Next — all model-free M30 work is now DONE** (live descent + deep-descent soak + telegraph audio; the fog
render + first-pass shaft/fall were already in). The Phase-IV §3 DONE criteria now hold **except M29**
(`m29-green` is model-blocked, ISSUE-5 — the code + determinism are done + committed; it needs `llama-server
:8080` up for the live-brain `valid_intents>=1` assertion). So the remaining decision is the DONE gate itself:
write `.brstate/DONE` with **M29 explicitly accepted-as-blocked** (operator-adjacent — M29 isn't `[x]`, so
strictly the DONE definition isn't met), or **hold** for the sidecar. Per the standing "never block, route
around operator-only acts" directive, the model-free roadmap is exhausted; a future session finishes M29 the
moment `:8080` is up (one gate run). Otherwise → perpetual-polish (ROADMAP §4). Per `_run_state/ROADMAP.md` §2/§3.

---

## Session 31 — M30: Open shafts & the abyss (the despair gradient)  ✅ COMPLETE — 🏷️ `m30-green`

`gate.ps1 -Milestone M30` exits 0; tagged `m30-green` + pushed. **The second vertical system is in** — a
rare deep void you can fall down, 5–10 floors in one drop. (Done before M29 finishes: M29's sacred-gate
Increment 2 needs the KEEL sidecar/model, so per the operator's "model-free fruit first" + round-robin
directive, model-free M30 took priority. M29 Increment 1 — per-floor core + escape — is committed `fa39ebe`.)

**Done (M30, first pass).** (1) `gen::shaft_at(seed,cx,cz)` — pure, per-column, **very rare** (`hash %
kShaftDensityN=1500`, the ~1.3 km cadence) — a hashed cell + top level + depth (5..10). `shaft_passes(s,
level)` lets any floor decide locally whether the void spans it (inclusive `[top-depth, top]`) — the
Z-analogue of the stair seam, no neighbour query. (2) `GenerateChunk` cuts the void's slice at the shaft
cell (floor open except the bottom landing, ceiling open except the top entry), additive to the M27 stair
holes. (3) **Soft-catch fall — no new physics:** the void has no floor, gravity drops the wanderer, the
bottom level's solid floor catches it via the existing swept-AABB collision (a soft landing — there is no
health/fail-state by design). `--shaftfall` is the headless proof.

**Gate.** clean build + full ctest (incl. a new `[m30]` test: rare `<0.005`, deep `5..10`, deterministic,
per-seed-relocated, `shaft_passes` spans exactly the range) + **`--shaftfall`** (seeds 1/7/42 fall the full
depth — 10/9/9 floors, ~36–38 m/s — and **land**, bounded, bit-identical ×2) + shafts render debug-clean
at levels 0/7/10 with **M5 golden bit-identical** (voids ~1/1500 miss the views — verified m4/m5/m1/m2
all 0-diff) + M27 ascent + M28 see-through regressions + INV-5/inventory.

**4 green increments** (`07f7d26` `shaft_at` + `[m30]` · `7c263ff` shaft holes in GenerateChunk · `30b6508`
`--shaftfall` · this gate). No new dependency; no golden change.

**Tracked M30 polish (not blocking m30-green):** the draft/wind **telegraph** audio (locked design decision
6 — accidental but always telegraphed), streaming several floors *down* a shaft for the **fog-to-black**
abyss render, and a **deep-descent soak**. **Gotcha:** none new — the fall reused the existing swept
collision wholesale, which is exactly why "soft-catch, no fail-state" needed zero new physics code.

**Next: finish M29** — Increment 2 (the sacred record→replay-across-a-descent gate + `Invoke-GateM29`).
**Needs the KEEL sidecar :7071** (launch via the autoloop's `Ensure-Sidecar`); the determinism check runs
model-offline. Then the M30 polish + the Phase-IV completion sweep. Per `_run_state/ROADMAP.md` §2.

---

## Session 30 — M28: Vertical streaming + see-through  ✅ COMPLETE — 🏷️ `m28-green`

`gate.ps1 -Milestone M28` exits 0; tagged `m28-green` + pushed. **You can stand at a stairwell and see the
floor above through the hole** — two floors are resident + rendered at once. Third Phase IV milestone.

**Done (M28).** `StreamManager::update(center, extra_level)` keeps TWO concentric rings — the wanderer's
level and one adjacent level — picking the adjacent level by stance: in the upper part of the 4 m level cell
(`pos.y - level_base_y > 2`, i.e. climbing) it pre-loads `level+1` (seamless arrival); otherwise `level-1`
(see DOWN through floor holes — the despair gradient). The 1-arg `update(center)` delegates to
`update(center, center.level)`, so `run_game`/soak/tests stay **bit-identical** (only the live walk opts in).
**See-through needed no renderer change:** chunk verts are already baked at their `level_base_y` and the GPU
upload cache is keyed by the full `ChunkKey`, so two levels at the same `(cx,cz)` draw at their true world-Y
and the M27 aligned holes line up to look straight through. Presentation-only — the ring never touches
`WorldState` (INV-1), residency is bounded by `2*(2r+1)^2` (INV-4), collision stays single-level.

**Gate.** clean build (no warnings) + full ctest (96; incl. a new `[m28]` test: two-level residency is
exactly `2x` the one-level ring, evicts the old adjacent level on switch, 1-arg path single-level) + **the
see-through proof** (`--vstream`: stand at a real stairwell, render one-floor vs two-floor; residency
`162 = 2x81` bounded, both debug-clean, the two-floor render differs by `see_through_diff ~57-62` = the floor
above is visibly drawn through the ceiling hole) + **regressions** (M5 raster golden **bit-identical**; M27
live ascent still climbs to level 1) + INV-5/inventory.

**3 green increments** (`050d08c` two-level residency in StreamManager + run_play/run_game + `[m28]` test ·
`--vstream` see-through mode + `Invoke-GateM28` · this gate). **No new dependency; no golden change**
(presentation-only — the second ring is off in every fixed-scene/golden render). ADR-054.

**Gotchas.** (1) The first `--vstream` camera sat at the stair-cell centre — **inside** the staircase
geometry — so the upward view was blocked and `see_through_diff` was 0; moved it to the clear low-approach
strip (−X of the risers) looking straight up. (2) PowerShell parse trap: `"...$seed: ..."` reads `$seed:` as
a drive/scope-qualified variable — write `${seed}:` (or avoid `:` right after a `$var`).

**Next: M29 — per-floor Shoggoth** (confined to its level, seeded per `(seed, level)`, descend a stair to
escape it; record→replay bit-exact with the model offline). **Needs the KEEL sidecar :7071.** Per ROADMAP §2.

---

## Session 29 — M27: Procedural stairs (the stack is connected + climbable)  ✅ COMPLETE — 🏷️ `m27-green`

`gate.ps1 -Milestone M27` exits 0; tagged `m27-green` + pushed. **The floors are now joined** — a wanderer
can climb a real procedural stair from one floor to the next. Second Phase IV milestone.

**Done (M27).** (1) `gen::stair_at(seed,L,cx,cz)` — pure, total — places an UP-stair via a density scatter
(`hash % kStairDensityN=13`) PLUS a 4×4-superblock backstop, so every superblock holds ≥1 up-stair (the
Z-analogue of INV-3) and both floors read the SAME function (level L cuts its ceiling hole + builds the
stairwell; level L+1 cuts its floor hole at the same cell — aligned by construction, INV-2 in Z, no
neighbour query). (2) `GenerateChunk` cuts the aligned holes + builds a climbable stairwell: 8 thin
grounded riser-slabs (0.5 m each, inset, abutting → passes `ValidateChunkGeometry`); the pillar pass skips
the stair cell. (3) `generate_layout` carves the stair cell's interior walls open (carving never
disconnects; perimeter/door walls untouched → seams still agree). (4) **Step-up locomotion** in
`move_and_collide` — a surgical pre-pass that mounts a low step (top within `kStepHeight=0.55 m` of the
feet) when lifting onto it is clear; **inert for full-height walls/pillars, so all prior collision +
determinism is bit-identical**. (5) `validate_vertical_connectivity` (Z flood-fill: horizontal doorways
always link, vertical links iff `stair_at` fires) makes "no floor is ever sealed" machine-checkable.

**Gate.** clean build (no warnings) + full ctest (95; incl. `[m27]` stair_at coverage + the
vertical-connectivity validator with a non-vacuity control) + **live ascent** (`--ascend`: the capsule
climbs a real up-stair ~3.98 m to **level 1**, seeds 1/7, bit-identical hash ×2) + `--descend` still
reaches level -1 (M7 regression) + **goldens** (the 2 ceiling-facing m4 top-down goldens re-baselined via
goldgen + **ADR-053**; m5 first-person shot + m1 frame-0 + m2 room **byte-identical**) + walk-bot 1 km × 0
stuck on 3 seeds + brain-off shoggoth determinism (M20) + INV-5/inventory.

**4 green increments** (`e7543bf` holes+stairwell+carve+goldens+ADR · `70f0eef` step-up + `--ascend` ·
this gate). **No new dependency** (pure `gen`/`core` code).

**Gotcha — `ValidateChunkGeometry` is strict.** It accepts only grounded (`mn.y==baseY`) thin walls or
small square pillars, with no fat overlaps. The first stairwell (full-cell nested boxes, `baseY-0.05`)
failed 4 tests; the fix was thin, grounded, abutting, inset riser-slabs. **Gotcha — no step-up existed:**
the axis-separated AABB resolver stopped the capsule flat at any riser, so stairs were unclimbable until
the step-up pre-pass; it's gated to low boxes only, keeping every existing hash bit-identical.

**Next: M28 — vertical streaming + see-through** (two-floor residency at a stairwell so the ascent crosses
the seam seamlessly + you can see down the hole). Per `_run_state/ROADMAP.md` §2.

---

## Session 28 — M26: Live multi-level (Phase IV foundation)  ✅ COMPLETE — 🏷️ `m26-green`

`gate.ps1 -Milestone M26` exits 0; tagged `m26-green` + pushed. **The live walk is multi-level** — the
infinite Backrooms now extends in Z (stacked floors). First Phase IV milestone (the Vertical Backrooms).

**Done (M26).** The wanderer's current floor is kept **IMPLICIT** — derived from `pos.y` via
`contracts::level_from_y` (the inverse of `level_base_y`) — so `world_state_hash` is unchanged and every
existing replay stays bit-identical. `run_play`/`run_game` now stream/collide/generate at that level; the
collision ground plane sits at `level_base_y(level)`; the raster fluorescent lights offset to the
wanderer's floor. `--shot --level N` renders any floor (the proof vehicle). `level_from_y(0)=0` → all
level-0 paths + the fixed-scene level-0 test modes are **byte-identical**.

**Gate.** clean build + full ctest (`[m26]`: `level_from_y` round-trip · non-repeating biome by level ·
INV-3 + valid geometry across ±16 levels) + **M5 raster golden bit-identical at level 0** + levels 7 and
-3 render debug-clean + distinct (diffs 6.35 / 3.19 / 7.77) + brain-off M20 determinism + INV-5/inventory.

**4 green increments** (`a273426` foundation · `a16c258` `[m26]` tests · `bd2cde0` per-level lights ·
this `--level` proof + `Invoke-GateM26`). Non-repeating floors were already solved (per-level
`chunk_seed`/`biome_at`, the M7 bones); M26 was integration (de-hardcode level 0), **no WorldState/hash
change** — the load-bearing no-regression constraint.

**Gotcha.** The fake `{-1e6,-1,-1e6}..{1e6,0,1e6}` ground plane became level-aware (`baseY=
level_base_y(c.level)`); real per-chunk floor collision + holes come with M27.

**Next: M27 — procedural stairs** (hybrid K=4: density + 4×4 superblock backstop · `stair_at` shared
vertical-seam hash · cut floor/ceiling holes · stair-aware layout · vertical-connectivity validator).
Per `_run_state/ROADMAP.md` §2.

---

## Session 27 — M25: The Shoggoth's body in the RAY-TRACED path  ✅ COMPLETE — 🏷️ `m25-green`

**`gate.ps1 -Milestone M25` exits 0; tagged `m25-green` + pushed. The creature is visible in BOTH
renderers now** — M20b's procedural body, which was raster-only, now shows + writhes in the DXR
(path-traced) path too, without regressing M19's frame rate.

**Done (M25; ADR-052).**
- **`DxrRenderer::update_creature(verts, count)`** — a DYNAMIC creature BLAS updated per frame WITHOUT
  rebuilding the cached chunk BLASes. `build_scene` reserves a `kMaxCreatureVerts` (4096) tail in the
  shade buffer + caches the chunk TLAS instances + chunk vert count; `update_creature` writes the creature
  verts into that tail (InstanceID = chunkVertCount), builds ONE creature BLAS, and rebuilds only the TLAS.
  Per-frame cost = 1 BLAS + 1 TLAS build (not 169) → cheap enough to animate every frame.
- **Salmon in the PT** — the path-tracer shades by material id, so the creature is tagged **material 7**
  at injection (a salmon `albedo_of` branch in the scene shader). The raster path is UNTOUCHED (it shades
  by vertex colour → M20b/M5 byte-for-byte identical; the material override is DXR-only).
- **Wired into `run_play` + `run_game`** RT branches + **`--shoggoth-dxr-shot`** (headless: pose 2
  creature-only / pose 1 world-only / pose 0 world+creature).
- **`Invoke-GateM25`** — clean build + full ctest (`[m25]` mesh-fits-the-tail) + **the body renders salmon
  in DXR** (866 salmon px vs 0 world-only; R>1.5·G isolates salmon from the yellow Backrooms) + **`--play
  --rt` debug-clean + M19 frame-rate preserved** (531 DXR frames/5 s >> 30) + raster byte-for-byte
  unchanged (M5 golden) + DXR world render + M20b raster body + INV-5/inventory. **gate.ps1 M25 exits 0.**

**Notes.** The cached-chunk design is the key: rebuilding all ~169 chunk BLASes per frame would have
dropped RT below the 30-frame floor; updating only the creature BLAS + TLAS keeps it at ~106 FPS. The PT
shades by MATERIAL not vertex colour, which is why M20b's salmon body rendered as yellow wallpaper in DXR
until tagged material 7. The salmon discriminator R>1.5·G cleanly separates the creature (0.90,0.42,0.34)
from the yellow walls (0.80,0.72,0.40). (Gate gotcha: `--play --rt` only prints `rt_luma_mean` when `--out`
is given — the gate now passes it.)

**Phase III's Shoggoth arc is now FULLY COMPLETE** — body, deterministic navigation, KEEL brain, live
async brain, vision, hearing, a PA voice, and the body visible in both renderers. Determinism stayed
sacred throughout (every AI decision → event log → bit-exact replay, all models offline).

---

## Session 26 — M24: The Backrooms PA gets a VOICE (procedural TTS)  ✅ COMPLETE — 🏷️ `m24-green`

**`gate.ps1 -Milestone M24` exits 0; tagged `m24-green` + pushed. The Backrooms speaks** — a from-scratch
procedural formant TTS (no assets) gives the PA a voice whisper can read back as WORDS, so the Shoggoth's
hearing is no longer just coarse ambient tags. The recorded chase still replays bit-for-bit with the TTS,
whisper, AND the model offline.

**Done (M24; ADR-051).**
- **`app/tts.h`** — a pure header-only Klatt/eSpeak-style **formant speech synthesizer** (NO assets):
  text → phonemes (a Backrooms-PA lexicon + a letter-to-sound fallback) → a sawtooth glottal source + a
  3-formant cascade of 2-pole resonators → 16-bit PCM. `synthesize_speech(text, sr)`.
- **The key to whisper-readability was PROSODY.** A flat monotone buzz transcribed as *singing*
  ("♪ … my glory ♪"); a **declining pitch contour + per-period jitter** + the sawtooth source turned it
  into speech. Measured (large-v3-turbo): "EVACUATE SECTOR FIVE" → **"Evacuate sector 5"**, "LEVEL THREE
  CONTAINMENT BREACH" → **"Level 3 ... breach"**.
- **`--tts-say`** writes a spoken line to a WAV; **`--tts-check`** round-trips text → TTS → whisper and
  reports the words recovered (the gate's intelligibility metric).
- **`run_shoggoth_pa_record`** (`--shoggoth-pa-record`) — mixes a spoken Backrooms PA announcement (the
  TTS) over a quiet ambient bed at the Shoggoth's ears → whisper (default large-v3-turbo) → the heard line
  into the brain → `ShoggothEvent` log. Byte-identical chase to `--shoggoth-record` → `--shoggoth-replay`
  reproduces it bit-for-bit with the TTS + whisper + the model OFFLINE.
- **`Invoke-GateM24`** — clean build + full ctest (`[m24]` TTS determinism + lexicon) + whisper (both
  models) + KEEL + **the TTS is intelligible** (≥2 words recovered) + **the PA-voice sacred gate**
  (record == replay all-offline) + 2 graceful no-ops + M21 text sacred gate + M20/M5/INV-5/inventory.
  **gate.ps1 M24 exits 0. Measured: "Evacuate sector 5" (2/3 words), 5 PA intents, record == replay.**

**Gotchas / notes.** (1) A flat monotone formant buzz reads as MUSIC to whisper — speech needs a moving
pitch (declination + jitter). (2) A `static thread_local` lowpass state in the synth leaked between calls
→ broke per-call determinism → made it a local (the `[m24]` determinism test caught it). (3) Calling
whisper-cli directly from PowerShell tripped `$ErrorActionPreference='Stop'` on whisper's stderr → moved
the call into the app (`--tts-check`, robust `CreateProcess`) and the gate reads the `recovered_words`
metric. No new build dependency (the TTS is pure code; whisper stays a shelled-out tool).

**Next: M25 — the Shoggoth body in the DXR (ray-traced) path** (currently raster-only). Then the Shoggoth
arc is fully complete (body + nav + brain + live brain + eyes + ears + voice, visible in both renderers).

---

## Session 25 — M23: The Shoggoth HEARS (whisper.cpp)  ✅ COMPLETE — 🏷️ `m23-green`

**`gate.ps1 -Milestone M23` exits 0; tagged `m23-green` + pushed. The monster has eyes AND ears** — it
renders the soundscape at its vantage, whisper.cpp transcribes it into its brain, and the recorded chase
**replays bit-for-bit with whisper AND the model offline** (the sacred gate, now with hearing). Phase III's
sensory arc is complete: navigation → brain → live brain → vision → hearing.

**Investigation (M23 STEP 1) — whisper works, but the Backrooms is non-speech.** `C:\whisper.cpp` is fully
built (`whisper-cli.exe` + `whisper-server.exe` + DLLs; models `ggml-base.en.bin` + `ggml-large-v3-turbo.bin`
in `C:\models`). CLI: `whisper-cli.exe -m <model> -f <wav> -otxt -np -l en` → `<wav>.txt`. A **live probe**
on the rendered Backrooms soundscape (`--render-wav`: hum+footsteps+reverb) returned **`(upbeat music)`** —
a coarse sound-event tag, NOT words (there's no speech to transcribe). Operator chose **M23-A** (ambient
sound-tag hearing) over a heavier procedural-TTS "PA voice" loop. (whisper-server defaults to `:8080` = the
llama-server's port, so the CLI is used.)

**Done (M23-A; ADR-050).**
- **`app/shoggoth_hearing.h`** (pure: `clean_transcript` + `render_shoggoth_hearing_prompt` = the M21 text
  brain + a "your ears hear: <tag>" line).
- **`run_shoggoth_hearing_record`** (`--shoggoth-hearing-record`): renders ~2.5 s of the soundscape AT THE
  SHOGGOTH'S EARS (M6 `Synth`: the drone + the wanderer's footfalls scaled by proximity) → `whisper_transcribe`
  (shells out to `whisper-cli.exe` via `CreateProcess`, reads `<wav>.txt`) → the tag into `keel_complete` →
  `parse_shoggoth_intent` → `ShoggothEvent` log. Byte-identical chase to `--shoggoth-record` so
  `--shoggoth-replay` reproduces it. `--whisper-exe` / `--whisper-model` default to the known paths.
- **`Invoke-GateM23`** — clean build + full ctest (`[m23]` transcript-trim + hearing-prompt) + whisper
  present + KEEL reachable + **THE SACRED GATE WITH EARS** (soundscape → whisper → ≥1 transcript → ≥1 intent
  → record == replay whisper+model off) + 2 graceful no-ops (whisper missing → "silence" but runs; KEEL down
  → 0 intents) + M21 text sacred gate + M20/M5/INV-5/inventory. **gate.ps1 M23 exits 0. Measured: 5 listens
  → 5 transcripts (`(upbeat music)`) → 5 intents, record == replay (whisper + model off).**

**Notes.** **No new build dependency** — whisper.cpp is an external process the app shells out to (runtime-
optional, like a tool; nothing linked/vcpkg'd/shipped; absent → graceful no-op). Determinism is robust even
though whisper's transcript depends on the audio: only the resulting INTENT is logged + replayed, so replay
never runs whisper or the model. The combined hash is per-run (the model is stochastic), but record == replay
holds every run.

**Next: Phase III sensory arc COMPLETE.** Remaining optional: a richer "PA voice" hearing loop (procedural
TTS of the Director's intercom captions → whisper hears words) and **M20b-in-DXR** (the in-world body renders
in the raster path only). Both deferred / operator's call.

---

## Session 24 — M22: The Shoggoth SEES (KEEL vision)  ✅ COMPLETE — 🏷️ `m22-green`

**`gate.ps1 -Milestone M22` exits 0; tagged `m22-green` + pushed. The monster has eyes** — a virtual
camera renders its POV, a local vision model (qwen-VL + mmproj) decides from what it sees, and the
recorded chase **replays bit-for-bit with the model offline** (the sacred gate, now multimodal).

**Investigation (M22 STEP 1) — the `:7071` sidecar does vision AS-IS; NO new infra.** The plan allowed
standing up a backrooms-local KEEL copy if `:7071` couldn't do vision. It can: its `keel-serve` (Stage-2
snapshot) parses OpenAI `image_url` parts → `Content::Image` → router FORCES local; `keel.lock` pins
`llm_vision: qwen3.5-9b-q5_k_m` + `mmproj: mmproj-F16`; the shared `llama-server :8080` (reused read-only)
runs `--mmproj`. A **live probe** (real 640×360 POV shot → `image_url` POST to `:7071`) returned an
accurate description in **~1.3 s** at `tier:local, cost:0.0`. `C:\KEEL` only read; never `:7070`.

**Done (M22; ADR-049).**
- **`director::keel_complete_vision(host,port,prompt,image_base64,timeout)`** — the M11 client with a
  `[text, image_url(data:image/png;base64,…)]` turn (a shared `keel_post` now backs text + vision).
- **`app/shoggoth_vision.h`** (`shoggoth_pov_camera` + `render_shoggoth_vision_prompt`) + **`app/base64.h`**
  (RFC-4648, unit-tested). **`run_shoggoth_vision_record`** renders the POV to an offscreen 384×216 snapshot
  (headless renderer → `readback` → `stbi_write_png_to_func` → base64) → vision → `parse_shoggoth_intent` →
  `ShoggothEvent` log. **Byte-identical chase to `--shoggoth-record`** so `--shoggoth-replay` reproduces it.
- **`Invoke-GateM22`** — clean build + full ctest (`[m22]` base64/POV-camera/vision-prompt/fenced-parse) +
  KEEL reachable + **THE SACRED GATE WITH EYES** (POV → ≥1 vision intent → record == replay model-off,
  snapshot debug-clean, first POV PNG written) + graceful vision-down no-op + the M21 text sacred gate +
  M20/M5/INV-5/inventory. **gate.ps1 M22 exits 0. Measured: 5 snapshots → 5 vision intents, record ==
  replay (model off).**

**Gotcha / fix.** The vision tier wraps its JSON in a markdown ```json fence, so the whole-string
`json::parse` in `parse_shoggoth_intent` rejected it → `valid_intents: 0`. Fix: extract the outermost
`{…}` span before parsing (handles fenced/prose-wrapped replies; bare JSON unchanged → M21 text gate
intact; non-JSON → safe Hunt default). The combined hash is per-run (the model is stochastic), but
record == replay holds every run. POV snapshots render debug-clean even from the creature's vantage.

**Next: M23 — hearing (whisper.cpp), operator-gated.** Nearby procedural audio → `C:\whisper.cpp` → text
→ the shoggoth's context, so it has eyes AND ears. *(Also still pending: M20b-in-DXR — in-world body shows
in the raster path only.)*

---

## Session 23 — M21b: The Shoggoth's LIVE async brain  ✅ COMPLETE — 🏷️ `m21b-green`

**`gate.ps1 -Milestone M21b` exits 0; tagged `m21b-green` + pushed. The monster thinks WHILE you
play** — KEEL inference runs off the 120 Hz frame thread, so the creature reasons live without ever
hitching the loop, and the headless record→replay determinism stays bit-exact.

**Done (M21b; ADR-048).**
- **`app/shoggoth_brain_host.h`** — a header-only `ShoggothBrainHost`, a faithful mirror of the
  Director's async `DirectorHost`: a worker thread, non-blocking latest-wins `submit(ShoggothSummary)`
  (≤1 inference in flight), non-blocking `poll()` → validated intents, atomic `requests()`/`produced()`.
  The worker runs the exact M21 brain off-thread (`keel_complete` → `render_shoggoth_prompt` →
  `parse_shoggoth_intent`); KEEL down → `ok=false` → graceful no-op (the M20 `Hunt` default keeps hunting).
- **Wired into `run_play` + `run_game`** — every ~3 s (ambient wall-clock) submit a summary; apply the
  returned intent to `sh.intent` at a tick boundary (the same discrete timestamped-event shape the
  headless sacred gate proves bit-exact). On by default; **`--no-shoggoth-brain`** kill switch. New
  metrics `brain_intents` / `brain_requests`.
- **`Invoke-GateM21b`** — clean build + full ctest (`[m21b]` host-lifecycle test) + KEEL reachable +
  **the live brain thinks (≥1 intent in `--play`) AND is async-isolated** + graceful dead-URL no-op +
  kill switch + `--game` debug-clean + **the M21 SACRED GATE** (record == replay, model off) + M20/M5/
  INV-5/inventory. **gate.ps1 M21b exits 0.**

**Async isolation, proven two ways** (M3 + M11/Gate-2). Absolute: p99 frame < 2× median, best-of-2 at
1440p (a blocking host — a multi-second inference on the frame thread — would explode this to ~100×).
Relative: brain-ON hitch ratio adds nothing over brain-OFF. **Measured (8 s `--play`, 1440p): on
p99/median 1.79× vs off 1.44× — ON ≈ OFF**; the residual is the uncapped loop's shared-GPU jitter
(ADR-039), not a stall. The live brain applied 3 intents over 8 s. Sacred gate: 5 LLM intents,
record == replay `12e9f56f9171fe5d` model offline.

**Gotchas / notes.** `parse_host_port` lives in a NESTED anonymous namespace in `main.cpp`; a plain
forward declaration for `run_play`/`run_game` (which precede the definition) created a second overload →
`ambiguous call`. Fix: wrap the forward decl in `namespace { … }` so it lands in the SAME anonymous
namespace as the definition. `--play`'s uncapped loop is naturally ~1.9× p99/median at 720p (loop jitter,
NOT the brain — brain-off measured the same), so the pacing gate runs at 1440p (~1.45×, stable) and ALSO
asserts ON ≈ OFF, which is robust to that jitter.

**Next: M22 — vision (operator-gated: confirm before building).** A POV snapshot → qwen-VL
(`mmproj-F16.gguf`) via a backrooms-local KEEL copy feeds richer intent to this same brain; first step is
investigating the KEEL vision setup. *(Also still pending: M20b-in-DXR — the in-world body shows in the
raster path only.)*

---

## Session 22 — M21: The Shoggoth's KEEL brain  ✅ COMPLETE — 🏷️ `m21-green`

**`gate.ps1 -Milestone M21` exits 0; tagged `m21-green` + pushed. The monster thinks** — a
local LLM drives the Shoggoth, and a recorded chase **replays bit-for-bit with the model
offline** (the sacred gate, now for the creature).

**Done (M21; commit `<wip>` + gate; ADR-047).**
- **`app/shoggoth_brain.h`** — `ShoggothSummary` → a **shoggoth system prompt** →
  **`director::keel_complete`** (reused verbatim) → a **validated** `ShoggothIntent
  {action: hunt/stalk/lurk/flank/flee, aggression}` (reuses the Director's JSON reader; bad
  input → safe `Hunt` default, never throws).
- **The cascade** — the intent only sets the *goal bias + mood*; the **M20 deterministic BFS
  navigator does the walking** every tick (`Hunt` = unchanged M20). LLM runs rarely (~2 s);
  motion every tick. The intent folds into `shoggoth_hash` but lives outside WorldState.
- **`--shoggoth-record`** (brain ON, live KEEL, logs intents at their ticks) / **`--shoggoth-replay`**
  (model OFF, applies the logged intents) → **identical combined hash**. The model lives only
  in the event log.
- **`Invoke-GateM21`** — ctest (`[m21]`) + **THE SACRED GATE**: record with **≥1 real KEEL
  inference** → replay model-off **bit-identical** (5 LLM intents, identical hash) + brain-off
  M20 regression + M5 golden + INV-5/inventory. **gate.ps1 M21 exits 0.**

**Gotchas / notes.** Fixed `parse_host_port` (it kept `http://` in the host — `rfind(':')`
split at the port → `WinHttpConnect failed`); now strips the scheme + path. The KEEL sidecar
at `:7071` must be up for the record (the Director's, reused). The intent default `Hunt` +
aggression 0.5 reproduces M20 exactly, so existing `[m20]`/`--shoggoth` gates are unchanged.

**Next: M22 — vision (deferred per operator: stop before M22).** A POV snapshot → qwen-VL
(`mmproj-F16.gguf`) via a **backrooms-local KEEL copy** feeds richer intent to this same brain;
first step is investigating the KEEL vision setup. *(Also pending: M21b live async brain in
`--game`, M20b-in-DXR.)*

---

## Session 21 — M20: The Shoggoth (deterministic maze-navigating chase)  ✅ COMPLETE — 🏷️ `m20-green`

**`gate.ps1 -Milestone M20` exits 0; tagged `m20-green` + pushed. Something lives in the
Backrooms now** — a deterministic monster that hunts the wanderer through the maze.

**Done (M20; commit `<wip>` + gate; ADR-046).**
- **`app/shoggoth.h`** — a deterministic chase creature **outside WorldState** (so every
  existing replay/walk-bot/Director hash is untouched — structurally unchanged
  `world_state_hash`). Pure `shoggoth_step(sh, wanderer, seed, pathfind)` + own
  `shoggoth_hash`. **BFS maze pathfinding** (gen layouts, bounded window, per-chunk cache)
  toward the wanderer's cell, steering to cell centres (stays in corridors, no
  wall-tunnelling); **lurk → hunt → chase → retreat** state machine; organic ooze.
- **`--shoggoth`** — a wanderer walks the maze (the M11 `MazeWalker`); the shoggoth hunts.
  Reports `shoggoth_hash` + `min_dist` + `moved` + `ever_hunted`. `--out` renders a CPU
  top-down **chase map** (maze walls + cyan wanderer trail + red shoggoth trail) — the visual.
- **`Invoke-GateM20`** — ctest (`[m20]`: determinism, lurk-far, hunt-and-close, navigates) +
  `--shoggoth` same-seed-identical-hash ×2 + engages/navigates/closes-in across 3 seeds +
  walk-bot/M5 regression (shoggoth separate → existing determinism intact) + INV-5/inventory.
  **gate.ps1 M20 exits 0.** Seed 1: caught the wanderer at **1.38 m** after routing **19 m**.

**Gotchas / notes.** The shoggoth lives in `app` (header-only) because navigation needs `gen`
but `core` depends on nothing — it's deterministic by construction (seeded, no wall-clock),
record→replay bit-identical on a build (the M20 gate proves it). The **body is a proxy** (a
top-down marker) per the operator's "movement logic first" steer; the **in-world 3D blob is
M20b**. The BFS-to-goal-cell design is the **cascade scaffold for M21**: the LLM will set the
goal cell / mood, this cheap navigator walks it there.

**M20b ✅ (`m20b-green`) — the in-world body.** A procedural **warm-orange radial-tentacle blob**
(operator reference; no assets) generated each frame in world space (`app/shoggoth_body.h`) and
**injected as a synthetic `ResidentChunk`** with a per-frame key → draws through the existing lit
pipeline (zero renderer change) and the writhe animates. Wired into `--play` + `--game` (Play) — the
shoggoth chases you, body visible first-person; `--shoggoth-shot` renders it for QC/gate. Gate
(extended M20): body renders in-world debug-clean + the live walk stays debug-clean. Fixed a UB
crash (`StreamManager::resident()` returns **by value** → `.begin()/.end()` of two temporaries).

**Next: M21** the Shoggoth's KEEL brain (shoggoth system prompt → schema-valid intent → this
navigator; replay bit-identical model-off), then **M22** vision (qwen-VL + mmproj via a
backrooms-local KEEL copy). *(M20b in DXR/ray-traced mode is a small follow-up — currently the
in-world body shows in the raster path; RT shows the world without the live creature.)*

---

## Session 20 — M19: Real-time ray-tracing toggle  ✅ COMPLETE — 🏷️ `m19-green`

**`gate.ps1 -Milestone M19` exits 0; tagged `m19-green` + pushed.** The live walk can now be
**path-traced** with a Settings toggle — default OFF, so it can't regress anything.

**Done (M19; commit `<wip>` + gate; ADR-045).**
- **Toggle** — Settings → "RAY TRACING" (`kSettingsItems` 5→6), **F2** hotkey, `--rt` flag,
  persisted in `backrooms.cfg` (the reserved `renderer` field). Default **off**.
- **The bridge** — `render_dxr` is a separate device doing headless readback; rather than share
  the swapchain, when RT is on the loop renders the path tracer at **2/3 internal res**,
  `readback`s, and presents via `render_d3d12::present_overlay_windowed`, which now **sizes its
  overlay texture to the source** and **upscales** via the fullscreen-triangle sampler. No
  cross-device sharing, no new pipeline (reuses the M15 overlay primitive). Accumulation resets
  on camera move; scene rebuilds on chunk-center change.
- **`Invoke-GateM19`** — ctest + **RT-on** (`--play --rt`: 663 DXR frames, luma 176, debug-clean)
  + **RT-off** (`--play`: raster, `rt_frames=0`, debug-clean) + **M5 raster golden bit-identical**
  (default-off = no regression) + menu goldens (incl. the new RAY TRACING row) + INV-5/inventory.

**Gotchas / notes.** `present_overlay_windowed` now takes a *source* size (the menu callers pass
window-size → unchanged; RT passes the smaller DXR size → upscaled). The **settings golden was
re-captured** via `goldgen` (Iron Rule 6, ADR-045) — only `settings.png` changed. RT perf is
fine because the per-frame cost is dominated by the readback (~constant); the headless `--play`
test sits static so it *accumulates* (clean), but moving resets each frame at the same cost. The
DXR→present bridge will be reused for **M22 shoggoth vision**.

**Next: M20** — the **Shoggoth**: a procedural amorphous 3D body + deterministic grid pathfinding
(WorldState entity, seeded/replayable), intermittent chase. Then M21 its KEEL brain, M22 vision.

---

## Session 19 — M18: Realistic walking (head-bob + run)  ✅ COMPLETE — 🏷️ `m18-green` · Phase III begins

**`gate.ps1 -Milestone M18` exits 0; tagged `m18-green` + pushed. Phase III ("the Backrooms
come alive", M18–M23) is underway.** The walk now bobs like a person walking, and Shift runs.

**Done (M18; commit `<wip>` + gate; ADR-044).**
- **Run** — `contracts::kButtonRun` (Shift / gamepad left-trigger or shoulder) → `core::tick`
  uses `kRunSpeed` (6.8 m/s ≈ 1.7× walk). Deterministic *through the input contract*: unset in
  every existing replay/walk-bot/gate, so they run at `kWalkSpeed` exactly as before — **M0–M17
  determinism untouched** (`world_state_hash` gains no field).
- **Head-bob** — `app/head_bob.h`, a **pure** `head_bob(distance, speed, walk, run) → {dy,dx}`
  driven by the **deterministic odometer** but applied to the **render camera only**
  (`apply_head_bob` after `wanderer_camera`) → never WorldState. Two vertical dips per stride
  cycle (footfalls) + half-frequency lateral sway (figure-8), eased in/out by speed; running
  raises cadence + amplitude. Wired into `--play` + `--game`.
- **`Invoke-GateM18`** — ctest (incl. `[m18]`: curve bounded/dips-only/2-per-cycle/continuous +
  run odometer ~1.7× deterministic) + `--game` debug-clean + **M5 raster golden bit-identical**
  (proves the bob is view-only) + INV-5/inventory. **gate.ps1 M18 exits 0.**

**Gotchas / notes.** The bob is reproducible *because* it reads the deterministic odometer, yet
golden-safe *because* it's a view-layer camera offset — `--shot` uses fixed poses (not
`wanderer_camera`), so the M5 golden is bit-identical (gate-checked). The Phase III template:
**sim-affecting input through the contract, cosmetics in the view.** Plan: memory
`project-phase-III-living-backrooms.md` + `docs/MILESTONES.md` Phase III.

**Next: M19** — real-time ray-tracing **toggle** in the windowed loop (Settings; default OFF →
no regression; bring `render_dxr` to the swapchain). Then M20–M22 the AI Shoggoth.

---

## Session 18 — M17: Portable packaging  ✅ COMPLETE — 🏁 `v2.0` (playable + packaged)

**`gate.ps1 -Milestone M17` exits 0; the full M0–M16 regression sweep is green; tagged
`v2.0` + pushed. PHASE II IS DONE.** The v1.0 headless-first visualization is now a
**playable, portable game** — unzip and run: an infinite procedural Backrooms with a main
menu, persistent settings, fullscreen, gamepad support, real-time audio, and a path tracer,
in a self-contained ~7 MB folder that needs no installer and no Windows SDK.

**Done (M17; commit `<wip>` + this; ADR-043).**
- **Release build config** — CMake `BACKROOMS_RELEASE` defines `BR_RELEASE`, which compiles
  the D3D12 debug layer + DRED **out** of `create_device_core` (a shipped exe never trips
  them) and builds optimized. Dev/gate builds are byte-identical to M16 (the `#ifndef` is
  always-true there), so M17 can't regress M0–M16.
- **Bundled DXC** — `scripts/package.ps1` copies `dxcompiler.dll` + `dxil.dll` from the SDK
  next to the exe. The loader already does `LoadLibraryW("dxcompiler.dll")` (exe dir first),
  so the bundled copy is used with **zero code change** — the DXR path works with no SDK.
- **`app/version.rc`** — text-only `VERSIONINFO` (2.0; no `.ico` — an icon would be an asset
  file). **`--credits`** prints the about text (also the package's `CREDITS.txt`).
- **The package** — `dist/backrooms-portable.zip` (~7 MB): exe + DXC pair + README + CREDITS
  + RUN.cmd. No installer (operator decision), no assets.
- **`Invoke-GateM17`** — clean build + ctest + package builds + **clean-env** (unzip to temp,
  **scrub the SDK out of PATH**, `--dxr-pt` renders via the bundled DXC, debug-clean + a
  `--game` smoke) + fresh-unzip VERSIONINFO/credits + M5/M4/M15 golden regression +
  INV-5/inventory. **gate.ps1 M17 exits 0.**
- **Docs** — new [`docs/USER_GUIDE.md`](USER_GUIDE.md) (player-facing) +
  [`docs/DESIGN.md`](DESIGN.md) (design & architecture narrative).

**Full regression sweep (for the `v2.0` tag).** All gates green: M0–M3 (run 1), M4–M16
(run 2). M4 first tripped a **transient ctest test-*discovery* lock** (the `unit_tests.exe`
couldn't be enumerated mid-run, after M3's 11-min soak — likely an AV scan); re-running was
clean (ctest 100%, 68 tests), and the M4 walk-bot determinism check had passed even in the
flaked run. Not a regression — the documented "re-run a transient under-load flake in
isolation" pattern. KEEL (`:7071`) was up, so M11/M12 ran the live Director gates.

**Gotchas / notes.** The inventory checker's allowlist gained `build-release`/`dist` (M17
packaging artifacts, gitignored, same class as `build`). The portable folder's only shipped
non-exe binaries are Microsoft's redistributable DXC DLLs; the three third-party *libraries*
remain Catch2 + stb + miniaudio. The clean-env gate is the real proof of self-containment.

**Polish (`v2.1`).** A **procedural app icon**: `tools/icongen` draws it from code
(Backrooms-yellow wallpaper + a dark doorway + a fluorescent bar), PNG-encodes it into a
`.ico` at **build time** via a CMake custom command, and `version.rc` embeds it — still no
committed asset (`app/backrooms.ico` is gitignored). The exe and window now carry a proper
icon. `gate.ps1 M17` re-runs green with it. Tagged `v2.1`.

**🏁 The build is complete: 18 milestones (M0–M17), every one gate-verified and tagged,
from an empty repo to a portable procedural game. `v1.0` was the visualization; `v2.0` is
the game** (and `v2.1` gave it a face). Post-v2.0 (operator-only / optional): Steam (Steamworks + depot upload, the
account/$100/store/Publish steps); polish (procedural app icon, in-game credits screen,
far-chunk camera-relative rendering).

---

## Session 17 — M16: Settings, persistence, windowing, gamepad  ✅ COMPLETE — 🏷️ `m16-green`

**`gate.ps1 -Milestone M16` exits 0; tagged `m16-green` + pushed. The game remembers and
adapts.** Settings now persist across launches, resolution/fullscreen actually change, and a
controller works. One milestone (M17, packaging) from `v2.0`.

**Done (M16; commit `<wip>` + gate; ADR-042).**
- **`app/config.h`** — a `key=value` config with **pure** `serialize`/`parse`/`sanitize`
  (header-only), so the write→read round-trip is a unit test (`test_config.cpp`, 4 cases).
  Unknown keys ignored, missing keys keep defaults (forward-compatible); `sanitize` clamps a
  hand-edited file. File I/O is a thin inline wrapper. Saved next to the exe (`backrooms.cfg`).
- **`app/gamepad.h`** — **pure** `gamepad_to_input(GamepadState, look_scale) → InputCommand`
  with a radial dead zone (`test_gamepad.cpp`, 4 cases). The live Win32 XInput poll just fills
  `GamepadState`, so a pad drives the **identical deterministic tick path** as keyboard/replay.
  XInput is a Windows **system import lib** (added to the app link line, no vcpkg).
- **`render_d3d12::resize(w,h)`** — waits GPU idle, `ResizeBuffers`, rebuilds RTVs + depth +
  (lazily) the overlay pipeline. Fullscreen is **borderless** (WS_POPUP cover-the-monitor +
  swapchain resize) — no DXGI exclusive mode, so no mode-set glitches.
- **`--game` (M16):** loads/saves the config (resolution, fullscreen, volumes, mouse-sens,
  seed all persist), **F11** toggles fullscreen, **XInput** drives the walk, mouse sensitivity
  comes from settings. **`--config-check`** (write→read→apply at the config resolution) and
  **`--resize-smoke`** (resolutions + borderless-fullscreen toggle) are the headless gate modes.
- **`Invoke-GateM16`** — ctest (`[m16]`) + `--config-check` (round-trips + renders at config
  res, debug-clean) + `--resize-smoke` (6 changes debug-clean) + `--game --config` (persists,
  debug-clean) + M5/M15 golden regression + INV-5/inventory. **gate.ps1 M16 exits 0.**

**Gotchas / notes.** Determinism untouched — config/gamepad/windowing are presentation/input
mapping; a gamepad emits the same `InputCommand` a key would. Borderless (not exclusive)
fullscreen keeps alt-tab clean + avoids mode-set debug-layer noise. `--game` writes
`backrooms.cfg` in the cwd by default → added to `.gitignore`; gate `--game` calls pass an
explicit `--config` under `runs/`. The renderer `resize` drops the overlay texture (sized to the
old resolution) so it rebuilds at the new size on the next present.

**Next: M17** — bundle `dxcompiler.dll`/`dxil.dll` beside the exe (no SDK needed), a release
build config, app icon/version/splash/credits, a portable **`.zip`**, clean-env test → tag `v2.0`.

---

## Session 16 — M15: Menus + game flow  ✅ COMPLETE — 🏷️ `m15-green`

**`gate.ps1 -Milestone M15` exits 0; tagged `m15-green` + pushed. The game has a front
end.** It no longer boots straight into the walk — splash → main menu → play → pause →
settings → quit, drawn on the existing CPU bitmap font (no new dependency/framework).

**Done (M15; commits `<wip>` + gate; ADR-041).**
- **`app/menu.h`** — the game-state machine: a **pure, header-only**
  `menu_step(MenuModel, UiAction) → UiCommand` (no rendering / wall-clock / globals). So the
  whole front end is **unit-testable headlessly** — `tests/unit/test_menu.cpp` (7 cases) drives
  synthetic input through every transition (the keystone exit gate). The test target adds
  `app/src` to its include path for that one header (nothing links the shell executable).
- **`hud.cpp build_menu_overlay`** — draws the current screen (title, item list, selection
  highlight, settings values) on the M8 5×7 font; +8 glyphs (A/B/G/Q/R/W/X/Y) + `fill_rect`.
- **`render_d3d12::present_overlay_windowed`** — blits the menu overlay to the swapchain via a
  fullscreen triangle. Its **own** texture/heap/root-sig/PSO, isolated from the VHS post pass
  (whose `rtvHeap` slot 1 collides with the 2nd back buffer windowed) → headless goldens untouched.
- **App modes:** `--game` (windowed shell: boot → menu → New Game → walk → Esc-pause →
  settings), `--menu-shot --screen <s> [--sel N]` (CPU-only deterministic menu golden),
  `--menu-smoke` (every screen GPU-composited debug-clean across state changes).
- **Goldens:** `goldens/m15/{splash,mainmenu,pause,settings}.png` via `goldgen capture` (ADR-041).
- **`Invoke-GateM15`** — ctest (incl. `[m15]`) + state-transition tests + 4 menu goldens
  bit-match + `--menu-smoke` debug-clean (8 composites) + `--game --seconds 4` debug-clean +
  M5 golden regression + INV-5/inventory. **gate.ps1 M15 exits 0.**

**Gotchas / notes.** Menu state machine is split from its rendering specifically so it's
deterministic + headless-gateable. The windowed overlay present had to be a **separate**
pipeline (not the post pass) to avoid the windowed `rtvHeap` slot-1/back-buffer collision —
the post/readback path is byte-for-byte untouched (M5/M8 goldens unaffected). Menu overlay is
CPU-only + deterministic → goldens need no GPU in CI. Settings (master/SFX/mouse/Director) are
in-memory for M15; **M16 persists them**. Verified frames (`runs/m15/*.png`) shown to the
operator: CRT-green main menu (NEW GAME highlighted, CONTINUE greyed) + settings screen.

**Next: M16** — settings + config persistence (write→read round-trip) + windowing
(fullscreen/resolution) + XInput gamepad → `InputCommand`. Then M17 portable `.zip` → `v2.0`.

---

## Session 15 — M14: Sound on (real-time audio output)  ✅ COMPLETE — 🏷️ `m14-green`

**`gate.ps1 -Milestone M14` exits 0; tagged `m14-green` + pushed. The game has sound.**
M6 built the whole procedural audio stack but deliberately deferred the speaker
backend (headless-first, ADR-028); M14 makes M6's *emulated* "device callback + ring"
real — audible playback during `--play`.

**Done (M14; commit `27a58a1`).**
- **miniaudio** (header-only, `vcpkg.json` + **ADR-040**, Iron Rule 8) — real-time
  playback backend. Runtime-loads WASAPI; **no DLLs to ship** (compiled into
  `audio/src/device.cpp` only; Catch2 + stb + miniaudio remain the only third-party libs).
- **`audio/ring.h`** — lock-free **SPSC float ring** (the real mixer→device hand-off),
  wait-free, allocation-free after `reset()`. Unit-tested: wrap-around integrity +
  **200 k frames across two threads, in-order/uncorrupted**.
- **`audio/device.h/.cpp`** — `AudioDevice`, an opaque PIMPL over a miniaudio
  `ma_device` (default endpoint, or the hardware-free **null backend** for gates).
  `<miniaudio.h>` never escapes `device.cpp`.
- **`AudioEngine::start_device(use_null)`** — the producer now fills the ring; the
  device callback drains it (dry → one underrun + silence). M6's headless `start()` /
  virtual-cursor `run()` is **untouched**. Added master + SFX volume (playback mix).
- **`app --play`** now opens the real WASAPI device (silent no-op if none);
  **`app --audiodev [--null] [--master V] [--sfx V]`** is the headless gate mode.
- **`Invoke-GateM14`** — clean build + ctest (incl. ring tests) + `--audiodev --null`
  (device opens, **0 underruns**, 580 blocks) + real WASAPI soft check (0 underruns) +
  `--render-wav` **bit-identical ×2** + wavcheck spectrum + ADR/vcpkg presence + M6
  soak regression + INV-5/inventory. **gate.ps1 M14 exits 0.**

**Gotchas / notes.** **Determinism is untouched (INV-1):** the offline `--render-wav`
uses `Synth` directly and is byte-identical before/after M14 (gate-checks ×2). The gate
hard-passes on the **null backend** (real-time-paced, hardware-free, CI-safe — the
headless-first doctrine); the real WASAPI path is a *soft* check (a headless host may
have no default endpoint) — on the dev box it opens clean at 0 underruns. miniaudio's TU
compiles warning-free under `/WX` because it's an angle-bracket include (`/external:W0`)
+ a `#pragma warning(push,0)` guard. The file-local data callback needs
`AudioDevice::Impl` → declared **public** (mirrors `render_d3d12/renderer.h`).

**Next: M15** — menus + game-state machine (splash → menu → play → pause → settings),
immediate-mode UI on the existing CPU bitmap-font HUD (no new framework). Then M16
(settings/config persistence + fullscreen/resolution + XInput), M17 (portable .zip →
v2.0).

---

## Session 14 — M13: Playable windowed walk (Phase II begins)  ✅ COMPLETE — 🏷️ `m13-green`

**`gate.ps1 -Milestone M13` exits 0; tagged `m13-green` + pushed.** Phase II (turn the
v1.0 visualization into a packaged game, M13→M17) is underway. **The sim is now genuinely
walkable in a window** — closing the M1 clear-frame gap.

**Done (M13; commits `7643b38` phase a, `c952d46` phase b, `5bdc51d` gate).**
- **`render_chunks_windowed()`** (render_d3d12) — a faithful twin of the M3-gated
  `render_chunks`: same geometry + forward lighting, targeting the swapchain back buffer
  (PRESENT→RT→draw→PRESENT, fence-synced) instead of an offscreen RT. The gated headless
  `render_chunks` is untouched. `init_windowed` now also builds the depth buffer.
- **`app --play`** — the real-time windowed walk: fixed-120 Hz tick accumulator decoupled
  from render, WASD + jump + mouse-look (recentered cursor), 3×3 collision rebuild as you
  cross chunks, Esc/close to exit. Spawns at the proven-open (2,2) cell. `--csv` writes
  per-frame pacing telemetry (frame_ms + residency/mem) via the existing `FrameCsv` contract.
- **`scripts/run.ps1 -Window`** now launches `--play` (was the `--window` clear frame).
- **`Invoke-GateM13`** — clean build + warnings + full ctest + **windowed `--play`
  (debug-clean + frame-pacing p99 < 2× median, best of 2)** + M5/M4 golden regression +
  INV-5 + inventory. **gate.ps1 M13 exits 0.**

**Gotchas / notes.** Frame pacing uses **best-of-2 at the same 2.0× threshold as the M3
walk gate (ADR-021)** — a single run trips ~2.7× right after a clean build + 52 tests
(peak load); in isolation the windowed path paces ~1.3–1.8×. Threshold NOT softened; both
attempts ≥2× would be a real regression. `--play` reads `GetAsyncKeyState`, so a headless
no-focus run drifts (env artifact, not a gate concern — debug-clean is what matters).
Spawn must be above a proven-open cell (reused the (2,2) cell from `--intro`).

**Next: M14** — real-time audio output (miniaudio, the M6-deferred dep; ADR + vcpkg.json;
zero underruns at runtime; offline `--render-wav` stays bit-identical). Then M15 (menus +
game-state machine, immediate-mode UI on the bitmap-font HUD), M16 (settings/config
persistence + fullscreen/resolution + XInput), M17 (portable .zip + clean-env test → v2.0).

---

## Session 13 — M12: Integration, Polish, Acceptance  ✅ COMPLETE — 🏁 `v1.0` (M0–M12 all green)

**`gate.ps1 -Milestone M12` exits 0; the M0–M11 regression sweep is green; tagged
`v1.0` + pushed. THE BUILD IS DONE** — an infinite, deterministic, fully-procedural
Backrooms walking sim (raster + DXR path tracer, procedural audio, 8 h-soak-hardened
streaming world, optional local-LLM Director that never breaks bit-exact replay),
across 13 milestones, every one gate-verified and tagged.

**Done (M12; commits `b2a9322`, `cd8e1c4`).**
- **`app --intro`** — the iconic **noclip into the Backrooms**: stand in a mundane
  room, the floor gives way, free-fall, land in the Level-0 maze. Pure scripted core
  sim (seeded ticks + collision phases) → deterministic (seed 1 ×2 identical; lands at
  y≈0.9 above the proven-open (2,2) cell). The visual fall is the windowed experience.
- **`scripts/run.ps1`** — one command, fresh clone → running exe (default builds + a
  lit smoke render proving the engine end to end; `-Window` launches the windowed app;
  `-Director` adds the KEEL probe).
- **`scripts/soak.ps1 -Director`** — the acceptance soak with the Director ON (full =
  `-Hours 12 -Director`). **`scripts/checks/check_contracts.ps1`** — every boundary has
  a documented contract header (8). Settings = the CLI flag surface; photo mode = the
  existing capture modes (`--shot`/`--dxr-pt`/`--topdown`).
- **`Invoke-GateM12`** (v1.0 exit gates): full ctest + one-command run → running exe +
  noclip intro (deterministic) + **12 h acceptance soak with the Director ON** (short/
  parameterized) + CI doc checks (inventory + contracts) + golden regression + INV-5.
  **gate.ps1 M12 exits 0; M0–M11 sweep green.**

**Gotchas / notes.** The acceptance soak + the Director gates need the KEEL sidecar up
(`C:\keel-sidecar-7071\start.cmd`); without it, `--no-director` runs the full sim
cleanly (INV-6). The literal 12 h run is `soak.ps1 -Hours 12 -Director` (the gate runs
a short version, like the M3/M10 soak shortening). M3's hitch gate stays best-of-2 /
re-run-on-jitter. Post-v1.0 (optional): far-chunk camera-relative rendering, vendoring
a release `keel-serve.exe` into `third_party/keel/`, GBNF over KEEL's HTTP.

---

## Session 12 — M11: The Director (via KEEL sidecar)  ✅ COMPLETE (`m11-green`, all 5 exit gates)

**`gate.ps1 -Milestone M11` exits 0 with all 5 exit gates; M0–M10 regression sweep
green; tagged `m11-green` + pushed. Next: M12 (integration, polish, 12 h acceptance).**
**Decision (ADR-038): the Director routes through the KEEL sidecar's OpenAI HTTP
egress, NOT embedded llama.cpp** — a dependency *removal* (Catch2 + stb stay the only
third-party libs; winhttp is a system lib). KEEL is the operator's sovereign LLM
substrate and Backrooms is its first proof-of-cell.

**Done (phases 11c + 11d — sim wiring, async host, eval, all 5 gates; commits
`ade4802`, `034e0bc`, `991faac`).**
- **Gate 4 (THE sacred one) — replay bit-identical with the model OFFLINE.** Director
  event log (`replay_v1` extension: `DirectorLogHeader` + `DirectorEvent` records).
  `--director-record` walks (reusing `MazeWalker`) with the Director ON (live KEEL),
  folding a combined run hash = per-tick `world_state_hash` ⊕ applied director-event
  bytes, logging each validated directive at its tick; `--director-replay` re-walks
  the SAME run with KEEL never contacted → **identical hash**. Proven from every
  angle: WorldState folds in (seeds differ), the director stream folds in (5-event ≠
  0-event), replay = 0 KEEL calls (structural), unreachable KEEL = graceful no-op.
- **Async `DirectorHost`** (worker thread; non-blocking submit/poll, latest-wins) +
  `--soak --director` (ambient wall-clock pacing) + `--no-director` kill switch (off
  by default → M10 soak byte-unchanged). The Voice (latest caption) + note cache.
- **`--director-eval`** (N scenarios → schema-valid % + p95 latency + samples).
- **Gate 2 reformulated honestly (ADR-039):** the async-isolation INVARIANT is
  unchanged; the "frame-time identical" metric was confounded by the KEEL sidecar's
  **shared-GPU reality** (Qwen-9B inference + renderer on one RTX). Gate 2 = A
  (integration overhead ~0 via dead-url; no new hitches via p99/median) + B (ambient
  median within a stated band of OFF). A recorded, reasoned redefinition — not a
  softening; surfaced to the operator, who ratified A+B / skip C.
- **`Invoke-GateM11`** (5 gates): #1 schema-valid (100% / 100), #2 async isolation
  (deadurl 1.00× / live 1.03× / hitch 1.06× off), #3 p95 ~0.5 s (< 5 s), #4 replay
  determinism (bit-identical), #5 `--no-director` clean. + golden regression + INV-5
  + inventory. **gate.ps1 M11 exits 0; M0–M10 sweep green.**

**Gotchas.** KEEL sidecar must be live at `:7071` (`C:\keel-sidecar-7071\start.cmd`);
use :7071 NOT :7070 (KEEL's own dev endpoint, volatile); do NOT touch `C:\KEEL`. GBNF
constrained decode is not exposed over KEEL's HTTP yet → the post-hoc validator is the
schema guarantee (100% in practice anyway). Director captions are sanitised to printable
ASCII (the HUD font). Sample Voice lines: *"Room 66 is unoccupied. Please do not linger
near the ventilation intake."*

**The wire (verified live).** `POST http://127.0.0.1:7071/v1/chat/completions` with
`{"messages":[{"role":"user","content":"<prompt>"}],"sovereign":true,"kind":
"scaffolding","think":false}` → KEEL forces local Qwen3.5-9B ($0, no cloud egress,
no escalation, single-shot). Directive JSON = `choices[0].message.content`; routing
under `"keel":{tier,cost,route,...}`. **Endpoint is a sidecar at `C:\keel-sidecar-
7071\`** (relaunch: `start.cmd`) — a local-only copy of `keel-serve` reusing the
running llama-server. **Use :7071, NEVER :7070** (KEEL's own dev endpoint, volatile).
**Do not touch `C:\KEEL` (read-only).**

**Done (phase 11a — contract + validator; commit `5ebe664`).** `contracts/
director_v1.h` (WandererSummary, Directive + DirectiveKind + bounds, DirectorEvent).
**Directives are presentation-layer only in M11** — they reach the sim solely as
recorded Event-Log entries at a deterministic `effective_tick` (INV-5) and never
touch the WorldState hash or generation, so INV-1/INV-2 stay provably intact;
replays consume the recorded log with the model offline → bit-identical. `director/
json.{h,cpp}` minimal dependency-free JSON reader (+`escape`). `validate_directive`
(schema + lint: bounded enums/ranges, sanitised printable-ASCII captions; pure/
total). 10 unit tests.

**Done (phase 11b — live KEEL client; commit `17fdd04`).** `director/keel_client.
{h,cpp}` (WinHTTP POST, RAII handles, never throws → graceful no-op on failure).
`render_prompt(summary)` (the directive-schema instruction). `app --director-probe
[--director-url host:port]`. **Live-validated: 5/5 sampled seeds → schema-valid,
on-theme directives** (e.g. `{"type":"sound","intensity":0.4,"detail":"hum of
fluorescent lights flickering in unison"}`; biome_bias with correct numeric biome),
tier local, $0.

---

## Session 11 — M10: Walk-Bot Soak + Long-Haul Hardening  ✅ COMPLETE (`m10-green`, all 3 exit gates)

**`gate.ps1 -Milestone M10` exits 0; M0–M9 regression sweep green; tagged
`m10-green` + pushed. Next: M11 (the Director — local LLM via llama.cpp; NEW deps =
ADRs + a model download step).** This session also completed M9 (DXR path-traced
mode, `m9-green`) end-to-end before M10 — see the Session 10 entry below.

**Done (M10 — soak + hardening; commits `05f79c9`, `ffdcb03`, `c081672`, `5257a4d`).**
- **`app --soak [--seconds S | --ticks N] [--csv f] [--out shots] [--shot-every N]`** —
  deterministic maze walk-bot over the **streaming raster renderer** (shipping path),
  writing the frame CSV (`frame_ms`→FPS, `mem_bytes`→slope), **periodic connectivity
  audits** (`gen::validate_connectivity` of the wanderer's 3×3), periodic screenshots,
  stuck detection. Memory plateaus ~187 MB after ring fill; steady-state spread ~1–2 MB.
- **`tools/contactsheet`** — tiles `shot_*.png` into a grid + mechanical all-black/
  white screen (mean luma); exit 1 on a degenerate frame. Links `br_stb`.
- **`telemetry/crash.*`** (`dbghelp` `MiniDumpWriteDump`, system import lib — no
  vcpkg) — `install_crash_handler(dir)` sets an unhandled-exception filter that writes
  `<dir>/minidump.dmp` + `crash.log` and exits with **`kCrashExitCode`=70**; app
  installs it at startup (all modes), `--crash-test`/`force_crash()` drive the drill.
- **`scripts/soak.ps1`** (real harness, was a stub) — runs the soak with **auto-
  restart-and-log** on a captured crash, analyzes the CSV, tiles via contactsheet.
  **Duration-parameterized**: `soak.ps1 -Hours 8` = full acceptance; gate runs `-Seconds
  30`. `-CrashDrill` runs the forced-crash + restart. **Gotcha:** native exe stderr
  becomes a terminating `NativeCommandError` under `ErrorActionPreference=Stop` even with
  `2>&1`; wrap native calls in a helper that sets `ErrorActionPreference=Continue` (and
  emit gate metrics via `Write-Output`, not `Write-Host`, so a caller can capture them).
- **`Invoke-GateM10`** (3 exit gates): (#1) 30 s soak — 1 %-low FPS ≥ 30 (≈125), steady
  mem spread ≤ 48 MB (≈1.7), zero audit-fail/stuck/debug; (#2) contact-sheet mechanical
  screen (0 black/white) + agent visual review (14 varied lit views, pass); (#3) crash
  drill — minidump + exit 70 + clean restart. ADR-037.

**Gotchas / open.** The M3 hitch gate (p99 < 2× median) is timing-flaky **under heavy
machine load** — it tripped at 2.22× during the back-to-back sweep but passes in
isolation (re-run); it's best-of-2 and not an M10 regression (M10 touches no render/
stream hot path). Walk-bot v2 (corridor-following/doorway-seeking) was deemed
unnecessary — the M4 walk-bot covers the soak (zero stuck over the run); the milestone's
v2 navigation polish can be revisited if a soak ever wedges. `soak.ps1` wipes
`runs\soak` on each run (so the gate's crash-drill step deletes the soak's contact
sheet — regenerate to view).

---

## Session 10 — M9: DXR Path-Traced Mode  ✅ COMPLETE (`m9-green`, all 4 exit gates)

**`gate.ps1 -Milestone M9` exits 0 with all 4 exit gates; M0–M8 regression sweep
green; tagged `m9-green` + pushed. Next: M10 (8 h walk-bot soak + hardening — the
real `soak.ps1`, walk-bot v2, contactsheet, minidump/auto-restart).** Raster stays
the default + fallback (INV-6); the DXR path tracer is enhancement-only.

**Done (phase 4 — interactive PT + streaming, gates #3 + #4 GREEN; commit
`a5b740f`). + phase 5 (regression sweep + tag).** `render_pt_frame(cam, samples,
seed, reset)` accumulates spp across frames; `reset` clears the accumulator on
movement (otherwise the per-sample RNG indices continue → progressive refine).
`render_pt` is now a `reset=true` wrapper (byte-identical → gate-#2 golden
preserved). App: `--dxr-fps` (times 1-spp moving frames → FPS), `--dxr-ghost`
(converge A, render B with/without reset → clean-vs-fresh ~0 vs ghost-vs-fresh
large), `--dxr-walk` (walk-bot K km in PT, rebuilding BLAS/TLAS as chunks stream).
**Gate #3:** 178.5 FPS @ 1440p (≥60) + no-ghost (clean 0, un-reset ghost 31).
**Gate #4:** 1 km PT walk, 13 TLAS rebuilds, 280 frames, debug/DRED-clean.
**Regression sweep M0–M8: all PASS** (M9 changes are additive; raster goldens
byte-unchanged). Tagged `m9-green`.

**Done (phase 3 — path-traced lighting + converged golden, gates #1 + #2 GREEN;
commits `f358f7d`, `91276d0`).** Inline DXR 1.1 **`RayQuery`** (SM 6.5) path tracer:
`build_scene` concatenates resident chunk verts into one `StructuredBuffer` (shadeVb)
and tags each TLAS instance's start vertex offset in **InstanceID**, so the shader
reads per-hit normal/material via `(InstanceID + 3·PrimitiveIndex)`. `render_pt(cam,
samples, seed)` accumulates spp in **RGBA32F** across batched dispatches (≤64
spp/dispatch, under the GPU watchdog) → Reinhard resolve to RGBA + NDC depth.
Lighting = emissive fluorescent grid as area lights (analytic **NEE + shadow rays**
from the `is_fluorescent_cell` formula, no light list), one cosine **diffuse-GI
bounce**, small **ambient floor**, seeded per-(pixel,sample) PCG RNG. `dxc` gained a
target-profile param (default `lib_6_3`; PT uses `lib_6_5`). `app --dxr-pt --pose P
--spp N`. Recursive `render_scene` (gate #1) untouched. **Goldens** `goldens/m9/
pt_pose{1,3,4}.png` (1024 spp, goldgen). **Gate #2:** 1024 spp × 3 poses,
deterministic ×2, mean-abs-diff vs golden < 1.0, luma band, debug-clean. ADR-036.
**Measured: deterministic (bit-identical ×2, single + multi-batch), ~1.06 s/pose,
diff 0.0, debug/DRED clean.**

**Done (phase 2b — cross-renderer depth compare, exit gate #1 GREEN; commit
`15427d3`).** `DxrRenderer::render_scene` writes **NDC depth** (R32_FLOAT UAV at
`u1`) via the *same* hyperbolic `proj_lh(near=0.05, far=500)` mapping as
`render_d3d12`; `readback_depth()` returns it. **Key finding:** the DXR ray basis
(fwd/right/up) already equals raster's `view_lh` exactly — fwd = (sin·cos, sin,
cos·cos), right = cross((0,1,0),fwd), up = cross(fwd,right) — and the screen mapping
matches, so per-pixel rays align with no basis change (the SESSION_LOG worry was
resolved by inspection). `render_d3d12` gained `readback_depth()` (copies the
D32_FLOAT buffer; additive — raster output byte-unchanged, M5/M4 goldens still
bit-match). `app --dxr-depth` renders both, linearizes NDC→eye-space metres,
compares per pixel. `Invoke-GateM9` (gate #1) = clean build + ctest + dxr-probe +
**5-pose depth compare** + golden regression + INV-5 + inventory → **exit 0**.
**Measured (640×360):** every pixel co-foreground, mean depth rel-err ~1e-5, max
~1e-4, **zero mismatches** except 1 silhouette pixel at pose 4; raster+DXR
debug/DRED clean. Gate thresholds clear the measured values ~250×.

**Done (phase 2a — BLAS/TLAS + TraceRay).** `DxrRenderer::build_scene(chunks)`
builds a BLAS per resident chunk (triangles from `ChunkVertex`, world-space) + a
TLAS (identity instances); the scene state object adds a hit group + miss, SBT =
raygen|miss|hitgroup (64 B records), TLAS bound as a root SRV (t0).
`render_scene(camera)` casts primary rays (yaw/pitch/fov ray basis), closest-hit
shades by distance. `app --dxr --pose P`. **Verified: 169-chunk scene, maze traced
with correct depth, debug/DRED clean.** Fix: AS **scratch** buffers must be created
in `COMMON` (D3D12 ignores `UNORDERED_ACCESS` initial state for buffers → 1 warning
per chunk otherwise).

**Remaining for M9 (start at phase 3).**
- **Phase 3 (gate #2):** closest-hit PT (emissive fluorescents as lights, shadow +
  diffuse-GI rays, seeded per-(pixel,sample) RNG), accumulation buffer; 1000+ spp
  at 3 poses; RMSE < threshold (via `soak.ps1`). Shading currently distance-only.
- **Phase 4 (gates #3, #4):** interactive PT (accum reset on move; ≥60 FPS;
  no-ghost histogram-after-teleport), TLAS refit on stream; walk-bot 1 km PT, zero
  debug/DRED.
- **Phase 5:** `Invoke-GateM9` (4 gates + regression) + tag `m9-green`.

**Earlier this session (phases 1a + 1b).** Both hard risks (DXR toolchain + the
DispatchRays machinery) retired and debug-clean.

**Done (phase 1a — toolchain).** `render_dxr/dxc.*` — runtime DXC wrapper (loads
`dxcompiler.dll` via LoadLibrary + SDK scan; compiles HLSL → **signed** SM 6.3
DXIL through `IDxcCompiler3`; `dxil.dll` signs from beside it). `probe_caps()` +
`app --dxr-probe`. **Measured: RTX 4070 Ti SUPER, device5=1, RaytracingTier 1.1,
DXC → signed DXIL, dxr_ready=1.** ADR-035. No vcpkg change; FXC raster untouched.

**Done (phase 1b — dispatch).** `render_dxr::DxrRenderer` (own Device5 + queue +
`ID3D12GraphicsCommandList4`): a raytracing **state object** (DXIL_LIBRARY raygen
+ shader/pipeline config + global root sig with a UAV), an **SBT** (one raygen
record), and **DispatchRays** writing a UV gradient to a UAV → readback. `app
--dxr-test`. **Verified: dispatch works, gradient correct, debug-layer/DRED clean
(0).** The state-object/SBT/dispatch scaffolding phases 2–4 build on is proven.

**Remaining for M9 (start at phase 2).**
- **Phase 2 — AS + depth compare (gate #1):** BLAS per `ResidentChunk` (triangles
  from `ChunkVertex`) + TLAS; raygen now `TraceRay`s primary rays → hit `RayTCurrent`
  → depth; `app --dxr`; compare DXR primary-hit depth vs raster depth within epsilon.
- **Phase 3 (gate #2):** closest-hit PT (emissive fluorescents as lights, shadow +
  diffuse-GI rays, seeded per-(pixel,sample) RNG), accumulation; 1000+ spp at 3
  poses; RMSE < threshold (via `soak.ps1`).
- **Phase 4 (gates #3, #4):** interactive PT (accum reset on move; ≥60 FPS;
  no-ghost histogram-after-teleport), TLAS refit on stream; walk-bot 1 km PT, zero
  debug/DRED.
- **Phase 5:** `Invoke-GateM9` (4 gates + regression) + tag `m9-green`.

**Gotchas.** Command list must be `ID3D12GraphicsCommandList4` (SetPipelineState1
+ DispatchRays). SBT records: shader id is `D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES`
(32), each record aligned to `D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT` (64),
table base to 64. AS buffers need `ALLOW_UNORDERED_ACCESS` + the
`RAYTRACING_ACCELERATION_STRUCTURE` state; build needs a scratch buffer.

**Feasibility (confirmed).**
- **GPU:** NVIDIA RTX 4070 Ti SUPER present → DXR 1.1 (Tier ≥ 1.0) capable.
  (Intel UHD 630 also present; the renderer already picks HIGH_PERFORMANCE.)
- **DXC (the new toolchain piece):** DXR shaders need SM 6.3+ DXIL, which the
  FXC/`D3DCompile` path used everywhere else cannot produce. `dxcompiler.dll` +
  `dxil.dll` (validator/signer — needed so the runtime accepts the DXIL) exist at
  `C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\` (also 26100).
  Headers `dxcapi.h` + `d3d12.h` (with the raytracing structs) are on the SDK
  include path. **NOT in System32/PATH** → the DLLs must be copied next to the
  exe (CMake post-build) or LoadLibrary'd from the SDK path.
- New **system dependency** (dxc) → needs an ADR (no vcpkg entry; like d3dcompiler).

**Architecture decision.** Build `render_dxr` **self-contained** (its own
`ID3D12Device5`, queue, AS/PSO/SBT) rather than literally sharing
`render_d3d12`'s device object — cleaner for headless determinism + isolation;
both renderers consume the same `contracts::ResidentChunk` geometry so the
cross-renderer depth compare is apples-to-apples. (Reconcile render_dxr MODULE.md:
"device/share" → "same adapter-selection + same geometry contract".)

**Plan (5 phases; tasks #40–#44).**
1. **Foundation:** Device5 + `CheckFeatureSupport(OPTIONS5)` RaytracingTier
   (graceful fail if absent); runtime DXC wrapper (`DxcCreateInstance` from
   dxcompiler.dll via dxcapi.h; copy both DLLs to `build/bin`); a trivial DXR
   state object + raygen writing a UAV + SBT + `DispatchRays` + readback —
   debug-layer/DRED clean. Proves the whole toolchain. ADR for the dxc dep.
2. **AS + depth compare (gate #1):** BLAS per resident chunk (triangles from
   `ChunkVertex`), TLAS; raygen primary rays → hit distance → depth; `app --dxr`;
   compare DXR primary-hit depth vs raster depth within epsilon.
3. **PT lighting + converged golden (gate #2):** closest-hit shading, emissive
   fluorescents as lights, shadow + diffuse-GI rays, **seeded per-(pixel,sample)
   RNG** (deterministic); accumulation buffer; 1,000+ spp at 3 fixed poses; RMSE
   vs stored reference < threshold (run via `scripts/soak.ps1`).
4. **Interactive PT + streaming (gates #3, #4):** reduced bounces while moving,
   **accumulation resets on camera movement** (no-ghost: histogram delta after a
   teleport); ≥ 60 FPS walking; TLAS refit on stream events; walk-bot 1 km in PT
   mode, **zero debug-layer/DRED**.
5. **Gate:** `Invoke-GateM9` (4 exit gates + M0–M8 regression + inventory) + ADRs
   + tag `m9-green`.

**Gotchas (anticipated).**
- DXR DXIL must be **signed** (dxil.dll next to dxcompiler.dll) or the device
  rejects the state object. Copy BOTH DLLs to bin.
- Path tracing is stochastic → goldens need a **fixed seed + fixed spp + a
  deterministic accumulation order**; compare by **RMSE** (not bit-exact) to
  absorb GPU float reassociation across vendors.
- Raster stays the **default + fallback** (INV-6) — DXR is enhancement-only; never
  let `core`/`gen`/`stream` depend on it.
- Keep the existing `D3DCompile` (SM 5.0) raster path untouched; dxc is a
  separate compile path used only by `render_dxr`.

---

## Session 9 — M8: VHS Post-Processing + HUD  ✅ gate green (`m8-green`)

**Status: `gate.ps1 -Milestone M8` exits 0.** The picture now has the analog-horror
VHS treatment — film grain, chromatic aberration, lens (barrel) warp, scanline/
interlace flicker, vignette — plus a CRT-green HUD (timestamp, seed, odometer,
chunk, level, FPS). ADR-034.

**Done.**
- **VHS post pass** (`render_d3d12`): a depth-less fullscreen-triangle PSO samples
  the scene RT as an SRV and composites into a second `postRt` (read back instead
  of the scene). PS: barrel-distorted scene sampling, per-channel chromatic
  aberration, screen-space scanlines/interlace + vignette, and **seeded film
  grain** (`hash(px,py,seed + floor(time*60))` → fixed time = fixed grain). Params
  via root 32-bit constants. **Off by default** (`set_post`) → all prior goldens
  byte-unchanged.
- **HUD** (`app/hud.{h,cpp}`): a 5×7 bitmap font + `build_hud_overlay` → a
  transparent RGBA overlay (TIME `HH:MM:SS`, SEED, ODO, CHUNK x,z, LVL, FPS),
  uploaded (`upload_hud_overlay`) and composited **undistorted** (crisp).
  `hud_timestamp(ticks)` echoed to telemetry for the OCR-free gate.
- **app `--post`** on `--shot` (HUD + timestamp) and `--stream` (VHS-only, perf).
- **Gate `Invoke-GateM8`** (4 exit gates): post ON/OFF goldens `goldens/m8/`
  bit-identical ×2 + golden-matched + clean A/B (diff 24.3) + debug-clean (#1, #4
  seeded grain); timestamp 305160 ticks → `00:42:23` via telemetry + pixel golden
  (#2); post pass **0.65 ms @1440p** < 1.5 ms (#3); + M5/M4 render-golden
  regression + inventory.

**Pending / next.** M9 — DXR path-traced mode (BLAS/TLAS per chunk, temporal
accumulation, emissive fluorescents as real lights; raster stays default).

**Gotchas.**
- Post is OFF by default — never let it leak into the existing render goldens
  (verified post-off bit-identical to the M5 golden).
- Film grain MUST be seeded + time-quantised (`floor(time*60)`) or goldens flake;
  the HUD is rasterised on the CPU (deterministic) and composited undistorted.
- The post-pass perf measure runs VHS-only (no per-frame HUD upload) so it times
  the shader, not a 14 MB texture upload; ~0.65 ms vs the 1.5 ms budget.
- The HUD bitmap font only defines the glyphs actually used (digits, `:` `,` `.`
  `-`, and the label letters) — extend `glyph_for` if new HUD text needs more.

---

## Session 8 — M7: Biomes, Set Pieces, Verticality  ✅ gate green (`m7-green`)

**Status: `gate.ps1 -Milestone M7` exits 0.** The world now has *character*: five
biomes in contiguous regions (classic yellow, cubicle farm, pipe corridors,
parking garage with columns, poolrooms), pillar set pieces, and a stairwell that
descends to a dimmer **level −1**. All 4 exit gates pass; ADRs 031–033.

**Done (phase 1).** The **biome field** — `gen/biome.{h,cpp}`: a pure,
low-frequency `biome_at(seed, level, cx, cz)` over a coarse **K=3** chunk lattice
with a weighted CDF → contiguous regions with designed proportions. 5 biomes:
ClassicYellow 44 %, CubicleFarm 22 %, PipeCorridors 16 %, ParkingGarage 12 %,
Poolrooms 6 %. `BiomeParams` (carve ratio, pillar density, floor tint, wall
darken). `test_biome`: determinism, low-frequency contiguity, and **distribution
over 102,400 chunks within ±2 %** (M7 exit gate #2 — green).

**Done (phase 2a).** Biomes wired into generation: `generate_layout_carve(seed,
key, carve_ratio)` (biome openness knob; connectivity holds for any ratio),
`generate_layout` derives it from `biome_at`; `GenerateChunk` applies the biome
floor tint + wall darken. **Edge-doorway protocol untouched** → cross-biome seams
connect (INV-3); seam-crack / regen / doorway-agreement tests stay green.
`test_biome`: **every biome's layout connected over 10k chunks** (exit gate #1
connectivity — 5×10k). M4 top-down + M5 lit-shot goldens **re-captured via
goldgen** (ADR-031; determinism + match verified, diff 0).

**Done (phase 2b).** **Pillars** — `GenerateChunk` adds 0.5 m square full-height
collidable columns at cell centres with prob `pillar_density` (consumed from the
chunk RNG only when the biome calls for it → pillar-free biomes stay
bit-identical). `ValidateChunkGeometry` extended to accept square pillars (thin
both axes ≤ 1 m) vs walls (thin one axis) — ADR-032. `test_biome`: **per-biome
geometry valid, 2000 chunks each** (incl pillars). Walk-bot 1 km × 5 seeds, 0
stuck (pillars don't wedge). `--biomeat` app mode → per-biome lit goldens
`goldens/m7/biome_<name>.png` (classic=seed1, cubicle=4, pipe=2, garage=11,
pool=25; parking garage shows columns). M5 seed-42 shots re-captured (pillar
biome).

**Done (phase 3 — verticality).** `contracts::level_base_y(level)=level*4 m`
(level 0 → Y=0 unchanged; level −1 → Y=−4, dimmer). `GenerateChunk` offsets all
geometry by it; `ValidateChunkGeometry` floor check is level-relative.
`gen::build_stairwell` emits descending step boxes (set piece). `app --descend`
walks the wanderer down via gravity/collision — measured **4.019 m drop (one
level)** to level −1, hash-reproducible, landing chunk connected + valid (ADR-033).
`test_biome`: 4,000 level −1 chunks connected + geometry-valid (exit gate #3).

**Done (phase 4 — gate).** `Invoke-GateM7`: clean build + no-warn + ctest (biome
distribution ±2 %, per-biome 10k connectivity, per-biome geometry incl pillars,
level −1 cross-level + full M0–M6 regression) + INV-5 + **(#3)** `--descend`
deterministic ×2 / level −1 reached / sublevel connected + **(#4)** 5 per-biome
goldens bit-identical ×2 + golden-matched + debug-clean + M4/M5 render-golden
regression + inventory. **PASSED.**

**Pending / next.** M8 — VHS post-processing + HUD (film grain, chromatic
aberration, scanlines, timestamp overlay, vignette; HUD odometer/seed/coords).

**Gotchas.**
- Biomes must never touch the edge-doorway protocol (`door_index`) — only internal
  layout/decoration — or cross-biome seams seal (INV-3).
- Generation changes ripple into M4/M5 goldens → re-capture via `goldgen` + ADR in
  the SAME commit (done twice in M7: tint, then pillars).
- Far-chunk float precision (ADR-022) false-fails the geometry validator past
  ~1M m; gen geometry tests must use precision-safe coords (connectivity tests can
  range freely — cell topology is coord-independent).
- Level Y mapping is `level*4 m`; level 0 = Y 0 (so level-0 output is unchanged).
  Capsule collision descends stairs via gravity (step-down only).
- **Phase 3 — verticality.** Level −1 generation (`ChunkKey.level=-1`, Y-offset,
  altered params) + a **stairwell set piece** (descending step boxes) placed
  deterministically/rare, connecting level 0↔−1. A **scripted-descent replay**
  (app mode): wanderer walks down, Y drops a level, lands in a level −1 chunk;
  assert cross-level connectivity + determinism hash reproduces (exit gate #3).
  Note: capsule collision already steps DOWN via gravity; no step-up needed.
- **Phase 4 — gate.** `Invoke-GateM7`: 4 exit gates + regression M0–M6 + ADRs +
  SESSION_LOG + tag `m7-green`.

**Gotchas.**
- Biomes must NOT touch the edge-doorway protocol (`door_index`) — only internal
  layout/decoration — or cross-biome seams seal (INV-3 fail).
- Phase 2 changes `GenerateChunk` → M4/M5 goldens change. Re-capture via `goldgen`
  + ADR in the SAME commit; never hand-edit goldens.
- `biome_params` is currently unused (built clean under /WX as external linkage);
  it gets consumed in phase 2.
- Distribution gate uses seed 1234 (K=3 gives ~4σ margin for the 44 % biome).

---

## Session 7 — M6: Procedural Audio  ✅ gate green (`m6-green`)

**Status: `gate.ps1 -Milestone M6` exits 0.** The backrooms now *sounds* like the
backrooms: a 60 Hz fluorescent hum, an HVAC drone bed, footsteps timed to the
walk, and reverb that opens up in larger rooms — all procedural, all deterministic,
all verified headlessly (no speakers). ADRs 028–030. **No new dependency**
(miniaudio deferred to real-time playback, headless-first).

**Done.**
- **Contract** `contracts/audio_events_v1.h` (core→audio): `AudioListener`,
  `FootstepEvent`, `kAudioSampleRate=48000`, `kAudioChannels=2`, `kStrideLength`.
- **core** (pure, additive, no hash/replay change): `footstep_count(odometer)` =
  floor(odometer/stride); `audio_listener(state)`. Footsteps derive from the
  already-hashed odometer → reproduce from a replay (INV-1); `core` stays
  audio-free (returns only contract types, INV-5).
- **audio** module: `Synth` (deterministic 60 Hz hum + harmonics, HVAC noise bed,
  footstep transients, Freeverb reverb sized by RT60), `room_probe` (16-ray
  mean-free-path → reverb seconds), header-only `wav.h` (PCM16), `AudioEngine`
  (headless real-time mixer thread, prebuffered producer ~170 ms headroom, fed
  lock-free).
- **app**: `--render-wav` (offline: maze walk → 400 frames/tick → PCM16 WAV +
  footstep log, bit-identical x2), `--footsteps` (independent reference log),
  `--audiosoak [--audio]` (real-time mixer soak; mean tick time + underruns).
- **tools/wavcheck**: WAV reader + self-contained radix-2 FFT; `spectrum` +
  `assert` (60 Hz fundamental + 120/180 Hz harmonics over noise floor; RMS
  silence check).
- **Gate `Invoke-GateM6`** (`-AudioSoakSeconds`, default 60): ctest (synth
  determinism, WAV round-trip, 60 Hz hum, room-probe, footstep floor + full
  regression) · INV-5 · offline WAV **deterministic x2** + `wavcheck assert` ·
  **footstep 1:1** (audiolog == replay reference) · **soak: 0 underruns** +
  audio-on tick time within 1.5× of off · M5/M4 render-golden regression ·
  inventory. Measured: 60 Hz at ~12000× floor, 64/64 footsteps aligned, 0
  underruns over 60 s (and a 10-min soak), tick-time delta ~0.4%.

**Pending / next.** M7 — Biomes, set pieces, verticality (biome field over chunk
space; rare set pieces; level −1 descent).

**Gotchas.**
- All sound-affecting randomness lives in `core`/`Synth` seeded state — never
  wall-clock — so the offline WAV is bit-identical across runs/processes.
  `AudioEngine` is the **only** place wall-clock is allowed (real-time pacing).
- The audio "golden" is the **WAV spectrum** (wavcheck FFT bands), not a committed
  byte-file — audio is per-toolchain like renders are per-GPU. The gate checks
  determinism-x2 + spectral bands instead of a stored WAV.
- Underruns must be measured against a **prebuffered** ring (headroom), not a
  zero-headroom deadline — the first engine model false-failed on Windows sleep
  jitter (110 underruns); the headroom model gives 0. Sim throughput off vs on is
  the proof the audio thread doesn't block the tick loop.
- `sr/120 = 400` is an exact integer (48000/120) → one tick maps to exactly 400
  audio frames, no resampling.

---

## Session 6 — M5: Procedural Materials + Raster Lighting v1  ✅ gate green (`m5-green`)

**Status: `gate.ps1 -Milestone M5` exits 0.** Backrooms now *looks* like the
backrooms: yellow wallpaper, damp carpet, dark ceiling tiles, glowing fluorescent
panels, even hazy fluorescent lighting. ADRs 025–027.

**Done.**
- **Procedural textures** — `render_d3d12/texgen.*` (D3D12-free, unit-tested):
  5 materials (Wallpaper/Carpet/CeilingTile/Fluorescent/Baseboard), deterministic
  per (kind,seed), + `texture_hash`.
- **Chunk geometry** — `ChunkVertex` gains uv + material (48B stride);
  `GenerateChunk` assigns materials (floor=Carpet, walls=Wallpaper) and emits a
  **ceiling** grid with the **fluorescent-tile pattern** (`is_fluorescent_cell` /
  `fluorescent_light_pos`, shared verbatim by gen + renderer). Ceiling render-only
  (not collidable). `ChunkContentHash` covers uv+material; M3 regen/seam re-green.
- **GPU textured + lit render** — `render_chunks(.., tick, ..)` uses a **lit
  pipeline**: Texture2DArray (5 slices, fenced upload + `CopyTextureRegion`),
  shader-visible SRV heap + static sampler + root descriptor table; samples by
  (material, uv) with a per-chunk hue tint.
- **Forward fluorescent lighting** — renderer gathers ceiling-grid lights within
  R=10 cells of the camera into a CBV (b1, ≤64), each scaled by
  **`core::light_flicker(seed, light_id, tick)`** (pure, `/fp:strict`, ~1/8 lights
  flicker — lives in `core` so replays reproduce lighting). Shader: ambient 0.22 +
  Σ Lambert×atten, then a **highlight knee** (compress >1.0 by 0.18×) so stacked
  lights keep the wallpaper hue instead of clipping to white. Intensity 1.0.
- **`--shot` mode** — deterministic fixed-pose lit render (5 canonical poses) at a
  fixed flicker tick; prints a luminance histogram. Bit-exact per (seed,pose,tick).
- **Gate `Invoke-GateM5`** — ctest (texgen determinism + full regression) · INV-5 ·
  **5 poses × 3 seeds (1,7,42) goldens** `goldens/m5/` (640×360, goldgen-captured):
  bit-identical ×2, golden-matched, **luminance band** (mean∈[50,220],
  frac_black≤0.35, frac_white≤0.20), debug-clean · **≥120 FPS @1440p** best-of-2
  (measured **~179 FPS**) · **regression**: M1/M2/M4 render goldens still bit-match.

**Pending / next.** M6 — Procedural Audio (room-probe reverb, fluorescent buzz).

**Gotchas.**
- Flicker MUST stay in `core` (replay reproducibility). Renderer only *reads* it.
- The lit shot is bit-identical across runs despite multithreaded streaming —
  co-planar same-material geometry is draw-order-invariant (verified ×3).
- Light intensity 1.0 + knee 0.18 + ambient 0.22 are the tuned look; raising
  intensity blows walls to pure white (lost the yellow). See ADR-026.
- `--shot` poses sit at the proven-open spawn cell (16,16) and vary only
  orientation, so no pose embeds the camera in a wall on any seed.

---

## Session 5 — M4: Level-0 Generator — Rooms, Doorways, Connectivity  ✅ gate green (`m4-green`)

**Done.**
- **gen maze:** `gen/layout.h/.cpp` — G=8 cell grid, **spanning-tree** maze
  (recursive backtracker, provably connected) + ~25% extra carves + 4 edge
  doorways from a **shared-edge hash** (neighbours agree: a vertical edge cx-1|cx
  keys on cx). `validate_connectivity` (flood-fill). `chunk.cpp` rewritten:
  `GenerateChunk` → floor + wall geometry (render verts + collision `BoxInstance`s);
  `ValidateChunkGeometry` (no degenerate/floating/fat/stacked walls).
  Shared `contracts/geometry_v1.h` (`BoxInstance`).
- **collision:** app gathers the 3×3 chunk walls around the wanderer (regen on
  chunk crossing, deterministic) + ground floor → `core::tick` (3-arg). `core`
  stays gen-free.
- **walk-bot v1** (`--walkbot`): seeded wander + escape-on-block sweep; stuck =
  position bounding-box extent ~0 over 10 s (motionless), not net displacement.
- **top-down** (`--topdown`): `render_topdown` ortho render of a 3×3 block.
- **Gate `Invoke-GateM4`:** ctest (incl **10,000-chunk connectivity** zero-sealed
  + **10,000-chunk geometry** validators, doorway agreement); INV-5 grep;
  **walk-bot 1 km × 5 seeds, 0 stuck, deterministic**; **top-down golden** per
  seed (1,7) bit-identical ×3, zero debug. M0–M3 regression green (M3 with maze
  geometry: p99/median 1.28×, memory flat). ADR-022/023/024.
- Goldens `goldens/m4/topdown_seed{1,7}.png`.

**Verified numbers.** 10k chunks each connected + geometry-valid; walk-bot seed 1
→ 1000 m in 38,738 ticks, hash `a1cfc90ef154da01` (reproducible).

**Pending.** M5 — procedural materials + raster lighting v1: startup-generated
textures (yellow wallpaper, carpet, ceiling tiles, emissive fluorescents),
clustered/forward fluorescent grid lighting + seeded deterministic flicker (RNG
in sim core); luminance-histogram gate; ≥120 FPS @1440p.

**Gotchas / notes for the next session.**
- **Catch2 test names must be ASCII** — an em-dash (—) in a TEST_CASE name made
  `catch_discover_tests` register a name that ctest's re-invocation couldn't
  match (Unicode arg round-trip), so the test "failed" only under ctest, not by
  tag. Burned ~20 min; keep names ASCII.
- Geometry is **world-coordinate**; far-chunk (>~16M m) float noise is real —
  the geometry validator thresholds sit between 0.3 m walls and 4 m cells to
  tolerate it. Camera-relative rendering still deferred.
- Collision is per-chunk AABB walls gathered by the **app** (not core) — keeps
  the DAG clean. The walk-bot regenerates the 3×3 neighbourhood synchronously
  (no streaming dependency) for determinism.
- M4 raised chunk vert count (~3000) → renderer chunk VB pool capacity bumped to
  6144 verts/slot; M3 median frame rose to ~4.4 ms (still p99/median ≈ 1.3×).

---

## Session 4 — M3: Infinite Chunk Streaming, Placeholder Geometry  ✅ gate green (`m3-green`)

**Done.**
- **gen:** `GenerateChunk(seed, ChunkKey)` (chunk_gen_v1, pure/total INV-2) —
  world-coord grid floor (per-chunk tint) + interior posts; `ChunkContentHash`.
  Seam-correct by construction. Tests: 1000-chunk regen bit-identical, adjacent
  seams match exactly.
- **stream:** `StreamManager` — `(2r+1)^2` ring around a moving center, background
  **worker-thread pool** generates missing chunks, main thread collects + evicts.
  Bounded residency (INV-4), decoupled from the sim (INV-1). Tests: ring fill +
  recenter stays bounded.
- **telemetry:** `FrameCsv` (telemetry_v1) — per-frame CSV the gates parse.
- **renderer:** `render_chunks` — pos/nrm/color pipeline, **persistently-mapped
  vertex-buffer pool** (allocation-free stream-in) + upload budget; frees evicted
  slots. **Fixed an upload hitch** (per-chunk CreateCommittedResource → pool):
  p99/median dropped from ~3x to **1.2x @1280×720**.
- **core:** `open_ground()` + a collision-parameterized `tick` overload so the
  streaming walk traverses open ground without `core` depending on `gen`/`stream`.
- **app `--stream`:** marching walk on open ground, moves the streaming center,
  renders resident chunks headless, logs frame CSV; untimed warmup; `--seconds`
  soak. (M1/M2 modes intact.)
- **Gate `Invoke-GateM3`:** clean build (0 warn); ctest (24, incl regen/seam/ring);
  INV-5 grep; **hitch gate** — walk 125 chunks @1280×720, p99 frame < 2× median;
  **memory soak** (default 600 s) private-bytes slope ~0; inventory.
- ADR-019 (streaming arch), ADR-020 (VB pool + warmup), ADR-021 (gate metrics);
  reconciled into ARCHITECTURE §8 + 5 MODULE.md + contracts/README.

**Verified numbers.** 1500-frame walk: 169 resident, +0.9 MB over the walk,
p99/median 1.18× @1280×720, debug-clean. 60 s soak: 42,424 frames, +1.26 MB.

**Pending.** M4 — Level-0 generator: real room/doorway layout per chunk,
edge-constrained doorways (`hash(seed, sharedEdge)`), `/gen` connectivity +
geometry validators (flood-fill, no sealed boxes), walk-bot v1 with stuck
detection. Replaces the placeholder grid; the wanderer collides with real
generated walls (collision will read streamed/queried chunk geometry).

**Gotchas / notes for the next session.**
- Chunk geometry is **world-coordinate**; fine to ~16M m, then float precision
  degrades (camera-relative rendering deferred). M4 keeps world coords.
- The chunk VB **pool** is allocation-free after warmup — keep stream-in a
  memcpy; don't reintroduce per-chunk `CreateCommittedResource`.
- Hitch gate is **p99 < 2× median** (NFR §9), tested at **2560×1440** (target
  res; jitter-resilient) with **best-of-2** retry and a 1 ms `timeBeginPeriod`
  timer. An earlier 1280×720 single-run variant flaked post-build (2.35×); see
  ADR-021. `-StreamSoakSeconds` parameterizes the soak (600 s for green; pass a
  smaller value for quick regression sweeps).
- Streaming is decoupled from the sim — worker-thread timing never affects the
  WorldState hash; M2 cross-process determinism still holds.

---

## Session 3 — M2: Sim Core — Camera, Input, Collision, Replay  ✅ gate green (`m2-green`)

**Done.**
- **Contracts:** `contracts/world_view_v1.h` (`CameraPose`, `BoxInstance`,
  `WorldView`) + `contracts/replay_v1.h` (`InputCommand`, `ReplayHeader`,
  magic/version), shared via a header-only `contracts` INTERFACE target.
- **`core` sim:** `math.h`/`aabb.h` (Vec3 + overlap), `world.h/.cpp` —
  `WorldState` (wanderer + owned `Pcg64` + tick + odometer), fixed **120 Hz tick**,
  first-person walk camera, **capsule-vs-AABB collision** (AABB proxy, per-axis
  swept + substepped → no penetration at any speed, sliding preserves tangential
  velocity, no floor tunneling), gravity/jump, hardcoded **test room** (single
  source of truth for sim + render), `world_state_hash`, `wanderer_camera`.
  `replay.h/.cpp` — record/playback of input streams (replay_v1). Zero graphics
  includes (INV-5 grep gate).
- **Renderer:** `render_world_view` (headless) — depth buffer, root-constant MVP,
  runtime-compiled (D3DCompile) PSO + HLSL, draws the lit, depth-tested test room
  from a `WorldView`. row-major LH view/proj math.
- **app:** `--scene` (room → PNG from a fixed pose), `--sim --ticks N
  --seed S --record/--replay f --hashlog f` (drive sim, per-tick hash log). M1
  `--headless`/`--window` clear paths intact.
- **Unit tests:** collision (3 gates), per-tick hash determinism, replay
  round-trip + reproduction + bad-header rejection. (19 ctest cases, all green.)
- **Gate `Invoke-GateM2`:** clean build (0 warnings); full ctest; INV-5 grep;
  **cross-process replay** (record then 2 replays → bit-identical 3000-line
  per-tick hash logs); **room golden** bit-identical ×3 + matches committed
  golden, zero D3D12 debug-layer msgs. `gate.ps1 -Milestone M2` exits 0.
- Golden `goldens/m2/room_640x360.png` (hash `38350c25c2ae2f7d`). ADR-016
  (collision model), ADR-017 (contracts), ADR-018 (golden); reconciled into
  ARCHITECTURE.md §8 + MODULE.md files. **M0 + M1 regression sweep green.**

**Verified numbers.** Seed 777 / 3000 ticks → final hash `0e6105f7c33e525b`,
74.9 m walked, identical across record + 2 replays + 2 separate processes.

**Pending.** M3 — infinite chunk streaming: `GenerateChunk(seed, cx, cz)` pure
function, load/unload ring around the wanderer, background-thread generation +
main-thread GPU upload, placeholder numbered-grid geometry, frame-time telemetry
CSV. (Replaces the single hardcoded room with streamed chunks.)

**Gotchas / notes for the next session.**
- `contracts::` is `br::contracts`; in non-`br::core` TUs use a namespace alias
  (`namespace contracts = br::contracts;`) — a bare `contracts::` won't resolve.
- Collision is an **AABB proxy** for the capsule (ADR-016) — correct for the
  axis-aligned world; square corners, not rounded. Substep cap is 256 @ 0.05 m.
- The room golden depends on the camera pose, geometry, shading, and projection;
  changing any is a `goldgen` update + ADR (INV-8).
- `--scene` is headless-only; the determinism gates run the same binary/GPU.

---

## Session 2 — M1: Window, D3D12 Device, Headless Mode  ✅ gate green (`m1-green`)

**Done.**
- `render_d3d12/renderer.h` + `renderer.cpp`: opaque-PIMPL `Renderer`. DXGI
  factory, adapter selection (prefers the RTX 4070 Ti SUPER via
  `EnumAdapterByGpuPreference`, WARP fallback), D3D12 device, **debug layer +
  InfoQueue + DRED** (auto-breadcrumbs + page-fault), command queue/allocator/
  list, fence sync. Headless: offscreen R8G8B8A8 RT (optimized clear value
  matches the issued clear), copy → readback buffer with `GetCopyableFootprints`
  row-pitch handling → tight CPU RGBA. Windowed: flip-discard swapchain, present.
  No D3D12/DXGI/`<windows.h>` leaks through the header (INV-5 holds).
- `app`: CLI `--headless/--window`, `--frames N | --seconds S`, `--out PNG`,
  `--width/--height`, `--version`. Creates the Win32 window (no focus-steal),
  writes PNG via the shared `br_stb`, reports `debug_error_count` + memory.
- Gate `Invoke-GateM1`: clean build (zero warnings); ctest regression; **frame-0
  PNG bit-identical across 3 runs**; matches committed golden; **zero D3D12
  debug-layer messages** (headless + 10 s windowed run); **60 s memory soak**
  (372,750 frames, +1.6 MB private bytes → flat, no fence timeouts). Plus the
  standing INV-5 + inventory checks. `gate.ps1 -Milestone M1` exits 0; M0
  regression sweep still green.
- Golden `goldens/m1/frame0_320x180.png` (clear RGBA 46,43,33,255; hash
  `65e8578815ec303c`) via `goldgen capture`. ADR-014 (private-bytes soak metric)
  + ADR-015 (golden), reconciled into ARCHITECTURE.md §8.

**Pending.** M2 — `/core` standalone lib: fixed 120 Hz tick, seeded RNG already
present, first-person walk camera, capsule-vs-AABB collision + sliding, **input
replay** (record/playback), per-tick WorldState hash. The replay system is the
enabler for every later automated movement test.

**Open questions.** None blocking. PT/DXR (M9) will reuse this device; the
renderer is structured to add a second (DXR) path without touching the sim.

**Gotchas / notes for the next session.**
- The active PostToolUse hook rebuilds+tests after every Edit/Write; during
  multi-file features intermediate states fail it harmlessly — push through, the
  final state is green. (Real verification is the explicit `build.ps1`/gate runs.)
- PowerShell **StrictMode** is on in `common.ps1`: `.Count` on a scalar throws —
  wrap pipeline results in `@(...)` before `.Count` (bit us once in the gate).
- D3D12 readback **must** honor the 256-byte row-pitch alignment
  (`GetCopyableFootprints`); the renderer copies row-by-row into tight RGBA.
- Windowed gate run opens a 10 s window (`SW_SHOWNOACTIVATE`, no focus steal).
- Clear color is fixed/deterministic; changing it = new golden + ADR (INV-8).

---

## Session 1 — M0: Scaffold + Verification Harness  ✅ gate green (`m0-green`)

**Done.**
- CMake/Ninja/vcpkg skeleton (`CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`)
  matching the full 10-module inventory with correct dependency arrows; static
  `x64-windows-static` triplet, `/W4 /WX /permissive-`, `/fp:strict` on `core`+`gen`.
- `core`: real **PCG64** (XSL-RR 128/64) seeded RNG — the determinism oracle
  (INV-1). Portable 128-bit math, locked output-vector regression test.
- Stub libs for `gen, stream, telemetry, audio, render_d3d12, render_dxr,
  director` + `app` console exe (links the whole DAG). `MODULE.md` for all 10.
- Tools: `hashdiff` (image hash + mean-abs-diff, stb) and `goldgen`
  (deterministic synth via core RNG + golden capture; sole `/goldens` writer).
- Catch2 under CTest: seed/determinism/statistical tests + smoke link-check;
  `gate_canary` (deliberately failing, DISABLED) for the test-the-gate proof.
- Scripts: `lib/common.ps1` (VS dev-env import + Ninja-on-PATH + vcpkg discovery),
  `build.ps1`, `quickcheck.ps1` (exit 2 on fail), `precommit.ps1`,
  `install-hooks.ps1`, `gate.ps1` (M0 dispatch), `soak.ps1` (stub), and
  invariant checks `check_core_isolation.ps1` (INV-5) + `check_inventory.ps1`.
- Activated `.claude/settings.json` (PostToolUse → quickcheck). git pre-commit
  hook installed. ADR-010..013 recorded + reconciled into ARCHITECTURE.md §8.
- **`scripts/gate.ps1 -Milestone M0` exits 0.** Clean build, 10/10 tests,
  hashdiff round-trip, canary-nonzero, hook present, INV-5, inventory — all green.

**Pending.** M1 — Win32 window, D3D12 device/swapchain, debug layer + DRED,
`--headless --frames N --out` offscreen PNG dump, frame-0 golden.

**Open questions.** Remote backup: no `origin` was configured in the starter
(`<your-remote-url>` placeholder). Tagged `m0-green` locally; push deferred until
a remote exists (see below).

**Gotchas / notes for the next session.**
- Scripts run under **Windows PowerShell 5.1** (`powershell.exe`), not pwsh 7 —
  no ternary/`??`/`&&` in `scripts/*.ps1`.
- Ninja is **not** globally installed; `common.ps1` prepends the VS-bundled Ninja
  to PATH. The MSVC env is imported via `vcvars64.bat` inside `Enter-VsDevEnv`.
- vcpkg lives at `C:\vcpkg` (baseline pinned in `vcpkg.json`); a fresh clone
  self-bootstraps into `extern/vcpkg`.
- `files/` and `*.zip` (the redundant starter archives) are gitignored.
- The PostToolUse hook now builds+tests after every Edit/Write; expect a few
  seconds per edit. Intermediate multi-file states may transiently fail it.

---

## Session 0 — (starter created)
- Done: canon documents (ARCHITECTURE.md, MILESTONES.md), CLAUDE.md, kickoff guide, hooks template.
- Pending: M0 — scaffold + verification harness.
- Gotchas: activate .claude/settings.json only after scripts/quickcheck.ps1 exists (see KICKOFF_PROMPT.md).
