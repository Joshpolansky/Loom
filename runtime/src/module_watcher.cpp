#include "loom/module_watcher.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>

namespace loom {

ModuleWatcher::ModuleWatcher(std::filesystem::path dir,
                             int pollIntervalMs,
                             int debounceMs)
    : dir_(std::move(dir)), pollIntervalMs_(pollIntervalMs), debounceMs_(debounceMs) {}

ModuleWatcher::~ModuleWatcher() {
    stop();
}

void ModuleWatcher::onChanged(ChangedFn fn) {
    callbacks_.push_back(std::move(fn));
}

void ModuleWatcher::start() {
    if (running_.load()) return;
    running_.store(true);
    thread_ = std::thread([this]() { run(); });
}

void ModuleWatcher::stop() {
    if (!running_.load()) return;
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

void ModuleWatcher::baseline() {
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
        if (entry.path().extension() != ".so" &&
            entry.path().extension() != ".dylib" &&
            entry.path().extension() != ".dll") continue;
        auto mtime = entry.last_write_time(ec);
        if (!ec) {
            auto stem = entry.path().stem().string();
            if (stem.starts_with("lib")) stem = stem.substr(3);
            mtimes_[stem] = mtime;
        }
    }
}

void ModuleWatcher::run() {
    spdlog::info("ModuleWatcher: watching '{}' (poll interval: {}ms, debounce: {}ms)",
                 dir_.string(), pollIntervalMs_, debounceMs_);

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs_));
        if (!running_.load()) break;

        auto now = std::chrono::steady_clock::now();

        std::error_code ec;
        for (auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
            if (ec) break;
            const auto& p = entry.path();
            if (p.extension() != ".so" &&
                p.extension() != ".dylib" &&
                p.extension() != ".dll") continue;

            auto mtime = entry.last_write_time(ec);
            if (ec) continue;

            auto id = p.stem().string();
            // Strip leading "lib" prefix on Linux/macOS shared libraries.
            if (id.starts_with("lib")) id = id.substr(3);

            auto it = mtimes_.find(id);
            if (it == mtimes_.end()) {
                // New file appeared — record and notify.
                mtimes_[id] = mtime;
                pending_[id] = now;
                spdlog::info("ModuleWatcher: new module detected: '{}' (queued)", id);
            } else if (it->second != mtime) {
                // File changed.
                it->second = mtime;
                pending_[id] = now;
                spdlog::info("ModuleWatcher: module '{}' changed on disk (queued)", id);
            }
        }

        std::vector<std::string> ready;
        ready.reserve(pending_.size());
        for (auto it = pending_.begin(); it != pending_.end();) {
            auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count();
            if (ageMs >= debounceMs_) {
                ready.push_back(it->first);
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }

        for (const auto& id : ready) {
            spdlog::info("ModuleWatcher: module '{}' settled, triggering reload", id);
            for (auto& cb : callbacks_) cb(id);
        }
    }

    spdlog::info("ModuleWatcher: stopped");
}

} // namespace loom
