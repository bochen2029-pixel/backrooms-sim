# MODULE: director (L4)

**Purpose.** Hosts llama.cpp on its own thread/process. Pipeline:
WandererSummary JSON -> Directive JSON, schema-validated; invalid output is
rejected + logged, never partially applied. Renders The Voice captions;
caches Wanderer Notes per location hash. Kill switch: `--no-director` (INV-6).

**Depends on:** `core` (contract only — directives enter as Event Log entries,
INV-5), `telemetry`.

**Public surface (M0).** `director/director.h` — identity stub.

**Planned.** llama.cpp host + schema validation + note cache + eval suite (M11).

**Contracts:** `contracts/director_v1/` (wanderer_summary + directive schemas).

**Status:** M0 stub. (llama.cpp dependency = future ADR.)
