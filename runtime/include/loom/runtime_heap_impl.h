#pragma once

#include "loom/runtime_heap.h"

#include <cstddef>
#include <memory>

namespace loom {

/// Concrete IRuntimeHeap owned by RuntimeCore. Backs allocate() with aligned
/// operator new; the returned shared_ptr<void>'s control block and deleter are
/// instantiated in the runtime TU (runtime_heap.cpp), hence resident and safe
/// to release after a consuming module unloads.
///
/// Note: this is a plain heap (one object + one control-block allocation per
/// call). A fixed/pooled strategy can replace the body later for hard real-time
/// determinism without changing the IRuntimeHeap interface or any caller.
class RuntimeHeap : public IRuntimeHeap {
public:
    std::shared_ptr<void> allocate(std::size_t size, std::size_t align) override;
};

} // namespace loom
