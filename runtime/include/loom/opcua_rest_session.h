#pragma once

#include "loom/opcua_rest_nodeid.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace crow::websocket { class connection; }

// mapp Connect facade — session / subscription / monitored-item bookkeeping.
//
// This is a minimal stand-in for OPC-UA sessions: integer IDs, no real auth.
// One std::mutex guards everything. Lock ordering (must be respected to avoid
// deadlock with RuntimeCore::moduleMutex): take SessionManager::mutex() BEFORE
// any shared_lock on the module mutex, never the reverse. REST handlers that
// need both must finish their SessionManager work (e.g. touch()) before taking
// the module lock.
namespace loom::opcrest {

struct MonitoredItem {
    uint64_t    monitoredItemId = 0;
    int64_t     clientHandle    = 0;
    std::string nodeId;
    ParsedNode  parsed;
    std::string attribute = "Value";

    // Change-detection state — mutated only by the pump thread while it holds
    // SessionManager::mutex().
    std::string lastJson;
    uint32_t    lastStatus = 0xFFFFFFFFu;  // sentinel: nothing sent yet
    bool        firstSent  = false;
};

struct Subscription {
    uint64_t    subscriptionId      = 0;
    double      publishingIntervalMs = 1000.0;
    std::unordered_map<uint64_t, MonitoredItem> items;  // keyed by monitoredItemId
    std::chrono::steady_clock::time_point nextDue{};
};

struct Session {
    uint64_t    id = 0;
    std::chrono::steady_clock::time_point lastSeen{};
    double      timeoutMs = 30000.0;
    std::unordered_map<uint64_t, Subscription> subs;  // keyed by subscriptionId
    crow::websocket::connection* pushConn = nullptr;
};

class SessionManager {
public:
    /// The pump thread locks this directly for an entire sample tick (snapshot,
    /// diff, and send). REST handlers use the methods below, which lock briefly.
    std::mutex& mutex() { return mu_; }

    // --- sessions ---
    uint64_t createSession(double timeoutMs) {
        std::lock_guard lk(mu_);
        uint64_t id = nextSessionId_++;
        Session s;
        s.id = id;
        s.timeoutMs = timeoutMs > 0 ? timeoutMs : 30000.0;
        s.lastSeen = clock_::now();
        sessions_.emplace(id, std::move(s));
        return id;
    }
    bool hasSession(uint64_t id) {
        std::lock_guard lk(mu_);
        return sessions_.count(id) != 0;
    }
    void touch(uint64_t id) {
        std::lock_guard lk(mu_);
        auto it = sessions_.find(id);
        if (it != sessions_.end()) it->second.lastSeen = clock_::now();
    }
    bool deleteSession(uint64_t id) {
        std::lock_guard lk(mu_);
        return sessions_.erase(id) != 0;
    }

    // --- subscriptions ---
    std::optional<uint64_t> createSubscription(uint64_t sessionId, double publishingMs) {
        std::lock_guard lk(mu_);
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) return std::nullopt;
        uint64_t sid = nextSubId_++;
        Subscription sub;
        sub.subscriptionId = sid;
        sub.publishingIntervalMs = publishingMs > 0 ? publishingMs : 1000.0;
        sub.nextDue = clock_::now();
        it->second.subs.emplace(sid, std::move(sub));
        return sid;
    }
    bool deleteSubscription(uint64_t sessionId, uint64_t subId) {
        std::lock_guard lk(mu_);
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) return false;
        return it->second.subs.erase(subId) != 0;
    }

    // --- monitored items ---
    std::optional<uint64_t> addMonitoredItem(uint64_t sessionId, uint64_t subId, MonitoredItem item) {
        std::lock_guard lk(mu_);
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) return std::nullopt;
        auto sit = it->second.subs.find(subId);
        if (sit == it->second.subs.end()) return std::nullopt;
        uint64_t mid = nextItemId_++;
        item.monitoredItemId = mid;
        sit->second.items.emplace(mid, std::move(item));
        return mid;
    }
    bool deleteMonitoredItem(uint64_t sessionId, uint64_t subId, uint64_t itemId) {
        std::lock_guard lk(mu_);
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) return false;
        auto sit = it->second.subs.find(subId);
        if (sit == it->second.subs.end()) return false;
        return sit->second.items.erase(itemId) != 0;
    }

    // --- pushchannel binding ---
    void bindPushConn(uint64_t sessionId, crow::websocket::connection* c) {
        std::lock_guard lk(mu_);
        auto it = sessions_.find(sessionId);
        if (it != sessions_.end()) it->second.pushConn = c;
    }
    void unbindPushConn(crow::websocket::connection* c) {
        std::lock_guard lk(mu_);
        for (auto& [id, s] : sessions_)
            if (s.pushConn == c) s.pushConn = nullptr;
    }

    /// Direct access for the pump thread, which already holds mutex().
    std::unordered_map<uint64_t, Session>& sessions() { return sessions_; }

private:
    using clock_ = std::chrono::steady_clock;
    std::mutex mu_;
    std::unordered_map<uint64_t, Session> sessions_;
    uint64_t nextSessionId_ = 1;
    uint64_t nextSubId_     = 1;
    uint64_t nextItemId_    = 1;
};

} // namespace loom::opcrest
