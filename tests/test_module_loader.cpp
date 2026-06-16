#include <gtest/gtest.h>

#include "loom/module_loader.h"
#include "module_test_util.h"

#include <filesystem>

namespace {

// Locate example_motor using the platform suffix + CMake-provided module dir.
std::filesystem::path findExampleMotor() {
    return loomtest::findModule("example_motor");
}

class ModuleLoaderTest : public ::testing::Test {
protected:
    loom::ModuleLoader loader;
};

TEST_F(ModuleLoaderTest, LoadNonExistentFile) {
    auto id = loader.load("/tmp/nonexistent_module.so");
    EXPECT_TRUE(id.empty());
}

TEST_F(ModuleLoaderTest, LoadExampleMotor) {
    auto path = findExampleMotor();
    if (path.empty()) {
        GTEST_SKIP() << "example_motor.so not found — build modules first";
    }

    auto id = loader.load(path);
    ASSERT_FALSE(id.empty());
    EXPECT_EQ(id, "ExampleMotor");

    auto* mod = loader.get(id);
    ASSERT_NE(mod, nullptr);
    EXPECT_EQ(mod->header.api_version, loom::kApiVersion);
    EXPECT_EQ(mod->state, loom::ModuleState::Loaded);
    EXPECT_NE(mod->instance, nullptr);
}

TEST_F(ModuleLoaderTest, LoadDuplicate) {
    auto path = findExampleMotor();
    if (path.empty()) {
        GTEST_SKIP() << "example_motor.so not found";
    }

    auto id1 = loader.load(path);
    ASSERT_FALSE(id1.empty());

    // Loading the same module again should fail (same name)
    auto id2 = loader.load(path);
    EXPECT_TRUE(id2.empty());
}

TEST_F(ModuleLoaderTest, UnloadModule) {
    auto path = findExampleMotor();
    if (path.empty()) {
        GTEST_SKIP() << "example_motor.so not found";
    }

    auto id = loader.load(path);
    ASSERT_FALSE(id.empty());

    EXPECT_TRUE(loader.unload(id));
    EXPECT_EQ(loader.get(id), nullptr);
}

TEST_F(ModuleLoaderTest, UnloadNonExistent) {
    EXPECT_FALSE(loader.unload("DoesNotExist"));
}

TEST_F(ModuleLoaderTest, ReloadModule) {
    auto path = findExampleMotor();
    if (path.empty()) {
        GTEST_SKIP() << "example_motor not found — build modules first";
    }

    auto id = loader.load(path);
    ASSERT_FALSE(id.empty());
    auto* before = loader.get(id);
    ASSERT_NE(before, nullptr);
    ASSERT_NE(before->instance, nullptr);

    // Warm-restart: unload + reload from the same path.
    EXPECT_TRUE(loader.reload(id));

    auto* after = loader.get(id);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->id, id);
    EXPECT_EQ(after->state, loom::ModuleState::Loaded);
    ASSERT_NE(after->instance, nullptr);  // a fresh instance is in place

    // The reloaded module is still usable.
    after->instance->init(loom::InitContext{});
    after->instance->cyclic();
    after->instance->exit();
}

TEST_F(ModuleLoaderTest, ModuleLifecycle) {
    auto path = findExampleMotor();
    if (path.empty()) {
        GTEST_SKIP() << "example_motor.so not found";
    }

    auto id = loader.load(path);
    ASSERT_FALSE(id.empty());

    auto* mod = loader.get(id);
    ASSERT_NE(mod, nullptr);

    // Test init
    mod->instance->init(loom::InitContext{});

    // Test cyclic
    mod->instance->cyclic();

    // Test exit
    mod->instance->exit();
}

TEST_F(ModuleLoaderTest, LoadDirectory) {
    auto path = findExampleMotor();
    if (path.empty()) {
        GTEST_SKIP() << "example_motor.so not found";
    }

    auto dir = path.parent_path();
    auto ids = loader.loadDirectory(dir);
    EXPECT_GE(ids.size(), 1u);
}

} // namespace
