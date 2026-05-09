#include "loom/runtime_core.h"
#include "loom/server.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

namespace loom {

namespace {
    std::atomic<bool> g_running{true};

    void signalHandler(int sig) {
        spdlog::info("Received signal {}, shutting down...", sig);
        g_running = false;
    }

    void printUsage(const char* argv0) {
        std::cerr << "Usage: " << argv0 << " --module-dir <path> [--data-dir <path>] [--cycle-ms <ms>] [--port <port>] [--bind <addr>]\n"
                  << "\n"
                  << "Options:\n"
                  << "  --module-dir <path>  Directory containing .so/.dylib module files (required)\n"
                  << "  --data-dir <path>    Directory for config/recipe persistence (default: ./data)\n"
                  << "  --cycle-ms <ms>      Default cycle period in milliseconds (default: 100)\n"
                  << "  --port <port>        HTTP/WebSocket server port (default: 8080)\n"
                  << "  --bind <addr>        Bind address (default: 127.0.0.1, use 0.0.0.0 for all interfaces)\n";
    }
} // namespace

int run(int argc, char* argv[]) {
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

    std::string moduleDir;
    std::string dataDir = "./data";
    std::string bindAddress = "127.0.0.1";
    int cycleMs = 100;
    int port = 8080;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--module-dir" && i + 1 < argc) {
            moduleDir = argv[++i];
        } else if (arg == "--data-dir" && i + 1 < argc) {
            dataDir = argv[++i];
        } else if (arg == "--cycle-ms" && i + 1 < argc) {
            cycleMs = std::stoi(argv[++i]);
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--bind" && i + 1 < argc) {
            bindAddress = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    if (moduleDir.empty()) {
        std::cerr << "Error: --module-dir is required\n";
        printUsage(argv[0]);
        return 1;
    }

    spdlog::info("Loom starting");
    spdlog::info("Module directory: {}", moduleDir);
    spdlog::info("Data directory: {}", dataDir);
    spdlog::info("Default cycle period: {}ms", cycleMs);
    spdlog::info("Server port: {}", port);
    spdlog::info("Bind address: {}", bindAddress);

    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    RuntimeConfig runtimeCfg;
    runtimeCfg.moduleDir           = moduleDir;
    runtimeCfg.dataDir             = dataDir;
    runtimeCfg.defaultCyclePeriod  = std::chrono::milliseconds(cycleMs);
    RuntimeCore core(runtimeCfg);
    core.loadModules();

    spdlog::info("Loom running. Press Ctrl+C to stop.");

    ServerConfig serverCfg;
    serverCfg.port = static_cast<uint16_t>(port);
    serverCfg.bindAddress = bindAddress;
    serverCfg.staticDir = dataDir + "/UI";
    Server server(core, serverCfg);
    server.start();

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    spdlog::info("Shutting down...");
    server.stop();
    core.shutdown();
    spdlog::info("Loom shutdown complete.");
    return 0;
}

} // namespace loom
