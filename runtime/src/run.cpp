#include "loom/runtime_core.h"
#include "loom/server.h"
#include "loom/version.h"
#include "loom/diag/crash_handler.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace loom {

namespace {
    std::atomic<bool> g_running{true};

    void signalHandler(int sig) {
        spdlog::info("Received signal {}, shutting down...", sig);
        g_running = false;
    }

    /// Directory containing this executable, or empty on platforms/failures we
    /// can't resolve. Used to find the installed Loom UI relative to the binary.
    std::filesystem::path executableDir() {
        std::error_code ec;
#ifdef __APPLE__
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::string buf(size, '\0');
        if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
        auto p = std::filesystem::canonical(buf, ec);
#elif defined(__linux__)
        auto p = std::filesystem::canonical("/proc/self/exe", ec);
#else
        std::filesystem::path p;
#endif
        return ec ? std::filesystem::path{} : p.parent_path();
    }

    /// Resolve the Loom monitoring UI dir, served always at /_loom. Prefers an
    /// install-relative location, independent of --data-dir so it stays reachable
    /// when the data dir hosts a user app. Falls back to <dataDir>/UI for
    /// dev/standby runs. Candidate order covers every shipping layout:
    ///   <exe>/ui                — flat release tarball (binary + ui/ + modules/)
    ///   <exe>/share/loom/ui     — flat tarball, share/-prefixed
    ///   <exe>/../share/loom/ui  — `cmake --install` prefix (bin/loom + share/...)
    ///   <exe>/../data/UI        — dev build tree (output/loom + data/UI symlink)
    std::string resolveLoomUiDir(const std::string& dataDir) {
        std::vector<std::filesystem::path> candidates;
        if (const auto exeDir = executableDir(); !exeDir.empty()) {
            candidates.push_back(exeDir / "ui");
            candidates.push_back(exeDir / "share" / "loom" / "ui");
            candidates.push_back(exeDir / ".." / "share" / "loom" / "ui");
            candidates.push_back(exeDir / ".." / "data" / "UI");
        }
        const std::filesystem::path dataUi = std::filesystem::path(dataDir) / "UI";
        candidates.push_back(dataUi);
        std::error_code ec;
        for (const auto& c : candidates) {
            if (std::filesystem::exists(c / "index.html", ec)) {
                auto resolved = std::filesystem::weakly_canonical(c, ec);
                return (ec ? c : resolved).string();
            }
        }
        return dataUi.string(); // best-effort; server logs a warning if missing
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
                  << "  --loom-ui-dir <path> Directory of the Loom monitoring UI served at /_loom\n"
                  << "                       (default: install-relative, falls back to <data-dir>/UI)\n"
                  << "  --cycle-ms <ms>      Default cycle period in milliseconds (default: 100)\n"
                  << "  --port <port>        HTTP/WebSocket server port (default: 8080)\n"
                  << "  --bind <addr>        Bind address (default: 127.0.0.1, use 0.0.0.0 for all interfaces)\n"
                  << "  --log-system-metrics Log a periodic process memory/CPU line (always served on /api/system)\n"
                  << "  --version            Print version and exit\n"
                  << "  --help, -h           Show this help and exit\n";
    }
} // namespace

int run(int argc, char* argv[]) {
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

    std::vector<std::string> moduleDirs;
    std::string dataDir = "./data";
    std::string loomUiDir; // empty → resolve install-relative (see resolveLoomUiDir)
    std::string bindAddress = "127.0.0.1";
    int cycleMs = 100;
    int port = 8080;
    bool logSystemMetrics = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--module-dir" && i + 1 < argc) {
            moduleDirs.emplace_back(argv[++i]);
        } else if (arg == "--data-dir" && i + 1 < argc) {
            dataDir = argv[++i];
        } else if (arg == "--loom-ui-dir" && i + 1 < argc) {
            loomUiDir = argv[++i];
        } else if (arg == "--cycle-ms" && i + 1 < argc) {
            cycleMs = std::stoi(argv[++i]);
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--bind" && i + 1 < argc) {
            bindAddress = argv[++i];
        } else if (arg == "--log-system-metrics") {
            logSystemMetrics = true;
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

    // Process-global crash capture: any fatal signal / SEH / unhandled C++
    // exception writes a crash report (faulting module/phase + build id + stack)
    // to <dataDir>/crash before the process dies. Covers module and runtime faults.
    loom::diag::CrashHandler::install({std::filesystem::path(dataDir) / "crash"});

    RuntimeConfig runtimeCfg;
    runtimeCfg.moduleDir = moduleDirs.front();
    for (size_t i = 1; i < moduleDirs.size(); ++i) {
        runtimeCfg.additionalModuleDirs.emplace_back(moduleDirs[i]);
    }
    runtimeCfg.dataDir             = dataDir;
    runtimeCfg.defaultCyclePeriod  = std::chrono::milliseconds(cycleMs);
    runtimeCfg.logSystemMetrics    = logSystemMetrics;
    RuntimeCore core(runtimeCfg);
    core.loadModules();

    spdlog::info("Loom running. Press Ctrl+C to stop.");

    ServerConfig serverCfg;
    serverCfg.port = static_cast<uint16_t>(port);
    serverCfg.bindAddress = bindAddress;
    serverCfg.staticDir = dataDir + "/UI";
    serverCfg.loomUiDir = loomUiDir.empty() ? resolveLoomUiDir(dataDir) : loomUiDir;
    spdlog::info("Loom UI directory (/_loom): {}", serverCfg.loomUiDir);
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
