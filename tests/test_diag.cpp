#include "loom/diag/breadcrumb.h"
#include "loom/diag/guard.h"

#include <gtest/gtest.h>

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
