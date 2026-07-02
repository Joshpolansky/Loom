#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "bus.h"

// ============================================================================
// PortRef<T> — consumer-side handle to a typed connection point.
//
// Caches the resolved pointer and re-resolves only when the Bus port registry
// changes (provider (un)registered / hot-reloaded), so steady-state access is a
// plain pointer load. get() returns nullptr if the port is absent or its type
// id doesn't match T::kTypeId.
// ============================================================================

namespace loom {

template <class T>
class PortRef {
public:
    PortRef() = default;
    PortRef(Bus* bus, std::string name) : bus_(bus), name_(std::move(name)) {}

    T* get() {
        if (!bus_) return nullptr;
        const uint64_t g = bus_->portGeneration();
        if (g != gen_) {            // registry changed (or first call) — re-resolve
            cached_ = bus_->port<T>(name_);
            gen_    = g;
        }
        return cached_;
    }

    explicit operator bool() { return get() != nullptr; }
    const std::string& name() const { return name_; }

private:
    Bus*        bus_   = nullptr;
    std::string name_;
    T*          cached_ = nullptr;
    uint64_t    gen_   = 0;          // != Bus initial generation, forces first resolve
};

} // namespace loom
