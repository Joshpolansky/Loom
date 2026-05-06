#include <gtest/gtest.h>

#include "loom/data_engine.h"
#include "loom/module.h"

// Test data types — must have external linkage for glaze reflection
struct TestConfig {
    int rate = 42;
    double gain = 3.14;
    std::string name = "test";
};

struct TestRecipe {
    double target = 100.0;
    bool enabled = true;
};

struct TestRuntime {
    double value = 0.0;
    uint64_t counter = 0;
};

struct VectorConfig {
    std::vector<int> speeds = {10, 20, 30, 40, 50};
    std::string label = "default";
};

struct VectorRecipe {
    std::vector<double> setpoints = {1.0, 2.0, 3.0};
};

struct VectorRuntime {
    std::vector<double> readings = {0.0, 0.0, 0.0};
    double scalar = 7.0;
};

struct MotorEntry {
    double speed = 0.0;
    bool enabled = false;
    int id = 0;
};

struct ObjectVectorConfig {
    std::vector<MotorEntry> motors = {{1.0, true, 1}, {2.0, false, 2}, {3.0, true, 3}};
    int count = 3;
};

class TestModule : public loom::Module<TestConfig, TestRecipe, TestRuntime> {
public:
    static loom::ModuleHeader moduleHeader() {
        return {.api_version = loom::kApiVersion, .name = "TestModule", .version = "1.0.0"};
    }
    const loom::ModuleHeader& header() const override {
        static auto hdr = moduleHeader();
        return hdr;
    }
    void init(const loom::InitContext& /*ctx*/) override {}
    void cyclic() override {
        runtime().counter++;
        runtime().value += 1.0;
    }
    void exit() override {}
    void longRunning() override {}
};

class VectorModule : public loom::Module<VectorConfig, VectorRecipe, VectorRuntime> {
public:
    static loom::ModuleHeader moduleHeader() {
        return {.api_version = loom::kApiVersion, .name = "VectorModule", .version = "1.0.0"};
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

struct EmptyRecipe {};
struct EmptyRuntime {};

class ObjectVectorModule : public loom::Module<ObjectVectorConfig, EmptyRecipe, EmptyRuntime> {
public:
    static loom::ModuleHeader moduleHeader() {
        return {.api_version = loom::kApiVersion, .name = "ObjVecModule", .version = "1.0.0"};
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

class DataEngineTest : public ::testing::Test {
protected:
    loom::DataEngine engine;
    TestModule module;

    void SetUp() override {
        engine.registerModule("test", &module);
    }
};

TEST_F(DataEngineTest, ReadConfig) {
    auto json = engine.readSection("test", loom::DataSection::Config);
    EXPECT_NE(json, "{}");
    // Should contain the default values
    EXPECT_NE(json.find("42"), std::string::npos);     // rate
    EXPECT_NE(json.find("3.14"), std::string::npos);   // gain
    EXPECT_NE(json.find("test"), std::string::npos);   // name
}

TEST_F(DataEngineTest, ReadRecipe) {
    auto json = engine.readSection("test", loom::DataSection::Recipe);
    EXPECT_NE(json.find("100"), std::string::npos);    // target
    EXPECT_NE(json.find("true"), std::string::npos);   // enabled
}

TEST_F(DataEngineTest, ReadRuntime) {
    auto json = engine.readSection("test", loom::DataSection::Runtime);
    EXPECT_NE(json.find("0"), std::string::npos);      // value and counter both 0
}

TEST_F(DataEngineTest, WriteConfig) {
    std::string newConfig = R"({"rate":99,"gain":2.71,"name":"updated"})";
    EXPECT_TRUE(engine.writeSection("test", loom::DataSection::Config, newConfig));

    // Verify it changed
    EXPECT_EQ(module.config().rate, 99);
    EXPECT_DOUBLE_EQ(module.config().gain, 2.71);
    EXPECT_EQ(module.config().name, "updated");
}

TEST_F(DataEngineTest, WriteRecipe) {
    std::string newRecipe = R"({"target":200.0,"enabled":false})";
    EXPECT_TRUE(engine.writeSection("test", loom::DataSection::Recipe, newRecipe));

    EXPECT_DOUBLE_EQ(module.recipe().target, 200.0);
    EXPECT_FALSE(module.recipe().enabled);
}

TEST_F(DataEngineTest, CyclicUpdatesRuntime) {
    module.cyclic();
    module.cyclic();
    module.cyclic();

    auto json = engine.readSection("test", loom::DataSection::Runtime);
    EXPECT_NE(json.find("3"), std::string::npos); // counter = 3 or value = 3.0
    EXPECT_EQ(module.runtime().counter, 3u);
    EXPECT_DOUBLE_EQ(module.runtime().value, 3.0);
}

TEST_F(DataEngineTest, ReadNonExistentModule) {
    auto json = engine.readSection("nonexistent", loom::DataSection::Config);
    EXPECT_EQ(json, "{}");
}

TEST_F(DataEngineTest, WriteNonExistentModule) {
    EXPECT_FALSE(engine.writeSection("nonexistent", loom::DataSection::Config, "{}"));
}

TEST_F(DataEngineTest, UnregisterModule) {
    engine.unregisterModule("test");
    auto json = engine.readSection("test", loom::DataSection::Config);
    EXPECT_EQ(json, "{}");
}

// ===========================================================================
// patchSection — scalar fields
// ===========================================================================
TEST_F(DataEngineTest, PatchConfigScalar) {
    EXPECT_TRUE(engine.patchSection("test", loom::DataSection::Config, "/rate", "99"));
    EXPECT_EQ(module.config().rate, 99);
    // Other fields must be untouched
    EXPECT_DOUBLE_EQ(module.config().gain, 3.14);
    EXPECT_EQ(module.config().name, "test");
}

TEST_F(DataEngineTest, PatchRecipeScalar) {
    EXPECT_TRUE(engine.patchSection("test", loom::DataSection::Recipe, "/target", "250.0"));
    EXPECT_DOUBLE_EQ(module.recipe().target, 250.0);
    EXPECT_TRUE(module.recipe().enabled);  // untouched
}

TEST_F(DataEngineTest, PatchRuntimeScalar) {
    EXPECT_TRUE(engine.patchSection("test", loom::DataSection::Runtime, "/value", "42.0"));
    EXPECT_DOUBLE_EQ(module.runtime().value, 42.0);
    EXPECT_EQ(module.runtime().counter, 0u);  // untouched
}

TEST_F(DataEngineTest, PatchNonExistentField) {
    // Unknown path: TagTable lookup fails → returns false
    EXPECT_FALSE(engine.patchSection("test", loom::DataSection::Config, "/does_not_exist", "1"));
}

TEST_F(DataEngineTest, PatchNonExistentModule) {
    EXPECT_FALSE(engine.patchSection("missing", loom::DataSection::Config, "/rate", "1"));
}

// ===========================================================================
// patchSection — vector elements (the original regression case)
// ===========================================================================
class VectorPatchTest : public ::testing::Test {
protected:
    loom::DataEngine engine;
    VectorModule module;
    void SetUp() override { engine.registerModule("vec", &module); }
};

TEST_F(VectorPatchTest, PatchConfigVectorElement) {
    // Update only the 3rd element (index 2); others must be untouched.
    EXPECT_TRUE(engine.patchSection("vec", loom::DataSection::Config, "/speeds/2", "99"));
    EXPECT_EQ(module.config().speeds[0], 10);
    EXPECT_EQ(module.config().speeds[1], 20);
    EXPECT_EQ(module.config().speeds[2], 99);
    EXPECT_EQ(module.config().speeds[3], 40);
    EXPECT_EQ(module.config().speeds[4], 50);
    // Other fields untouched
    EXPECT_EQ(module.config().label, "default");
}

TEST_F(VectorPatchTest, PatchRecipeVectorElement) {
    EXPECT_TRUE(engine.patchSection("vec", loom::DataSection::Recipe, "/setpoints/1", "9.9"));
    EXPECT_DOUBLE_EQ(module.recipe().setpoints[0], 1.0);
    EXPECT_DOUBLE_EQ(module.recipe().setpoints[1], 9.9);
    EXPECT_DOUBLE_EQ(module.recipe().setpoints[2], 3.0);
}

TEST_F(VectorPatchTest, PatchRuntimeVectorElement) {
    EXPECT_TRUE(engine.patchSection("vec", loom::DataSection::Runtime, "/readings/0", "5.5"));
    EXPECT_DOUBLE_EQ(module.runtime().readings[0], 5.5);
    EXPECT_DOUBLE_EQ(module.runtime().readings[1], 0.0);
    EXPECT_DOUBLE_EQ(module.runtime().readings[2], 0.0);
    EXPECT_DOUBLE_EQ(module.runtime().scalar, 7.0);  // untouched
}

TEST_F(VectorPatchTest, PatchMultipleVectorElementsIndependently) {
    // Simulate sequential UI edits — each must not clobber the previous.
    EXPECT_TRUE(engine.patchSection("vec", loom::DataSection::Config, "/speeds/0", "11"));
    EXPECT_TRUE(engine.patchSection("vec", loom::DataSection::Config, "/speeds/4", "55"));
    EXPECT_EQ(module.config().speeds[0], 11);
    EXPECT_EQ(module.config().speeds[1], 20);  // untouched
    EXPECT_EQ(module.config().speeds[2], 30);  // untouched
    EXPECT_EQ(module.config().speeds[3], 40);  // untouched
    EXPECT_EQ(module.config().speeds[4], 55);
}

TEST_F(VectorPatchTest, PatchConfigScalarAfterVectorPatch) {
    // Ensure that patching the vector doesn't corrupt other fields.
    EXPECT_TRUE(engine.patchSection("vec", loom::DataSection::Config, "/speeds/2", "77"));
    EXPECT_TRUE(engine.patchSection("vec", loom::DataSection::Config, "/label", "\"updated\""));
    EXPECT_EQ(module.config().speeds[2], 77);
    EXPECT_EQ(module.config().label, "updated");
    // Remaining vector elements untouched
    EXPECT_EQ(module.config().speeds[0], 10);
    EXPECT_EQ(module.config().speeds[1], 20);
}

// ===========================================================================
// patchSection — vector of objects (nested struct fields)
// ===========================================================================
class ObjectVectorPatchTest : public ::testing::Test {
protected:
    loom::DataEngine engine;
    ObjectVectorModule module;
    void SetUp() override { engine.registerModule("ov", &module); }
};

TEST_F(ObjectVectorPatchTest, PatchNestedFieldInVectorElement) {
    // Update motors[1].speed; all other fields and elements must be untouched.
    EXPECT_TRUE(engine.patchSection("ov", loom::DataSection::Config, "/motors/1/speed", "99.9"));
    EXPECT_DOUBLE_EQ(module.config().motors[0].speed, 1.0);   // untouched
    EXPECT_DOUBLE_EQ(module.config().motors[1].speed, 99.9);  // updated
    EXPECT_DOUBLE_EQ(module.config().motors[2].speed, 3.0);   // untouched
    // Sibling fields within the same element untouched
    EXPECT_FALSE(module.config().motors[1].enabled);
    EXPECT_EQ(module.config().motors[1].id, 2);
}

TEST_F(ObjectVectorPatchTest, PatchBoolFieldInVectorElement) {
    EXPECT_TRUE(engine.patchSection("ov", loom::DataSection::Config, "/motors/0/enabled", "false"));
    EXPECT_FALSE(module.config().motors[0].enabled);
    // Other elements untouched
    EXPECT_TRUE(module.config().motors[2].enabled);
}

TEST_F(ObjectVectorPatchTest, PatchIntFieldInVectorElement) {
    EXPECT_TRUE(engine.patchSection("ov", loom::DataSection::Config, "/motors/2/id", "42"));
    EXPECT_EQ(module.config().motors[2].id, 42);
    EXPECT_EQ(module.config().motors[0].id, 1);  // untouched
    EXPECT_EQ(module.config().motors[1].id, 2);  // untouched
}

TEST_F(ObjectVectorPatchTest, PatchMultipleNestedFieldsIndependently) {
    EXPECT_TRUE(engine.patchSection("ov", loom::DataSection::Config, "/motors/0/speed", "10.0"));
    EXPECT_TRUE(engine.patchSection("ov", loom::DataSection::Config, "/motors/1/enabled", "true"));
    EXPECT_TRUE(engine.patchSection("ov", loom::DataSection::Config, "/motors/2/id", "99"));
    // Each edit is independent
    EXPECT_DOUBLE_EQ(module.config().motors[0].speed, 10.0);
    EXPECT_TRUE(module.config().motors[1].enabled);
    EXPECT_EQ(module.config().motors[2].id, 99);
    // Other fields untouched
    EXPECT_DOUBLE_EQ(module.config().motors[1].speed, 2.0);
    EXPECT_EQ(module.config().count, 3);
}

TEST_F(ObjectVectorPatchTest, PatchTopLevelScalarUnaffectedByNestedPatch) {
    EXPECT_TRUE(engine.patchSection("ov", loom::DataSection::Config, "/motors/0/speed", "5.0"));
    EXPECT_EQ(module.config().count, 3);  // untouched top-level field
}

} // namespace
