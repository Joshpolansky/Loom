#include "loom/diag/system_metrics.h"

#include <spdlog/spdlog.h>

#include <thread>

// ---------------------------------------------------------------------------
// Platform readers
// ---------------------------------------------------------------------------
#if defined(_WIN32)
#  include <windows.h>
#  include <psapi.h>
#elif defined(__APPLE__)
#  include <mach/mach.h>
#  include <sys/resource.h>
#  include <unistd.h>
#else  // Linux & other POSIX
#  include <sys/resource.h>
#  include <unistd.h>
#  include <cstdio>
#endif

namespace loom::diag {

unsigned SystemMetrics::cpuCount() {
    unsigned n = std::thread::hardware_concurrency();
    return n ? n : 1;
}

#if defined(_WIN32)

uint64_t SystemMetrics::readRssBytes() {
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc))
        return static_cast<uint64_t>(pmc.WorkingSetSize);
    return 0;
}

uint64_t SystemMetrics::readPeakRssBytes() {
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc))
        return static_cast<uint64_t>(pmc.PeakWorkingSetSize);
    return 0;
}

uint64_t SystemMetrics::readCpuTimeNs() {
    FILETIME creation, exit, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        return 0;
    auto to100ns = [](const FILETIME& ft) -> uint64_t {
        return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    // FILETIME units are 100 ns.
    return (to100ns(kernel) + to100ns(user)) * 100ull;
}

#elif defined(__APPLE__)

uint64_t SystemMetrics::readRssBytes() {
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return info.resident_size;
    return 0;
}

uint64_t SystemMetrics::readPeakRssBytes() {
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return info.resident_size_max;
    return 0;
}

uint64_t SystemMetrics::readCpuTimeNs() {
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
    auto tvNs = [](const timeval& tv) -> uint64_t {
        return static_cast<uint64_t>(tv.tv_sec) * 1'000'000'000ull +
               static_cast<uint64_t>(tv.tv_usec) * 1'000ull;
    };
    return tvNs(ru.ru_utime) + tvNs(ru.ru_stime);
}

#else  // Linux

uint64_t SystemMetrics::readRssBytes() {
    // /proc/self/statm: size resident shared ... (in pages)
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long sizePages = 0, residentPages = 0;
    int got = std::fscanf(f, "%lu %lu", &sizePages, &residentPages);
    std::fclose(f);
    if (got < 2) return 0;
    long pageSize = sysconf(_SC_PAGESIZE);
    return static_cast<uint64_t>(residentPages) * static_cast<uint64_t>(pageSize);
}

uint64_t SystemMetrics::readPeakRssBytes() {
    // ru_maxrss is in kilobytes on Linux.
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
    return static_cast<uint64_t>(ru.ru_maxrss) * 1024ull;
}

uint64_t SystemMetrics::readCpuTimeNs() {
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
    auto tvNs = [](const timeval& tv) -> uint64_t {
        return static_cast<uint64_t>(tv.tv_sec) * 1'000'000'000ull +
               static_cast<uint64_t>(tv.tv_usec) * 1'000ull;
    };
    return tvNs(ru.ru_utime) + tvNs(ru.ru_stime);
}

#endif

// ---------------------------------------------------------------------------
// Sampler
// ---------------------------------------------------------------------------

SystemMetrics::SystemMetrics(Config cfg) : cfg_(cfg) {
    if (cfg_.intervalMs < 100)  cfg_.intervalMs = 100;   // floor
    if (cfg_.historyMax < 1)    cfg_.historyMax = 1;
}

SystemMetrics::~SystemMetrics() { stop(); }

void SystemMetrics::start() {
    if (running_.exchange(true)) return;
    startTime_ = std::chrono::steady_clock::now();
    thread_ = std::thread(&SystemMetrics::run, this);
}

void SystemMetrics::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void SystemMetrics::run() {
    uint64_t lastCpuNs  = readCpuTimeNs();
    auto     lastWall   = std::chrono::steady_clock::now();
    const double cores  = static_cast<double>(cpuCount());

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.intervalMs));
        if (!running_.load()) break;

        const auto now      = std::chrono::steady_clock::now();
        const uint64_t cpuNs = readCpuTimeNs();

        const double wallNs = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - lastWall).count());
        double cpuPct = 0.0;
        if (wallNs > 0.0 && cpuNs >= lastCpuNs) {
            // (CPU time used / wall time) gives 0–N_cores; normalize to 0–100%.
            cpuPct = (static_cast<double>(cpuNs - lastCpuNs) / wallNs) * 100.0 / cores;
            if (cpuPct < 0.0) cpuPct = 0.0;
        }
        lastCpuNs = cpuNs;
        lastWall  = now;

        SystemSample s;
        s.tsMs = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        s.rssBytes     = readRssBytes();
        s.peakRssBytes = readPeakRssBytes();
        s.cpuPercent   = cpuPct;
        s.uptimeSec    = std::chrono::duration_cast<std::chrono::seconds>(now - startTime_).count();

        {
            std::lock_guard lock(mx_);
            latest_ = s;
            history_.push_back(s);
            while (history_.size() > static_cast<size_t>(cfg_.historyMax)) history_.pop_front();
        }

        if (cfg_.logEnabled) {
            spdlog::info("system: rss={} MB  peak={} MB  cpu={:.1f}%  uptime={}s",
                         s.rssBytes / (1024 * 1024), s.peakRssBytes / (1024 * 1024),
                         s.cpuPercent, s.uptimeSec);
        }
    }
}

SystemSample SystemMetrics::current() const {
    std::lock_guard lock(mx_);
    return latest_;
}

std::vector<SystemSample> SystemMetrics::history() const {
    std::lock_guard lock(mx_);
    return {history_.begin(), history_.end()};
}

} // namespace loom::diag
