#include <gtest/gtest.h>

#include "loom/data_engine.h"
#include "loom/data_store.h"
#include "loom/module.h"

#include <filesystem>
#include <fstream>

// Test data types — must have external linkage for glaze reflection
struct StoreConfig {
    int value = 10;
    std::string label = "default";
};

struct StoreRecipe {
    double speed = 50.0;
};

struct StoreRuntime {
    double pos = 0.0;
};

class StoreModule : public loom::Module<StoreConfig, StoreRecipe, StoreRuntime> {
public:
    static loom::ModuleHeader moduleHeader() {
        return {.api_version = loom::kApiVersion, .name = "StoreTest", .version = "1.0.0"};
    }
    const loom::ModuleHeader& header() const override {
        static auto hdr = moduleHeader();
        return hdr;
    }
    void init(const loom::InitContext& /*ctx*/) override {}
    void cyclic() override {}
    void exit() override {}
    void longRunning() override {}
};

namespace {

class DataStoreTest : public ::testing::Test {
protected:
    std::filesystem::path testDir;
    std::unique_ptr<loom::DataStore> store;
    std::unique_ptr<loom::DataEngine> engine;
    StoreModule module;

    void SetUp() override {
        // Use unique dir per test instance to avoid parallel test conflicts
        testDir = std::filesystem::temp_directory_path() /
                  ("crt_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::remove_all(testDir);

        store = std::make_unique<loom::DataStore>(testDir);
        engine = std::make_unique<loom::DataEngine>();
        engine->registerModule("store_test", &module);
    }

    void TearDown() override {
        std::filesystem::remove_all(testDir);
    }
};

TEST_F(DataStoreTest, SaveAndLoadConfig) {
    // Modify config
    module.config().value = 42;
    module.config().label = "modified";

    // Save
    EXPECT_TRUE(store->saveConfig("store_test", *engine));

    // Verify file exists
    auto path = testDir / "store_test" / "config.json";
    EXPECT_TRUE(std::filesystem::exists(path));

    // Reset module config
    module.config().value = 0;
    module.config().label = "";

    // Load
    EXPECT_TRUE(store->loadConfig("store_test", *engine));

    // Verify restored values
    EXPECT_EQ(module.config().value, 42);
    EXPECT_EQ(module.config().label, "modified");
}

TEST_F(DataStoreTest, LoadNonExistentConfig) {
    EXPECT_FALSE(store->loadConfig("store_test", *engine));
    // Config should remain at defaults
    EXPECT_EQ(module.config().value, 10);
}

TEST_F(DataStoreTest, SaveAndLoadRecipe) {
    module.recipe().speed = 99.9;

    EXPECT_TRUE(store->saveRecipe("store_test", "fast", *engine));

    // Verify file
    auto path = testDir / "store_test" / "recipes" / "fast.json";
    EXPECT_TRUE(std::filesystem::exists(path));

    // Reset
    module.recipe().speed = 0.0;

    // Load
    EXPECT_TRUE(store->loadRecipe("store_test", "fast", *engine));
    EXPECT_DOUBLE_EQ(module.recipe().speed, 99.9);
}

TEST_F(DataStoreTest, ListRecipes) {
    // Save two recipes
    module.recipe().speed = 10.0;
    store->saveRecipe("store_test", "slow", *engine);

    module.recipe().speed = 100.0;
    store->saveRecipe("store_test", "fast", *engine);

    auto recipes = store->listRecipes("store_test");
    EXPECT_EQ(recipes.size(), 2u);
    // Should be sorted
    EXPECT_EQ(recipes[0], "fast");
    EXPECT_EQ(recipes[1], "slow");
}

TEST_F(DataStoreTest, ListRecipesEmpty) {
    auto recipes = store->listRecipes("store_test");
    EXPECT_TRUE(recipes.empty());
}

} // namespace
