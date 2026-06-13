# KICKOFF_PROMPT.md — Getting the autonomous build started

## One-time human pre-flight (the only manual part)

Do this once on the Windows machine, then the agent takes over.

1. **Install toolchain:**
   - Git for Windows (required by Claude Code on Windows)
   - Visual Studio 2022 Build Tools — "Desktop development with C++" workload + latest Windows 11 SDK (includes DXR headers)
   - CMake ≥ 3.28 and Ninja (both on PATH)
   - NVIDIA driver current; CUDA Toolkit can wait until M11 (llama.cpp build) but installing now avoids a stall later
2. **Create the repo:** unzip this starter into `C:\backrooms\`, then:
   ```
   cd C:\backrooms
   git init && git add -A && git commit -m "starter: canon + milestones"
   git remote add origin <your-remote-url>   # the remote IS the self-backup target
   git push -u origin main --tags
   ```
3. **Start Claude Code in the repo:** `cd C:\backrooms` → `claude`
4. **Grant autonomy:** in the session run `/permissions` and allow the Bash/Edit/Write operations the build needs within this project (or use your preferred auto-accept mode). The whole point is no approval prompts mid-milestone — set this once, deliberately.
5. Verify it loaded `CLAUDE.md` (ask: "summarize your Iron Rules").

## Session 1 prompt (paste verbatim)

```
Read docs/ARCHITECTURE.md fully, then the M0 section of docs/MILESTONES.md.

Execute Milestone M0 per the session protocol: produce the change manifest first
(files, tests, rollback plan, diff budget), then implement.

M0 scope reminder: CMake/Ninja/vcpkg skeleton matching the module inventory,
Catch2 wired into CTest, scripts/build.ps1 and scripts/gate.ps1, the hashdiff and
goldgen tools, activate .claude/settings.json from the template once
scripts/quickcheck.ps1 exists, and stub MODULE.md files for every module.

Definition of done: scripts/gate.ps1 -Milestone M0 exits 0, including the
test-the-gate check (a deliberately failing test must block a commit).
Then: git tag m0-green, push branch and tags, write the SESSION_LOG.md entry.
```

## Every subsequent session (template)

```
Read docs/ARCHITECTURE.md, the latest docs/SESSION_LOG.md entry, and the M<N>
section of docs/MILESTONES.md. Execute Milestone M<N> per the session protocol:
change manifest first, then implement, then run scripts/gate.ps1 -Milestone M<N>
until it exits 0. Re-run gates M0..M<N-1> as a regression sweep. Tag m<N>-green,
push, write the SESSION_LOG entry.
```

## Hook activation note

`.claude/settings.json.template` ships in this starter. M0 renames it to
`.claude/settings.json` **after** creating `scripts/quickcheck.ps1` (a fast
incremental build + affected-tests run). Activating it earlier would fire a hook
that points at a script that doesn't exist yet.

## If a session ends mid-milestone

Just start a new session with the same template prompt. SESSION_LOG.md +
the last green tag tell the agent exactly where reality is. Never resume by
pasting old conversation — the repo is the memory, not the chat.
