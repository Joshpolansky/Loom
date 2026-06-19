#pragma once

#include "loom/diag/breadcrumb.h"

#include <exception>
#include <string_view>

// ============================================================================
// loom::diag — module-call guard (C++ exception path)
//
// Wraps a module entry-point call in a breadcrumb + try/catch. Catches a thrown
// std::exception (or anything), reports it via the caller-supplied onFault, and
// returns false — turning an exception that would otherwise terminate the worker
// thread / process into a contained, attributed fault. Hardware faults
// (segfault/FPE) are NOT exceptions; those are captured by the crash handler.
//
// `onFault` is a template parameter (not std::function) so the happy path is
// zero-cost and allocation-free. The scheduler passes a small lambda that sets
// the module's faulted/Error state.
// ============================================================================

namespace loom::diag {

struct FaultInfo {
    const char*      moduleId;
    const char*      className;
    Phase            phase;
    std::string_view message;   // exception what() (valid for the onFault call)
};

template <class Fn, class OnFault>
bool guard(Phase phase, const char* moduleId, const char* className,
           Fn&& fn, OnFault&& onFault) {
    BreadcrumbScope crumb(phase, moduleId, className);
    try {
        fn();
        return true;
    } catch (const std::exception& e) {
        onFault(FaultInfo{moduleId, className, phase, e.what()});
        return false;
    } catch (...) {
        onFault(FaultInfo{moduleId, className, phase, "unknown (non-std exception)"});
        return false;
    }
}

} // namespace loom::diag
