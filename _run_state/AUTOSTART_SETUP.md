# BACKROOMS SIM — AUTOSTART setup (the ONE-TIME wiring that turns on the self-perpetuating loop)

After this one setup, the Backrooms build drives itself toward completion across sessions with **no
further handoff** — the same rig that builds KEEL (`C:\KEEL\_run_state\`), adapted to this project's
gate-as-oracle + `m<N>-green` tag discipline. Three parts: **(A)** auto-rehydrate (already wired),
**(B)** the unattended permission posture, **(C)** run the supervisor.

## A · Auto-rehydrate (already in place)
`.claude/settings.json` already runs `scripts/rehydrate.ps1` on the `SessionStart` sources
`startup|resume|compact` — so an interactive check-in **and** a post-compaction session auto-print the
brief (last tag/commits · tree status · newest `SESSION_LOG.md` entry). The autonomous supervisor does
**not** rely on this — it passes `_run_state/AUTOSTART.md` to `claude -p` directly — but it makes your
own interactive sessions self-orient too. Nothing to add. *(Optional later: have `rehydrate.ps1` also
echo "an autoloop run is active" when `.brstate\` is non-empty — not required.)*

## B · Unattended permission posture (your TRUSTED box only)
The supervisor runs `claude -p "<AUTOSTART>" --dangerously-skip-permissions` so a headless session can
build / `gate.ps1` / commit / tag / push without interactive prompts. This is **full autonomy** —
appropriate ONLY on your own trusted machine. It stays governed because **the inviolable guards live in
`AUTOSTART.md` + the Iron Rules + the gate, not the permission layer**: never hand-edit a golden or relax
a gate threshold (Iron Rule 6), revert-to-`m<N>-green` on regression (Iron Rule 2), determinism is
sacred (INV-1..8), new dep = an ADR (Iron Rule 8), secret-scan every diff, `C:\KEEL` is read-only, every
slice committed + tagged + pushed (fully auditable). For a tighter policy, pass an allowlist instead:
`pwsh -File scripts/autoloop.ps1 -ClaudeArgs '--allowedTools','Bash,Read,Edit,Write,Glob,Grep'`
*(caveat: a tool that isn't allowed will HANG a headless run — full-skip avoids that on a trusted box).*

## C · Run it
```
cd C:\backrooms
pwsh -File scripts\autoloop.ps1 -DryRun                     # first: verify wiring, spawn nothing
pwsh -File scripts\autoloop.ps1                             # up to 50 sessions; stop after 2 no-progress
pwsh -File scripts\autoloop.ps1 -MaxSessions 200 -StallLimit 3   # longer unattended run
```
**The loop:** spawn a fresh `claude -p` → it reconstitutes from `AUTOSTART.md` (→ `SESSION_LOG` →
`ROADMAP` → `MILESTONES` → memory; verify by artifact) → executes `ROADMAP.md` `[ ]` slices to ~90%
context → **gate-green → commit → `m<N>-green` → push** each → exits → the supervisor respawns → … until
`.brstate\DONE` (Phase IV complete → perpetual-polish, ROADMAP §4), `.brstate\STALLED` (only
operator-gated items remain — ROADMAP §5 ISSUES), a no-progress stall (HEAD unchanged `-StallLimit`
times), or the `-MaxSessions` cap. Full log: `runs\autoloop.log`; every action is also in `git` + the tags.

## Backrooms-specific wrinkles
- **Run it on the box with the RTX GPU** — the gates open real D3D12 windows / DXR / readback.
- **The supervisor keeps the KEEL sidecar up** (`C:\keel-sidecar-7071\start.cmd`, `:7071` — NEVER
  `:7070`) for the brain/vision/hearing gates (Phase IV: only M29's per-floor Shoggoth brain). If it's
  down, those gates **graceful-no-op** (never a false fail). `-SkipSidecar` opts out.
- **The gate is the oracle.** Unsupervised iteration is safe because `gate.ps1 -Milestone M<N>` asserts
  bit-exact determinism (record == replay, models offline), golden bit-identity, debug-layer
  cleanliness, frame-pacing / connectivity / memory bounds — a wrong step **cannot** tag green, and a
  regression reverts to the last green tag.

## What you do after this: nothing required
Glance — whenever you feel like it — at `docs/SESSION_LOG.md` (the trail) and `ROADMAP.md` §5 ISSUES
(the operator-only queue: the open-shaft soft-resolution feel, anything outward-facing). Resolving an
ISSUE + deleting `.brstate\STALLED` unblocks its slices on the next respawn. Until then the loop works
everything else. **You never do a manual handoff again** — until quota or power runs out.
