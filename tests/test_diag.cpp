#include "loom/diag/breadcrumb.h"
#include "loom/diag/guard.h"
#include "loom/diag/fault_report.h"
#include "loom/diag/fault_store.h"

#include <glaze/glaze.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

using namespace loom::diag;

// --- BreadcrumbScope -------------------------------------------------------

TEST(DiagBreadcrumb, SetsAndRestores) {
    EXPECT_EQ(tlsBreadcrumb.phase, Phase::None);
    {
        BreadcrumbScope s(Phase::Cyclic, "mod_a", "ClassA");
        EXPECT_EQ(tlsBreadcrumb.phase, Phase::Cyclic);
        EXPECT_STREQ(tlsBreadcrumb.moduleId, "mod_a");
        EXPECT_STREQ(tlsBreadcrumb.className, "ClassA");
    }
    EXPECT_EQ(tlsBreadcrumb.phase, Phase::None);  // restored
}

TEST(DiagBreadcrumb, NestedRestores) {
    BreadcrumbScope outer(Phase::Cyclic, "mod_a", "A");
    {
        BreadcrumbScope inner(Phase::Service, "mod_b", "B");
        EXPECT_EQ(tlsBreadcrumb.phase, Phase::Service);
        EXPECT_STREQ(tlsBreadcrumb.moduleId, "mod_b");
    }
    EXPECT_EQ(tlsBreadcrumb.phase, Phase::Cyclic);  // back to outer
    EXPECT_STREQ(tlsBreadcrumb.moduleId, "mod_a");
}

// --- guard -----------------------------------------------------------------

TEST(DiagGuard, NoThrowReturnsTrueNoFault) {
    bool faulted = false;
    bool ran = false;
    bool ok = guard(Phase::Cyclic, "m", "C",
                    [&]{ ran = true; },
                    [&](const FaultInfo&){ faulted = true; });
    EXPECT_TRUE(ok);
    EXPECT_TRUE(ran);
    EXPECT_FALSE(faulted);
    EXPECT_EQ(tlsBreadcrumb.phase, Phase::None);  // breadcrumb restored
}

TEST(DiagGuard, StdExceptionCaughtReported) {
    std::string msg;
    Phase capturedPhase = Phase::None;
    bool ok = guard(Phase::PreCyclic, "m", "C",
                    [&]{ throw std::runtime_error("boom"); },
                    [&](const FaultInfo& f){ msg = std::string(f.message); capturedPhase = f.phase; });
    EXPECT_FALSE(ok);
    EXPECT_EQ(msg, "boom");
    EXPECT_EQ(capturedPhase, Phase::PreCyclic);
    EXPECT_EQ(tlsBreadcrumb.phase, Phase::None);  // restored even on throw
}

TEST(DiagGuard, NonStdThrowCaught) {
    bool faulted = false;
    bool ok = guard(Phase::Cyclic, "m", "C",
                    [&]{ throw 42; },              // non-std exception
                    [&](const FaultInfo&){ faulted = true; });
    EXPECT_FALSE(ok);
    EXPECT_TRUE(faulted);
}

// --- FaultReport JSON ------------------------------------------------------

static FaultReport sampleReport() {
    FaultReport r;
    r.id = "mod_a-123-0";
    r.tsMs = 123;
    r.kind = FaultKind::Exception;
    r.reason = "boom \"quoted\"\nnewline";   // exercise escaping
    r.sdkVersion = "0.3.0";
    r.gitSha = "abc123";
    r.buildType = "Debug";
    r.moduleId = "mod_a";
    r.className = "ClassA";
    r.phase = Phase::Cyclic;
    r.cycle = 7;
    r.frames.push_back(SymFrame{0xdead, "foo()", "foo.cpp", 42});
    FaultSections s;
    s.runtime = R"({"pos":1.5})";
    r.sections = s;
    return r;
}

TEST(DiagFaultReport, ToJsonIsParseableAndPreservesFields) {
    std::string json = toJson(sampleReport());

    glz::json_t doc;
    ASSERT_FALSE(glz::read_json(doc, json)) << "report JSON must parse";
    ASSERT_TRUE(doc.is_object());

    EXPECT_EQ(doc["id"].get<std::string>(), "mod_a-123-0");
    EXPECT_EQ(doc["kind"].get<std::string>(), "exception");
    EXPECT_EQ(doc["reason"].get<std::string>(), "boom \"quoted\"\nnewline");  // round-trips escaping
    EXPECT_EQ(doc["breadcrumb"]["module"].get<std::string>(), "mod_a");
    EXPECT_EQ(doc["breadcrumb"]["phase"].get<std::string>(), "cyclic");
    EXPECT_EQ(static_cast<int>(doc["breadcrumb"]["cycle"].get<double>()), 7);
    ASSERT_TRUE(doc["frames"].is_array());
    EXPECT_EQ(doc["frames"][0]["function"].get<std::string>(), "foo()");
    // sections embed as real nested JSON, not an escaped string
    ASSERT_TRUE(doc["sections"]["runtime"].is_object());
    EXPECT_EQ(static_cast<double>(doc["sections"]["runtime"]["pos"].get<double>()), 1.5);
}

// --- FaultStore ------------------------------------------------------------

TEST(DiagFaultStore, RecordListDetailRoundTrip) {
    auto dir = std::filesystem::temp_directory_path() /
               ("loom_faults_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    std::filesystem::remove_all(dir);

    FaultStore store(dir);
    EXPECT_TRUE(store.list().empty());

    std::string id = store.record(sampleReport());
    EXPECT_EQ(id, "mod_a-123-0");

    auto list = store.list();
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].id, "mod_a-123-0");
    EXPECT_EQ(list[0].kind, "exception");
    EXPECT_EQ(list[0].moduleId, "mod_a");
    EXPECT_EQ(list[0].phase, "cyclic");

    auto detail = store.detailJson(id);
    ASSERT_TRUE(detail.has_value());
    EXPECT_NE(detail->find("\"boom"), std::string::npos);

    EXPECT_FALSE(store.detailJson("nope").has_value());

    // A fresh store over the same dir re-loads the persisted report.
    FaultStore reopened(dir);
    ASSERT_EQ(reopened.list().size(), 1u);
    EXPECT_EQ(reopened.list()[0].id, "mod_a-123-0");

    std::filesystem::remove_all(dir);
}

// The POSIX crash handler writes a raw loom-crash-<pid>.txt fallback AND a
// best-effort structured loom-crash-<pid>.json. When both exist for the same
// crash, the store must surface only the structured one.
TEST(DiagFaultStore, JsonSupersedesRawTxtSibling) {
    auto dir = std::filesystem::temp_directory_path() /
               ("loom_faults_super_" +
                std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    auto write = [&](const std::string& name, const std::string& content) {
        std::ofstream f(dir / name, std::ios::binary | std::ios::trunc);
        f << content;
    };

    // Crash A: both .txt and .json present -> json wins.
    FaultReport a = sampleReport();
    a.id = "loom-crash-111";
    a.kind = FaultKind::Signal;
    write("loom-crash-111.txt", "=== raw ===\n");
    write("loom-crash-111.json", toJson(a));
    // Crash B: only the raw .txt survived (structured pass failed) -> raw shown.
    write("loom-crash-222.txt", "=== raw ===\n");

    FaultStore store(dir);
    auto list = store.list();
    ASSERT_EQ(list.size(), 2u);

    auto byId = [&](const std::string& id) {
        return std::find_if(list.begin(), list.end(),
                            [&](const auto& s) { return s.id == id; });
    };
    auto a_it = byId("loom-crash-111");
    auto b_it = byId("loom-crash-222");
    ASSERT_NE(a_it, list.end());
    ASSERT_NE(b_it, list.end());
    EXPECT_EQ(a_it->kind, "signal");  // structured json, not the raw txt
    EXPECT_EQ(b_it->kind, "raw");     // only the txt existed

    std::filesystem::remove_all(dir);
}

// Retention cap: the store keeps only the newest kMaxEntries faults, in memory
// AND on disk — an over-full crash dir is trimmed at construction, and record()
// evicts the oldest entry (deleting its report file) once at the cap.
TEST(DiagFaultStore, RetentionCapPrunesMemoryAndDisk) {
    auto dir = std::filesystem::temp_directory_path() /
               ("loom_faults_cap_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    // Seed cap+50 structured reports on disk, ids ordered by timestamp.
    const std::size_t seeded = FaultStore::kMaxEntries + 50;
    auto idFor = [](std::size_t i) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "fault-%04zu", i);
        return std::string(buf);
    };
    for (std::size_t i = 0; i < seeded; ++i) {
        std::ofstream f(dir / (idFor(i) + ".json"), std::ios::binary);
        f << R"({"ts":)" << (1000 + i) << R"(,"kind":"exception","reason":"synthetic"})";
    }

    // Construction trims to the cap: newest kept, oldest gone, disk pruned.
    FaultStore store(dir);
    auto list = store.list();  // newest first
    ASSERT_EQ(list.size(), FaultStore::kMaxEntries);
    EXPECT_EQ(list.front().id, idFor(seeded - 1));
    EXPECT_EQ(list.back().id,  idFor(seeded - FaultStore::kMaxEntries));
    std::size_t onDisk = 0;
    for (const auto& de : std::filesystem::directory_iterator(dir))
        if (de.is_regular_file()) ++onDisk;
    EXPECT_EQ(onDisk, FaultStore::kMaxEntries);
    EXPECT_FALSE(std::filesystem::exists(dir / (idFor(0) + ".json")));

    // record() at the cap evicts exactly the oldest (memory + its file).
    const std::string evicted = list.back().id;
    FaultReport r = sampleReport();
    r.id   = "newest-recorded";
    r.tsMs = 999999;
    store.record(r);
    list = store.list();
    ASSERT_EQ(list.size(), FaultStore::kMaxEntries);
    EXPECT_EQ(list.front().id, "newest-recorded");
    EXPECT_NE(list.back().id, evicted);
    EXPECT_FALSE(std::filesystem::exists(dir / (evicted + ".json")));
    EXPECT_TRUE(std::filesystem::exists(dir / "newest-recorded.json"));

    std::filesystem::remove_all(dir);
}
