#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

// ============================================================================
// Runtime heap — resident allocator for cross-module shared objects
//
// Hot-reload safety problem: a std::shared_ptr's control block (its vtable) and
// deleter are instantiated wherever the shared_ptr is *constructed*. If a module
// constructs one and is later unloaded while another module / the runtime still
// holds a (weak_)ptr, the final release dispatches through a vtable in unmapped
// code → crash. For an arbitrary module-defined T the destructor is also module
// code, so it can never be safely run after the module unloads.
//
// Solution: the resident runtime hands out storage already owned by a
// shared_ptr<void> whose control block + deleter live in the runtime binary.
// A module turns that into a typed shared_ptr<T> via the *aliasing constructor*,
// which shares the runtime's control block rather than creating a new one. The
// resident deleter only frees bytes, so T must be trivially destructible.
//
// This is the general mechanism for any trivially-destructible type a module
// needs to share across the boundary (e.g. an async-command status struct).
// ============================================================================

namespace loom {

/// Allocator owned by the resident runtime. Injected into every module before
/// init() (see Module::runtimeHeap()).
class IRuntimeHeap {
public:
    virtual ~IRuntimeHeap() = default;

    /// Allocate `size` bytes aligned to `align`, owned by a shared_ptr<void>
    /// whose control block + deleter are runtime-resident. Empty on failure.
    /// Prefer the makeShared<T>() helper over calling this directly.
    virtual std::shared_ptr<void> allocate(std::size_t size, std::size_t align) = 0;
};

/// Construct a T in runtime-owned storage and return a typed shared_ptr that
/// shares the runtime's resident control block (via the aliasing constructor).
/// Returns an empty shared_ptr if the heap allocation fails.
///
/// T must be trivially destructible so the resident deleter — which only frees
/// bytes — never has to run module-defined destructor code.
template <class T, class... Args>
std::shared_ptr<T> makeShared(IRuntimeHeap& heap, Args&&... args) {
    static_assert(std::is_trivially_destructible_v<T>,
        "loom::makeShared requires a trivially destructible T so the resident "
        "deleter needs no module code (hot-reload safety).");
    std::shared_ptr<void> block = heap.allocate(sizeof(T), alignof(T));
    if (!block) return {};
    T* obj = ::new (block.get()) T(std::forward<Args>(args)...);
    // Aliasing constructor: shares `block`'s (resident) control block; no new
    // control block is created here in the module TU.
    return std::shared_ptr<T>(std::move(block), obj);
}

} // namespace loom
