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

**Public surface.**
- `core/rng.h` — `Pcg64`, canonical PCG64 (XSL-RR 128/64), seeded, portable.
- `core/math.h`, `core/aabb.h` — `Vec3` math + AABB overlap (deterministic).
- `core/world.h` (M2) — `WorldState` (wanderer kinematics + owned RNG + tick +
  odometer), fixed 120 Hz `tick`, `move_and_collide` (per-axis swept capsule-vs-
  AABB with sliding + substepping), the hardcoded `test_room`, `world_state_hash`,
  `wanderer_camera`.
- `core/replay.h` (M2) — `write_replay`/`read_replay` (replay_v1 on-disk format).
- `core/version.h` — version banner shared by all modules/tools.

**Planned (later milestones).** Event Log consumption + directive application
(M11), level transitions (M7).

**Contracts produced/consumed:** `contracts/world_view_v1.h`,
`contracts/replay_v1.h` (M2); `contracts/audio_events_v1.h` (M6).

**Status:** M2 — 120 Hz tick, walk camera, capsule-vs-AABB collision + sliding,
input replay, per-tick WorldState hash. Bit-exact across runs/processes.
