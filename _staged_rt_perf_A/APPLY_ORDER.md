# RT performance — staged changeset bundle (apply order)

Three independent, drift-robust changesets are staged to make RT playable + presentable. All are STAGED only —
apply by anchor-matching against the CURRENT code (never file-swap), after the other instance is done + the
operator approves. Tag `pre-rtperf` before starting (rollback anchor).

Grounded in `docs/RT_PERF_PLAN.md`. Each changeset has its own validation checklist; run it + commit GREEN after
each (so each is independently revertable). The order:

1. **`_staged_rt_perf_ghost/`** — the creature ghosting fix (material-7 history reject). **Do first.**
   Independent, **golden-bit-identical**, lowest risk, highest *visual* impact (kills the "quantum-superposition"
   smear = the "unpresentable" complaint). Pure PT-shader change.

2. **`_staged_rt_perf_A/`** (CHANGESET.md) — kill the per-frame cross-device GPU→CPU→GPU readback (single-device:
   DxrRenderer reuses the raster `Device5`; present the PT output as a same-device texture). The biggest *structural*
   "slow" win. Self-contained — deliberately KEEPS `render_pt_frame`'s `wait_idle` so no cross-queue fence is needed.

3. **`_staged_rt_perf_B/`** — fold the denoise into one command list (−1 GPU stall/frame) + per-frame AS rebuilds →
   `PREFER_FAST_BUILD`. More "slow". A-compatible (keeps the final `wait_idle`).

**Then MEASURE** (PIX / in-game timing — RT_PERF_PLAN Step 0). If still slow, the deferred bigger levers (documented
in `_staged_rt_perf_B/CHANGESET.md` §DEFERRED):
- **B4 — N frames-in-flight** (the biggest pipelining win) — note: it removes the final `wait_idle`, so it must ALSO
  add a **cross-queue fence** to item A's `present_pt_texture` (raster queue waits on the DXR fence before sampling).
- **D — stochastic light sampling** (the dominant *ray* cost in open rooms) — NOT golden-safe; needs golden
  regeneration via `goldgen` + an ADR.
- **SVGF** — the proper temporal denoiser (fixes ghosting cleanly AND enables low-noise 1-spp during motion) — a milestone.

Apply each via `_staged_rt_perf_A/APPLY_PROMPT.txt`'s protocol (anchor-match, validate, log to CHANGE_AUDIT_LOG,
commit on green; revert on any M9 divergence / M30 debug-layer error — do not debug forward).
