#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace loom {

/// Result of a service call.
struct CallResult {
    bool ok = false;
    std::string response;   // JSON response payload
    std::string error;      // Error message if !ok
};

/// Callback type for topic subscriptions.
using TopicCallback = std::function<void(std::string_view topic, std::string_view payload)>;

/// Callback type for service handlers. Returns JSON response.
using ServiceHandler = std::function<CallResult(std::string_view request)>;

/// Address helper — combines module instance ID with a local name.
/// e.g., moduleAddress("left_motor", "status") -> "left_motor/status"
inline std::string moduleAddress(std::string_view moduleId, std::string_view name) {
    std::string addr;
    addr.reserve(moduleId.size() + 1 + name.size());
    addr.append(moduleId);
    addr.push_back('/');
    addr.append(name);
    return addr;
}

/// Central message bus for inter-module communication.
///
/// Provides three communication patterns:
///   1. **Topic pub/sub** — Fire-and-forget broadcast messages (JSON payloads).
///   2. **Service RPC** — Synchronous request/response calls.
///   3. **Async RPC** — Returns std::future<CallResult> for non-blocking calls.
///
/// Namespace convention: prefix with module instance ID (e.g., "left_motor/status").
/// All operations are thread-safe.
class Bus {
public:
    Bus() = default;
    ~Bus() = default;

    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;

    // =========================================================================
    // Topic Pub/Sub
    // =========================================================================

    inline uint64_t subscribe(const std::string& topic, TopicCallback callback) {
        std::lock_guard lock(topicMutex_);
        auto id = nextSubId_++;
        topicSubs_[topic].push_back({id, topic, std::move(callback)});
        return id;
    }

    inline void unsubscribe(uint64_t subscriptionId) {
        std::lock_guard lock(topicMutex_);
        for (auto& [topic, subs] : topicSubs_) {
            auto it = std::remove_if(subs.begin(), subs.end(),
                [subscriptionId](const Subscription& s) { return s.id == subscriptionId; });
            if (it != subs.end()) {
                subs.erase(it, subs.end());
                return;
            }
        }
    }

    inline void unsubscribeByPrefix(std::string_view prefix) {
        std::lock_guard lock(topicMutex_);
        std::vector<std::string> emptyTopics;
        for (auto& [topic, subs] : topicSubs_) {
            if (topic.starts_with(prefix)) {
                subs.clear();
                emptyTopics.push_back(topic);
            }
        }
        for (auto& t : emptyTopics) {
            topicSubs_.erase(t);
        }
    }

    inline size_t publish(const std::string& topic, std::string_view payload) {
        std::lock_guard lock(topicMutex_);
        auto it = topicSubs_.find(topic);
        if (it == topicSubs_.end()) return 0;
        size_t count = 0;
        for (auto& sub : it->second) {
            sub.callback(topic, payload);
            ++count;
        }
        return count;
    }

    // =========================================================================
    // Service RPC
    // =========================================================================

    /// Register a service handler. schema is a JSON Schema string describing
    /// the request payload (auto-populated by typed registerLocalService).
    inline bool registerService(const std::string& name, ServiceHandler handler,
                                std::string schema = "") {
        std::lock_guard lock(serviceMutex_);
        if (serviceHandlers_.contains(name)) return false;
        serviceHandlers_.emplace(name, std::move(handler));
        serviceSchemas_.emplace(name, std::move(schema));
        return true;
    }

    inline void unregisterService(const std::string& name) {
        std::lock_guard lock(serviceMutex_);
        serviceHandlers_.erase(name);
        serviceSchemas_.erase(name);
    }

    inline void unregisterServicesByPrefix(std::string_view prefix) {
        std::lock_guard lock(serviceMutex_);
        for (auto it = serviceHandlers_.begin(); it != serviceHandlers_.end(); ) {
            if (it->first.starts_with(prefix)) {
                serviceSchemas_.erase(it->first);
                it = serviceHandlers_.erase(it);
            } else {
                ++it;
            }
        }
    }

    inline CallResult call(const std::string& service, std::string_view request) {
        ServiceHandler handler;
        {
            std::lock_guard lock(serviceMutex_);
            auto it = serviceHandlers_.find(service);
            if (it == serviceHandlers_.end()) {
                return {false, {}, "service not found: " + service};
            }
            handler = it->second;
        }
        return handler(request);
    }

    // =========================================================================
    // Async RPC
    // =========================================================================

    inline std::future<CallResult> callAsync(const std::string& service, std::string request) {
        return std::async(std::launch::async, [this, service, req = std::move(request)]() {
            return call(service, req);
        });
    }

    // =========================================================================
    // Introspection
    // =========================================================================

    inline std::vector<std::string> topics() const {
        std::lock_guard lock(topicMutex_);
        std::vector<std::string> result;
        result.reserve(topicSubs_.size());
        for (auto& [topic, subs] : topicSubs_) {
            if (!subs.empty()) result.push_back(topic);
        }
        return result;
    }

    inline std::vector<std::string> services() const {
        std::lock_guard lock(serviceMutex_);
        std::vector<std::string> result;
        result.reserve(serviceHandlers_.size());
        for (auto& [name, _] : serviceHandlers_) {
            result.push_back(name);
        }
        return result;
    }

    /// Returns all services with their request schemas (for UI introspection).
    struct ServiceInfo {
        std::string name;
        std::string schema; ///< JSON Schema for request payload, or empty
    };
    inline std::vector<ServiceInfo> serviceInfos() const {
        std::lock_guard lock(serviceMutex_);
        std::vector<ServiceInfo> result;
        result.reserve(serviceHandlers_.size());
        for (auto& [name, _] : serviceHandlers_) {
            auto sit = serviceSchemas_.find(name);
            result.push_back({name, sit != serviceSchemas_.end() ? sit->second : ""});
        }
        return result;
    }

private:
    struct Subscription {
        uint64_t id;
        std::string topic;
        TopicCallback callback;
    };

    mutable std::mutex topicMutex_;
    std::unordered_map<std::string, std::vector<Subscription>> topicSubs_;
    uint64_t nextSubId_ = 1;

    mutable std::mutex serviceMutex_;
    std::unordered_map<std::string, ServiceHandler> serviceHandlers_;
    std::unordered_map<std::string, std::string> serviceSchemas_;
};

} // namespace loom
