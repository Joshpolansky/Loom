#include <gtest/gtest.h>

#include "loom/bus.h"
#include "loom/command.h"
#include "loom/module_loader.h"
#include "loom/runtime_heap_impl.h"
#include "module_test_util.h"

#include <memory>

// ============================================================================
// End-to-end integration of the async-command primitive across a real loaded
// module boundary: load CommandProbe, inject bus + runtime heap (as the runtime
// does), submit a command, verify write-through, then verify channel cleanup on
// unload and that a heap-backed status cell survives the module's unload.
// ============================================================================

namespace {

class CommandIntegrationTest : public ::testing::Test {
protected:
    loom::ModuleLoader loader;
    loom::Bus          bus;
    loom::RuntimeHeap  heap;
};

TEST_F(CommandIntegrationTest, CommandRoundTripAcrossModuleBoundary) {
    auto path = loomtest::findModule("command_probe");
    if (path.empty()) GTEST_SKIP() << "command_probe not built";

    const auto id = loader.load(path);
    ASSERT_FALSE(id.empty());
    auto* mod = loader.get(id);
    ASSERT_NE(mod, nullptr);

    // Wire dependencies exactly as RuntimeCore::startModule does.
    mod->instance->setBus(&bus, id);
    mod->instance->setRuntimeHeap(&heap);
    mod->instance->init(loom::InitContext{});

    // provideCommands() registered the channel on the bus under the module id.
    auto* channel = bus.commandChannel(id);
    ASSERT_NE(channel, nullptr);

    // Act as a consumer: allocate a status cell from the (resident) heap and
    // submit a command to the provider's channel.
    auto status = loom::makeShared<loom::CommandStatus>(heap);
    ASSERT_NE(status, nullptr);
    channel->submit(loom::CommandSubmission{/*command*/ 7, /*target*/ 2, "{}",
                                            loom::BufferMode::Aborting, status});

    EXPECT_FALSE(status->done.load());
    mod->instance->cyclic();            // provider drains + writes status through weak_ptr
    EXPECT_TRUE(status->done.load());
    EXPECT_EQ(status->phase.load(), loom::CmdPhase::Done);
    EXPECT_DOUBLE_EQ(status->progress.load(), 1.0);

    // Teardown order from RuntimeCore: cleanup (unregisters the channel) then unload.
    mod->instance->cleanupSubscriptions();
    EXPECT_EQ(bus.commandChannel(id), nullptr);   // channel gone after cleanup

    ASSERT_TRUE(loader.unload(id));
    EXPECT_EQ(loader.get(id), nullptr);

    // The status cell was allocated by the resident heap, so it remains valid
    // after the module is unloaded, and frees through the resident deleter.
    EXPECT_TRUE(status->done.load());
    status.reset();                     // must not crash (deleter is resident)
    SUCCEED();
}

TEST_F(CommandIntegrationTest, ChannelUnavailableAfterUnload) {
    auto path = loomtest::findModule("command_probe");
    if (path.empty()) GTEST_SKIP() << "command_probe not built";

    const auto id = loader.load(path);
    ASSERT_FALSE(id.empty());
    auto* mod = loader.get(id);
    ASSERT_NE(mod, nullptr);
    mod->instance->setBus(&bus, id);
    mod->instance->setRuntimeHeap(&heap);
    mod->instance->init(loom::InitContext{});
    ASSERT_NE(bus.commandChannel(id), nullptr);

    mod->instance->cleanupSubscriptions();
    loader.unload(id);

    // A late consumer lookup fails cleanly rather than returning a dangling ptr.
    EXPECT_EQ(bus.commandChannel(id), nullptr);
}

} // namespace
