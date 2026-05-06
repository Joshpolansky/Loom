#pragma once

#include "loom/module_loader.h"
#include "loom/scheduler_config.h"
#include "loom/oscilloscope.h"
#include "loom/data_engine.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <shared_mutex>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

namespace loom {

// Forward declarations
class IOMapper;

/// Cycle and jitter metric sample for historical tracking.
struct MetricSample {
    int64_t timestampMs;  ///< system_clock milliseconds
    int64_t cycleTimeUs;  ///< Cycle execution time (µs)
    int64_t jitterUs;     ///< Scheduling jitter (µs)
};

/// Simple fixed-size ring buffer for metrics (much faster than deque).
/// Preallocated, no dynamic allocation, O(1) writes.
struct MetricRingBuffer {
    static constexpr int CAPACITY = 2000;
    MetricSample samples[CAPACITY] = {};
    int writeIdx = 0;
    int count = 0;

    void push(const MetricSample& s) {
        samples[writeIdx] = s;
        writeIdx = (writeIdx + 1) % CAPACITY;
        if (count < CAPACITY) count++;
    }

    std::vector<MetricSample> getAll() const {
        std::vector<MetricSample> result;
        result.reserve(count);
        if (count < CAPACITY) {
            // Not yet full: samples are at [0, count)
            for (int i = 0; i < count; ++i) result.push_back(samples[i]);
        } else {
            // Wrapped around: start from writeIdx, go to capacity, then 0 to writeIdx
            for (int i = writeIdx; i < CAPACITY; ++i) result.push_back(samples[i]);
            for (int i = 0; i < writeIdx; ++i) result.push_back(samples[i]);
        }
        return result;
    }
};

/// Per-module scheduling state and live statistics.
/// Stored via unique_ptr so raw pointers handed to class runners remain stable.
struct TaskState {
    std::thread cyclicThread;          ///< Only used for isolated (non-class) modules
    std::thread longRunningThread;
    std::atomic<bool>     running{false};
    std::atomic<bool>     faulted{false};  ///< Module is skipped by the class loop when true
    std::atomic<uint64_t> cycleCount{0};
    std::atomic<uint64_t> overrunCount{0};
    std::atomic<int64_t>  lastCycleTimeUs{0};
    std::atomic<int64_t>  maxCycleTimeUs{0};
    std::atomic<int64_t>  lastJitterUs{0};      ///< |actualStart − prevStart| − period (µs)
    std::atomic<int64_t>  lastCyclicStartNs{0}; ///< Used internally to compute per-module jitter

    // Historical cycle/jitter data for charting (fixed-size ring buffer)
    MetricRingBuffer cycleHistory;
    mutable std::mutex cycleHistoryMx;
};

/// Resolved scheduling configuration for a single module (assigned by RuntimeCore).
struct TaskConfig {
    std::string               cyclicClass{"normal"}; ///< Class name. "" = isolated thread.
    std::chrono::microseconds cyclePeriod{10'000};   ///< Period for isolated threads only.
    int                       order = 0;             ///< Execution order within the class.
    bool                      isolateThread = false; ///< Force a dedicated thread.
    bool                      enableLongRunning = false;
};

/// Public read-only snapshot of a class's health (returned by classStats()).
struct ClassStats {
    std::string      name;
    int64_t          lastJitterUs    = 0;  ///< Wake latency of the class thread (µs)
    int64_t          lastCycleTimeUs = 0;  ///< Total time executing all members last tick (µs)
    int64_t          maxCycleTimeUs  = 0;  ///< Max total cycle time ever observed (µs)
    uint64_t         tickCount       = 0;
    int64_t          lastTickStartMs = 0;  ///< Wall-clock ms (system_clock) of last tick start
    int              memberCount     = 0;
    std::vector<std::string> moduleIds;  ///< Ordered list of module IDs in this class
};

/// Manages cyclic and long-running threads for all loaded modules.
///
/// Modules are either pooled into named cyclic classes (one shared thread per class,
/// sequential execution) or isolated on dedicated threads.
///
/// Lifecycle:
///   1. configure(cfg)         — apply scheduler.json config
///   2. start(mod, cfg, ctx)   — call init(), enqueue (or start isolated thread)
///   3. startClasses()         — start all class threads
///   4. stop(id) / stopAll()   — tear down
class Scheduler {
public:
    Scheduler();
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    /// Apply class definitions from a SchedulerConfig. Must be called before start().
    /// Built-in classes (fast/normal/slow) are always ensured to exist.
    void configure(const SchedulerConfig& cfg);

    /// Register or update a single class definition. Thread-safe pre-start.
    void registerClass(const ClassDef& def);

    /// Call init(ctx) on the module and enqueue it:
    ///   - Class-based: add to class members (thread not started yet).
    ///   - Isolated: start dedicated cyclic thread immediately.
    /// Call startClasses() after all modules are enqueued.
    bool start(LoadedModule& mod, const TaskConfig& config = {}, const InitContext& ctx = {});

    /// Start all class threads. Call once after all modules are enqueued via start().
    /// Idempotent: already-running classes are skipped.
    void startClasses();

    /// Configure optional targets for cycle-aligned sampling.
    /// Pass pointers to the `Oscilloscope`, `DataEngine`, `ModuleLoader`, and
    /// the runtime `moduleMutex` so the scheduler can enqueue snapshots after
    /// each module's `cyclic()` without taking ownership.
    void setSamplingTargets(Oscilloscope* osc, DataEngine* engine,
                            ModuleLoader* loader, std::shared_mutex* moduleMutex);

    /// Configure the IOMapper for runtime field-value copying.
    /// The scheduler will call executeForClass/executeForModule at appropriate times.
    void setIOMapper(IOMapper* mapper);

    /// Stop a module. Removes from class (or stops isolated thread). Joins long-running thread.
    /// Does NOT call exit() — caller's responsibility.
    bool stop(const std::string& moduleId);

    /// Stop all modules and all class threads.
    void stopAll();

    /// Move a loaded module to a different class at runtime.
    /// Both classes are paused for one tick during the swap.
    std::error_code reassignClass(const std::string& moduleId, const std::string& newClass,
                                  std::optional<int> newOrder = std::nullopt);

    // --- Accessors ---

    const TaskState* taskState(const std::string& moduleId) const;
    std::optional<ClassStats> classStats(const std::string& name) const;
    std::vector<ClassStats>   allClassStats() const;
    std::vector<ClassDef>     classConfigs() const;

    /// Returns the class name the given module is currently assigned to.
    /// Returns empty string if the module is isolated or not found.
    std::string moduleClass(const std::string& moduleId) const;

    /// Get the cycleHistory buffer for a class. Returns a copy of the deque.
    std::deque<MetricSample> classHistory(const std::string& name) const;

    /// Update the period/priority/affinity of an existing class definition.
    /// Safe to call at runtime; takes effect at the start of the next tick.
    /// Returns false if the class does not exist.
    bool updateClassDef(const ClassDef& def);

    /// Create a new empty class definition. Returns false if a class with that name already exists.
    bool addClassDef(const ClassDef& def);

    /// Returns a snapshot of the full scheduler config (classes + assignments).
    /// Useful for persisting the current state to scheduler.json.
    SchedulerConfig fullConfig() const;

private:
    // ---- Internal types ---------------------------------------------------------

    struct ClassMember {
        std::string   moduleId;
        int           order  = 0;
        LoadedModule* mod    = nullptr;  ///< Non-owning; valid while member is in class
        TaskState*    state  = nullptr;  ///< Non-owning; points into unique_ptr in tasks_
    };

    struct ClassRunnerState {
        ClassDef                  def;
        std::vector<ClassMember>  members;   ///< Sorted ascending by order
        std::thread               thread;
        std::atomic<bool>         running{false};

        // Pause / hot-mutation synchronisation
        std::mutex                pauseMx;
        std::condition_variable   pauseCv;
        bool                      pauseRequested = false;
        bool                      pauseAcked     = false;

        // Class-level stats
        std::atomic<int64_t>      lastJitterUs{0};     ///< Class wake jitter (µs)
        std::atomic<int64_t>      lastCycleTimeUs{0};  ///< Total execution time last tick (µs)
        std::atomic<int64_t>      maxCycleTimeUs{0};   ///< Max total execution time ever (µs)
        std::atomic<uint64_t>     tickCount{0};
        std::atomic<int64_t>      lastTickStartMs{0};  ///< Wall-clock ms of last tick start (system_clock)

        // Historical cycle/jitter data for charting (fixed-size ring buffer)
        MetricRingBuffer cycleHistory;
        mutable std::mutex cycleHistoryMx;

        ClassRunnerState() = default;
        ClassRunnerState(const ClassRunnerState&) = delete;
        ClassRunnerState& operator=(const ClassRunnerState&) = delete;
    };

    // ---- Helpers ----------------------------------------------------------------

    /// Get or create a class runner (caller must hold mutex_).
    ClassRunnerState& getOrCreateClass(const std::string& name);

    /// Add a member to a class, sorting by order.
    /// Pauses the class runner if it is already running.
    /// Caller must hold mutex_.
    void insertMember(ClassRunnerState& runner, ClassMember member);

    /// Remove a member by moduleId from a class.
    /// Pauses the class runner if it is already running.
    /// Caller must hold mutex_.
    void removeMember(ClassRunnerState& runner, const std::string& moduleId);

    static void sortMembers(ClassRunnerState& runner);

    /// Block until the class runner acks the pause (caller holds mutex_).
    void pauseClass(ClassRunnerState& runner);

    /// Signal the class runner to resume (caller holds mutex_).
    void unpauseClass(ClassRunnerState& runner);

    // ---- Thread functions -------------------------------------------------------

    void classLoop(ClassRunnerState& runner);
    void isolatedLoop(LoadedModule& mod, TaskConfig config, TaskState& state);

    // ---- State ------------------------------------------------------------------

    SchedulerConfig                                                  schedCfg_;
    std::unordered_map<std::string, std::unique_ptr<TaskState>>      tasks_;
    std::unordered_map<std::string, TaskConfig>                      configs_;
    std::unordered_map<std::string, std::unique_ptr<ClassRunnerState>> classes_;
    bool                                                             classesStarted_ = false;
    mutable std::mutex                                               mutex_;
    // Optional pointers to subsystems used for lightweight sampling from
    // the scheduler hot loop. Not owned.
    Oscilloscope*                                                    oscilloscope_ = nullptr;
    DataEngine*                                                      dataEngine_   = nullptr;
    ModuleLoader*                                                    loader_       = nullptr;
    std::shared_mutex*                                               moduleMutex_  = nullptr;
    IOMapper*                                                        ioMapper_     = nullptr;
};

} // namespace loom

