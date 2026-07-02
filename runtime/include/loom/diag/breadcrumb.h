#pragma once

#include <cstdint>

// ============================================================================
// loom::diag — execution breadcrumb
//
// A per-thread record of what the runtime is currently executing (which module,
// class, and lifecycle phase). Set cheaply via RAII around every module entry
// call; read by the crash handler (on the faulting thread) to attribute a fault
// to a specific module/phase.
//
// Stores *stable pointers* into the module's id/class strings (which outlive the
// call) plus a phase byte — no allocation, no copy. Reading the raw pointers/
// bytes from a signal handler is allocator-free and async-signal-safe.
// ============================================================================

namespace loom::diag {

enum class Phase : uint8_t {
    None = 0, Init, PreCyclic, Cyclic, PostCyclic, LongRunning, Exit, Service,
};

/// Human-readable phase name (no allocation — safe in a signal handler).
inline const char* phaseName(Phase p) noexcept {
    switch (p) {
        case Phase::Init:        return "init";
        case Phase::PreCyclic:   return "preCyclic";
        case Phase::Cyclic:      return "cyclic";
        case Phase::PostCyclic:  return "postCyclic";
        case Phase::LongRunning: return "longRunning";
        case Phase::Exit:        return "exit";
        case Phase::Service:     return "service";
        case Phase::None:        return "none";
    }
    return "?";
}

struct Breadcrumb {
    const char* moduleId  = nullptr;  // stable pointer into the module's id
    const char* className = nullptr;  // stable pointer into the module's class name
    Phase       phase     = Phase::None;
    uint64_t    cycle     = 0;
};

/// The current thread's breadcrumb. The crash handler runs on the faulting
/// thread, so reading this names the exact module/phase that was executing.
extern thread_local Breadcrumb tlsBreadcrumb;

/// RAII: stamp the breadcrumb on construction, restore the previous value on
/// destruction (so nested calls — e.g. a service invoked from cyclic — unwind
/// correctly).
class BreadcrumbScope {
public:
    BreadcrumbScope(Phase p, const char* moduleId, const char* className) noexcept
        : prev_(tlsBreadcrumb) {
        tlsBreadcrumb.moduleId  = moduleId;
        tlsBreadcrumb.className = className;
        tlsBreadcrumb.phase     = p;
    }
    ~BreadcrumbScope() noexcept { tlsBreadcrumb = prev_; }

    BreadcrumbScope(const BreadcrumbScope&) = delete;
    BreadcrumbScope& operator=(const BreadcrumbScope&) = delete;

private:
    Breadcrumb prev_;
};

} // namespace loom::diag
