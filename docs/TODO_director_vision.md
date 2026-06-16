# TODO — Director should SEE the player (VLM-grounded narration), not recite canned flavor

**Status:** filed for later (operator: "note all this as a todo and to be hashed out"). NOT started.

## The problem (operator, verbatim intent)
The in-game Director narration "might as well be fortune-cookie canned script that could have been
generated in DOS-DOOM days — it talks about dripping faucets that don't exist." It is **not using the
VLM capability of Qwen3.5 to SEE the player** and narrate things only knowable through sight (and thus
impossible pre-LLM). "Otherwise why even bother with LLM and VLM."

## Root cause (confirmed in code)
`run_game`'s Director sends **text stats only** — `br::director::render_prompt(WandererSummary)`, where the
summary is `{tick, seed, biome=INDEX, distance_m, dwell_seconds, route_loops, location_hash}`. The model never
sees anything; it free-text-completes "narrate a creepy liminal space" from numbers → confabulated tropes
(faucets, dust) ungrounded in the actual screen. It's a fancier flavor-text RNG.

## The key asset: the VLM pipeline ALREADY exists + works (M22 Shoggoth)
`br::director::keel_complete_vision(host, port, prompt, image_base64, 30000)` + the Qwen-VL `mmproj` projector
(loaded by start-all.cmd's `--mmproj`) are proven: the **Shoggoth** uses them (`run_shoggoth_vision_record`,
main.cpp ~2835) — render the creature POV at 384×216 → in-memory PNG (`stbi_write_png_to_func`) →
`app::base64_encode` → `keel_complete_vision(render_shoggoth_vision_prompt, b64)`. The Director is the ONLY LLM
consumer still on the text-stats path.

## The fix (proposed)
Point the proven vision pipeline at the **player's POV**: render/grab the player's view → base64 → Qwen-VL with a
"facility surveillance AI" prompt → narrate what it ACTUALLY sees (yellow corridor, doorway left, shaft ahead,
the entity behind you) + the wander context ("you've returned to this junction 3×"). Reuses M22 infra.

## Design decisions to hash out
1. **POV source.** RT mode (operator's `renderer=1`): `dxr->readback(rt)` already IS the player's frame (free to
   grab+downscale). Mode-independent alt: a dedicated 384×216 headless POV render each cycle (the M22 pattern).
   Lean: dedicated small render (uniform across raster/RT, isolated from the present path).
2. **Vision-only vs vision+context (hybrid).** Send the image, or image + a short situational line (distance,
   loops, level, entity-near). Hybrid fuses sight + memory ("you keep coming back here"). Lean: hybrid.
3. **Cadence.** VLM ~2–5 s/call (off-thread). Sparse ~25–30 s is atmospheric + hides latency. Optional faster
   reaction when a threat enters view (creature/shaft).
4. **Tone/role.** Frame as the facility surveillance intelligence narrating *your camera feed* — makes grounded
   observation land as uncanny.
5. **Anti-hallucination prompt.** "Describe ONLY what is visible; do not invent objects." Kills the faucets.

## Non-issues
- Determinism: the in-game Director is a non-gated live presentation path (like the creature brain); vision never
  touches the sim/replay goldens.
- Perf: off-thread at 25–30 s cadence → no frame hitch. Cost: $0 (local Qwen-VL).

## Caveats
- Qwen-VL 9B on a sparse frame (blank wall) is plainer; it shines on doorways/junctions/the creature/shafts.
- Narration lags the view a few seconds (the "surveillance feed" framing makes that feel intentional).

## Recommended first step (de-risk before building)
Run a quick **vision experiment**: render a handful of representative real POVs (corridor, junction, creature in
view, shaft) + send each to `keel_complete_vision` with a draft PA-narration prompt + read what Qwen-VL actually
says. ~20 min, zero commitment. Validates the payoff before the live-host plumbing.

## Open questions for the operator
- (a) sparse/atmospheric (~30 s) or more talkative?
- (b) pure observation, or also REACT to threats it sees (creature/shaft)?
- (c) run the validation experiment first?
