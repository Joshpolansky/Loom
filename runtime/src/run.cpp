#include "loom/runtime_core.h"
#include "loom/server.h"
#include "loom/version.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace loom {

namespace {
    std::atomic<bool> g_running{true};

    void signalHandler(int sig) {
        spdlog::info("Received signal {}, shutting down...", sig);
        g_running = false;
    }

    void printUsage(const char* argv0) {
        std::cerr << "Usage: " << argv0 << " --module-dir <path> [--module-dir <path> ...] [--data-dir <path>] [--cycle-ms <ms>] [--port <port>] [--bind <addr>]\n"
                  << "\n"
                  << "Options:\n"
                  << "  --module-dir <path>  Directory containing .so/.dylib module files. Repeatable;\n"
                  << "                       modules are loaded from all dirs. The FIRST is the\n"
                  << "                       writable/watched dir (uploads land there, edits to it\n"
                  << "                       trigger hot-reload). Subsequent dirs are read-only.\n"
                  << "  --data-dir <path>    Directory for config/recipe persistence (default: ./data)\n"
                  << "  --cycle-ms <ms>      Default cycle period in milliseconds (default: 100)\n"
                  << "  --port <port>        HTTP/WebSocket server port (default: 8080)\n"
                  << "  --bind <addr>        Bind address (default: 127.0.0.1, use 0.0.0.0 for all interfaces)\n"
                  << "  --version            Print version and exit\n"
                  << "  --help, -h           Show this help and exit\n";
    }
} // namespace

int run(int argc, char* argv[]) {
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

    std::vector<std::string> moduleDirs;
    std::string dataDir = "./data";
    std::string bindAddress = "127.0.0.1";
    int cycleMs = 100;
    int port = 8080;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--module-dir" && i + 1 < argc) {
            moduleDirs.emplace_back(argv[++i]);
        } else if (arg == "--data-dir" && i + 1 < argc) {
            dataDir = argv[++i];
        } else if (arg == "--cycle-ms" && i + 1 < argc) {
            cycleMs = std::stoi(argv[++i]);
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--bind" && i + 1 < argc) {
            bindAddress = argv[++i];
        } else if (arg == "--version") {
            std::cout << "loom " << kSdkVersion << "\n";
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    if (moduleDirs.empty()) {
        std::cerr << "Error: at least one --module-dir is required\n";
        printUsage(argv[0]);
        return 1;
    }

    spdlog::info("Loom starting");
    spdlog::info("Primary module directory (writable, watched): {}", moduleDirs.front());
    for (size_t i = 1; i < moduleDirs.size(); ++i) {
        spdlog::info("Additional module directory (read-only): {}", moduleDirs[i]);
    }
    spdlog::info("Data directory: {}", dataDir);
    spdlog::info("Default cycle period: {}ms", cycleMs);
    spdlog::info("Server port: {}", port);
    spdlog::info("Bind address: {}", bindAddress);

    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    RuntimeConfig runtimeCfg;
    runtimeCfg.moduleDir = moduleDirs.front();
    for (size_t i = 1; i < moduleDirs.size(); ++i) {
        runtimeCfg.additionalModuleDirs.emplace_back(moduleDirs[i]);
    }
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
