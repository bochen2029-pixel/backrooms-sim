# MODULE: app (L2)

**Purpose.** Win32 shell, config (`config.toml`), composition root, CLI flags
(`--headless`, `--replay`, `--no-director`, `--render-wav`). Maps typed module
errors to process exit codes for the gate scripts (ARCHITECTURE.md §7).

**Depends on:** all modules. Nothing depends on `app`.

**Public surface (M0).** `backrooms` console executable — prints the build
banner, exercises the core RNG, links the full DAG, exits 0. `--version` flag.

**Planned.** Win32 window + D3D12 device + `--headless --frames N --out` (M1),
`config.toml` + flag/config mirroring (M12), noclip intro + photo mode (M12).

**Status:** M0 stub executable.
