#include <gtest/gtest.h>
#include "loom/tag_table.hpp"
#include <array>
#include <deque>
#include <list>
#include <map>
#include <unordered_map>
#include <optional>

// ---------------------------------------------------------------------------
// Test fixtures — plain aggregates (no macros), glaze reflects them via C++23
// ---------------------------------------------------------------------------
struct TagInner {
    double x = 1.0;
    double y = 2.0;
};

struct TagRoot {
    int                               scalar    = 42;
    std::string                       name      = "hello";
    TagInner                          nested    = {};
    std::vector<TagInner>             vec       = {{1.0, 2.0}, {3.0, 4.0}};
    std::array<int, 3>                arr       = {10, 20, 30};
    std::deque<double>                dq        = {1.1, 2.2, 3.3};
    std::list<int>                    lst       = {100, 200};
    std::map<std::string, int>        str_map   = {{"alpha", 1}, {"beta", 2}};
    std::optional<int>                maybe     = 99;
    std::optional<int>                empty_opt = std::nullopt;
    std::optional<TagInner>           opt_inner = TagInner{7.0, 8.0}; // engaged struct optional
    std::optional<TagInner>           opt_empty = std::nullopt;        // disengaged struct optional
};

class TagTableTest : public ::testing::Test {
protected:
    TagRoot root{};
    loom::TagTable<TagRoot> table{root};
};

// ---------------------------------------------------------------------------
// Scalar & nested struct
// ---------------------------------------------------------------------------
TEST_F(TagTableTest, ScalarRegistered) {
    EXPECT_TRUE(table.contains("scalar"));
    EXPECT_EQ(*table.read_json("scalar"), "42");
    EXPECT_NE(table.ptr("scalar"), nullptr);
    EXPECT_EQ(*table.type_of("scalar"), glz::name_v<int>);
}

TEST_F(TagTableTest, StringRegistered) {
    EXPECT_TRUE(table.contains("name"));
    EXPECT_EQ(*table.read_json("name"), "\"hello\"");
}

TEST_F(TagTableTest, NestedStructFields) {
    EXPECT_TRUE(table.contains("nested/x"));
    EXPECT_TRUE(table.contains("nested/y"));
    EXPECT_EQ(*table.read_json("nested/x"), "1");
    // ptr should point directly into root.nested
    auto* px = static_cast<double*>(table.ptr("nested/x"));
    ASSERT_NE(px, nullptr);
    EXPECT_EQ(px, &root.nested.x);
    root.nested.x = 9.9;
    EXPECT_EQ(*table.read_json("nested/x"), "9.9");
}

// ---------------------------------------------------------------------------
// std::vector
// ---------------------------------------------------------------------------
TEST_F(TagTableTest, VectorSelfRegistered) {
    EXPECT_TRUE(table.contains("vec"));
    EXPECT_NE(table.ptr("vec"), nullptr);
}

TEST_F(TagTableTest, VectorElementsExpanded) {
    EXPECT_TRUE(table.contains("vec/0/x"));
    EXPECT_TRUE(table.contains("vec/0/y"));
    EXPECT_TRUE(table.contains("vec/1/x"));
    EXPECT_TRUE(table.contains("vec/1/y"));
    EXPECT_EQ(*table.read_json("vec/0/x"), "1");
    EXPECT_EQ(*table.read_json("vec/1/y"), "4");
}

TEST_F(TagTableTest, VectorElementPtrLive) {
    auto* p = static_cast<double*>(table.ptr("vec/0/x"));
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p, &root.vec[0].x);
    root.vec[0].x = 7.7;
    EXPECT_EQ(*table.read_json("vec/0/x"), "7.7");
}

// ---------------------------------------------------------------------------
// std::array
// ---------------------------------------------------------------------------
TEST_F(TagTableTest, ArraySelfRegistered) {
    EXPECT_TRUE(table.contains("arr"));
    EXPECT_NE(table.ptr("arr"), nullptr);
}

TEST_F(TagTableTest, ArrayElementsExpanded) {
    EXPECT_TRUE(table.contains("arr/0"));
    EXPECT_TRUE(table.contains("arr/1"));
    EXPECT_TRUE(table.contains("arr/2"));
    EXPECT_EQ(*table.read_json("arr/0"), "10");
    EXPECT_EQ(*table.read_json("arr/2"), "30");
}

TEST_F(TagTableTest, ArrayElementPtrLive) {
    auto* p = static_cast<int*>(table.ptr("arr/1"));
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p, &root.arr[1]);
    root.arr[1] = 999;
    EXPECT_EQ(*table.read_json("arr/1"), "999");
}

// ---------------------------------------------------------------------------
// std::deque
// ---------------------------------------------------------------------------
TEST_F(TagTableTest, DequeExpanded) {
    EXPECT_TRUE(table.contains("dq"));
    EXPECT_TRUE(table.contains("dq/0"));
    EXPECT_TRUE(table.contains("dq/2"));
    EXPECT_EQ(*table.read_json("dq/1"), "2.2"); // glaze keeps significant decimals
}

// ---------------------------------------------------------------------------
// std::list
// ---------------------------------------------------------------------------
TEST_F(TagTableTest, ListExpanded) {
    EXPECT_TRUE(table.contains("lst"));
    EXPECT_TRUE(table.contains("lst/0"));
    EXPECT_TRUE(table.contains("lst/1"));
    EXPECT_EQ(*table.read_json("lst/0"), "100");
}

TEST_F(TagTableTest, ListElementPtrLive) {
    auto* p = static_cast<int*>(table.ptr("lst/0"));
    ASSERT_NE(p, nullptr);
    auto it = root.lst.begin();
    EXPECT_EQ(p, &(*it));
    *p = 777;
    EXPECT_EQ(*table.read_json("lst/0"), "777");
}

// ---------------------------------------------------------------------------
// std::map (string-keyed)
// ---------------------------------------------------------------------------
TEST_F(TagTableTest, MapSelfRegistered) {
    EXPECT_TRUE(table.contains("str_map"));
    EXPECT_NE(table.ptr("str_map"), nullptr);
}

TEST_F(TagTableTest, MapEntriesExpanded) {
    EXPECT_TRUE(table.contains("str_map/alpha"));
    EXPECT_TRUE(table.contains("str_map/beta"));
    EXPECT_EQ(*table.read_json("str_map/alpha"), "1");
    EXPECT_EQ(*table.read_json("str_map/beta"), "2");
}

TEST_F(TagTableTest, MapEntryPtrLive) {
    auto* p = static_cast<int*>(table.ptr("str_map/alpha"));
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p, &root.str_map.at("alpha"));
    root.str_map["alpha"] = 42;
    EXPECT_EQ(*table.read_json("str_map/alpha"), "42");
}

// ---------------------------------------------------------------------------
// std::optional
// ---------------------------------------------------------------------------
TEST_F(TagTableTest, OptionalEngagedRegistered) {
    EXPECT_TRUE(table.contains("maybe"));
    EXPECT_NE(table.ptr("maybe"), nullptr);
    EXPECT_EQ(*table.read_json("maybe"), "99");
}

TEST_F(TagTableTest, OptionalEmptyRegisteredAsNull) {
    EXPECT_TRUE(table.contains("empty_opt"));
    EXPECT_NE(table.ptr("empty_opt"), nullptr);
    // glaze serializes std::nullopt as null
    EXPECT_EQ(*table.read_json("empty_opt"), "null");
}

TEST_F(TagTableTest, OptionalStructTransparentRecursion) {
    // Engaged optional<TagInner> — inner fields visible at "opt_inner/x", "opt_inner/y"
    EXPECT_TRUE(table.contains("opt_inner"));
    EXPECT_TRUE(table.contains("opt_inner/x"));
    EXPECT_TRUE(table.contains("opt_inner/y"));
    EXPECT_EQ(*table.read_json("opt_inner/x"), "7");
    EXPECT_EQ(*table.read_json("opt_inner/y"), "8");
    // ptr points into the live optional value
    auto* px = static_cast<double*>(table.ptr("opt_inner/x"));
    ASSERT_NE(px, nullptr);
    EXPECT_EQ(px, &root.opt_inner->x);
    root.opt_inner->x = 99.0;
    EXPECT_EQ(*table.read_json("opt_inner/x"), "99");
}

TEST_F(TagTableTest, OptionalStructDisengagedNoInnerTags) {
    // Disengaged optional — only the optional path itself, no inner sub-paths
    EXPECT_TRUE(table.contains("opt_empty"));
    EXPECT_FALSE(table.contains("opt_empty/x"));
    EXPECT_FALSE(table.contains("opt_empty/y"));
}

// ---------------------------------------------------------------------------
// needs_refresh() — staleness detection
// ---------------------------------------------------------------------------

TEST(TagTableRefreshTest, CleanAfterBuild) {
    TagRoot root{};
    loom::TagTable<TagRoot> t(root);
    EXPECT_FALSE(t.needs_refresh());
}

TEST(TagTableRefreshTest, VectorGrowthDetected) {
    TagRoot root{};
    loom::TagTable<TagRoot> t(root);
    root.vec.push_back({5.0, 6.0});
    EXPECT_TRUE(t.needs_refresh());
}

TEST(TagTableRefreshTest, VectorShrinkDetected) {
    TagRoot root{};
    loom::TagTable<TagRoot> t(root);
    root.vec.pop_back();
    EXPECT_TRUE(t.needs_refresh());
}

TEST(TagTableRefreshTest, MapGrowthDetected) {
    TagRoot root{};
    loom::TagTable<TagRoot> t(root);
    root.str_map["gamma"] = 3;
    EXPECT_TRUE(t.needs_refresh());
}

TEST(TagTableRefreshTest, OptionalEngagementDetected) {
    TagRoot root{};
    loom::TagTable<TagRoot> t(root);
    root.empty_opt = 42;
    EXPECT_TRUE(t.needs_refresh());
}

TEST(TagTableRefreshTest, OptionalDisengagementDetected) {
    TagRoot root{};
    loom::TagTable<TagRoot> t(root);
    root.maybe.reset();
    EXPECT_TRUE(t.needs_refresh());
}

TEST(TagTableRefreshTest, ScalarChangeDoesNotTriggerRefresh) {
    // Scalars have stable structure — only container size/presence matters
    TagRoot root{};
    loom::TagTable<TagRoot> t(root);
    root.scalar = 999;
    EXPECT_FALSE(t.needs_refresh());
}
