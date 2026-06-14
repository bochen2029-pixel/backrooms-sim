#include "telemetry/crash.h"

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <string>

namespace br::telemetry {

namespace {

std::string g_dir = "runs\\crash";

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Best-effort post-mortem: create the crash dir, write a minidump + a marker line,
// then exit with a known code so the harness can distinguish a fault from a clean
// nonzero exit. Keeps to async-signal-safe-ish Win32 calls (no heap churn beyond
// the path strings prepared up front).
LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    const std::wstring wdir = widen(g_dir);
    CreateDirectoryW(wdir.c_str(), nullptr);

    const std::wstring dump = wdir + L"\\minidump.dmp";
    HANDLE f = CreateFileW(dump.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithThreadInfo);
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, type, &mei, nullptr, nullptr);
        CloseHandle(f);
    }

    const std::string logp = g_dir + "\\crash.log";
    if (std::FILE* lf = std::fopen(logp.c_str(), "ab")) {
        const unsigned long code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0u;
        std::fprintf(lf, "crash code=0x%08lx minidump=minidump.dmp exit=%d\n", code, kCrashExitCode);
        std::fclose(lf);
    }
    std::fprintf(stderr, "[crash] minidump written to %s\\minidump.dmp\n", g_dir.c_str());
    std::fflush(stderr);

    ExitProcess(static_cast<UINT>(kCrashExitCode));
    return EXCEPTION_EXECUTE_HANDLER;  // unreachable
}

}  // namespace

void install_crash_handler(const std::string& crash_dir) {
    g_dir = crash_dir.empty() ? std::string("runs\\crash") : crash_dir;
    SetUnhandledExceptionFilter(crash_filter);
}

void force_crash() {
    // Raise a genuine access-violation exception; with no __try/__except in scope
    // it routes to the installed unhandled-exception filter (which never returns).
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    ExitProcess(static_cast<UINT>(kCrashExitCode));  // unreachable; satisfies [[noreturn]]
}

}  // namespace br::telemetry
