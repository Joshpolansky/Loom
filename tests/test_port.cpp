#include <gtest/gtest.h>

#include "loom/bus.h"
#include "loom/port.h"

#include <string_view>

namespace {

using namespace loom;

struct Widget { static constexpr std::string_view kTypeId = "Widget/1"; int x = 0; };
struct Other  { static constexpr std::string_view kTypeId = "Other/1";  int y = 0; };

TEST(Port, RegisterResolveTyped) {
    Bus bus; Widget w; w.x = 42;
    bus.registerPort("a/0", &w, Widget::kTypeId);
    Widget* p = bus.port<Widget>("a/0");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->x, 42);
}

TEST(Port, TypeMismatchReturnsNull) {
    Bus bus; Widget w;
    bus.registerPort("a/0", &w, Widget::kTypeId);
    EXPECT_EQ(bus.port<Other>("a/0"), nullptr);   // wrong type id → null
}

TEST(Port, MissingReturnsNull) {
    Bus bus;
    EXPECT_EQ(bus.port<Widget>("none"), nullptr);
}

TEST(Port, UnregisterRemoves) {
    Bus bus; Widget w;
    bus.registerPort("a/0", &w, Widget::kTypeId);
    bus.unregisterPort("a/0");
    EXPECT_EQ(bus.port<Widget>("a/0"), nullptr);
}

TEST(Port, RefCachesAndReresolvesOnGeneration) {
    Bus bus; Widget w1; w1.x = 1; Widget w2; w2.x = 2;
    PortRef<Widget> ref(&bus, "a/0");
    EXPECT_EQ(ref.get(), nullptr);                 // not registered yet

    bus.registerPort("a/0", &w1, Widget::kTypeId);
    ASSERT_NE(ref.get(), nullptr);
    EXPECT_EQ(ref.get()->x, 1);

    bus.registerPort("a/0", &w2, Widget::kTypeId); // provider re-registered (e.g. reload)
    ASSERT_NE(ref.get(), nullptr);
    EXPECT_EQ(ref.get()->x, 2);                     // ref re-resolved on generation bump

    bus.unregisterPort("a/0");
    EXPECT_EQ(ref.get(), nullptr);
}

} // namespace
