#pragma once

#include "loom/opcua_rest_session.h"

#include <atomic>
#include <string>
#include <thread>

// Forward-declare crow::SimpleApp so this header stays free of <crow.h>.
// crow::App is an alias template over the Crow<> class, and SimpleApp == Crow<>,
// so we declare the underlying class template here (matching crow/app.h).
namespace crow {
template <typename... Middlewares> class Crow;
using SimpleApp = Crow<>;
} // namespace crow

namespace loom {

class RuntimeCore;

/// mapp Connect-compatible facade: a subset of B&R's mapp Connect REST API
/// (base path /api/1.0) plus the /api/1.0/pushchannel WebSocket. Lets a
/// LuxConnect client connect, browse, read/write, and stream live values from
/// this runtime. Additive — it shares RuntimeCore with the existing Server and
/// does not touch /ws, /ws/watch, or /api/*.
class OpcUaRestServer {
public:
    explicit OpcUaRestServer(RuntimeCore& core);
    ~OpcUaRestServer();

    OpcUaRestServer(const OpcUaRestServer&) = delete;
    OpcUaRestServer& operator=(const OpcUaRestServer&) = delete;

    /// Register all facade routes on the Crow app. Must be called before app.run().
    void registerRoutes(crow::SimpleApp& app);

    /// Start the single-producer notification pump (one thread for all sessions).
    void startPump(int intervalMs);
    void stopPump();

private:
    RuntimeCore&          core_;
    opcrest::SessionManager sm_;

    std::atomic<bool> pumpRunning_{false};
    std::thread       pumpThread_;
    int               pumpIntervalMs_ = 50;

    void pumpLoop();
};

} // namespace loom
