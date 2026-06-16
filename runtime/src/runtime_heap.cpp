#include "loom/runtime_heap_impl.h"

#include <new>

namespace loom {

std::shared_ptr<void> RuntimeHeap::allocate(std::size_t size, std::size_t align) {
    const std::align_val_t a{align};
    void* p = ::operator new(size, a);
    // Deleter + control block are constructed here in the resident runtime TU,
    // so releasing the last (weak_)ptr from any module is safe after unload.
    return std::shared_ptr<void>(p, [a](void* q) noexcept {
        ::operator delete(q, a);
    });
}

} // namespace loom
