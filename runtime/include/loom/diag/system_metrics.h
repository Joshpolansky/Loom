#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

// ============================================================================
// loom::diag — process resource metrics (memory + CPU)
//
// A lightweight background sampler that reads the process's resident memory and
// CPU usage from the OS (no dependencies) on a fixed interval, keeps a small
// history ring for charting, and — when enabled — logs a periodic line. Exposed
// over the runtime server (GET /api/system + the WS live frame), alongside the
// existing per-class/per-module cycle metrics.
//
// CPU is sampled as a delta between ticks, so the first sample reports 0.
// cpuPercent is normalized to 0–100% of the whole machine (total process CPU
// time / wall time / core count).
// ============================================================================

namespace loom::diag {

struct SystemSample {
    int64_t  tsMs         = 0;   ///< system_clock milliseconds
    uint64_t rssBytes     = 0;   ///< current resident set size
    uint64_t peakRssBytes = 0;   ///< peak resident set size since start
    double   cpuPercent   = 0.0; ///< process CPU over the last interval, 0–100% of the machine
    int64_t  uptimeSec    = 0;   ///< seconds since the sampler started
};

class SystemMetrics {
public:
    struct Config {
        bool logEnabled = false;  ///< emit a periodic spdlog line (the "setting")
        int  intervalMs = 1000;   ///< sample period
        int  historyMax = 600;    ///< samples retained for charting (~10 min @ 1s)
    };

    // NB: no `= {}` default — a defaulted Config argument would force the
    // nested struct's default member initializers within the enclosing class
    // definition, which GCC/Clang reject (MSVC allows it). Callers pass a Config.
    explicit SystemMetrics(Config cfg);
    ~SystemMetrics();

    SystemMetrics(const SystemMetrics&) = delete;
    SystemMetrics& operator=(const SystemMetrics&) = delete;

    /// Start the background sampler (idempotent).
    void start();
    /// Stop and join the sampler (idempotent; also called by the destructor).
    void stop();

    /// Latest sample (thread-safe snapshot).
    SystemSample current() const;
    /// Recent samples, oldest first (thread-safe copy).
    std::vector<SystemSample> history() const;

    // --- raw OS readers (static; also usable for one-off reads) ---
    static uint64_t readRssBytes();
    static uint64_t readPeakRssBytes();
    static uint64_t readCpuTimeNs();   ///< total process CPU time (user+system)
    static unsigned cpuCount();

private:
    void run();

    Config            cfg_;
    std::thread       thread_;
    std::atomic<bool> running_{false};

    mutable std::mutex      mx_;
    SystemSample            latest_;
    std::deque<SystemSample> history_;

    std::chrono::steady_clock::time_point startTime_{};
};

} // namespace loom::diag
