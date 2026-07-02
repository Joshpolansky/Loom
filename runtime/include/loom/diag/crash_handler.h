#pragma once

#include <filesystem>

// ============================================================================
// loom::diag — process-global crash handler (hardware-fault / unhandled path)
//
// Installs fatal-signal handlers (POSIX) / an unhandled-exception filter
// (Windows) / std::set_terminate, so a segfault/FPE/abort or an escaped C++
// exception — in a module OR in the runtime itself — produces a crash report
// (faulting thread's breadcrumb + signal/exception + build identity + raw stack
// addresses) before the process exits. Symbolization is layered on later
// (Phase 2). Install once, early in startup.
// ============================================================================

namespace loom::diag {

struct CrashConfig {
    std::filesystem::path crashDir;   // where crash reports are written (e.g. <dataDir>/crash)
};

class CrashHandler {
public:
    /// Install the handlers. Idempotent; call once after logging is set up.
    static void install(const CrashConfig& cfg);
};

} // namespace loom::diag
