#pragma once

#include "loom/diag/fault_sink.h"
#include "loom/diag/fault_store.h"

#include <atomic>
#include <cstdint>

// ============================================================================
// loom::diag — concrete fault sink (runtime layer)
//
// Bridges the scheduler's exception path to the rest of the runtime: builds a
// structured FaultReport from a module-call exception, captures the module's
// live data sections (safe off the signal path), persists it via the
// FaultStore, and publishes it on the `loom/faults` bus topic for the UI and
// any subscribing module. Keeping it here (not in the scheduler) is what lets
// diag stay free of DataEngine/Bus dependencies.
// ============================================================================

namespace loom {
class DataEngine;
class Bus;
}

namespace loom::diag {

class RuntimeFaultSink : public IFaultSink {
public:
    RuntimeFaultSink(FaultStore& store, DataEngine& engine, Bus& bus);

    void onModuleFault(const FaultEvent& ev) override;

private:
    FaultStore&           store_;
    DataEngine&           engine_;
    Bus&                  bus_;
    std::atomic<uint64_t> seq_{0};
};

} // namespace loom::diag
