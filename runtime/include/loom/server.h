#pragma once

#include "loom/runtime_core.h"

#include <atomic>
#include <string>
#include <thread>

namespace loom {

/// Configuration for the HTTP/WebSocket server.
struct ServerConfig {
    uint16_t port = 8080;
    std::string bindAddress = "127.0.0.1";
    int wsUpdateIntervalMs = 100;
    std::string staticDir = "./data/UI";
};

/// REST + WebSocket server — thin translation layer over RuntimeCore.
///
/// REST endpoints:
///   GET  /api/modules                         — List all loaded modules
///   GET  /api/modules/:id                     — Module detail (header, state, stats, data)
///   GET/POST /api/modules/:id/data/:section   — Read/write config, recipe, or runtime
///   POST /api/modules/:id/reload              — Warm-restart a module
///   GET  /api/bus/topics                      — List bus topics
///   GET  /api/bus/services                    — List bus services
///   POST /api/bus/call/:name                  — Call a bus service
///
/// WebSocket:
///   /ws  — Live runtime data stream.
///
class Server {
public:
    Server(RuntimeCore& core, const ServerConfig& config = {});
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void start();
    void stop();

    bool isRunning() const { return running_.load(); }

private:
    RuntimeCore&  core_;
    ServerConfig  config_;

    std::atomic<bool> running_{false};
    std::thread serverThread_;
};

} // namespace loom
