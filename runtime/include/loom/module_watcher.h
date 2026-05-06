#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>

namespace loom {

/// Watches a directory for changes to .so files and fires a callback when one changes.
///
/// Uses filesystem polling (cross-platform). Checks every pollIntervalMs milliseconds.
/// When a module file's last_write_time changes relative to the last recorded mtime,
/// the onChanged callback is called with the module ID (filename stem).
///
/// Also detects new module files that appear after the watcher is started.
class ModuleWatcher {
public:
    using ChangedFn = std::function<void(const std::string& moduleId)>;

    /// Construct a watcher for the given directory.
    explicit ModuleWatcher(std::filesystem::path dir,
                           int pollIntervalMs = 500,
                           int debounceMs = 1500);
    ~ModuleWatcher();

    // Non-copyable
    ModuleWatcher(const ModuleWatcher&) = delete;
    ModuleWatcher& operator=(const ModuleWatcher&) = delete;

    /// Register a callback invoked whenever a .so file changes or appears.
    /// Multiple callbacks can be registered. Not thread-safe — call before start().
    void onChanged(ChangedFn fn);

    /// Start the background polling thread.
    void start();

    /// Stop the background polling thread. Blocks until the thread exits.
    void stop();

    /// Take a snapshot of current mtimes (called once after initial module load
    /// so that files that existed at boot don't trigger spurious reloads).
    void baseline();

private:
    void run();

    std::filesystem::path dir_;
    int pollIntervalMs_;
    int debounceMs_;
    std::vector<ChangedFn> callbacks_;
    std::unordered_map<std::string, std::filesystem::file_time_type> mtimes_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> pending_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace loom
