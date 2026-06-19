#include "loom/diag/crash_handler.h"
#include "loom/diag/breadcrumb.h"
#include "loom/diag/fault_report.h"
#include "loom/diag/symbolizer.h"
#include "loom/version.h"

#include <atomic>
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
// Crash handler. Phase 1: capture (breadcrumb + signal/exception + build id +
// raw stack addresses) and write a text crash report, then let the process die.
// Symbolization (cpptrace) and structured JSON come in later phases.
//
// POSIX handler bodies must be async-signal-safe: only open()/write()/_exit and
// raw reads — no malloc/printf/ofstream/locks. The Windows unhandled-exception
// filter is NOT signal-constrained, so it may use richer calls.
// ============================================================================

namespace loom::diag {
namespace {

std::atomic_flag g_reporting = ATOMIC_FLAG_INIT;   // first faulting thread wins
char             g_reportPath[1024] = {};           // precomputed at install

void buildIdentityLine(char* buf, size_t n) {
    std::snprintf(buf, n, "build: sdk=%s git=%s type=%s",
                  loom::kSdkVersion, loom::kGitSha, LOOM_BUILD_TYPE);
}

} // namespace
} // namespace loom::diag

// ---------------------------------------------------------------------------
#if defined(_WIN32)
// ---------------------------------------------------------------------------
#include <windows.h>   // CaptureStackBackTrace (RtlCaptureStackBackTrace, in kernel32)

namespace loom::diag {
namespace {

char g_reportId[256] = {};   // crash-report id / filename stem (precomputed at install)

// Build a structured JSON crash report and write it to g_reportPath. The Windows
// unhandled-exception filter is NOT async-signal-constrained, so we may allocate:
// symbolize in-process via cpptrace and serialize the same FaultReport the
// exception path uses, so signal-path crashes surface in /api/faults too.
void writeReportWin(FaultKind kind, const char* reason, int code,
                    void* const* frames, unsigned nframes) {
    const Breadcrumb& b = tlsBreadcrumb;  // faulting thread

    FaultReport r;
    r.id           = g_reportId;
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

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* ep) {
    if (g_reporting.test_and_set()) return EXCEPTION_EXECUTE_HANDLER;
    void* frames[64];
    unsigned n = CaptureStackBackTrace(0, 64, frames, nullptr);
    const DWORD code = ep ? ep->ExceptionRecord->ExceptionCode : 0UL;
    char reason[64];
    std::snprintf(reason, sizeof reason, "SEH exception 0x%08lx", code);
    writeReportWin(FaultKind::Signal, reason, static_cast<int>(code), frames, n);
    return EXCEPTION_EXECUTE_HANDLER;   // run default handler → terminate
}

void terminateHandler() {
    if (!g_reporting.test_and_set()) {
        void* frames[64];
        unsigned n = CaptureStackBackTrace(0, 64, frames, nullptr);
        writeReportWin(FaultKind::Signal, "std::terminate (unhandled C++ exception)",
                       0, frames, n);
    }
    std::abort();
}

} // namespace

void CrashHandler::install(const CrashConfig& cfg) {
    std::error_code ec;
    std::filesystem::create_directories(cfg.crashDir, ec);
    std::snprintf(g_reportId, sizeof g_reportId, "loom-crash-%lu",
                  GetCurrentProcessId());
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

char  g_buildId[256] = {};   // precomputed at install (no formatting in handler)
void* g_primeFrames[4];      // backtrace priming target

void handler(int sig, siginfo_t*, void*) {
    if (g_reporting.test_and_set()) { signal(sig, SIG_DFL); raise(sig); _exit(128 + sig); }
    int fd = ::open(g_reportPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        const Breadcrumb& b = tlsBreadcrumb;
        sWrite(fd, "=== Loom crash report ===\nsignal: ");
        sWriteHex(fd, (uintptr_t)sig);
        sWrite(fd, "\nmodule: "); sWrite(fd, b.moduleId ? b.moduleId : "(none/runtime)");
        sWrite(fd, "  class: ");  sWrite(fd, b.className ? b.className : "(none)");
        sWrite(fd, "  phase: ");  sWrite(fd, phaseName(b.phase));
        sWrite(fd, "\n");         sWrite(fd, g_buildId); sWrite(fd, "\n");
        sWrite(fd, "frames (raw addresses — symbolize offline):\n");
        void* frames[64];
        int n = backtrace(frames, 64);
        for (int i = 0; i < n; ++i) { sWrite(fd, "  "); sWriteHex(fd, (uintptr_t)frames[i]); sWrite(fd, "\n"); }
        ::close(fd);
    }
    signal(sig, SIG_DFL);
    raise(sig);          // re-raise for a core dump with default disposition
    _exit(128 + sig);
}

void terminateHandler() {
    if (!g_reporting.test_and_set()) {
        int fd = ::open(g_reportPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            sWrite(fd, "=== Loom crash report ===\nstd::terminate (unhandled C++ exception)\n");
            sWrite(fd, g_buildId); sWrite(fd, "\n");
            void* frames[64]; int n = backtrace(frames, 64);
            for (int i = 0; i < n; ++i) { sWrite(fd, "  "); sWriteHex(fd, (uintptr_t)frames[i]); sWrite(fd, "\n"); }
            ::close(fd);
        }
    }
    std::abort();
}

} // namespace

void CrashHandler::install(const CrashConfig& cfg) {
    std::error_code ec;
    std::filesystem::create_directories(cfg.crashDir, ec);
    auto path = (cfg.crashDir / ("loom-crash-" + std::to_string((long)getpid()) + ".txt")).string();
    std::snprintf(g_reportPath, sizeof g_reportPath, "%s", path.c_str());
    buildIdentityLine(g_buildId, sizeof g_buildId);

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
