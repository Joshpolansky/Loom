#include "loom/diag/crash_handler.h"
#include "loom/diag/breadcrumb.h"
#include "loom/diag/fault_report.h"
#include "loom/diag/symbolizer.h"
#include "loom/version.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef LOOM_BUILD_TYPE
#define LOOM_BUILD_TYPE "unknown"
#endif

// ============================================================================
// Crash handler. Captures the faulting thread's breadcrumb + a stack trace and
// writes a crash report, then lets the process die.
//
// Both platforms produce the SAME structured, symbolized JSON report (built from
// the FaultReport model, resolved via cpptrace) so mac/linux crashes are as
// readable as Windows ones. cpptrace allocates and is therefore NOT async-
// signal-safe, so on the POSIX signal path we ALSO write a minimal async-signal-
// safe raw text report FIRST as a guaranteed fallback; the structured JSON is
// then attempted best-effort. A g_reporting flag stops a fault during reporting
// from looping. (The Windows unhandled-exception filter is not signal-
// constrained, so it writes the JSON directly.)
// ============================================================================

namespace loom::diag {
namespace {

std::atomic_flag g_reporting = ATOMIC_FLAG_INIT;   // first faulting thread wins
char             g_reportPath[1024] = {};           // structured JSON report path
char             g_reportId[256]    = {};           // report id / filename stem

// Build + symbolize + write the structured JSON crash report. NOT async-signal-
// safe (cpptrace allocates). The FaultReport/JSON is fully built in memory and
// then written in one shot, so the file is either complete or absent — never
// half-written garbage.
void writeStructuredReport(FaultKind kind, const char* reason, int code,
                           void* const* frames, unsigned nframes) {
    const Breadcrumb& b = tlsBreadcrumb;  // faulting thread

    FaultReport r;
    r.id           = g_reportId;
    r.tsMs         = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count();
    r.kind         = kind;
    r.signalOrCode = code;
    r.reason       = reason ? reason : "";
    r.sdkVersion   = loom::kSdkVersion;
    r.gitSha       = loom::kGitSha;
    r.buildType    = LOOM_BUILD_TYPE;
    r.moduleId     = b.moduleId ? b.moduleId : "";
    r.className    = b.className ? b.className : "";
    r.phase        = b.phase;
    r.cycle        = b.cycle;
    r.frames       = symbolize(reinterpret_cast<const void* const*>(frames), nframes);

    std::string json = toJson(r);
    std::ofstream f(g_reportPath, std::ios::binary | std::ios::trunc);
    if (f) f << json;
}

} // namespace
} // namespace loom::diag

// ---------------------------------------------------------------------------
#if defined(_WIN32)
// ---------------------------------------------------------------------------
#include <windows.h>   // CaptureStackBackTrace (RtlCaptureStackBackTrace, in kernel32)

namespace loom::diag {
namespace {

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* ep) {
    if (g_reporting.test_and_set()) return EXCEPTION_EXECUTE_HANDLER;
    void* frames[64];
    unsigned n = CaptureStackBackTrace(0, 64, frames, nullptr);
    const DWORD code = ep ? ep->ExceptionRecord->ExceptionCode : 0UL;
    char reason[64];
    std::snprintf(reason, sizeof reason, "SEH exception 0x%08lx", code);
    writeStructuredReport(FaultKind::Signal, reason, static_cast<int>(code), frames, n);
    return EXCEPTION_EXECUTE_HANDLER;   // run default handler → terminate
}

void terminateHandler() {
    if (!g_reporting.test_and_set()) {
        void* frames[64];
        unsigned n = CaptureStackBackTrace(0, 64, frames, nullptr);
        writeStructuredReport(FaultKind::Signal, "std::terminate (unhandled C++ exception)",
                              0, frames, n);
    }
    std::abort();
}

} // namespace

void CrashHandler::install(const CrashConfig& cfg) {
    std::error_code ec;
    std::filesystem::create_directories(cfg.crashDir, ec);
    std::snprintf(g_reportId, sizeof g_reportId, "loom-crash-%lu", GetCurrentProcessId());
    auto path = (cfg.crashDir / (std::string(g_reportId) + ".json")).string();
    std::snprintf(g_reportPath, sizeof g_reportPath, "%s", path.c_str());
    SetUnhandledExceptionFilter(unhandledFilter);
    std::set_terminate(terminateHandler);
}

} // namespace loom::diag

// ---------------------------------------------------------------------------
#else  // POSIX
// ---------------------------------------------------------------------------
#include <csignal>
#include <cstdlib>
#include <execinfo.h>
#include <fcntl.h>      // open(), O_WRONLY/O_CREAT/O_TRUNC (not transitively included on macOS)
#include <unistd.h>

namespace loom::diag {
namespace {

char  g_reportPathRaw[1024] = {};   // async-signal-safe raw text fallback (.txt)
char  g_buildId[256] = {};          // precomputed at install (no formatting in handler)
void* g_primeFrames[4];             // backtrace priming target

// async-signal-safe helpers --------------------------------------------------
// NB: std::strlen is NOT in the POSIX async-signal-safe list, so compute the
// length with a plain loop (which is) before the single write().
void sWrite(int fd, const char* s) {
    if (!s) return;
    size_t n = 0;
    while (s[n]) ++n;
    ssize_t r = ::write(fd, s, n); (void)r;
}
void sWriteHex(int fd, uintptr_t v) {
    char buf[2 + sizeof(uintptr_t) * 2]; buf[0] = '0'; buf[1] = 'x';
    const char* hex = "0123456789abcdef";
    int i = 2 + (int)sizeof(uintptr_t) * 2;
    char* p = buf + i;
    if (v == 0) { *--p = '0'; } else { while (v) { *--p = hex[v & 0xf]; v >>= 4; } }
    sWrite(fd, "0x"); ssize_t r = ::write(fd, p, (buf + i) - p); (void)r;
}

const char* signalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (segmentation fault)";
        case SIGABRT: return "SIGABRT (abort)";
        case SIGFPE:  return "SIGFPE (floating-point exception)";
        case SIGILL:  return "SIGILL (illegal instruction)";
        case SIGBUS:  return "SIGBUS (bus error)";
        default:      return "fatal signal";
    }
}

// Guaranteed async-signal-safe report: breadcrumb + raw return addresses. Always
// written first so there is a report even if the structured pass fails. Skipped
// by the FaultStore when a sibling .json exists.
void writeRawReport(int sig, void* const* frames, int n) {
    int fd = ::open(g_reportPathRaw, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    const Breadcrumb& b = tlsBreadcrumb;
    sWrite(fd, "=== Loom crash report (raw) ===\nsignal: ");
    sWriteHex(fd, (uintptr_t)sig);
    sWrite(fd, "\nmodule: "); sWrite(fd, b.moduleId ? b.moduleId : "(none/runtime)");
    sWrite(fd, "  class: ");  sWrite(fd, b.className ? b.className : "(none)");
    sWrite(fd, "  phase: ");  sWrite(fd, phaseName(b.phase));
    sWrite(fd, "\n");         sWrite(fd, g_buildId); sWrite(fd, "\n");
    sWrite(fd, "frames (raw addresses — symbolize offline):\n");
    for (int i = 0; i < n; ++i) { sWrite(fd, "  "); sWriteHex(fd, (uintptr_t)frames[i]); sWrite(fd, "\n"); }
    ::close(fd);
}

void handler(int sig, siginfo_t*, void*) {
    // Re-raise with kill() (async-signal-safe); the handler was installed with
    // SA_RESETHAND, so the disposition is already SIG_DFL — no signal() needed
    // (signal() is NOT async-signal-safe).
    if (g_reporting.test_and_set()) { kill(getpid(), sig); _exit(128 + sig); }

    void* frames[64];
    int n = backtrace(frames, 64);

    // 1. Guaranteed: async-signal-safe raw report.
    writeRawReport(sig, frames, n);
    // 2. Best-effort: structured + symbolized JSON (mirrors Windows). cpptrace
    //    allocates — not strictly async-signal-safe, but works for the common
    //    (non-heap-corruption) crash; the raw report above is the fallback.
    writeStructuredReport(FaultKind::Signal, signalName(sig), sig,
                          frames, static_cast<unsigned>(n));

    // Re-raise for a core dump with the default disposition (async-signal-safe).
    kill(getpid(), sig);
    _exit(128 + sig);
}

void terminateHandler() {
    // Not in a signal context, but mirror the signal path for a consistent report.
    if (!g_reporting.test_and_set()) {
        void* frames[64];
        int n = backtrace(frames, 64);
        writeRawReport(SIGABRT, frames, n);
        writeStructuredReport(FaultKind::Signal, "std::terminate (unhandled C++ exception)",
                              0, frames, static_cast<unsigned>(n));
    }
    std::abort();
}

} // namespace

void CrashHandler::install(const CrashConfig& cfg) {
    std::error_code ec;
    std::filesystem::create_directories(cfg.crashDir, ec);

    std::snprintf(g_reportId, sizeof g_reportId, "loom-crash-%ld", (long)getpid());
    auto jsonPath = (cfg.crashDir / (std::string(g_reportId) + ".json")).string();
    auto rawPath  = (cfg.crashDir / (std::string(g_reportId) + ".txt")).string();
    std::snprintf(g_reportPath,    sizeof g_reportPath,    "%s", jsonPath.c_str());
    std::snprintf(g_reportPathRaw, sizeof g_reportPathRaw, "%s", rawPath.c_str());
    std::snprintf(g_buildId, sizeof g_buildId, "build: sdk=%s git=%s type=%s",
                  loom::kSdkVersion, loom::kGitSha, LOOM_BUILD_TYPE);

    // Prime backtrace() so its first (lazy libgcc) call already happened — the
    // handler's backtrace() is then effectively async-signal-safe.
    backtrace(g_primeFrames, 4);

    // Alternate signal stack so a stack-overflow SIGSEGV still has stack to run.
    // Use a fixed compile-time size: since glibc 2.34, SIGSTKSZ expands to a
    // sysconf() call (not a constant), so it can't size a static array. 64 KiB
    // comfortably exceeds SIGSTKSZ on every supported platform.
    static constexpr size_t kAltStackSize = 64 * 1024;
    static char altStack[kAltStackSize];
    stack_t ss{}; ss.ss_sp = altStack; ss.ss_size = sizeof altStack; ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

    struct sigaction sa{};
    sa.sa_sigaction = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    for (int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS}) sigaction(sig, &sa, nullptr);

    std::set_terminate(terminateHandler);
}

} // namespace loom::diag
#endif
