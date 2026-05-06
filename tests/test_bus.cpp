#include "loom/bus.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

// =============================================================================
// Topic Pub/Sub Tests
// =============================================================================

TEST(BusTest, SubscribeAndPublish) {
    loom::Bus bus;
    std::string received;

    bus.subscribe("motor_1/status", [&](std::string_view topic, std::string_view payload) {
        received = std::string(payload);
    });

    auto count = bus.publish("motor_1/status", R"({"speed":100})");
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(received, R"({"speed":100})");
}

TEST(BusTest, MultipleSubscribers) {
    loom::Bus bus;
    int callCount = 0;

    bus.subscribe("estop", [&](auto, auto) { callCount++; });
    bus.subscribe("estop", [&](auto, auto) { callCount++; });
    bus.subscribe("estop", [&](auto, auto) { callCount++; });

    auto count = bus.publish("estop", "{}");
    EXPECT_EQ(count, 3u);
    EXPECT_EQ(callCount, 3);
}

TEST(BusTest, PublishNoSubscribers) {
    loom::Bus bus;
    auto count = bus.publish("nobody/listening", "{}");
    EXPECT_EQ(count, 0u);
}

TEST(BusTest, Unsubscribe) {
    loom::Bus bus;
    int callCount = 0;

    auto id = bus.subscribe("topic", [&](auto, auto) { callCount++; });
    bus.publish("topic", "{}");
    EXPECT_EQ(callCount, 1);

    bus.unsubscribe(id);
    bus.publish("topic", "{}");
    EXPECT_EQ(callCount, 1); // no change
}

TEST(BusTest, UnsubscribeByPrefix) {
    loom::Bus bus;
    int motorCount = 0;
    int otherCount = 0;

    bus.subscribe("motor_1/speed", [&](auto, auto) { motorCount++; });
    bus.subscribe("motor_1/position", [&](auto, auto) { motorCount++; });
    bus.subscribe("sensor/temp", [&](auto, auto) { otherCount++; });

    bus.publish("motor_1/speed", "{}");
    bus.publish("motor_1/position", "{}");
    bus.publish("sensor/temp", "{}");
    EXPECT_EQ(motorCount, 2);
    EXPECT_EQ(otherCount, 1);

    // Remove all motor_1 subscriptions
    bus.unsubscribeByPrefix("motor_1/");
    bus.publish("motor_1/speed", "{}");
    bus.publish("motor_1/position", "{}");
    bus.publish("sensor/temp", "{}");
    EXPECT_EQ(motorCount, 2); // unchanged
    EXPECT_EQ(otherCount, 2); // incremented
}

TEST(BusTest, TopicReceivesCorrectTopicName) {
    loom::Bus bus;
    std::string receivedTopic;

    bus.subscribe("motor_1/fault", [&](std::string_view topic, std::string_view) {
        receivedTopic = std::string(topic);
    });

    bus.publish("motor_1/fault", "{}");
    EXPECT_EQ(receivedTopic, "motor_1/fault");
}

// =============================================================================
// Service RPC Tests
// =============================================================================

TEST(BusTest, RegisterAndCallService) {
    loom::Bus bus;

    bus.registerService("motor_1/home", [](std::string_view request) -> loom::CallResult {
        return {true, R"({"accepted":true})", ""};
    });

    auto result = bus.call("motor_1/home", "{}");
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.response, R"({"accepted":true})");
    EXPECT_TRUE(result.error.empty());
}

TEST(BusTest, CallNonExistentService) {
    loom::Bus bus;
    auto result = bus.call("nonexistent", "{}");
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.empty());
}

TEST(BusTest, DuplicateServiceRegistration) {
    loom::Bus bus;

    EXPECT_TRUE(bus.registerService("svc", [](auto) -> loom::CallResult {
        return {true, "first", ""};
    }));

    EXPECT_FALSE(bus.registerService("svc", [](auto) -> loom::CallResult {
        return {true, "second", ""};
    }));

    // First handler should still be active
    auto result = bus.call("svc", "{}");
    EXPECT_EQ(result.response, "first");
}

TEST(BusTest, UnregisterService) {
    loom::Bus bus;

    bus.registerService("motor_1/stop", [](auto) -> loom::CallResult {
        return {true, "stopped", ""};
    });

    bus.unregisterService("motor_1/stop");
    auto result = bus.call("motor_1/stop", "{}");
    EXPECT_FALSE(result.ok);
}

TEST(BusTest, UnregisterServicesByPrefix) {
    loom::Bus bus;

    bus.registerService("motor_1/home", [](auto) -> loom::CallResult { return {true, "ok", ""}; });
    bus.registerService("motor_1/stop", [](auto) -> loom::CallResult { return {true, "ok", ""}; });
    bus.registerService("sensor/read", [](auto) -> loom::CallResult { return {true, "ok", ""}; });

    bus.unregisterServicesByPrefix("motor_1/");

    EXPECT_FALSE(bus.call("motor_1/home", "{}").ok);
    EXPECT_FALSE(bus.call("motor_1/stop", "{}").ok);
    EXPECT_TRUE(bus.call("sensor/read", "{}").ok);
}

TEST(BusTest, ServiceReceivesRequest) {
    loom::Bus bus;
    std::string receivedRequest;

    bus.registerService("motor_1/set_speed", [&](std::string_view request) -> loom::CallResult {
        receivedRequest = std::string(request);
        return {true, "ok", ""};
    });

    bus.call("motor_1/set_speed", R"({"speed":500})");
    EXPECT_EQ(receivedRequest, R"({"speed":500})");
}

// =============================================================================
// Async RPC Tests
// =============================================================================

TEST(BusTest, AsyncCall) {
    loom::Bus bus;

    bus.registerService("motor_1/status", [](auto) -> loom::CallResult {
        return {true, R"({"running":true})", ""};
    });

    auto future = bus.callAsync("motor_1/status", "{}");
    auto result = future.get();
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.response, R"({"running":true})");
}

TEST(BusTest, AsyncCallNonExistent) {
    loom::Bus bus;
    auto future = bus.callAsync("nope", "{}");
    auto result = future.get();
    EXPECT_FALSE(result.ok);
}

// =============================================================================
// Introspection Tests
// =============================================================================

TEST(BusTest, ListTopics) {
    loom::Bus bus;
    bus.subscribe("motor_1/speed", [](auto, auto) {});
    bus.subscribe("sensor/temp", [](auto, auto) {});

    auto topics = bus.topics();
    EXPECT_EQ(topics.size(), 2u);
}

TEST(BusTest, ListServices) {
    loom::Bus bus;
    bus.registerService("motor_1/home", [](auto) -> loom::CallResult { return {}; });
    bus.registerService("sensor/calibrate", [](auto) -> loom::CallResult { return {}; });

    auto svcs = bus.services();
    EXPECT_EQ(svcs.size(), 2u);
}

// =============================================================================
// Module Address Helper Tests
// =============================================================================

TEST(BusTest, ModuleAddress) {
    EXPECT_EQ(loom::moduleAddress("motor_1", "status"), "motor_1/status");
    EXPECT_EQ(loom::moduleAddress("left_motor", "home"), "left_motor/home");
}

// =============================================================================
// Thread Safety Smoke Test
// =============================================================================

TEST(BusTest, ConcurrentPublishSubscribe) {
    loom::Bus bus;
    std::atomic<int> totalReceived{0};

    // 4 subscribers
    for (int i = 0; i < 4; ++i) {
        bus.subscribe("stress", [&](auto, auto) { totalReceived++; });
    }

    // 4 publisher threads, each publishing 100 messages
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 100; ++i) {
                bus.publish("stress", "{}");
            }
        });
    }

    for (auto& t : threads) t.join();

    // 4 threads * 100 messages * 4 subscribers = 1600
    EXPECT_EQ(totalReceived.load(), 1600);
}
