#include <gtest/gtest.h>

#include "loom/module_loader.h"

#include <filesystem>

// Helper to find example_motor.so (copied from test_module_loader)
std::filesystem::path findExampleMotor() {
#ifdef LOOM_MODULE_DIR
    auto p = std::filesystem::path(LOOM_MODULE_DIR) / "example_motor.so";
    if (std::filesystem::exists(p)) return p;
#endif
    std::filesystem::path candidates[] = {
        "modules/example_motor.so",
        "../modules/example_motor.so",
        "../../modules/example_motor.so",
    };
    for (auto& p : candidates) if (std::filesystem::exists(p)) return p;
    if (auto* dir = std::getenv("CRUNTIME_MODULE_DIR")) {
        auto p = std::filesystem::path(dir) / "example_motor.so";
        if (std::filesystem::exists(p)) return p;
    }
    return {};
}

class TraceCacheTest : public ::testing::Test {
protected:
    loom::ModuleLoader loader;
};

TEST_F(TraceCacheTest, ExampleMotorRuntimeFieldsCached) {
    auto path = findExampleMotor();
    if (path.empty()) GTEST_SKIP() << "example_motor.so not found — build modules first";

    auto id = loader.load(path);
    ASSERT_FALSE(id.empty());
    auto* mod = loader.get(id);
    ASSERT_NE(mod, nullptr);

    // initialize to populate runtime defaults
    mod->instance->init(loom::InitContext{});

    std::vector<std::string> fields = {"current_speed","position","at_speed","cycle_count", "history", "history/0/current_speed", "history/0/position", "history/0/at_speed", "history/1/current_speed", "history/1/position", "history/1/at_speed"};
    for (auto& f : fields) {
        SCOPED_TRACE(f);
        auto ptr = mod->instance->tracePtr(loom::DataSection::Runtime, std::string("/" + f));
        EXPECT_TRUE(ptr.has_value());
        if (ptr) EXPECT_NE(*ptr, nullptr);
        auto tn = mod->instance->traceTypeName(loom::DataSection::Runtime, std::string("/" + f));
        EXPECT_TRUE(tn.has_value());
        if (tn) EXPECT_FALSE(tn->empty());
    }

    // cleanup
    loader.unload(id);
}
