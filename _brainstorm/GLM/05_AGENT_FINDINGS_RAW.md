# 05 — Raw Agent Findings (the grounding for 03 and 04)

> This file holds the lightly-edited output of the 7 fan-out Explore subagents used to ground
> the Shoggoth brainstorm (03) and the packaging investigation (04). Kept verbatim-ish so the
> file:line citations in those docs trace back to source.
>
> All citations against repo state at HEAD = 90410c3 (ADR-076).

---

# A. Shoggoth system map (Shoggoth body + brain)

## Files in scope

All shoggoth headers are header-only, in `namespace br::app`, in `C:\backrooms\app\src\`:
- `shoggoth.h` — body + navigator + state machine + determinism hash
- `shoggoth_body.h` — procedural tentacle mesh
- `shoggoth_brain.h` — summary, prompt, intent validator, event log
- `shoggoth_brain_host.h` — the LIVE async KEEL brain
- `shoggoth_vision.h` — POV camera + vision prompt (PURE, no host)
- `shoggoth_hearing.h` — transcript cleaner + hearing prompt (PURE, no host)

**No `shoggoth_vision_host.h` and no `shoggoth_hearing_host.h`** — confirmed by glob/grep. Vision
and hearing are record-only; no live async host. The only live host is the text brain.

## 1. The Shoggoth struct — `shoggoth.h:37-47`

```
struct Shoggoth {
    br::core::Vec3 pos{0,0,0};
    int32_t level = 0;          // floor it patrols (derived from pos.y; never crosses a seam)
    float yaw = 0.0f;           // facing, radians
    float writhe = 0.0f;        // organic body phase (drives mesh + ooze)
    ShoggothState state = Lurk;
    uint32_t state_ticks = 0;
    int64_t wander_gi=0, wander_gj=0;  // current lurk goal cell
    ShoggothIntent intent;      // set by the KEEL brain (M21)
    br::core::Pcg64 rng{0x5170990000000001ull};  // lurk wander repick only
};
```
- `pos` — only x and z ever move; `pos.y` never changed by `shoggoth_step` (per-floor, M29).
- `level` — derived: `contracts::level_from_y(pos.y)`.
- `yaw` — `atan2(ddx,ddz)` toward next cell centre (`shoggoth.h:211`); feeds POV camera + audio.
- `writhe` — monotonic: `writhe += dt * (2.5 + speed)` (`shoggoth.h:218`).
- `rng` — Pcg64 seeded `0x5170990000000001`. **Lurk-wander repick only.** Deterministic.

Lives **OUTSIDE WorldState** by design (`shoggoth.h:5-6`) — existing replay/walk-bot/Director
hashes untouched.

State enum (`shoggoth.h:25`): `ShoggothState { Lurk=0, Hunt=1, Chase=2, Retreat=3 }`
Action enum (`shoggoth.h:30`): `ShoggothAction { Hunt=0, Stalk=1, Lurk=2, Flank=3, Flee=4 }`

## 2. ShoggothIntent — `shoggoth.h:32-35`

```
struct ShoggothIntent {
    ShoggothAction action = Hunt;   // default = the M20 behavior
    float aggression = 0.5f;        // 0..1, scales speed
};
```
Flow into `shoggoth_step` (`shoggoth.h:134`): state-machine picks goal cell (`:167-179`), then
intent overrides/modulates (`:183-196`):
- `Hunt` — no change (default).
- `Stalk` — goal = wanderer cell, speed × 0.55 (`:216`).
- `Lurk` — goal = the random wander cell.
- `Flank` — goal = wanderer cell rotated 90° (`:188-189`). **Half-built** — integer `/2` of a
  1-cell delta rounds to 0 → Flank ≈ Hunt at single-cell range.
- `Flee` — goal = 4 cells directly away (`:192-194`).
Aggression scales speed: `speed *= 0.7f + 0.6f * aggression` (`:217`). At 0.5 = ×1.0 (M20 default).

The intent is the **only** thing the KEEL brain can touch. Never couples to position/state directly.

## 3. ShoggothSummary — `shoggoth_brain.h:24-41`

```
struct ShoggothSummary {
    int64_t sgi, sgj;   // shoggoth cell
    int64_t wgi, wgj;   // wanderer cell
    float distance_m;    // 2D distance (x,z)
    int state;           // ShoggothState as int
    uint64_t tick;
};
```
**All** the brain receives today: cell coords of both parties, 2D distance, locomotion state,
tick. No yaw, writhe, velocity, vision pixels, audio. `distance_m` is straight-line even when
prey is on another floor (the per-floor blanking happens inside `shoggoth_step`, not the summary).
Three prompts consume this same struct: `render_shoggoth_prompt` (`shoggoth_brain.h:43-60`),
`render_shoggoth_vision_prompt` (`shoggoth_vision.h:41-60`),
`render_shoggoth_hearing_prompt` (`shoggoth_hearing.h:36-56`). All three demand the same JSON
schema `{"action":..., "aggression":...}`.

## 4. shoggoth_step — `shoggoth.h:134-222`

**Cadence:** once per sim tick. `dt = kTickDt = 1/120 s` (`shoggoth.h:135`,
`core/include/core/world.h:23-24`). In every live path invoked with `pathfind = (s.tick % 8u) == 0u`
→ BFS recomputes every 8 ticks (~15 Hz). See `main.cpp:974,1638,3169,3246,3426,3581,4883,5200`.

**FSM transitions** (`shoggoth.h:145-160`), gated on `dist` (with per-floor blanking at `:138-141`):
- Lurk→Hunt if `dist < kShogHuntRange` (44 m)
- Hunt→Chase if `dist < kShogChaseRange` (9 m); →Lurk if `dist > kShogLoseRange` (70 m)
- Chase→Retreat if `dist < kShogCatchRange` (2.2 m); →Hunt if `dist > 18 m`
- Retreat→Hunt after 300 ticks (~2.5 s)
Range constants at `shoggoth.h:117-120`.

**Per-floor sensing (M29, load-bearing):** `shoggoth.h:137-141`. If not same level,
`dist = kShogLoseRange + 1` → can never sense prey on another floor, stays in Lurk. **No vertical
motion** — `pos.y` never written. Climbing a stair escapes it. Test `test_shoggoth.cpp:79-119`.

**Speeds** (`shoggoth.h:122-129`): Chase 5.6, Hunt 3.3, Retreat 4.2, Lurk 1.1 m/s. Then intent
modulation.

**BFS** — `next_step_dir` (`shoggoth.h:84-113`): bounded, window R=22 cells (~88 m, `:86`).
Neighbour order fixed E,W,N,S (`:91-92`) — **determinism-critical.** Returns first-step direction
0..3 or -1. Per-call chunk-layout cache. `maze_open` (`:63-79`) memoises layouts.

**Steering** (`shoggoth.h:199-211`): toward centre of next cell along BFS route.

**Writhe/ooze** (`shoggoth.h:218-221`): `writhe += dt*(2.5+speed)`;
`ooze = 0.92 + 0.18*sin(writhe*1.7)`. Position advances `ddx*speed*ooze*dt`. Never glides
constant-speed.

**Is there a Stroller class?** Yes — `struct Stroller` at `main.cpp:2246-2425` — but it is **NOT
the Shoggoth**. It's the screensaver's autonomous camera navigator. Reused only in
`--screensaver` (`main.cpp:5112`). The Shoggoth's own locomotion is solely `shoggoth_step`.

## 5. build_shoggoth_mesh — `shoggoth_body.h:49-95`

Header-only, pure. **13 tapered tentacle-spokes** around a core, in WORLD space, in
`contracts::ChunkVertex` format. Constants (`:53-55`): T=13 tentacles, core=0.30*scale,
SEG=4, RAD=4. Per tentacle: SEG×RAD×2 = 32 triangles = 96 verts. Total 13×96 = **1248 verts,
416 triangles**, stable across writhe. ≤4096 (`kMaxCreatureVerts`) per `test_shoggoth.cpp:235-246`.
Writhe animation: tentacle length pulses `1.0+0.4*sin(w*1.3)` (`:63`); curving wave toward tip
(`:72`). Material tagging: raster = vertex colour salmon `(0.92,0.42,0.34)*bright` (`:37`),
`v.material=0.0f`; DXR overrides `v.material=7.0f` after building (`main.cpp:1022,1777`) so PT
shades it salmon. Raster path byte-for-byte unchanged (ADR-052).

## 6. The brain host — `shoggoth_brain_host.h`

`class ShoggothBrainHost` (`:37-116`), the **only** live async host. Mirror of Director's
`DirectorHost`. Construction `(host, port, timeout_ms=8000)` spawns `worker_loop`.
- `submit(ShoggothSummary)` (`:56-63`): non-blocking, latest-wins. At most one in flight.
- `poll()` (`:68-73`): non-blocking, move-swaps `ready_`.
- `worker_loop` (`:79-102`): blocking `keel_complete` **outside the lock, on the worker**.
  KEEL down/`!resp.ok` → `continue` (graceful no-op). Invalid JSON → dropped.
- Counters: `requests_`, `produced_`, both atomic.

**Cadence in live game** (`main.cpp:935-937,1465-1467`): `brain_interval = 3000 ms` wall-clock.
Submit when `now - last_brain >= 3 s`. `poll()` drained every frame; latest overwrites
`sh.intent` (`main.cpp:986,1647,5205`). **No event-logging in the live path** — mutates
`sh.intent` directly (only headless record path logs events).

## 7. The vision host — does NOT exist

Confirmed: no `shoggoth_vision_host.h`, no `ShoggothVisionHost`. Vision is **record-only**.
Pieces that exist (pure, no renderer/network): `shoggoth_pov_camera` (`shoggoth_vision.h:26-36`)
— `CameraPose` at body, eye-height, yaw=facing, pitch -0.08, fov_y 70°. 
`render_shoggoth_vision_prompt` (`:41-60`). Actual render + KEEL vision call in
`run_shoggoth_vision_record` (`main.cpp:3190-3258`): renders 384×216 offscreen (`:3195`),
readbacks RGBA, PNG-in-memory, base64, `keel_complete_vision` (`:3236`). **Record-time only;
snapshot never re-rendered at replay.**

Hearing likewise record-only: `shoggoth_hearing.h` has only `clean_transcript` +
`render_shoggoth_hearing_prompt`. Audio render + whisper shell-out in `main.cpp`
(`shoggoth_listen_wav` `:3266-3290`, `whisper_transcribe` `:3295-3313`), called from
`run_shoggoth_hearing_record` (`:3389`) and `run_shoggoth_pa_record` (`:3491`).

## 8. parse_shoggoth_intent — `shoggoth_brain.h:64-93`

`ShoggothIntent parse_shoggoth_intent(const std::string& content, bool& ok)`. Default Hunt/0.5,
`ok=false` (`:65-66`). **Never throws.** JSON tolerance (`:70-73`): finds outermost `{`...`}`,
swallows markdown fences. Parses with `br::director::json::parse`. `action` must be string in
`hunt|stalk|lurk|flank|flee` (`:80-85`); unknown → reject. `aggression` optional, clamped [0,1]
(`:86-90`). Tests at `test_shoggoth.cpp:147-158,209-215`.

## 9. Record/replay sacred gate

Pattern: LLM runs at RECORD time only; validated intents enter deterministic shoggoth as
timestamped event-log entries; REPLAY with model offline reproduces bit-for-bit.

**Event struct** — `shoggoth_brain.h:96-100`:
`struct ShoggothEvent { uint64_t effective_tick; int32_t action; float aggression; };`

**Log format** — `write/read_shoggoth_log` (`:102-130`): binary, magic `"SHOGLOG1"`, seed (8B),
ticks (8B), count (8B, cap 1M), then raw records. **No version field beyond magic.**

**Determinism hash** — `shoggoth_hash` (`shoggoth.h:225-236`): FNV-1a over `pos.xyz, yaw,
writhe, state, state_ticks, wander_gi, wander_gj, level, intent.action, intent.aggression`.
**Does NOT hash `rng` state** (safe only because rng draws are deterministic given step sequence).

**Record paths** (each writes event log + combined hash): `run_shoggoth_record` (`main.cpp:3136`),
`run_shoggoth_vision_record` (`:3190`), `run_shoggoth_hearing_record` (`:3389`),
`run_shoggoth_pa_record` (`:3491`). All use identical apply+log shape: `sh.intent = intent;
events.push_back({t, action, aggression}); H = fold_bytes(H, &events.back(), sizeof(ShoggothEvent));`
then `shoggoth_step(...)` and `H = fold_u64(H, shoggoth_hash(sh))`.

**Replay** — `run_shoggoth_replay` (`main.cpp:3559`): re-reads seed+ticks+events, re-runs same
`MazeWalker`, applies each event at `effective_tick` (`:3575-3580`), **never contacts KEEL**.
Combined hash must equal record's.

**Gate** — `scripts/gate.ps1:2031-2043`: same seed → identical `shoggoth_hash` across two
`--shoggoth` runs; different seeds → different hashes. The `>=1 real intent` clause makes it
non-vacuous.

**Surprise (AUDIT flag):** `docs/AUDIT_2026-06-15.md:207` — `run_shoggoth_vision_record` does
NOT carry the M29 prey-offset logic that replay applies; a `--level != 0` vision record would
NOT replay bit-exact. Latent determinism gap in the vision record path specifically.

## 10. hunt/stalk/lurk/flank/flee — real behaviors or multipliers?

**Both, but mostly multipliers/goal-overrides.** The locomotion FSM has 4 states; the 5
`ShoggothAction` values are **intent-layer goal overrides + speed multipliers**, not separate
states. All flow through the same `next_step_dir` BFS. No separate "stalk pathfinder."
`test_shoggoth.cpp:160-173` only asserts `Flee` ends farther than `Hunt`.

## Cross-cutting flags

- **No live vision or hearing.** Only text brain live. A live shoggoth with vision/hearing needs
  new hosts mirroring `ShoggothBrainHost` (the `DirectorVisionHost` at `director_vision.h:89` is
  the template).
- **The shoggoth has no speech/thought of its own.** TTS/PA voice are the Director's. The
  shoggoth only *hears* the Director's PA (M24). `vision_entity_context` (`main.cpp:1336`) is a
  cue the Director narrator receives — not the shoggoth's output.
- **Per-floor confinement is load-bearing.** `pos.y` never moved; `same_level` blanks
  cross-floor sensing (`shoggoth.h:138-141`). Tested + hashed.
- **`shoggoth_hash` does not mix `rng` state.** Safe only because rng draws are deterministic
  given step sequence; the per-tick hash chain via `fold_u64` captures every step.
- **Flank is half-built** (integer `/2` on 1-cell deltas, `shoggoth.h:189`).
- **Vision record path M29 gap** (AUDIT_2026-06-15.md:207).
- **Mesh vertex count fixed 1248** (13×96), under DXR `kMaxCreatureVerts=4096`. DXR creature
  BLAS rebuilt per frame via `update_creature` (`render_dxr/src/dxr.cpp:1008+`); chunk BLASes
  cached (ADR-052).
- **`shoggoth_step` invoked identically everywhere** with `pathfind = (tick % 8 == 0)` — live
  play, screensaver, all four record/replay paths. Uniformity is what makes record==replay hold.

---

# B. Director sensory infrastructure map (eyes/ears/voice/brain)

## Topology

Three independent off-thread hosts, one localhost KEEL sidecar, one WinHTTP client. All
live-presentation-only (never touches sim/replay/goldens — INV-1/INV-6).

| Organ | Host class | File | Submits | Returns | Cadence |
|---|---|---|---|---|---|
| Text brain (M11) | `br::director::DirectorHost` | `director/include/director/host.h` | `WandererSummary` | `contracts::Directive` | 18 s |
| Eyes (VLM) | `br::app::DirectorVisionHost` | `app/src/director_vision.h` | b64 PNG + context | narration line | 28 s |
| Two-way voice | `br::app::DirectorChatHost` | `app/src/director_chat.h` | WAV + b64 PNG + context | `DirectorExchange{user,reply}` | utterance-driven |
| Voice output | fn `speak_pa` + `synthesize_speech` | `app/src/tts.h` | text | PCM16 via winmm | per line |
| Ears (VAD) | `br::app::MicCapture` | `app/src/mic_capture.h` | (polled) | `vector<int16_t>` | continuous |

No `app/src/director.h`; no `director_vision_host.h` (folded into `director_vision.h`).
`director/include/director/director.h` is the *text* brain only.

## 1. DirectorVisionHost — `director_vision.h:89-168`

Ctor `(host, port, timeout_ms=30000)` spawns `worker_loop`. Dtor sets stop_, joins (RAII).
- `submit(string image_b64, string context)` (`:108-116`): non-blocking, latest-wins, one in flight.
- `poll()` (`:119-124`): swaps `ready_` queue.
- `worker_loop` (`:130-153`): blocking `keel_complete_vision(host_, port_,
  render_director_vision_prompt(ctx), b64, timeout_)` **outside the lock** (`:144-145`).
  `!resp.ok` → continue (graceful). Empty cleaned line dropped.
- Counters `requests()`/`produced()`.

Explicitly "a faithful copy of ShoggothBrainHost's latest-wins / non-blocking poll /
graceful-no-op pattern" (`director_vision.h:14-18`). **Canonical async-LLM-host template.**

## 2. keel_complete / keel_complete_vision — `keel_client.h:14-22,27-28,36-37` + `keel_client.cpp:99-120`

`KeelResponse { ok, http_status, content, tier, route, cost, error }`.
- Text: `keel_complete(host, port, prompt, timeout_ms=8000)` (`keel_client.cpp:99-106`). One
  `user` message + `{"sovereign":true,"kind":"scaffolding","think":false}`.
- Vision: `keel_complete_vision(host, port, prompt, image_base64, timeout_ms=30000)`
  (`:108-120`). `user` content is an array: `{"type":"text","text":...}` then
  `{"type":"image_url","image_url":{"url":"data:image/png;base64,<b64>"}}`. Same flags.
- Transport `keel_post` (`:32-95`): WinHTTP, plain HTTP, `POST /v1/chat/completions`, parses
  OpenAI envelope + optional `keel.{tier,route,cost}`. Never throws.
- `service_up(host, port, timeout_ms=800)` (`:122-137`): `GET /health`.
- Host/port: not hardcoded; app default `127.0.0.1:7071` (`main.cpp:2742-2753`), overridable
  `--director-url`. Sidecar: keel-serve on `:7071` reusing llama-server on `:8080`
  (`main.cpp:671-675`).

## 3. DirectorChatHost — `director_chat.h:85-165`

Ctor `(host, port, TranscribeFn transcribe, timeout_ms=30000)`. `TranscribeFn =
function<string(const string)>` (`:87`) — whisper injected as functor (no whisper dep in
header). Live wiring: `[wexe,wmodel](w){ return whisper_transcribe(w,wexe,wmodel); }`
(`main.cpp:1712-1713`).
- `submit(wav_path, pov_b64, context)` (`:101-110`): latest-wins, one in flight.
- `poll()` (`:112-117`): `vector<DirectorExchange>` where `DirectorExchange{user, reply}` (`:80`).
- `worker_loop` (`:123-150`): (1) drain pending; (2) `heard = transcribe_ ? transcribe_(wav) :
  string()` (`:136`); (3) `if (!plausible_utterance(heard)) continue` (`:137`); (4)
  `render_director_chat_prompt(heard, ctx, !b64.empty())` (`:139`); (5) empty b64 →
  `keel_complete`, else `keel_complete_vision` (`:140-142`); (6) `reply =
  clean_vision_line(resp.content)` (`:144`); (7) push `DirectorExchange{heard, reply}` (`:148`).
- Reply spoken by caller via `speak_pa` (`main.cpp:1733`).

## 4. MicCapture — `mic_capture.h:30-142`

Header-only, winmm. **16 kHz mono PCM16** (`kRate=16000`, `:32`). 8 buffers × 1600 samples
(100 ms each). `CALLBACK_NULL` (polled). `poll(out)` (`:80-120`): drains ready buffers, runs VAD.
**VAD state machine** (`:91-113`): per-buffer RMS; warmup ~400 ms learns `noise_floor_`;
onset threshold `max(noise_floor_*3.5, 0.010)`; end threshold `max(noise_floor_*2.0, 0.005)`;
ambient drift 0.97/0.03 EMA; end-of-utterance ~700 ms hangover OR 12 s cap; min length 0.4 s.
**Echo control (half-duplex):** `suspend_until(until_ms)` (`:76`); when suspended, drops audio
+ resets VAD (`:88-89`). Frame loop syncs from global `g_paSuspendUntilMs` (`main.cpp:1716`).
No acoustic echo cancellation — pure time-gating.

## 5. tts.h — `tts.h:382-384`

`inline std::vector<int16_t> synthesize_speech(const std::string& text, uint32_t sr)`. One-shot,
whole `vector<int16_t>`. Deterministic, no assets. Pipeline: text → phonemes → formant tracks →
PCM16. Phoneme set ARPABET-ish; G2P = PA lexicon (`:136-199`, ~70 words incl. FLUORESCENT,
HUMMING, CARPET) + rule-based fallback (`:205-260`). `synthesize_phonemes` (`:286-377`):
`base_f0=105.0f` (deep, `:288`); `kRate=1.22f` stretch (`:289`); 3 cascaded 2-pole resonators
with formant transitions + diphthong end-targets; sawtooth glottal source through one-pole tilt
+ aspiration noise; **declining pitch contour** `f0 = base_f0*(1.12 - 0.34*prog)` (`:334-335`) +
per-period jitter (`:335`); stops 45 ms closure + burst; raised-cosine envelope; normalised
peak 0.85 → PCM16. **Render-then-play, not streaming.** `speak_pa` (`main.cpp:531-552`) builds
whole WAV → `PlaySoundW(..., SND_MEMORY|SND_ASYNC|SND_NODEFAULT)` (`:547`); static
`vector<uint8_t> g_pa` holds buffer alive; `SND_PURGE` cuts prior line. (PA = public-address
intercom, not PortAudio; API is winmm.)

## 6. Prompt rendering

`render_director_vision_prompt(context)` (`director_vision.h:39-54`): surveillance-AI framing;
**hard constraint** "Speak ONE short sentence (≤18 words)… Describe ONLY what you can see;
never invent objects not there"; "calm, clinical, quietly menacing"; output only the line. If
context non-empty (`:48-52`): appends "Facility sensors also report: <context>. You MAY
reference this." — the **sight+sensor hybrid**.

`render_director_chat_prompt(user_said, context, has_image)` (`director_chat.h:62-78`):
conversational; `has_image` toggles grounding clauses; "ONE or TWO short sentences, IN
CHARACTER… never mention being an AI."

## 7. POV grab — `encode_pov_b64` `main.cpp:1302-1333`

Downscale to **384×216** box-average (`:1303,1306-1323`) → in-memory PNG via
`stbi_write_png_to_func` (`:1326-1330`) → base64 (`:1332`). Cadence `vision_interval = 28000 ms`
(`:1484`); first POV ~8 s in. Submit inside RT readback block, only on clean frame before
caption composite (`:1786-1790`). Chat POV: `wantChatPov=true` (`:1724`), grabs same clean RT
readback next frame (`:1792-1795`).

## 8. g_visionAvailable / VRAM tier — `main.cpp:608,590-605,650-653`

`static atomic<bool> g_visionAvailable{true}` (`:608`). `detect_vram_mb()` DXGI largest adapter
DedicatedVideoMemory. `use9b = (vram==0) || (vram>=11000)`; ≥11 GB → 9B+mmproj (vision), else
4B text-only. Gating: text Director (`:1652`, negated) vs vision Director (`:1686`); chat
text-vs-vision (`:1724` vs `:1725`). 4B tier gets text brain + text-only chat, no eyes. 9B
drops the text Director.

## 9. clean_vision_line / plausible_utterance — the cages

`clean_vision_line(raw)` (`director_vision.h:59-83`): strip `<think>` block; collapse whitespace;
strip wrapping quotes; **hard cap 220 chars**. Shared by vision host + chat host.

`plausible_utterance(t)` (`director_chat.h:36-57`): strip `[...]`/`(...)` tags (kills whisper's
`[BLANK_AUDIO]`); trim; require ≥4 alphabetic chars; lowercase + reject denylist (`you`,
`thank you`, `thanks`, `thanks for watching`, `bye`, `okay`, `ok`, `yeah`). Comment: "Imperfect
but keeps the Director from answering the room tone."

`clean_transcript` (`shoggoth_hearing.h:25-31`): simpler whitespace-only trim after whisper.

## 10. Priority / queueing — NONE

No client-side queue/priority. Three independent worker threads; only serialization point is
KEEL sidecar (single llama-server, `-c 8192`). Latest-wins within a host, **no cross-host
coordination**. A 28 s vision call in flight blocks a chat turn behind it. PA-layer co-existence
guard: single `last_pa_line` dedupes consecutive spoken lines; `speak_pa` does `SND_PURGE`
before each new line (cuts older). Subtitle slot last-writer-wins. Echo gate is the only real
cross-organ coupling. **If a Shoggoth wants see+hear+speak+think concurrently through one KEEL,
a slow vision call starves chat/brain. No priority channel, no preemption today.**

## Surprises

1. keel_complete_vision + mmproj were **proven by the M22 Shoggoth** (`director_vision.h:7-12`);
   the Director is the late adopter. A creature reusing this is third in line — pattern battle-tested.
2. `clean_vision_line` reused for chat replies too (`director_chat.h:144`) though named "vision".
3. Text-only chat turn is reachable (4B tier / RT off) — ears+brain+voice loop works without eyes.
4. `speak_pa` cuts itself off — interruptible, no queue, no ducking.
5. Whisper blocking shell-out, 120 s hard cap (`main.cpp:3306`), on chat worker thread.
6. Mic WAV files rotate 0..15 (`main.cpp:1720`) into temp dir.
7. `encode_pov_b64` runs on frame thread (`main.cpp:1788,1794`) — once per ~28 s + once per chat turn.
8. `g_visionAvailable` defaults true; unknown-VRAM → tries 9B → fails → silent no-op (not text fallback).
9. KEEL `sovereign:true, kind:scaffolding, think:false` hardcoded (`keel_client.cpp:104,118`).
10. `keel.lock` hashes all `TODO` — reproducibility contract not pinned.

---

# C. Engine ground-truth geometry audit (the geometry oracle)

**Verdict: YES, conditionally.** Engine exposes a pure deterministic total geometry API that
can resolve most semantic targets (doorway/stairs/shaft/wanderer/proximity/sector-ish) to real
cells with perfect ground truth. Two gaps: no LOS/raycast-through-maze, no vertical pathing
(creature hard-locked to one floor). `Stroller::plan` + `draft_intensity_near_shaft` are working
precedents of the "semantic → metric" pattern.

## 1. maze_open — `shoggoth.h:63-79`

```cpp
inline bool maze_open(uint64_t seed, int64_t gi, int64_t gj, int dir,
                      unordered_map<int64_t, br::gen::ChunkLayout>& cache, int32_t level = 0);
```
`gi,gj` global cells. `dir`: 0=E(+x),1=W(-x),2=N(+z),3=S(-z). Maps to chunk via `floor_div(gi,G)`
(`:50-54,66-67`), memoizes `ChunkLayout` keyed by `(cx,cz)` packed int64 (`:68-71`), reads wall
arrays (`:73-78`): dir0 `!L.vwall[li+1][lj]`, dir1 `!L.vwall[li][lj]`, dir2 `!L.hwall[li][lj+1]`,
dir3 `!L.hwall[li][lj]`. Cache per-call. `level` threaded into `generate_layout`. **Connectivity
oracle.** Wall layout level-folded (`layout.cpp:46-53`).

## 2. ChunkLayout + verticality — `gen/include/gen/layout.h:22-29`

```
struct ChunkLayout {
    bool vwall[kCellsPerChunk + 1][kCellsPerChunk];   // x-line xi (0..G), z-cell j (0..G-1)
    bool hwall[kCellsPerChunk][kCellsPerChunk + 1];   // x-cell i (0..G-1), z-line zj (0..G)
    int door_left, door_right, door_bottom, door_top; // doorway cell index (0..G-1) per chunk edge
};
```
`kCellsPerChunk=8` (`layout.h:18`); 8×8 cell grid. **Doorway cells per chunk edge** (`:28`):
`door_left/right/bottom/top` = gap in perimeter wall, cell index 0..7. Carved from shared-edge
hash so neighbors agree (`layout.cpp:130-137`). **Per-chunk "doorway" semantic → the cell index.**

`stair_at` (`layout.h:66-70`, impl `layout.cpp:55-76`):
`struct StairSpec { bool present=false; int cell_i=0, cell_j=0; };`
`StairSpec stair_at(uint64_t world_seed, int32_t level, int64_t cx, int64_t cz);`
UP-stair `level`→`level+1`. Pure hash. Hybrid coverage: density ~1 per `kStairDensityN=13`
chunks + guaranteed per-superblock `kStairSuperblock=4` (≥1 per 4×4-chunk / ~128 m block).
**Enumerate "all stair-up cells on this level":** loop `(cx,cz)`, collect where `.present`;
local cell → global via `cx*8 + cell_i`.

`shaft_at` (`layout.h:91-97`, impl `layout.cpp:228-243`):
`struct ShaftSpec { bool present=false; int cell_i,cell_j; int32_t top_level=0; int32_t depth=0; };`
`ShaftSpec shaft_at(uint64_t world_seed, int64_t cx, int64_t cz);` (no level — per-column).
Descending void. ~1 per `kShaftDensityN=1500` chunks (~1.3 km). `top_level` in `[-30,30]`;
`depth` in `[5,10]`. Helpers `shaft_passes/shaft_floor_open/shaft_ceil_open` (`:101-111`).

`floor_hole_at`/`ceiling_hole_at` (`layout.h:134-135`, impl `layout.cpp:245-255`): per-cell
boolean. `floor_hole_at` true iff a DOWN-stair from `level-1` lands here OR a passing shaft is
floor-open. **Single source of truth** the live-walk collision uses (`main.cpp:828`).
`Stroller::cell_hole` (`main.cpp:2264-2269`) wraps it.

`validate_vertical_connectivity` (`layout.h:79`, impl `layout.cpp:190-226`): flood-fills a slab,
`(L,cx,cz)<->(L+1,cx,cz)` linked iff `stair_at` fires — proves the stack is one component.
**Stairs are the only intended up-path; shafts are down-only voids.**

## 3. World ↔ cell math

`kChunkSize=32.0 m` (`chunk_gen_v1.h:18`); `kCellsPerChunk=8`; `kCellSize=4.0 m` (`layout.h:19`,
`chunk_gen_v1.h:60`); `kCeilingHeight=3.0 m` (`chunk_gen_v1.h:61`); `kLevelHeight=4.0 m`
(`:66`). `level_base_y(level)=level*4.0` (`:67`); `level_from_y(y)=floor(y/4.0)` (`:71`).
`chunk_key_at(level,x,z)` (`:37-43`): `cx=floor(x/32), cz=floor(z/32)`. `world_to_cell(x,z,gi,gj)`
(`shoggoth.h:55-58`): `gi=floor(x/kCellSize), gj=floor(z/kCellSize)`. `cell_center(g)=(g+0.5)*4.0`
(`:59`). `floor_div` (`:50-54`) negative-safe. **Cell size = 4 m.**

## 4. Stroller::plan — bearing-scored BFS — `main.cpp:2314-2363`

`void plan(uint64_t seed, int32_t level, int64_t sgi, int64_t sgj, float facing)`. Window R=26,
W=53 → **~104 m** look-ahead (`:2315`). BFS from `(sgi,sgj)` over `maze_open` (`:2340`), skipping
`cell_hole` cells (`:2344`). Parent dir in `par[]`. **Scoring** (`:2331-2338`): for each visited
cell (excl start), `dirYaw=atan2(ddi,ddj)`, `cosrel=cos(wrap_pi(dirYaw-facing))` (1 ahead, -1
behind), `fclamp=(cosrel>0)?cosrel:0` (forward cone only), `score = sqrt(d2)*(0.15+0.85*fclamp)
+ 0.3*rng.next_double()`. Best-scoring `(bgi,bgj)` = goal → **farthest reachable cell within
forward 180° cone**, small jitter. Path reconstruction (`:2350-2362`) walks `par` back, reverses
→ `path` (start..goal). Re-plan only when path consumed/stuck/stale (`:2382-2386`). **Direct
precedent for the proposed resolver.**

## 5. Stroller::steer — `main.cpp:2294-2309`

24-direction fan (15° steps, `:2298-2299`). Each: `clearance(...)`; skip if `clr<1.2`. Score
(`:2302-2304`): `1.7*cos(wrap_pi(a-goalDir)) + 0.9*(clr/reach) + 0.5*cos(wrap_pi(a-facing))`
(reach=6.5 m, `:2296`). Best → free continuous angle. Fallback `goalDir` if boxed. Called every
5 ticks; smoothly chased with human weave.

## 6. clearance — `main.cpp:2272-2288`

`static float clearance(seed, level, p, yaw, maxReach, col)`. Marches `(sin(yaw),cos(yaw))` from
`d=0.5` in `0.45` steps to `maxReach`. Blockers: (a) floor hole via `floor_hole_at`
(`:2281-2282`); (b) collision AABB overlap with swept circle `r=kWandererRadius+0.18` (`:2275,
2283-2285`) at chest height. Returns distance to first blocker or `maxReach`.

## 7. shoggoth_step locomotion — does NOT reuse Stroller

`shoggoth.h:134-222` — has its **own** simpler navigator. `next_step_dir` (`:84-113`): bounded
BFS R=22 (~88 m), returns only first-step direction 0..3, re-floods every call (every 8 ticks).
`shoggoth_step` picks goal from FSM (`:167-179`): Hunt/Chase→wanderer cell; Retreat→3 cells away;
Lurk→random ±3 (`:174-177`) repicked when reached or every 360 ticks. Intent overrides (`:183-196`).
Steering (`:198-211`): aim at centre of single next cell. No 24-dir fan, no clearance probe.
`Stroller` is **screensaver-only** (`main.cpp:2235-2245`).

## 8. Line-of-sight — NONE for maze

Searched `line_of_sight|los|raycast|occluded|visible|can_see|cells_between`. **No maze-cell LOS
exists.** Hits: `audio/include/audio/room_probe.h` (AABB raycast for reverb sizing, not maze
connectivity); UI comments. Closest substitute: `Stroller::clearance` (single-dir AABB ray-march).
**GAP: no occlusion-aware LOS.** An LLM "I see the wanderer ahead" can't be verified against
geometry; only Euclidean distance + bearing.

## 9. Sensory range

Distance to wanderer: `shoggoth.h:136-141`, only if `same_level` (else `dist=kShogLoseRange+1`).
Range thresholds (`:117-120`): hunt 44 m, chase 9 m, catch 2.2 m, lose 70 m. `ShoggothSummary`
(`shoggoth_brain.h:24-30`) carries `sgi,sgj,wgi,wgj,distance_m,state,tick`. **No "cells between"
query** — trivially buildable from `maze_open` BFS. `draft_intensity_near_shaft` (`main.cpp:844-861`)
is a concrete existing example of resolving a semantic target ("nearest open shaft") to metric
distance via `shaft_at` + `shaft_floor_open` + world-cell math. **Exact "geometry oracle" pattern,
already in production for audio.**

## 10. Verticality — NO

Creature hard-locked to one floor. `Shoggoth.level` derived from `pos.y`, never deliberately
changed (`shoggoth.h:137`). `shoggoth_step` does no vertical motion — only `pos.x`,`pos.z`
(`:220-221`). Cross-floor sensing disabled (`:138-141`). `next_step_dir` BFS is 2D single-level.
`Stroller::plan` excludes holes. **The vertical graph exists** (`stair_at` up-links, `shaft_at`
down-voids, `validate_vertical_connectivity` proves connected), and stairs connect
`(L,cx,cz)<->(L+1,cx,cz)`. A resolver could find a cross-level route, but **no such pathing
exists, and locomotion can't execute it** (no Y integration, no stair-mounting).

## Summary table

| Semantic target | Resolvable? | Mechanism |
|---|---|---|
| doorway, ahead-left, near | YES | `ChunkLayout.door_*` + bearing-scored BFS; `Stroller::plan` template |
| the stairs up | YES | `stair_at`; enumerate by scanning chunks (≥1 per 4×4 superblock) |
| nearest open shaft | YES (precedent) | `shaft_at`+`shaft_floor_open`+distance — `draft_intensity_near_shaft` |
| the wanderer | YES | `world_to_cell` + Euclidean; same-floor only |
| far reachable cell ahead | YES | `Stroller::plan` 104 m forward-cone BFS |
| is the way ahead clear | YES | `Stroller::clearance` AABB ray-march |
| sector / region | PARTIAL | no spatial sector; `FlickerSector` is light-index; `biome.h:36` coarse region label |
| can I see X from here (LOS) | NO | no maze-cell raycast; only AABB reverb probe + single-dir `clearance` |
| take stairs to level+1 (resolve) | YES | `stair_at` gives the cell |
| take stairs to level+1 (execute) | NO | creature has no Y integration / stair-mounting |

---

# D. Determinism shell map (the seams for live AI)

## 1. INV-1..INV-8 — `docs/ARCHITECTURE.md:46-53`

CLAUDE.md Iron Rule 4 (`:29`): "Determinism is sacred. INV-1..INV-8 override convenience."
- **INV-1 Determinism**: World state hash after N ticks = pure function of `(WorldSeed, input
  stream, Event Log)`. No wall-clock in sim, no unseeded RNG. Sim core `/fp:strict`,
  single-threaded, seeded PCG64. **Master invariant.**
- **INV-2 Generation purity**: `GenerateChunk(WorldSeed, ChunkKey)` pure/total.
- **INV-3 Connectivity**: every chunk traversable.
- **INV-4 Boundedness**: memory bounded by streaming ring.
- **INV-5 Isolation of sim core**: `/core` zero includes from render/audio/director/OS-UI.
  CI grep-gated.
- **INV-6 Fallbacks**: sim runs fully with `--no-director` and raster alone. DXR + Director are
  enhancements, never dependencies. **The live-AI invariant.**
- **INV-7 Headless parity**: every observable has a headless verification path.
- **INV-8 Golden integrity**: goldens change only via `goldgen` + DECISIONS.md entry same commit.

Live-AI constraints: INV-1 (hash pure), INV-5 (raw model text never crosses into `core`), INV-6
(sim runs with AI off), INV-7 (headless proof path).

## 2. INV-6 in detail — "model nudges intent as a logged event"

- `shoggoth_brain_host.h:17-18`: "the LLM never couples to the sim continuously; it only ever
  nudges `sh.intent`" / "intent enters as a timestamped event."
- `DECISIONS.md:255` (ADR-048).
- `ARCHITECTURE.md:124-125`: a Directive is "presentation-only POD, applied via the recorded
  event log at a deterministic effective_tick (raw model text never crosses the boundary)."

**Enforcement (3 layers):**
1. LLM physically off the frame thread — `ShoggothBrainHost::worker_loop` (`:79-102`) runs
   `keel_complete` on a background thread; frame thread only `submit()`/`poll()`.
2. Model output reduced to tiny validated POD before touching anything — `parse_shoggoth_intent`
   (`shoggoth_brain.h:64-93`); bad → safe `Hunt/0.5`, `ok=false`, never throws.
3. "Logged event" = `ShoggothEvent` at fixed `effective_tick` (`shoggoth_brain.h:96-100`).

Director parallel: `contracts/director_v1.h:58-59` `struct DirectorEvent { uint64_t
effective_tick=0; ... }`.

## 3. The record==replay sacred gate

**Record** (`run_shoggoth_record`, `main.cpp:3136-3182`): MazeWalker + Shoggoth; brain every
`kBrainEvery=240` ticks (~2s, `:3139`); builds `ShoggothSummary`, `keel_complete` (`:3157`),
parse → `ShoggothIntent`; on valid: `sh.intent=intent` (`:3163`), push `ShoggothEvent{t,action,
aggression}` (`:3164`), `H = fold_bytes(H, &events.back(), sizeof(ShoggothEvent))` (`:3165`);
every tick: `shoggoth_step` (`:3169`), `H = fold_u64(H, shoggoth_hash(sh))` (`:3170`);
`write_shoggoth_log` (`:3173`).

**Replay** (`run_shoggoth_replay`, `main.cpp:3559-3590`): `read_shoggoth_log` (`:3563`) →
seed, ticks N, events. **KEEL never contacted.** Re-walks same maze (`:3565`); per tick while
`events[ei].effective_tick == t` (`:3575`) applies recorded intent (`:3576-3577`) + folds same
event bytes (`:3578`); steps + folds `shoggoth_hash` (`:3581-3582`); combined hash must equal
record's.

**Hash ingredients** (`shoggoth.h:225-236`): FNV-1a over `pos.xyz, yaw, writhe, state,
state_ticks, wander_gi, wander_gj, level, intent.action, intent.aggression`. Combined run hash
`H` (init `1469598103934665603ull`, `:3145`) folds per tick `fold_u64(H, shoggoth_hash(sh))` and
(when event fires) `fold_bytes(H, &event, sizeof(ShoggothEvent))`. `fold_*` at `main.cpp:2728-2739`.

**Binary log** (`shoggoth_brain.h:102-130`): `SHOGLOG1` magic (8B) + seed (8) + ticks (8) +
count (8, cap 1M) + n raw records. **No version field beyond magic.**

**Gate** (`gate.ps1:2031-2043`): record `--shoggoth-record --seed 3 --ticks 1200`; assert
`valid_intents >= 1`; `recHash = combined_hash`; replay `--shoggoth-replay`; assert
`replay_events >= 1`; assert `combined_hash == recHash` ("the model leaked into the sim").

## 4. M21 → M21b graduation (record-only to live)

M21 shipped brain headless-only; playable game ran default `Hunt`. M21b graduated to live:
1. Background worker, latest-wins, off frame thread — `ShoggothBrainHost` mirrors `DirectorHost`.
2. Worker reuses exact M21 brain (`keel_complete` → `render_shoggoth_prompt` →
   `parse_shoggoth_intent`).
3. Intent enters live sim as discrete tick-boundary event — `run_play` (`main.cpp:981-987`),
   `run_game` (`:1642-1648`): on ~3s cadence `brain->submit(summary)`; then
   `for (const auto& it : brain->poll()) shog.intent = it;`. Assignment outside tick loop, at
   tick boundary — same "timestamped event" shape, just not serialized.
4. Determinism untouched — ADR-048: "M21b changes only the LIVE presentation path; headless
   record/replay byte-for-byte unchanged."
5. Async isolation proven two ways (p99 frame < 2× median; brain-ON hitch ratio ≈ brain-OFF).

**Seam:** live brain = presentation-layer nudge to `sh.intent` at tick boundaries; determinism
proof lives in headless record/replay, which live path never touches.

## 5. M22/M23/M24 — same pattern

All reuse the exact `ShoggothEvent` log + `--shoggoth-replay`; only record-time perception
differs. Each ADR says the chase is "byte-identical to `--shoggoth-record`."

- **M22 Vision** (`run_shoggoth_vision_record`, `main.cpp:3190-3261`): each thought renders POV
  (`shoggoth_pov_camera`) to 384×216 → PNG → base64 → `keel_complete_vision` (`:3236`) → same
  `parse_shoggoth_intent`. Apply+log identical (`:3241-3243`). Vision-tier JSON markdown-fenced →
  `parse_shoggoth_intent` extracts outermost `{…}` (`shoggoth_brain.h:69-73`).
- **M23 Hearing** (`run_shoggoth_hearing_record`, `main.cpp:3384+`): each thought renders ~2.5s
  soundscape at shoggoth's ears (`shoggoth_listen_wav`, `:3266-3290`) → WAV →
  `whisper_transcribe` (`:3295+`, shells out to `whisper-cli.exe`) → tag into `keel_complete`
  → same parse → `ShoggothEvent` log (`:3421-3423`). Whisper = external process, runtime-optional,
  graceful no-op.
- **M24 Voice/TTS** (`run_shoggoth_pa_record`, `main.cpp:3486+`): each thought mixes spoken PA
  announcement (formant TTS `app/tts.h`) over ambient bed at shoggoth's ears → whisper → brain
  → `ShoggothEvent` log (`:3535-3536`). Replay reproduces bit-for-bit with TTS+whisper+model
  offline. A determinism bug caught: `static thread_local` lowpass state leaked between calls →
  broke per-call determinism → made per-call local. `[m24]` test caught it.

**Unifying rule** (`shoggoth_brain.h:5-9`): "The LLM runs at RECORD time ONLY; validated intents
enter the (deterministic) shoggoth as event-log entries at fixed effective_ticks, so a replay
with the model OFFLINE reproduces the chase bit-for-bit. Raw model text never drives the
creature except through `parse_shoggoth_intent`."

## 6. Goldens + INV-8

A "golden" = committed reference artifact under `/goldens` (PNG or hash). Generation only via
`tools/goldgen` (`goldgen capture`). Iron Rule 6 (`CLAUDE.md:31`). Shoggoth `combined_hash` is
**not** a frozen golden — per-run (model stochastic) — gate checks record==replay within a run.
Shoggoth work never touches goldens because shoggoth lives outside WorldState (ADR-046,
`DECISIONS.md:240`).

## 7. Graceful no-op (KEEL/LLM down)

1. Validator returns safe default — `parse_shoggoth_intent`: any failure → `Hunt/0.5`, `ok=false`.
2. Host drops failed inferences — `worker_loop`: `if (!resp.ok) continue; if (!ok) continue;`.
3. Kill switch — `--no-shoggoth-brain` (`main.cpp:239`) skips constructing host.
4. Determinism independent of AI — `shoggoth_step` pure/seeded; `Hunt/0.5` reproduces M20;
   brain-off == brain-on-but-KEEL-down.

**Key invariant: the AI can only add intents; it can never block or stall the tick.** `shoggoth_step`
runs every tick regardless. Brain runs *beside* the tick.

## 8. Live presentation vs gated determinism

`DECISIONS.md:256`: "M21b changes only the LIVE presentation path; headless
record/replay byte-for-byte unchanged."

**Gated determinism (the proof):** headless `--shoggoth-record`/`--shoggoth-replay`. Record
captures LLM into binary log; replay re-runs with model offline, asserts `combined_hash` matches.
Machine-checked by `gate.ps1:2031-2043`. **Must never be perturbed by a live feature.**

**Live presentation (the experience):** `--game`/`--play`, `ShoggothBrainHost` nudges `sh.intent`
at tick boundaries. Interactive play is "inherently non-replayable (records no input stream), so
no event log is written." Live path can do anything wall-clock-driven.

**Contract a live-AI feature must obey:**
- Touch only live/presentation path, never record/replay variants.
- Intent flows through `parse_shoggoth_intent`, lands on `sh.intent` as discrete tick-boundary
  assignment — never continuously inside `shoggoth_step`.
- Graceful no-op when substrate down (INV-6).
- Perception may run at record time in a separate `run_shoggoth_*_record` variant sharing the
  identical chase/log path, so existing `--shoggoth-replay` reproduces it (M22/M23/M24 template).

## 9. ShoggothIntent schema constraints + shoggoth_hash coverage

`shoggoth_hash` computed `shoggoth.h:225-236` (only definition). Folded into combined `H` at
`main.cpp:3170` (record), `:3247` (vision), `:3427` (hearing), `:3540` (PA), `:3582` (replay).
Fields: `pos.x,pos.y,pos.z, yaw, writhe, state, state_ticks, wander_gi, wander_gj, level,
intent.action, intent.aggression`. `ShoggothEvent` bytes folded separately via `fold_bytes`.

**Would extending `ShoggothIntent` break hash/replay?** Yes, two ways:
1. Adding a field without hashing it → replay still matches (unhashed field invisible), **but**
   if it drives motion inside `shoggoth_step`, sim diverges between record (model picks value)
   and replay (reads only `action`+`aggression`). `run_shoggoth_replay` (`main.cpp:3576-3577`)
   reconstructs `sh.intent` from **only** `events[ei].action` and `events[ei].aggression`.
   **Any new intent field affecting `shoggoth_step` MUST be added to `ShoggothEvent` (serialized)
   AND to `shoggoth_hash`, or record≠replay.** Tightest constraint.
2. Changing `ShoggothEvent` byte layout breaks the binary log (`SHOGLOG1`, no version field) →
   old logs unreadable. Schema change needs magic bump (e.g. `SHOGLOG2`) + ADR.
3. `shoggoth_hash` versionless — changing folded fields changes hash for all runs, but
   combined_hash is per-run so gate still holds within a build. Schema change still needs ADR
   (Iron Rule 7).

**Cleanest current seam for live embodied-AI insert:** `ShoggothBrainHost::worker_loop`
(`shoggoth_brain_host.h:79-102`) is where perception→prompt→`parse_shoggoth_intent` happens
off-thread. A richer live sense could feed `render_shoggoth_prompt` there without touching the
gate, *provided* intent still flows through `parse_shoggoth_intent` and lands on `sh.intent` at
a tick boundary — exactly M21b/M22.

## 10. Iron Rules — `CLAUDE.md:24-35`

1. Gate runner is law — milestone done only when `gate.ps1 M<N>` exits 0.
2. Tag on green, push, revert on regression — never debug forward from broken state.
3. Headless first — every feature ships a headless verification path before visual polish.
4. Determinism is sacred — INV-1..INV-8 override convenience; sim core `/fp:strict`, no
   wall-clock, seeded PCG64 only, zero render/audio/director includes (CI grep-gated).
5. Diff budget ≤ 400 LOC per change unless milestone authorizes more.
6. Never edit `/goldens` by hand; never relax a gate threshold.
7. Spec reconciliation in same commit (contract/boundary change → update ARCHITECTURE.md/MODULE.md).
8. New dependency = ADR; no library enters `vcpkg.json` without DECISIONS.md entry.
9. One milestone per session; end every session running the gate + writing SESSION_LOG.md entry.

Supporting: C++20, MSVC, warnings-as-errors; errors cross boundaries as typed results never
exceptions; no globals outside `app` composition root; headers self-contained. "What NOT to do":
no game mechanics, no networking, no asset files (everything procedural), no browser tech, no
Vulkan/OpenGL, no second windowing framework. Live-AI-specific: no new build deps (whisper + KEEL
kept as external processes / HTTP sidecars — `DECISIONS.md:272,181`); ≤400 LOC diff budget;
headless verification path mandatory (record/replay gate IS that path); manifest step before code.

---

# E. keel/llama/whisper build & layout map

## 1. keel-serve source IN this repo? — NO

Prebuilt binary, staged by `package.ps1` from external dev-box install. `package.ps1:21`
`$KEEL='C:\keel-sidecar-7071'`; `:76-77` copies `keel-serve.exe` + `keel.lock`. No
`C:\backrooms\keel-sidecar\` dir. Repo-wide glob `**/*keel*` returns only `keel_client.{h,cpp}`,
staged `dist\Backrooms\runtime\keel\{keel-serve.exe,keel.lock}`, logs, build `.obj`. **No source,
no CMakeLists, no Rust/Cargo anywhere** (`git ls-files | grep -iE '\.(rs|toml)$|cargo'` =
nothing). Dev-box `C:\keel-sidecar-7071\` has only `README.md`, `keel-serve.exe`, `keel.lock`,
`start.cmd`, `start-all.cmd`, `.keelstate\` — no source tree. Per its README: "a port-patched
copy (debug build snapshot from KEEL Stage-2 state)" — byte-patched (one char `7070→7071`, same
length). KEEL itself lives in separate `C:\KEEL\` (not in repo). **Rust binary ~12.9 MB,
self-contained.** Staged copy is debug snapshot, not release build. `keel-serve.exe` gitignored.

## 2. What keel-serve DOES — thin HTTP router/proxy

Loads NO model. Binds `:7071`, OpenAI-compatible `POST /v1/chat/completions`. Translates request,
forces **local** tier (Qwen3.5-9B via llama-server :8080), adds KEEL routing fields under `"keel"`
envelope (`tier, cost, route`) — `keel_client.cpp:88-92`. **Tier/routing from keel.lock
(`:68-79`):** `tiers.local` → `local_llama`, `qwen3.5-9b-q5_k_m`, vision true, $0 (`:71`);
`tiers.cheap-API` → deepseek $0.435 (`:72`); `tiers.frontier` → claude-opus-4-8 $5.0 (`:73`);
`router.default_tier: local`, `sovereign_forces: local`, `perception_forces: local` (`:76-78`);
escalate after 2 oracle failures (`:79`). **Resolver order** (`:61-66`): `probe_running`
[:1234 LM Studio, :11434 Ollama, :8080 llama-server] → `launch_local` [llama_cpp, whisper] →
`cloud_tier` → `embedded_tiny` → fail. Shipped flow is **probe-only on :8080** — game starts
llama-server first, keel finds :8080 serving, never invokes `launch_local`; `C:\llama.cpp` /
`C:\models` paths in `keel.lock:54,58` vestigial. keel-serve = **local-tier-only
OpenAI-compatible gateway + routing/tier/cost/ledger layer** fronting llama-server.

## 3. llama-server — stock binary

`package.ps1:19` `$LLAMA='C:\llama.cpp'`; `:72-73` copies `llama-server.exe` + all `*.dll`.
`keel.lock:54` pins `llama_cpp: { path: "C:\\llama.cpp", build: b9627, cuda: "12.4", endpoint:
"http://127.0.0.1:8080", launch: managed, prev_build: "C:\\llama.cpp-b8931-april" }`. No
CMakeLists references llama/ggml/cuda. Staged build matches stock llama.cpp release:
`llama-server.exe` (9 KB stub) + `llama-server-impl.dll` (9 MB), `llama-common.dll`, `llama.dll`,
`mtmd.dll` (multimodal/vision), `ggml-cuda.dll`, etc.

## 4. whisper-cli — stock

`package.ps1:20` `$WHISPER='C:\whisper.cpp'`; `:80-81` copies `whisper-cli.exe` + all `*.dll`.
`keel.lock:57` `whisper: { path: "C:\\whisper.cpp", mode: "cli|server" }`. Invoked as subprocess
(`main.cpp:3295-3313`): `whisper-cli.exe -m <model> -f <wav> -otxt -np -l en -nth 0.60`, output
from `<wav>.txt`. **CPU-only** (libopenblas, no CUDA DLLs).

## 5. Ports/endpoints

- `:8080` llama-server (`keel.lock:54`, `main.cpp:668`). `--host 127.0.0.1 --port 8080 -ngl 99
  -c 8192` + `-m` + (9B) `--mmproj` (`main.cpp:666-668`).
- `:7071` keel-serve (`main.cpp:672`). `keel-serve.exe keel.lock`, cwd = its dir (`:674`).
- `POST /v1/chat/completions` to keel `:7071` (`keel_client.cpp:47`). Body forces
  `"sovereign":true,"kind":"scaffolding","think":false` (`:103-104`); vision adds `image_url`
  data-URI (`:113-118`).
- `GET /health` — `service_up` (`keel_client.cpp:131`, `keel_client.h:39-42`).
- keel-side resolver also probes `:1234/v1`, `:11434/v1` (`keel.lock:62`) — not the game.

## 6. Shared-memory / in-process? — NO

100% localhost HTTP to separate processes. `director/include/director/keel_client.h:4-6`:
"thin HTTP client... **No embedded llama.cpp; inference lives in a separate process reached over
localhost.**" `director/CMakeLists.txt:6` links only `winhttp`. `app/CMakeLists.txt:23-26` links
`core gen stream telemetry audio render_d3d12 render_dxr director br_stb user32 winmm xinput
dxgi` — **nothing AI**. ADR-038 deliberate. keel.lock describes in-process organs for KEEL
project's future (`embedding`, `privacy` via ONNX/ort, `store` sqlite — `keel.lock:24-50`) but
**none realized in this repo** — spec aspirations keel-serve would implement. **Realistic
in-process option is only llama.cpp (stock C++ lib at C:\llama.cpp); keel would stay sidecar or
need reimplementation in C++ in `director` (no keel source here to link).**

## 7. DLLs sidecars need

**llama** (`runtime\llama\`, ~1.1 GB, `package.ps1:72-73` copies `*.dll` wholesale):
`cudart64_12.dll` (554 KB), `cublas64_12.dll` (100 MB), `cublasLt64_12.dll` (474 MB),
`ggml-cuda.dll` (565 MB — CUDA kernels); `ggml.dll`, `ggml-base.dll`, 13× `ggml-cpu-<arch>.dll`
(alderlake, cannonlake, cascadelake, cooperlake, haswell, icelake, ivybridge, piledriver,
sandybridge, sapphirerapids, skylakex, sse42, x64, zen4); `llama.dll`, `llama-common.dll` (7.6 MB),
`llama-server-impl.dll` (9 MB), `llama-bench/batched-bench/cli/completion/fit-params/perplexity/
quantize-impl.dll` (spare tools); `mtmd.dll` (multimodal — **required for vision/mmproj**);
`libomp140.x86_64.dll`, `ggml-rpc.dll`. **NOT bundled:** `nvcuda.dll` (driver-owned,
non-redistributable — `package.ps1:97-98`).

**whisper** (`runtime\whisper\`, ~51 MB, `:80-81` `*.dll`): `whisper.dll`, `whisper-cli.exe`,
`ggml-base.dll`, `ggml-cpu.dll`, `ggml.dll`, `ggml-blas.dll`, `libopenblas.dll` (51 MB),
`SDL2.dll` (2.5 MB — pulled by glob, unused by CLI path).

**keel** (`runtime\keel\`, ~13 MB): just `keel-serve.exe` (12.9 MB self-contained Rust) +
`keel.lock` + `.keelstate\`. **No external DLLs** — statically linked Rust.

## 8. Bundle size/breakdown

**10.9 GB, 55 files** (store-mode zip, `SESSION_LOG.md:21-22`). **Models dominate (~10.4 GB)** in
`dist\Backrooms\models\`: `Qwen3.5-9B-Q5_K_M.gguf` 6.13 GB, `Qwen3.5-4B-Q4_K_M.gguf` 2.55 GB,
`mmproj-F16.gguf` 0.86 GB, `ggml-base.en.bin` 0.14 GB. **llama runtime ~1.13 GB** (`ggml-cuda.dll`
565 MB + `cublasLt64_12.dll` 474 MB + `cublas64_12.dll` 100 MB + rest). **whisper ~51 MB**
(mostly `libopenblas.dll`). **keel ~13 MB**. Root: `Backrooms.exe`, `dxcompiler.dll`+`dxil.dll`,
`RUN.cmd`, `README.txt`, `CREDITS.txt`, `licenses\NOTICE.txt`, `logs\`.

## 9. keel.lock enforced? — NO (spec only)

Repo-wide grep for `keel\.lock|keel-serve|read.*lock|parse.*lock` in `*.cpp/h/rs/ps1`: only
`main.cpp:644,671,674` (passing string `"keel.lock"` as CLI arg + comment) and `package.ps1:76-77,99`
(copying). **No code opens/parses/validates keel.lock.** `gate.ps1` treats keel as black box —
probes `:7071` via `--director-probe`, checks `keel_ok` metric. The "invariants/pins for the
freeze check" (`keel.lock:85-87`: `golden.set_sha256`, `thresholds`) is a contract **keel-serve**
enforces against its own ledger — Backrooms has no part. Header (`keel.lock:1-4`): "The substrate
resolver (kernel::lifecycle) reads this" — that resolver lives inside keel-serve.

## 10. Version/hash pinning

Version IDs pinned; hashes NOT — every `sha256` is `TODO`. Version pins in `keel.lock`:
`keel: { version: 0.2.0, stage: stage0 }` (`:6`); `llama_cpp: { build: b9627, cuda: "12.4" }`
(`:54`); model IDs by name (`:13,17,19,25,33,38`). **Hash pinning absent** — every `sha256`/
`mmproj_sha256` literally `TODO` (`:14,15,20,26,34,39`); `golden.set_sha256: TODO` (`:86`).
`keel.lock:4` states "Hashes are the reproducibility contract" — contract currently unfulfilled.
`package.ps1` does **no** hash verification (`:85` just `Copy-Item`; `Need()` at `:40` only
checks path existence). Bundle's whisper model (`ggml-base.en.bin`) differs from keel.lock's
canonical `whisper-large-v3-turbo` — ships the lock's *fallback*.

**Console-popup sources (integration goal):** current code **already eliminates console popups**
(Session 36 / ADR-076). Old popup source was `C:\keel-sidecar-7071\start-all.cmd` (powershell
`Start-Process -WindowStyle Minimized` → visible minimized windows). Replaced by
`launch_hidden_in_job()` (`main.cpp:612-631`): `CreateProcessW` `CREATE_NO_WINDOW |
CREATE_SUSPENDED`, stdio → `logs\*.log`, `AssignProcessToJobObject` + `ResumeThread`, job
`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` (`:659`). Both llama-server + keel-serve go through this
(`:669,674`). whisper-cli `CreateProcessA(... CREATE_NO_WINDOW ...)` (`:3303`). **No DOS/console
windows pop in the shipped flow** — from the sidecars. (The remaining console is the GAME's own
CONSOLE-subsystem exe — see F.)

---

# F. Console-window source audit

## 1. PRIMARY OFFENDER: the main exe's own console (CONSOLE subsystem)

Three independent facts confirm:
- **Entry point `int main(int argc, char** argv)`** — `app/src/main.cpp:5241`. No `WinMain`/
  `wWinMain` anywhere (grep zero hits).
- **No subsystem ever set.** `app/CMakeLists.txt:19` declares `add_executable(backrooms
  src/main.cpp src/hud.cpp version.rc)` with no `WIN32_EXECUTABLE`, no `/SUBSYSTEM:`, no
  `LINK_FLAGS`. Top-level `CMakeLists.txt` sets global flags but nothing about subsystem/entry.
  Repo-wide grep `SUBSYSTEM|WIN32_EXECUTABLE|LINK_FLAGS|link_options` = **no matches**.
- **MSVC default**: `main` + no `/SUBSYSTEM:` → `/SUBSYSTEM:CONSOLE`. Console allocated by Windows
  the moment process starts, before `main()`. Owned by game process itself = visible black box.
  Heavily uses `std::printf`/`fprintf(stderr,...)` (`main.cpp:406-411,1055-1073,1207-1213,1249,
  1847`) → console actively written, can't be transient. Every interactive entry (`run_play`
  `:5289`, `run_screensaver` `:5308`) + every diagnostic writes to it.

## 2. CreateProcess sites — BOTH correctly pass CREATE_NO_WINDOW

Two process-spawning sites (repo-wide grep `CreateProcess|ShellExecute|WinExec|_popen|system(`
found only these):

| # | Site | File:line | Flag | Verdict |
|---|------|-----------|------|---------|
| A | `launch_hidden_in_job` | `main.cpp:622-625` | `CREATE_NO_WINDOW \| CREATE_SUSPENDED` | Correct |
| B | `whisper_transcribe` | `main.cpp:3303-3304` | `CREATE_NO_WINDOW` | Correct |

Site A (`:612-631`): `STARTUPINFOW` stdio → log file (`STARTF_USESTDHANDLES`, `:620`),
`CreateProcessW(... CREATE_NO_WINDOW | CREATE_SUSPENDED ...)` (`:624`), children in kill-on-close
job (`AssignProcessToJobObject`, `:628`) then resumed. Site B (`:3295-3313`):
`CreateProcessA(... CREATE_NO_WINDOW ...)` (`:3303`), no stdio redirect but CREATE_NO_WINDOW
suppresses console. **No code path calls either without CREATE_NO_WINDOW** — both hard-code it.
Callers: `try_start_sidecar` (`:638`) → `launch_hidden_in_job` for llama (`:669`) + keel (`:674`),
called from `run_play`/`run_game` at `:694,770,1461,1654,1688,1708`; `whisper_transcribe` at
`:784,3338,3365,3412,3525,3720`. All covered. **Sidecars do NOT pop windows.**

## 3. No .bat/.cmd/.ps1 invoked at runtime

No `start-all.cmd` in repo (glob `**/start-all.*` nothing). Only `.cmd` are launchers:
- `dist\Backrooms\RUN.cmd` — `@echo off` / `start "" "%~dp0Backrooms.exe"` (3 lines), generated
  `package.ps1:123-126`. `start ""` briefly opens a transient cmd.exe host that runs the `start`
  builtin then exits — **can flash a console when launcher used**. Not involved when exe started
  directly.
- `runs\gate-m17-clean\RUN.cmd` — `start "" "%~dp0backrooms.exe" --game` (2 lines). Dev/CI artifact.

Grep `.bat|.cmd|.ps1|powershell|cmd.exe` in `app/src/main.cpp` = **no runtime invocations** —
every `.ps1`/`.cmd` reference is build/CI tooling or docs. Game never shells to a script
interpreter.

## 4. No system()/_popen/ShellExecute/WinExec

Repo-wide grep across `*.{cpp,h,hpp,c,cc}` for `\bsystem\s*\(|_popen|_wpopen|popen\s*\(|exec[lv]
|CreateProcess|ShellExecute|WinExec` = only the two `CreateProcess` sites in main.cpp + two
`WinHttpOpen` calls in `keel_client.cpp:37,124` (HTTP, not process). **Zero** `system()`,
`_popen`, `ShellExecute`, `WinExec`.

## 5. stderr/printf don't force console allocation

No `AllocConsole`/`FreeConsole`/`AttachConsole`/`GetConsoleWindow` anywhere (grep = only the
line-610 comment "Launch a console exe HIDDEN"). `printf`/`fprintf(stderr,...)` rely on the
console Windows already allocated due to CONSOLE subsystem (item 1) — they don't create a new
one. But they keep writing, so the console stays visible for process lifetime. **Confirms item 1
is root cause:** remove the console allocation and these prints become no-ops (or need redirection).

## 6. Debug vs release — subsystem identical

`BACKROOMS_RELEASE` (`CMakeLists.txt:56-59`) only defines `BR_RELEASE` (compiles out D3D12 debug
layer/DRED). Does NOT touch subsystem/entry. `CMakePresets.json` only preset `ninja-vcpkg`
(RelWithDebInfo, Ninja). `package.ps1:46-48` builds Release `-DBACKROOMS_RELEASE=ON`. Neither
sets subsystem. MSVC runtime static (`MultiThreaded[Debug]`, `CMakeLists.txt:21`). **Both debug
and release produce CONSOLE-subsystem exe.**

## Summary — complete list of console-window sources

1. **`backrooms.exe`'s own console window** — CONSOLE subsystem (default, since `main()` entry +
   no `/SUBSYSTEM:WINDOWS`). `app/src/main.cpp:5241` (entry); `app/CMakeLists.txt:19` (no
   subsystem); `CMakeLists.txt:1-103` (no subsystem global). **#1, overwhelmingly likely sole
   offender.** A windowed GUI exe with `main` entry must switch to `/SUBSYSTEM:WINDOWS` with
   `wWinMain` (or set `/ENTRY:mainCRTStartup` to keep `main`), or hide the allocated console.
2. **`RUN.cmd` launcher transient flash** — `dist/Backrooms/RUN.cmd:2` `start ""` — cmd.exe host
   can briefly flash before exiting. Only when launching via `.cmd`, not exe directly.
3. **CreateProcess sites — none leak a window.** `main.cpp:624` + `:3303` both suppress. All
   runtime children (llama-server, keel-serve, whisper-cli) hidden.
4. **No runtime .bat/.cmd/.ps1** invocations. No `system()`/`_popen`/`ShellExecute`/`WinExec`.
   No `AllocConsole`/`AttachConsole`/`GetConsoleWindow`.

**Investigation points to item 1 (and possibly item 2 if launching via RUN.cmd) as the source(s)
of the black window. Items 3-4 are clean.**

---

# G. package.ps1 + bundle layout map

## 1. What package.ps1 stages (file-by-file, with origin)

| Staged path | Origin | Built?/Copied? |
|---|---|---|
| `Backrooms.exe` | `build-release\bin\backrooms.exe` | Built (`BACKROOMS_RELEASE=ON`, `:46-52`); copied `:63` |
| `dxcompiler.dll` | Windows SDK — `Find-SdkDll` (`:31-39`) recurses `${env:ProgramFiles(x86)}\Windows Kits\10\bin` + `$env:ProgramFiles\Windows Kits\10\bin`, highest `\x64\` | Copied from SDK, `:66` |
| `dxil.dll` | Same SDK search | Copied, `:67` |
| `runtime\llama\llama-server.exe` | `C:\llama.cpp\llama-server.exe` | Copied, `:72` |
| `runtime\llama\*.dll` (ALL) | `Get-ChildItem C:\llama.cpp\*.dll` | Copied every DLL, `:73` |
| `runtime\keel\keel-serve.exe` | `C:\keel-sidecar-7071\keel-serve.exe` | Copied, `:76` |
| `runtime\keel\keel.lock` | `C:\keel-sidecar-7071\keel.lock` | Copied (vestigial C:\ paths), `:77` |
| `runtime\whisper\whisper-cli.exe` | `C:\whisper.cpp\whisper-cli.exe` | Copied, `:80` |
| `runtime\whisper\*.dll` (ALL) | `Get-ChildItem C:\whisper.cpp\*.dll` | Copied every DLL, `:81` |
| `models\Qwen3.5-9B-Q5_K_M.gguf` | `C:\models\` | Copied, `:24,85` |
| `models\mmproj-F16.gguf` | `C:\models\` | Copied, `:24,85` |
| `models\Qwen3.5-4B-Q4_K_M.gguf` | `C:\models\` | Copied, `:24,85` |
| `models\ggml-base.en.bin` | `C:\models\` | Copied, `:24,85` |
| `CREDITS.txt` | Generated via `Backrooms.exe --credits` | Generated, `:88` |
| `licenses\NOTICE.txt` | Hardcoded here-string | Generated, `:89-104` |
| `README.txt` | Hardcoded here-string | Generated, `:106-121` |
| `RUN.cmd` | Hardcoded here-string (`start "" "%~dp0Backrooms.exe"`) | Generated, `:123-126` |
| `logs\` | Empty dir | Created `:58` |
| `licenses\` | Empty dir (NOTICE.txt placed) | Created `:58` |

Stage tree wiped/recreated `:57-60`. 4 model filenames = `$MODEL_FILES` (`:24`). Nothing downloaded —
every binary/model copied from `C:\` dev-box install.

## 2. Exact folder layout

```
dist\Backrooms\
  Backrooms.exe              <- ROOT (renamed from lowercase)
  dxcompiler.dll             <- ROOT, beside exe
  dxil.dll                   <- ROOT
  RUN.cmd                    <- ROOT (double-click launcher)
  README.txt, CREDITS.txt    <- ROOT
  runtime\
    llama\   llama-server.exe + ALL *.dll from C:\llama.cpp   (34 files)
    keel\    keel-serve.exe + keel.lock
    whisper\ whisper-cli.exe + ALL *.dll from C:\whisper.cpp
  models\
    Qwen3.5-9B-Q5_K_M.gguf
    mmproj-F16.gguf
    Qwen3.5-4B-Q4_K_M.gguf
    ggml-base.en.bin
  licenses\  NOTICE.txt
  logs\      (empty; servers' stdout/stderr land here at runtime)
```
Exe at ROOT, not `bin\`. `runtime\{llama,keel,whisper}\` + `models\` siblings of exe — load-bearing
for the resolver (§9).

## 3. Bundle size + breakdown

**Total ~10.9 GB, 55 files** (computed `:129-130`, recorded `SESSION_LOG.md:21-22`,
`DECISIONS.md:449`).
- `runtime\llama`: ~1.1 GB (CUDA DLLs dominate — `ggml-cuda.dll` ~539 MB, `cublasLt64_12.dll`
  ~452 MB, `cublas64_12.dll` ~95 MB; 34 files).
- `runtime\whisper`: ~51 MB (mostly `libopenblas.dll` ~48.8 MB).
- `runtime\keel`: ~13 MB (self-contained Rust).
- `models\`: ~9.9 GB (9B-Q5 ~6.13 GB + mmproj ~0.86 GB + 4B-Q4 ~2.55 GB + whisper base.en ~0.14 GB).
- exe + dxc pair + text: negligible.
**Models ≈ 9.9 GB of 10.9 GB (~91%).**

## 4. Final deliverable

**Store-mode ZIP**, `:133-138`. Output `dist\Backrooms-portable.zip` (`:29`). Via
`[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip,
[CompressionLevel]::NoCompression, $true)` (`:137`). `NoCompression` deliberate (GGUFs
incompressible). `-StageOnly` (`:131`) skips zip. `-SkipBuild` (`:12,43`) reuses existing
build-release exe. **No installer, no MSI, no MSIX.** Proposal: "no installer (per operator
decision)" (`DECISIONS.md:218`). Distribution: itch.io via `butler push` — but `:139` is just a
note; script never invokes butler, only zips. Butler push = manual operator step.

## 5. DLLs staged + sources

**Exe root:** `dxcompiler.dll`, `dxil.dll` (Windows SDK via `Find-SdkDll` `:31-39,66-67`). Raster
doesn't need them (`PACKAGING_PROPOSAL.md:108-109`). **No MSVC runtime DLL** — CRT statically
linked (`CMakeLists.txt:19-22` `MultiThreaded`/`MultiThreaded$<CONFIG:Debug>:Debug` to match
`x64-windows-static`, `package.ps1:47`). **No `nvcuda.dll`** — driver-owned, non-redistributable.

**runtime\llama\** (all `*.dll` blind from `C:\llama.cpp`, `:73`): `ggml-cuda.dll`, `cublasLt64_12.dll`,
`cublas64_12.dll`, `cudart64_12.dll`, `llama-server-impl.dll`, `llama-common.dll`, `llama.dll`,
`mtmd.dll` (vision), `ggml-base.dll`, `ggml.dll`, `libomp140.x86_64.dll`, `ggml-cpu-*` arch variants.
CUDA + ggml DLLs from dev-box.

**runtime\whisper\** (all `*.dll` from `C:\whisper.cpp`, `:81`): `whisper.dll`, `ggml-base.dll`,
`ggml-cpu.dll`, `libopenblas.dll`. CPU-only.

**Divergence from proposal:** `PACKAGING_PROPOSAL.md:54-65` §3 specified curated allowlist (~12
specific llama DLLs, "skip ggml-cpu-* arch variants and bench/quantize/cli tools"). Actual script
does NOT implement allowlist — copies every `*.dll` (`:73, :81`). Comment `:70` rationalizes as
"guaranteed-complete."

## 6. Version/commit stamping + code signing

**Version static, hardcoded, no commit.** `app\version.rc:10-11,24,28` embeds `FILEVERSION
2,0,0,0` / `PRODUCTVERSION 2,0,0,0` / `"FileVersion" "2.0.0.0"`. No git-commit injection — `git
rev-parse` never called. `CMakeLists.txt:4` `project(... VERSION 0.0.0)` not wired into rc. README/CREDITS
carry no version. **Code signing: none.** No signtool/cert/authenticode. Icon procedural (generated
by `tools/icongen`, `app/CMakeLists.txt:10-18`; `backrooms.ico` gitignored, ADR-043
`DECISIONS.md:219`).

## 7. README installation requirements

Two READMEs:
- **Top-level `C:\backrooms\README.md`** (dev/repo): `:46-49` "Windows 10/11 x64; VS 2022 + Windows
  11 SDK; CMake ≥ 3.28, Ninja; NVIDIA RTX GPU for path-traced mode." No VRAM/driver AI minimums.
  `:99` still says "no model is bundled" — **outdated post-ADR-076.**
- **In-bundle `README.txt`** (generated `package.ps1:106-121`, user-facing, authoritative):
  `:115-117` "Windows 10/11 x64. A Direct3D-12 GPU. For the local AI Director/voice: an NVIDIA RTX
  GPU + recent driver (>= 551.61). AI auto-selects by VRAM (>= 12 GB -> 9B + vision; less -> 4B).
  With no/old GPU the game still plays -- the AI simply stays quiet." `:118` "First launch spends
  ~20-40 s loading the model."
So: OS Win10/11 x64; any D3D12 GPU to play; AI needs RTX + driver ≥ 551.61 + (≥12 GB VRAM vision
tier, else 4B text). No CPU/RAM/disk figure. 551.61 floor from `PACKAGING_PROPOSAL.md:112,114`.

## 8. No-AI / small build variant — NONE

`package.ps1` takes only `-StageOnly` + `-SkipBuild` (`:12`) — no `-NoAI`/`-RasterOnly`/`-ModelsOnly`.
Always stages full 10.9 GB (4 models + both runtimes). Proposal's model-strategy table
(`PACKAGING_PROPOSAL.md:122-135`) discusses 9B vs 4B tiers (which models to ship), not a model-less
variant — and script ships BOTH tiers (auto-selected at runtime by VRAM).

## 9. How exe finds runtime — exe-relative via GetModuleFileNameW

`app\src\main.cpp:554-587`: `exe_dir_w()` (`:559-564`) `GetModuleFileNameW` + strip filename → exe
dir with trailing slash. `bundled_w(rel, fallback)` (`:576-579`) returns `exe_dir + rel` if exists
(`GetFileAttributesW != INVALID_FILE_ATTRIBUTES`), **else** `C:\` dev fallback. `try_start_sidecar()`
(`:638-676`) resolves every runtime path through `bundled_w`: llama `:643`, keel `:644`, 9B `:645`,
4B `:646`, mmproj `:647`, log dir `:648`. Whisper defaults (`:586-587`). **No registry, no env vars,
no `C:\` in shipped path** — `C:\` fallback only fires when bundled file absent (dev box). On bundle,
bundled path always exists, `C:\` never touched (`main.cpp:556-558`, ADR-076 `DECISIONS.md:447`).
Servers launched hidden + job-managed (`launch_hidden_in_job`, `:612-631`):
`CREATE_NO_WINDOW | CREATE_SUSPENDED`, `AssignProcessToJobObject`, `ResumeThread`, stdio →
`logs\*.log`, `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` (`:655-662`). llama `:8080` first; keel probes+
reuses (`:663-675`).

## 10. Fragile/manual about current packaging

1. **Hardcoded `C:\` source paths** (`:19-22`): `$LLAMA`, `$WHISPER`, `$KEEL`, `$MODELS`. Dev-box
   literals, no override/env-var fallback, no existence check until `Need()` (`:40`). Throws
   mid-staging on any machine without exactly these four installs.
2. **Manual model placement** at `C:\models` with exact filenames (`:24`). Hardcoded list; renamed/
   missing aborts (`Need` `:85`). Open decision #1 (`PACKAGING_PROPOSAL.md:178-185`) wanted 4B tier
   = Qwen3-VL-4B + mmproj (vision on small cards) but dev box only has text-only 4B-Q4 + 9B mmproj
   — small tier ships **text-only**, 4B-mmproj never fetched. Unresolved.
3. **Blind `*.dll` copy** (`:73, :81`) instead of curated allowlist (`PACKAGING_PROPOSAL.md:54-65`).
   Whatever sits in `C:\llama.cpp\*.dll` + `C:\whisper.cpp\*.dll` ships — spare CPU backends,
   non-deterministic across dev-box states. Could MISS a DLL if dev install incomplete (no
   completeness check; comment `:70` hopes "guaranteed-complete").
4. **`Find-SdkDll` recursive full-tree scan** (`:31-39`) of Windows SDK `bin` for `dxcompiler.dll`/
   `dxil.dll`, "highest-sorted `\x64\` hit." Slow, order-dependent; multiple SDKs → silently picks
   one by string sort, not the one exe built against.
5. **`keel.lock` ships with vestigial `C:\` paths** (`:77`). Safe only because keel probes `:8080`
   and never reads `llama_cpp.path`/`models_dir` in shipped flow — but lock NOT trimmed to
   probe-only/relative substrate as proposal recommended. Latent footgun if keel's resolver order
   changes.
6. **No verification step in script.** Stages + zips but does NOT run staged exe, does NOT check
   `git grep "C:\\"` in app/ (acceptance #5, `PACKAGING_PROPOSAL.md:12`), does NOT confirm bundle
   `C:\`-free. "Structure-verified" = manual eyeball. Final on-GPU smoke **PENDING/blocked**
   (`SESSION_LOG.md:28-39`, `DECISIONS.md:449`) — dev GPU stuck TDR, bundle never run end-to-end
   from clean folder. Code committed + ctest passes, but bundle unproven on real hardware.
7. **No butler automation.** `:139` just a `Write-Note` hint; itch.io upload manual operator step.
8. **Version static `2.0.0.0`** (`version.rc:10-11`) — every build same version; no way to tell two
   bundles apart.
9. **Repo README stale** w.r.t. bundle: `README.md:99` "no model is bundled"; `:77,82-84` describe
   old ~7 MB DXC-only zip (ADR-043/M17, `DECISIONS.md:219-221`), not 10.9 GB ADR-076 bundle.
10. **`dist\` + `build-release\` gitignored** (`.gitignore:37-38`) — bundle throwaway local artifact,
    not reproducible from repo alone (need four `C:\` installs).

## Small no-AI build — what it takes

**At exe level: already trivial / gracefully handled.** Game is enhancement-only (INV-6):
`--no-director` default; `try_start_sidecar()` (`main.cpp:638-676`) fire-once with explicit
graceful-no-op guards — checks `GetFileAttributesW(llamaExe) != INVALID_FILE_ATTRIBUTES` (`:665`)
and `service_up` (`:664,672`) before launching; if no runtime/model → silently skips, game plays
on deterministic AI. Raster renderer doesn't need `dxcompiler.dll`/`dxil.dll` (only DXR `LoadLibrary`s
them; raster unaffected; DXR toggle falls back to raster if missing — `PACKAGING_PROPOSAL.md:108-109`).
CRT statically linked → exe has **no external DLL dependency at all** for raster mode. ADR-043
(`DECISIONS.md:219-221`): pre-AI M17 bundle was exe + dxc pair at **~7 MB**. On clean user machine
with no-AI build, `bundled_w` returns `C:\` fallback string when bundled file absent, but that `C:\`
path also won't exist → `GetFileAttributesW` fails → launch skipped → degradation works without any
`C:\` install. **A single `Backrooms.exe` (release) dropped in a folder = functional raster-only
game.**

**What still needs work to produce cleanly:**
- `package.ps1` has **no `-NoAI`/`-RasterOnly` switch** (`:12`) — today you'd run `-StageOnly` then
  manually delete `runtime\` + `models\`, or bypass script and just `cmake --build` + copy exe. No
  one-flag small-bundle path.
- Generated `README.txt` (`:106-121`), `CREDITS.txt` (`:88`), `licenses\NOTICE.txt` (`:89-104`) are
  **hardcoded to full-AI bundle text** — mention VRAM tiers, 20-40 s model load, Qwen/llama/whisper/
  CUDA licensing. A no-AI build wants a different (shorter) README/NOTICE → script's text-generation
  blocks need a no-AI branch.
- Whether to still ship `dxcompiler.dll`/`dxil.dll` is a choice: include (~few MB) to keep DXR
  available, or omit for pure-raster micro-build (exe handles absence). Either works, no code change.

**Net:** the *runtime* supports a no-AI build for free (graceful degradation proven + wired); the
*packaging script* does not — needs a small amount of work (a flag + README/NOTICE variant) to
produce a clean, self-describing small bundle rather than a manually-pruned full one.
