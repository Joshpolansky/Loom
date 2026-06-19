#include "loom/diag/crash_handler.h"
#include "loom/diag/breadcrumb.h"
#include "loom/version.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>

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

void writeReportWin(const char* reason, void* const* frames, unsigned nframes) {
    HANDLE h = CreateFileA(g_reportPath, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char line[1200];
    auto put = [&](const char* s) { DWORD w; WriteFile(h, s, (DWORD)std::strlen(s), &w, nullptr); };

    const Breadcrumb& b = tlsBreadcrumb;  // faulting thread
    put("=== Loom crash report ===\n");
    std::snprintf(line, sizeof line, "reason: %s\n", reason); put(line);
    std::snprintf(line, sizeof line, "module: %s  class: %s  phase: %s  cycle: %llu\n",
                  b.moduleId ? b.moduleId : "(none/runtime)",
                  b.className ? b.className : "(none)", phaseName(b.phase),
                  (unsigned long long)b.cycle); put(line);
    buildIdentityLine(line, sizeof line); put(line); put("\n");
    put("frames (raw addresses — symbolize offline with matching PDBs):\n");
    for (unsigned i = 0; i < nframes; ++i) {
        std::snprintf(line, sizeof line, "  #%-2u 0x%p\n", i, frames[i]);
        put(line);
    }
    FlushFileBuffers(h);
    CloseHandle(h);
}

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* ep) {
    if (g_reporting.test_and_set()) return EXCEPTION_EXECUTE_HANDLER;
    void* frames[64];
    unsigned n = CaptureStackBackTrace(0, 64, frames, nullptr);
    char reason[64];
    std::snprintf(reason, sizeof reason, "SEH exception 0x%08lx",
                  ep ? ep->ExceptionRecord->ExceptionCode : 0UL);
    writeReportWin(reason, frames, n);
    return EXCEPTION_EXECUTE_HANDLER;   // run default handler → terminate
}

void terminateHandler() {
    if (!g_reporting.test_and_set()) {
        void* frames[64];
        unsigned n = CaptureStackBackTrace(0, 64, frames, nullptr);
        writeReportWin("std::terminate (unhandled C++ exception)", frames, n);
    }
    std::abort();
}

} // namespace

void CrashHandler::install(const CrashConfig& cfg) {
    std::error_code ec;
    std::filesystem::create_directories(cfg.crashDir, ec);
    auto path = (cfg.crashDir / ("loom-crash-" + std::to_string(GetCurrentProcessId()) + ".txt")).string();
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
#include <unistd.h>

namespace loom::diag {
namespace {

// async-signal-safe helpers --------------------------------------------------
void sWrite(int fd, const char* s) { ssize_t r = ::write(fd, s, std::strlen(s)); (void)r; }
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
    static char altStack[SIGSTKSZ < 65536 ? 65536 : SIGSTKSZ];
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
