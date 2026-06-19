#pragma once

#include "loom/diag/breadcrumb.h"
#include "loom/diag/symbolizer.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// ============================================================================
// loom::diag — structured fault report
//
// The machine-readable record of a single fault, written to
// <dataDir>/crash/<id>.json and served over /api/faults for the LoomUI crash
// viewer. Built and serialized OFF the signal path only (it allocates): the
// exception path (scheduler guard) and the Windows unhandled-exception filter.
// The POSIX fatal-signal handler writes a raw text report instead (async-
// signal-safe) which is symbolized offline.
// ============================================================================

namespace loom::diag {

enum class FaultKind : uint8_t {
    Exception,  ///< C++ exception caught by a module-call guard
    Signal,     ///< Fatal signal / SEH exception caught by the crash handler
};

const char* faultKindName(FaultKind);

/// Module data sections captured at fault time (exception path only — reading a
/// module's state is safe off the signal path). Each holds raw JSON or "".
struct FaultSections {
    std::string config;
    std::string recipe;
    std::string runtime;
    std::string summary;
};

struct FaultReport {
    std::string id;             ///< Unique within a run, also the filename stem
    int64_t     tsMs = 0;       ///< system_clock milliseconds
    FaultKind   kind = FaultKind::Exception;
    int         signalOrCode = 0;   ///< signal number / SEH exception code (0 for exceptions)
    std::string reason;         ///< what() or signal/exception description

    // Build identity (so a report maps back to a commit + matching symbols).
    std::string sdkVersion;
    std::string gitSha;
    std::string buildType;

    // Execution breadcrumb (which module/phase was running on the faulting thread).
    std::string moduleId;       ///< "" → runtime code (no module on the thread)
    std::string className;
    Phase       phase = Phase::None;
    uint64_t    cycle = 0;

    std::vector<SymFrame>        frames;    ///< symbolized stack (empty if unavailable)
    std::optional<FaultSections> sections;  ///< captured live values (exception path)
};

/// Serialize to clean, nested JSON (sections embed as real JSON objects, not
/// escaped strings). Allocates — off-signal use only.
std::string toJson(const FaultReport&);

} // namespace loom::diag
