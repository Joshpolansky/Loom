#include "loom/runtime_heap.h"        // IRuntimeHeap, loom::makeShared
#include "loom/runtime_heap_impl.h"   // loom::RuntimeHeap (concrete, runtime-owned)
#include "loom/command.h"             // loom::CommandStatus (a real makeShared user)

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <new>
#include <thread>
#include <type_traits>
#include <vector>

// The whole design depends on this: makeShared targets are trivially
// destructible, so the resident deleter never runs module-defined code.
static_assert(std::is_trivially_destructible_v<loom::CommandStatus>);

namespace {

struct Pod { int a = 1; double b = 2.0; };

struct WithCtor {
    int v;
    explicit WithCtor(int x) : v(x) {}   // trivial dtor → still ok for makeShared
};

/// Test double that counts allocate/free and records the requested size/align.
struct CountingHeap : loom::IRuntimeHeap {
    int    allocs = 0;
    int    frees  = 0;
    std::size_t last_size  = 0;
    std::size_t last_align = 0;

    std::shared_ptr<void> allocate(std::size_t size, std::size_t align) override {
        ++allocs;
        last_size  = size;
        last_align = align;
        const std::align_val_t a{align};
        void* p = ::operator new(size, a);
        return std::shared_ptr<void>(p, [this, a](void* q) {
            ++frees;
            ::operator delete(q, a);
        });
    }
};

/// Always fails to allocate (simulates an exhausted/failed heap).
struct NullHeap : loom::IRuntimeHeap {
    std::shared_ptr<void> allocate(std::size_t, std::size_t) override { return {}; }
};

} // namespace

// =============================================================================
// Concrete RuntimeHeap
// =============================================================================

TEST(RuntimeHeapTest, AllocateReturnsAlignedStorage) {
    loom::RuntimeHeap heap;
    auto block = heap.allocate(128, 64);
    ASSERT_NE(block, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(block.get()) % 64u, 0u);
    EXPECT_EQ(block.use_count(), 1);
}

TEST(RuntimeHeapTest, MakeSharedDefaultConstructs) {
    loom::RuntimeHeap heap;
    auto p = loom::makeShared<Pod>(heap);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->a, 1);
    EXPECT_EQ(p->b, 2.0);
}

TEST(RuntimeHeapTest, MakeSharedForwardsArgs) {
    loom::RuntimeHeap heap;
    auto i = loom::makeShared<int>(heap, 42);
    ASSERT_NE(i, nullptr);
    EXPECT_EQ(*i, 42);

    auto c = loom::makeShared<WithCtor>(heap, 9);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->v, 9);
}

// =============================================================================
// weak_ptr as the generational handle (the hot-reload-safety property)
// =============================================================================

TEST(RuntimeHeapTest, WeakPtrExpiresWhenSharedDropped) {
    loom::RuntimeHeap heap;
    auto sp = loom::makeShared<int>(heap, 5);
    std::weak_ptr<int> wp = sp;

    EXPECT_FALSE(wp.expired());
    ASSERT_NE(wp.lock(), nullptr);
    EXPECT_EQ(*wp.lock(), 5);

    sp.reset();  // issuer drops its ref → observer's handle goes stale
    EXPECT_TRUE(wp.expired());
    EXPECT_EQ(wp.lock(), nullptr);
}

TEST(RuntimeHeapTest, WriteThroughWeakPtr) {
    // The CommandStatus pattern: issuer holds shared_ptr, executor holds weak.
    loom::RuntimeHeap heap;
    auto issuer = loom::makeShared<loom::CommandStatus>(heap);
    std::weak_ptr<loom::CommandStatus> executor = issuer;

    if (auto s = executor.lock()) {     // executor writes status THROUGH the weak ref
        s->phase.store(loom::CmdPhase::Done);
        s->done.store(true);
    }
    EXPECT_TRUE(issuer->done.load());
    EXPECT_EQ(issuer->phase.load(), loom::CmdPhase::Done);

    issuer.reset();                     // issuer gone → executor write becomes a no-op
    EXPECT_TRUE(executor.expired());
    EXPECT_EQ(executor.lock(), nullptr);
}

// =============================================================================
// makeShared contract against a test double
// =============================================================================

TEST(RuntimeHeapTest, MakeSharedUsesHeapWithCorrectSizeAndFrees) {
    CountingHeap heap;
    {
        auto p = loom::makeShared<Pod>(heap);
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(heap.allocs, 1);
        EXPECT_EQ(heap.last_size, sizeof(Pod));
        EXPECT_EQ(heap.last_align, alignof(Pod));
        EXPECT_EQ(heap.frees, 0);       // still held
    }
    EXPECT_EQ(heap.frees, 1);            // released via the heap's deleter on last drop
}

TEST(RuntimeHeapTest, StorageOutlivesIssuerWhileObserverHoldsWeak) {
    // The object's storage is reclaimed when the last *shared* ref drops, even
    // if a weak_ptr is still outstanding (the control block lingers, the bytes
    // are freed). Verifies the free happens at strong-count zero.
    CountingHeap heap;
    std::weak_ptr<Pod> w;
    {
        auto sp = loom::makeShared<Pod>(heap);
        w = sp;
        EXPECT_EQ(heap.frees, 0);
    }
    EXPECT_EQ(heap.frees, 1);
    EXPECT_TRUE(w.expired());
}

TEST(RuntimeHeapTest, MakeSharedReturnsNullWhenHeapFails) {
    NullHeap heap;
    auto p = loom::makeShared<int>(heap, 1);
    EXPECT_EQ(p, nullptr);
}

// =============================================================================
// Thread-safety smoke test (operator new/delete; no shared state in the heap)
// =============================================================================

TEST(RuntimeHeapTest, ConcurrentAllocateAndRelease) {
    loom::RuntimeHeap heap;
    std::atomic<int> ok{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 1000; ++i) {
                auto p = loom::makeShared<Pod>(heap);
                if (p && p->a == 1) ++ok;   // construct + read, then drop (frees)
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(ok.load(), 8 * 1000);
}
