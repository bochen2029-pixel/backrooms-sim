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
  (schema + lint: bounded enums/ranges, sanitised captions; pure/total); plus
  `render_prompt(summary)` (WandererSummary → the directive-schema instruction).
- `director/keel_client.h` (M11) — `keel_complete(host, port, prompt)` → `KeelResponse`
  (WinHTTP POST to the KEEL sidecar's OpenAI egress, forcing local tier; never throws,
  graceful no-op on failure).
- `director/json.h` — minimal dependency-free JSON reader + `escape` (directive +
  KEEL envelope).

**Contracts:** `contracts/director_v1.h` (`WandererSummary` + `Directive` +
`DirectorEvent`).

**Status:** M11 phase b — contract + validator + JSON reader (10 unit tests) + the
**live KEEL client**. `app --director-probe` validated end-to-end against the sidecar:
WandererSummary → local Qwen3.5-9B → schema-valid Directive (e.g. `{"type":"sound",
…}`), tier local, $0. Async sim host + The Voice + note cache + eval suite next.
