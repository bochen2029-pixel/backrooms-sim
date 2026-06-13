# MODULE: core (L1)

**Purpose.** Deterministic simulation core: fixed-timestep tick loop, seeded
RNG, wanderer kinematics, capsule collision, WorldState + per-tick hashing,
Event Log consumption. This is the determinism oracle for the entire build.

**Depends on:** nothing. (`core` is the root of the dependency DAG.)

**Invariants owned here.**
- INV-1 Determinism — WorldState hash after N ticks is a pure function of
  (WorldSeed, input stream, Event Log). No wall-clock, no unseeded RNG, no
  nondeterministic float paths. Compiled `/fp:strict`; tick is single-threaded.
- INV-5 Isolation — zero includes from render / audio / director / OS UI.
  Enforced mechanically by `scripts/checks/check_core_isolation.ps1`.

**Public surface (M0).**
- `core/rng.h` — `Pcg64`, the canonical PCG64 (XSL-RR 128/64) generator.
  Seeded, portable, 128-bit state exposed for hashing.
- `core/version.h` — version banner shared by all modules/tools.

**Planned (later milestones).** Tick loop + WorldState + hashing (M2),
capsule-vs-AABB collision with sliding (M2), input replay (M2), Event Log
consumption + directive application (M11).

**Contracts produced:** `contracts/world_view_v1.h` (M2+),
`contracts/replay_v1.h` (M2), `contracts/audio_events_v1.h` (M6).

**Status:** M0 — RNG + version only; tick loop pending M2.
