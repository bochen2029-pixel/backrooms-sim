# GLM Brainstorm Dump — Session 2026-06-17

This folder holds the full read-only analysis produced across one long session on the
C:\backrooms repo. Nothing here is code; nothing here was applied to the repo. It is
investigation + brainstorm + proposal, organized into three threads and one synthesis.

## Files

| # | File | What it is |
|---|------|------------|
| 00 | `00_INDEX.md` | This index. |
| 01 | `01_RTX_RENDERING_EFFICIENCY.md` | First pivot — the RTX path-tracer is noisy and slow. Diagnosis of why (temporal accumulation disabled in the interactive path, 3 sync GPU round-trips/frame, CPU readback stall, per-frame AS rebuild) + a tiered fix plan. Read-only. |
| 02 | `02_RTX_CRASH_ROOT_CAUSE.md` | Pivot — RT mode crashes immediately on a healthy Win11 + RTX box after a hard reboot. Root-cause analysis: NOT a TDR afterimage; the M9 gate never tests the live dual-device path; candidate faults A/B/C/D + a fix set + a decision tree. Read-only. |
| 03 | `03_AI_SHOGGOTH_BRAINSTORM.md` | Pivot — make a fully-embodied emergent AI Shoggoth (eyes/ears/voice/brain/body) reusing Qwen-VL, whisper, the formant TTS, and the existing brain/locomotion. Fanned out 4 agents to map the whole codebase, then synthesized a grounded architecture. Brainstorm only. |
| 04 | `04_AI_INTEGRATION_PACKAGING.md` | Pivot — the AI sidecars (llama/keel/whisper) pop DOS consoles on startup; integrate them into one unified, console-free, higher-caliber package. Fanned out 3 agents; found the console is the GAME's own CONSOLE-subsystem exe; proposed a tiered integration plan. Read-only. |
| 05 | `05_AGENT_FINDINGS_RAW.md` | The raw, lightly-edited output of the 7 fan-out subagents (4 for the Shoggoth, 3 for the packaging). Kept so the file:line citations in the other docs are traceable back to source. |

## How to read

- If you want the **rendering efficiency** thread: read 01.
- If you want the **RT crash** thread: read 02 (it supersedes part of 01's diagnosis — the
  reboot ruled out the TDR hypothesis).
- If you want the **creature design** thread: read 03, then 05 for the grounding.
- If you want the **packaging/integration** thread: read 04, then 05 for the grounding.
- The threads are independent; each was a hard pivot from the previous one.

## Status of every thread

- **01 (rendering efficiency):** proposed plan, not executed. The fix tiers are still valid.
- **02 (RT crash):** root-caused to a code bug in the live dual-device path; fix set proposed,
  not executed. The single most important next step is the `--dxr-probe` / `--dxr-pt` headless
  test to pin it to candidate A/C.
- **03 (AI Shoggoth):** brainstorm. The schema change is the load-bearing-risk increment;
  Explore is the proof mode; the dial + cages are the real craft.
- **04 (integration/packaging):** proposed plan. Tier 1 (console fix) is ~1 day and independent;
  the end-to-end bundle smoke test is a non-negotiable prerequisite to any of it.

## Conventions

- All file:line citations are against the repo state at session start (HEAD = 90410c3, ADR-076).
- "Read-only" = no edits were made to the repo during that thread; "brainstorm" = no code proposed.
- The agent fan-outs used the Explore subagent type; their raw output is in 05.
