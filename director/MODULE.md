# MODULE: director (L4)

**Purpose.** The ambient game-master. Pipeline: `WandererSummary` → **KEEL sidecar**
(OpenAI-compatible HTTP egress; **ADR-038 — NOT embedded llama.cpp**) → `Directive`
JSON, schema-validated; invalid output is rejected + logged, never partially
applied. Renders The Voice captions; caches Wanderer Notes per location hash. Kill
switch: `--no-director` (INV-6). Directives are **presentation-layer only** in M11 —
they reach the sim solely as recorded Event-Log entries at a deterministic
`effective_tick` (INV-5) and never perturb the WorldState hash or chunk generation,
so INV-1/INV-2 stay provably intact. Replays consume the recorded log, not the model.

**Depends on:** `core` (contract only), `telemetry`. M11 adds a WinHTTP client (a
Windows system lib — **no vcpkg dependency**, like `dxc`/`dbghelp`) + a
dependency-free JSON reader. The sim's only third-party deps stay **Catch2 + stb**.

**Public surface.**
- `director/director.h` (M11) — `validate_directive(content)` → `DirectiveResult`
  (schema + lint: bounded enums/ranges, sanitised captions; pure/total). The KEEL
  HTTP client + async host land in phase 11b/11c.
- `director/json.h` — minimal dependency-free JSON reader (directive + KEEL envelope).

**Contracts:** `contracts/director_v1.h` (`WandererSummary` + `Directive` +
`DirectorEvent`).

**Status:** M11 phase a — contract + schema validator + JSON reader, 10 unit tests
green. KEEL sidecar verified live at `127.0.0.1:7071` (Qwen3.5-9B-Q5, local, $0).
Client + sim wiring + The Voice + note cache + eval suite next.
