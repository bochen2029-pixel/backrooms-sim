#pragma once
//
// telemetry/crash.h — minidump capture + forced-crash drill (M10).
//
// Installs a process-wide unhandled-exception filter so a fatal fault during a
// long soak leaves a post-mortem minidump + a one-line marker the soak harness
// (scripts/soak.ps1) detects to auto-restart-and-log. Windows-only (dbghelp).
//
#include <string>

namespace br::telemetry {

// Process exit code used after a captured crash (distinct from normal failures so
// the harness can tell a fault from a clean nonzero exit).
constexpr int kCrashExitCode = 70;

// Install the unhandled-exception filter. On a fatal fault it writes
// <crash_dir>/minidump.dmp + appends a line to <crash_dir>/crash.log, then exits
// with kCrashExitCode. Idempotent; call once at startup. Empty dir -> "runs\\crash".
void install_crash_handler(const std::string& crash_dir);

// Trigger a fatal fault (the forced-crash drill). Routes through the installed
// filter; never returns.
[[noreturn]] void force_crash();

}  // namespace br::telemetry
