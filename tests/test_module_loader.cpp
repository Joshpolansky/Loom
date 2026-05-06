#include <gtest/gtest.h>

#include "loom/module_loader.h"

#include <filesystem>

namespace {

// Helper to find the example_motor.so in the build directory
std::filesystem::path findExampleMotor() {
    // Use compile-time path from CMake if available
#ifdef LOOM_MODULE_DIR
    auto p = std::filesystem::path(LOOM_MODULE_DIR) / "example_motor.so";
    if (std::filesystem::exists(p)) return p;
#endif

    // Fallback: look relative to working directory
    std::filesystem::path candidates[] = {
        "modules/example_motor.so",
        "../modules/example_motor.so",
        "../../modules/example_motor.so",
    };

    // Also try via environment variable or CMake-provided path
    for (auto& p : candidates) {
        if (std::filesystem::exists(p)) return p;
    }

    // Try from CRUNTIME_MODULE_DIR env
    if (auto* dir = std::getenv("CRUNTIME_MODULE_DIR")) {
        auto p = std::filesystem::path(dir) / "example_motor.so";
        if (std::filesystem::exists(p)) return p;
    }

    return {};
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
