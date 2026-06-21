# 02 — RTX Mode Crashes Immediately (Root-Cause Analysis)

> Thread: second pivot. Read-only investigation — no edits made.
> Repo state: HEAD = 90410c3 (ADR-076).
>
> This supersedes 01's "TDR-degraded GPU" hypothesis: the operator reported a **hard reboot**
> with **other games working fine**, which rules out driver state and confirms a code bug in
> the live RT path on a healthy GPU.

## The complaint

The game crashes immediately when RTX/ray tracing is turned ON (in settings, or resuming in RT
mode). Raster mode works fine. Crashes persist after a hard reboot; other games work fine on
the same Win11 + RTX box.

## What the reboot ruled out

- Hard reboot + other games fine → **GPU hardware and driver are healthy.** Eliminates the
  TDR-degraded-driver hypothesis (my earlier leading candidate).
- Raster mode works → the raster D3D12 device comes up clean. ADR-076's `device_usable` probe
  + WARP fallback did their job.
- RT crashes immediately → the failure is in code that runs **only** in the RT path, on an
  otherwise-healthy machine. **Genuine code bug in the DXR interactive path.**

## The structural fact that explains everything

The interactive RT path creates **two independent `ID3D12Device`s on the same physical GPU**:

1. `renderer.init_windowed(...)` at `main.cpp:1379` — the **raster device** + swapchain + window.
   Always created first, at startup.
2. `dxr->init(rw, rh)` at `main.cpp:1012` (and `:1768` for the resume path) — a **separate
   `ID3D12Device5`**, created *lazily, only when RT is toggled on*, while the raster device +
   swapchain are already live.

This dual-device design is intentional (ADR-019 — DXR owns its own device). **But the M9 gate
never exercises it.** `Invoke-GateM9` (`scripts/gate.ps1:894`) runs `--dxr-probe`,
`--dxr-depth`, `--dxr-pt`, `--dxr-denoise` — every one constructs a *single* `DxrRenderer` in a
fresh headless process with **no raster device, no swapchain, no window, no competing D3D12
device.** The gate proves "DXR works in isolation." It does **not** prove "DXR works when
created on top of a live raster device + swapchain." That's the gap, and that's where it crashes.

Session 36 (ADR-076) admits: *"the final on-GPU bundle render+vision smoke + gates M30/M9 are
PENDING a GPU reboot."* The interactive RT path was **never re-verified** after the last two
commits (`2c5642c` denoiser — the last RT-logic change; `90410c3` ADR-076 — device-creation
surroundings).

## Candidate root causes (narrowed to the live-RT path)

### Candidate A (most likely): DXR device created on a GPU already saturated — `CreateStateObject`/AS build faults

When RT toggles on at runtime, the raster device + swapchain + the ADR-076 auto-started LLM
sidecar (9B + mmproj in VRAM) are all already holding the GPU. The DXR `init()` then asks for:
a `Device5`, four UAV textures, a root signature, DXIL compile, and a `CreateStateObject`
(`dxr.cpp:377`) — all on a second device on the same saturated GPU. `CreateStateObject` and the
lazy `build_scene` TLAS/BLAS builds are the heaviest first-touch operations. They're checked
with `FAILED()`, so a *clean* failure → raster fallback (no crash). **A crash here means the
failure isn't returning cleanly — a driver/device fault inside `CreateStateObject` or the AS
build that propagates as a hard fault before the HRESULT is checked.** The complete absence of
`GetDeviceRemovedReason()` checks in the DXR path (verified: zero occurrences) means a
device-removed-during-heavy-work fault has no graceful path.

### Candidate B (likely, cleanest fix): DXR device-creation has none of ADR-076's hardening

The DXR adapter loop (`dxr.cpp:242-250`) has **no `device_usable()` probe and no WARP fallback**
— the exact hardening ADR-076 added to raster (`renderer.cpp:214-228`, `:267`). So a borderline
DXR device (passes `CreateDevice`, fails on the first `ALLOW_UNORDERED_ACCESS` resource or
`CreateStateObject`) is accepted and crashes where raster would have probed+fallen-back.

### Candidate C (possible): per-frame `update_creature` TLAS realloc drops a buffer the GPU is still reading

In `update_creature` (`dxr.cpp:1088-1097`), every frame the TLAS scratch + result are
**reallocated** (`d.tlas = make_buffer(...)`) and rebuilt with `PREFER_FAST_TRACE` (no
`ALLOW_UPDATE`). The previous frame's `d.tlas` is dropped (ComPtr release) while the *previous*
`render_pt_frame`'s `DispatchRays` — which bound `d.tlas->GetGPUVirtualAddress()` at `:1197` —
may not have fully completed. `render_pt_frame` binds the TLAS by raw GPU virtual address with
no lifetime tracking. A use-after-free of the TLAS buffer is a classic
immediate-crash-on-healthy-GPU signature. Crashes on the **first or second RT frame**, not at
init.

### Candidate D (less likely): `dxcompiler.dll` not beside the exe in the bundle

ADR-076 made runtimes exe-relative but the DXC loader (`dxc.cpp:15-57`) still only searches the
DLL path + the Windows SDK — **no exe-relative probe**. If running the packaged bundle without
the SDK, `dxc.available()` is false → `init` returns `"dxcompiler.dll unavailable"` → graceful
fallback to raster (not a crash). *Unless* the bundle ships a mismatched `dxcompiler.dll` that
loads but emits bad DXIL → `CreateStateObject` faults.

## The decision tree (read-only, before any fix)

1. **`Backrooms.exe --dxr-probe`** (`main.cpp:4410`). Single-device, no raster, no window.
   - `dxr_ready: 0` → Candidate D or B. Read `detail:`.
   - `dxr_ready: 1` → DXR works in isolation → crash is the dual-device live path → A or C.
2. **`Backrooms.exe --dxr-pt --seed 1 --pose 1 --spp 64 --width 640 --height 360 --out probe.png`**
   (`run_dxr_pt`, `main.cpp:~4496`). Single-device headless path-traced render.
   - Succeeds → full `init` + `build_scene` + `render_pt_frame` + denoiser work in isolation →
     confirms A/C (the live dual-device path is the only difference).
   - Crashes → the bug is in DXR code itself, reproducible headless → C (or a PSO/shader fault).
3. **Check the bundle for `dxcompiler.dll` + `dxil.dll`** beside the exe. Absent → D.

## The fix set (proposed, not applied)

| # | Fix | LOC | Candidate(s) |
|---|---|---|---|
| 1 | Port `device_usable()` + WARP fallback to DXR init (`dxr.cpp:242-250`) | ~15 | A, B |
| 2 | `GetDeviceRemovedReason()` after every `wait_idle()` (8 sites) + check `WaitForSingleObject` return (`:214`) | ~25 | A, C |
| 3 | Stop reallocating the TLAS every frame in `update_creature` — `ALLOW_UPDATE` + `UpdateTlas`, or stable result buffer | ~30 | C |
| 4 | Add the interactive dual-device RT path to the M9 gate — windowed `--game --rt --auto-play --frames N` smoke | ~40 (gate) | A, C (regression) |
| 5 | Ship `dxcompiler.dll` + `dxil.dll` beside exe + exe-relative probe in `DxcCompiler` | ~15 | D |
| 6 | Surface HRESULT + removed-reason in DXR error strings (like raster's `renderer.cpp:1132`) | ~10 | A, B, C (diagnosis) |

**Minimum to stop the crash:** 1 + 2 + 6 (probe + removed-reason handling + diagnosable errors),
then 4 to keep it from regressing. **~50 LOC + one gate addition.** TLAS-lifetime fix (#3)
warranted regardless (latent crash + per-frame perf cost).

## The honest summary

The reboot ruled out TDR afterimage. This is a real bug in the **interactive RT path**, and it
shipped because **the M9 gate tests DXR only in isolated single-device headless mode — it never
tests the live path where a raster device+swapchain is already running and a second DXR device
is created on top.** The two most probable faults: (A) the DXR device faults under contention
with no `GetDeviceRemovedReason` handling, and (C) `update_creature` reallocates the TLAS every
frame while `render_pt_frame` binds it by raw GPU virtual address (use-after-free). The denoiser
commit (`2c5642c`, last RT-logic change) and ADR-076 (device-creation surroundings) are the
perturbations most likely to have pushed a previously-tolerant init over the edge.

**Do first, no code:** `--dxr-probe` and `--dxr-pt --spp 64 --out probe.png` headless. If both
succeed, the crash is conclusively the live dual-device path (A/C) and the fix is #1+#2+#3+#4.

## Key file:line references

- DXR adapter loop (no probe, no WARP): `render_dxr/src/dxr.cpp:242-250`
- Raster `device_usable` probe (the model to copy): `render_d3d12/src/renderer.cpp:209-228`, `:267`
- `wait_idle` (no removed-reason check): `render_dxr/src/dxr.cpp:209-216`
- 8 `wait_idle` call sites: `dxr.cpp:431,900,1016,1102,1160,1242,1260`
- `CreateStateObject` (heavy first-touch): `dxr.cpp:377`
- `update_creature` per-frame TLAS realloc: `dxr.cpp:1088-1097`
- TLAS raw-address bind: `dxr.cpp:1197`
- DXC loader (no exe-relative probe): `render_dxr/src/dxc.cpp:15-57`
- Interactive RT init sites: `main.cpp:1012`, `:1768`
- M9 gate (single-device only): `scripts/gate.ps1:894`
- Session 36 (unverified RT): `docs/SESSION_LOG.md` top entry
