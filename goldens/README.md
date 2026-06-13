# goldens/

Reference artifacts (images, state hashes, WAV spectra, converged PT renders).

**INV-8 Golden integrity.** Goldens change **only** via the `goldgen` tool,
accompanied by a `docs/DECISIONS.md` entry in the same commit. Editing a golden
by hand to make a failing gate pass is forbidden. If a gate seems wrong, fix the
gate with an ADR — never the golden.

M0 has no committed goldens yet; the M0 gate fabricates its image pair at
runtime via `goldgen synth`. First committed goldens arrive in M1
(headless frame-0 PNG).
