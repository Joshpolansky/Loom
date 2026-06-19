#pragma once

#include "loom/diag/breadcrumb.h"

#include <cstdint>
#include <string>

// ============================================================================
// loom::diag — fault sink interface
//
// The scheduler depends only on this interface (injected, not owned) so it
// stays thin: when a guarded module call throws, it reports a FaultEvent and
// the concrete sink (in the runtime layer) does the heavy lifting — capture
// live sections, build + persist a FaultReport, publish the `loom/faults`
// topic. Keeping the interface here lets diag stay free of DataEngine/Bus deps.
// ============================================================================

namespace loom::diag {

struct FaultEvent {
    std::string moduleId;
    std::string className;
    Phase       phase = Phase::None;
    uint64_t    cycle = 0;
    std::string message;   ///< exception what()
};

class IFaultSink {
public:
    virtual ~IFaultSink() = default;

    /// Called from the faulting worker thread, off the signal path. Implementations
    /// must be thread-safe and must not throw.
    virtual void onModuleFault(const FaultEvent&) = 0;
};

} // namespace loom::diag
