#include "loom/scheduler.h"
#include "loom/scheduler_config.h"
#include "loom/io_mapper.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <shared_mutex>

// ---- Platform-specific RT and affinity helpers ----------------------------------

#ifdef __APPLE__
#  include <mach/mach.h>
#  include <mach/mach_time.h>
#  include <mach/thread_policy.h>

/// Apply Mach time-constraint real-time policy to the calling thread.
/// priority_pct (1–99) scales the computation fraction relative to the period.
static void applyMachRealtimePolicy(uint32_t period_us, int priority_pct) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);

    auto toMach = [&](uint64_t ns) -> uint32_t {
        return static_cast<uint32_t>(ns * tb.denom / tb.numer);
    };

    const uint64_t period_ns   = static_cast<uint64_t>(period_us) * 1000ULL;
    const double   comp_frac   = std::clamp(priority_pct, 1, 99) / 100.0;

    const uint64_t computation_ns = static_cast<uint64_t>(period_ns * comp_frac);
    // constraint must satisfy: period >= constraint >= computation
    // Use the larger of 50% of the period or the computation budget itself.
    const uint64_t constraint_ns  = std::max(period_ns / 2, computation_ns);

    thread_time_constraint_policy_data_t policy{};
    policy.period      = toMach(period_ns);
    policy.computation = toMach(computation_ns);
    policy.constraint  = toMach(constraint_ns);
    policy.preemptible = TRUE;

    kern_return_t kr = thread_policy_set(
        mach_thread_self(),
        THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&policy),
        THREAD_TIME_CONSTRAINT_POLICY_COUNT);

    if (kr != KERN_SUCCESS) {
        spdlog::warn("applyMachRealtimePolicy: thread_policy_set failed ({})", kr);
    } else {
        spdlog::debug("Mach RT policy applied (period={}µs, comp={:.0f}%)", period_us, comp_frac * 100);
    }
}

static void applyAffinityPolicy(int cpuCore) {
    if (cpuCore < 0) return;
    thread_affinity_policy_data_t policy{ static_cast<integer_t>(cpuCore + 1) };
    thread_policy_set(mach_thread_self(), THREAD_AFFINITY_POLICY,
                      reinterpret_cast<thread_policy_t>(&policy), 1);
    spdlog::debug("Mach affinity set to core {}", cpuCore);
}

#elif defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <timeapi.h>
#  pragma comment(lib, "winmm.lib")

static bool s_timerPeriodSet = false;

static void applyWindowsRealtimePolicy(uint32_t /*period_us*/, int priority) {
    if (!s_timerPeriodSet) {
        if (timeBeginPeriod(1) == TIMERR_NOERROR) {
            s_timerPeriodSet = true;
            spdlog::debug("Windows multimedia timer resolution set to 1ms");
        } else {
            spdlog::warn("applyWindowsRealtimePolicy: timeBeginPeriod(1) failed");
        }
    }
    int winPriority = (priority >= 91) ? THREAD_PRIORITY_TIME_CRITICAL :
                      (priority >= 61) ? THREAD_PRIORITY_ABOVE_NORMAL :
                      (priority >= 31) ? THREAD_PRIORITY_NORMAL :
                                         THREAD_PRIORITY_BELOW_NORMAL;
    if (!SetThreadPriority(GetCurrentThread(), winPriority)) {
        spdlog::warn("applyWindowsRealtimePolicy: SetThreadPriority failed ({})", GetLastError());
    } else {
        spdlog::debug("Windows thread priority set to {}", winPriority);
    }
}

static void applyAffinityPolicy(int cpuCore) {
    if (cpuCore < 0) return;
    SetThreadAffinityMask(GetCurrentThread(), 1ULL << cpuCore);
    spdlog::debug("Windows affinity set to core {}", cpuCore);
}

#elif defined(__linux__)
#  include <pthread.h>
#  include <sched.h>
#  include <sys/utsname.h>
#  include <cstring>

static bool isPreemptRT() {
    struct utsname u{};
    if (uname(&u) != 0) return false;
    return std::strstr(u.version, "PREEMPT_RT") != nullptr ||
           std::strstr(u.version, "PREEMPT RT") != nullptr;
}

static void applyLinuxRealtimePolicy(uint32_t /*period_us*/, int priority) {
    struct sched_param param{};
    int min = sched_get_priority_min(SCHED_FIFO);
    int max = sched_get_priority_max(SCHED_FIFO);
    param.sched_priority = std::clamp(priority, min, max);

    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
        spdlog::warn("applyLinuxRealtimePolicy: pthread_setschedparam failed (errno={}). "
                     "Run as root or set /etc/security/limits.conf rtprio.", errno);
    } else {
        spdlog::debug("Linux SCHED_FIFO applied (priority={})", param.sched_priority);
    }
}

static void applyAffinityPolicy(int cpuCore) {
    if (cpuCore < 0) return;
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(cpuCore, &cs);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs) != 0) {
        spdlog::warn("applyAffinityPolicy: pthread_setaffinity_np failed for core {}", cpuCore);
    } else {
        spdlog::debug("Linux affinity set to core {}", cpuCore);
    }
}
#endif // platform

// ---- Scheduler implementation ---------------------------------------------------

namespace loom {

Scheduler::Scheduler() {
    // Pre-create built-in class runners from defaults so they are always present.
    for (const auto& def : defaultSchedulerConfig().classes) {
        auto runner         = std::make_unique<ClassRunnerState>();
        runner->def         = def;
        classes_[def.name]  = std::move(runner);
    }
}

void Scheduler::setSamplingTargets(Oscilloscope* osc, DataEngine* engine,
                                  ModuleLoader* loader, std::shared_mutex* moduleMutex) {
    std::lock_guard lock(mutex_);
    oscilloscope_ = osc;
    dataEngine_ = engine;
    loader_ = loader;
    moduleMutex_ = moduleMutex;
}

void Scheduler::setIOMapper(IOMapper* mapper) {
    std::lock_guard lock(mutex_);
    ioMapper_ = mapper;
}

Scheduler::~Scheduler() {
    stopAll();
}

// ---- Configuration --------------------------------------------------------------

void Scheduler::configure(const SchedulerConfig& cfg) {
    std::lock_guard lock(mutex_);
    schedCfg_ = cfg;

    // Ensure built-in classes always exist (merge defaults for any missing names).
    for (const auto& def : defaultSchedulerConfig().classes) {
        if (!classes_.count(def.name)) {
            auto runner        = std::make_unique<ClassRunnerState>();
            runner->def        = def;
            classes_[def.name] = std::move(runner);
        }
    }

    // Apply user class definitions (update period/priority/affinity but don't
    // discard members if the class was already partially populated).
    for (const auto& def : cfg.classes) {
        auto it = classes_.find(def.name);
        if (it != classes_.end()) {
            it->second->def = def;
        } else {
            auto runner        = std::make_unique<ClassRunnerState>();
            runner->def        = def;
            classes_[def.name] = std::move(runner);
        }
    }
}

void Scheduler::registerClass(const ClassDef& def) {
    std::lock_guard lock(mutex_);
    auto it = classes_.find(def.name);
    if (it != classes_.end()) {
        it->second->def = def;
    } else {
        auto runner        = std::make_unique<ClassRunnerState>();
        runner->def        = def;
        classes_[def.name] = std::move(runner);
    }
}

// ---- start / startClasses -------------------------------------------------------

bool Scheduler::start(LoadedModule& mod, const TaskConfig& config, const InitContext& ctx) {
    std::lock_guard lock(mutex_);

    auto existing = tasks_.find(mod.id);
    if (existing != tasks_.end() && existing->second->running.load()) {
        spdlog::warn("Module '{}' is already running", mod.id);
        return false;
    }

    // Init module before any thread touches it.
    spdlog::info("Initializing module '{}' (reason: {})", mod.id, static_cast<int>(ctx.reason));
    mod.instance->init(ctx);
    mod.state = ModuleState::Initialized;

    // Create fresh TaskState (unique_ptr for pointer stability).
    auto state = std::make_unique<TaskState>();
    state->running.store(true);
    TaskState* statePtr = state.get();
    tasks_[mod.id]  = std::move(state);
    configs_[mod.id] = config;

    // Start long-running thread immediately (independent of class membership).
    if (config.enableLongRunning) {
        statePtr->longRunningThread = std::thread([&mod, statePtr]() {
            spdlog::info("Long-running task started for '{}'", mod.id);
            while (statePtr->running.load()) {
                mod.instance->longRunning();
            }
            spdlog::info("Long-running task ended for '{}'", mod.id);
        });
    }

    const bool isIsolated = config.isolateThread || config.cyclicClass.empty();

    if (isIsolated) {
        statePtr->cyclicThread = std::thread([this, &mod, config, statePtr]() {
            isolatedLoop(mod, config, *statePtr);
        });
    } else {
        // Add to class (thread started later by startClasses, or live-inserted if running).
        auto& runner = getOrCreateClass(config.cyclicClass);
        insertMember(runner, { mod.id, config.order, &mod, statePtr });
    }

    mod.state = ModuleState::Running;
    spdlog::info("Module '{}' started (class: {}, order: {})",
                 mod.id, config.cyclicClass.empty() ? "isolated" : config.cyclicClass, config.order);
    return true;
}

void Scheduler::startClasses() {
    std::lock_guard lock(mutex_);
    classesStarted_ = true;

    for (auto& [name, runner] : classes_) {
        if (runner->members.empty() || runner->running.load()) continue;

        runner->running.store(true);
        runner->thread = std::thread([this, r = runner.get()]() {
            classLoop(*r);
        });
        spdlog::info("Class '{}' started (period: {}µs, members: {})",
                     runner->def.name, runner->def.period_us,
                     static_cast<int>(runner->members.size()));
    }
}

// ---- stop / stopAll -------------------------------------------------------------

bool Scheduler::stop(const std::string& moduleId) {
    std::thread cyclicToJoin, longRunToJoin;

    {
        std::lock_guard lock(mutex_);

        auto stateIt = tasks_.find(moduleId);
        if (stateIt == tasks_.end()) {
            spdlog::warn("Cannot stop '{}': not scheduled", moduleId);
            return false;
        }

        auto& state   = *stateIt->second;
        auto  cfgIt   = configs_.find(moduleId);
        bool  classBased = cfgIt != configs_.end()
                        && !cfgIt->second.isolateThread
                        && !cfgIt->second.cyclicClass.empty();

        if (classBased) {
            // Remove from class runner (pauses class if running).
            auto classIt = classes_.find(cfgIt->second.cyclicClass);
            if (classIt != classes_.end()) {
                removeMember(*classIt->second, moduleId);
            }
        } else {
            // Signal isolated cyclic thread to exit before moving it for join.
            state.running.store(false);
            cyclicToJoin = std::move(state.cyclicThread);
        }

        // Signal long-running thread to exit (applies to both isolated and class-based).
        if (classBased) {
            state.running.store(false);
        }
        longRunToJoin = std::move(state.longRunningThread);

        spdlog::info("Module '{}' stopped ({} cycles, {} overruns)",
                     moduleId, state.cycleCount.load(), state.overrunCount.load());
        tasks_.erase(moduleId);
        configs_.erase(moduleId);
    }

    // Join outside the lock so we don't block the mutex.
    if (cyclicToJoin.joinable())  cyclicToJoin.join();
    if (longRunToJoin.joinable()) longRunToJoin.join();

    return true;
}

void Scheduler::stopAll() {
    // Collect raw pointers before modifying the maps.
    std::vector<ClassRunnerState*> runnerPtrs;
    std::vector<TaskState*>        statePtrs;

    {
        std::lock_guard lock(mutex_);

        for (auto& [name, runner] : classes_) {
            runner->running.store(false);
            // Unblock any pending pause so the thread can exit.
            {
                std::lock_guard plk(runner->pauseMx);
                runner->pauseRequested = false;
            }
            runner->pauseCv.notify_all();
            runnerPtrs.push_back(runner.get());
        }

        for (auto& [id, state] : tasks_) {
            state->running.store(false);
            statePtrs.push_back(state.get());
        }
    }

    // Join class threads first (they hold references to per-module state).
    for (auto* r : runnerPtrs) {
        if (r->thread.joinable()) r->thread.join();
    }

    // Join per-module threads.
    for (auto* s : statePtrs) {
        if (s->cyclicThread.joinable())     s->cyclicThread.join();
        if (s->longRunningThread.joinable()) s->longRunningThread.join();
    }

    {
        std::lock_guard lock(mutex_);
        tasks_.clear();
        configs_.clear();
    }
}

// ---- reassignClass --------------------------------------------------------------

std::error_code Scheduler::reassignClass(const std::string& moduleId,
                                         const std::string& newClass,
                                         std::optional<int> newOrder) {
    std::lock_guard lock(mutex_);

    auto stateIt = tasks_.find(moduleId);
    if (stateIt == tasks_.end())
        return std::make_error_code(std::errc::no_such_process);

    auto cfgIt = configs_.find(moduleId);
    if (cfgIt == configs_.end())
        return std::make_error_code(std::errc::invalid_argument);

    const std::string& oldClass = cfgIt->second.cyclicClass;
    if (oldClass == newClass) return {};  // no-op

    // Locate the mod pointer in the old class members.
    LoadedModule* modPtr = nullptr;
    auto oldIt = classes_.find(oldClass);
    if (oldIt != classes_.end()) {
        for (auto& m : oldIt->second->members) {
            if (m.moduleId == moduleId) { modPtr = m.mod; break; }
        }
    }
    if (!modPtr)
        return std::make_error_code(std::errc::invalid_argument);

    TaskState* statePtr = stateIt->second.get();
    int order = newOrder.value_or(cfgIt->second.order);

    // Remove from old class.
    if (oldIt != classes_.end()) {
        removeMember(*oldIt->second, moduleId);
    }

    // Insert into new class.
    auto& destRunner = getOrCreateClass(newClass);
    insertMember(destRunner, { moduleId, order, modPtr, statePtr });

    // Update in-memory config.
    cfgIt->second.cyclicClass = newClass;
    cfgIt->second.order       = order;

    // Keep schedCfg_.assignments in sync so fullConfig() reflects the change.
    auto& asg = schedCfg_.assignments[moduleId];
    asg.classId = newClass;
    asg.order   = order;

    spdlog::info("Module '{}' reassigned from class '{}' to '{}' (order={})",
                 moduleId, oldClass, newClass, order);
    return {};
}

// ---- Accessors ------------------------------------------------------------------

const TaskState* Scheduler::taskState(const std::string& moduleId) const {
    std::lock_guard lock(mutex_);
    auto it = tasks_.find(moduleId);
    return it != tasks_.end() ? it->second.get() : nullptr;
}

std::optional<ClassStats> Scheduler::classStats(const std::string& name) const {
    std::lock_guard lock(mutex_);
    auto it = classes_.find(name);
    if (it == classes_.end()) return std::nullopt;
    const auto& r = *it->second;
    ClassStats cs;
    cs.name             = name;
    cs.lastJitterUs     = r.lastJitterUs.load();
    cs.lastCycleTimeUs  = r.lastCycleTimeUs.load();
    cs.maxCycleTimeUs   = r.maxCycleTimeUs.load();
    cs.tickCount        = r.tickCount.load();
    cs.lastTickStartMs  = r.lastTickStartMs.load();
    cs.memberCount      = static_cast<int>(r.members.size());
    for (auto& m : r.members) cs.moduleIds.push_back(m.moduleId);
    return cs;
}

std::vector<ClassStats> Scheduler::allClassStats() const {
    std::lock_guard lock(mutex_);
    std::vector<ClassStats> out;
    out.reserve(classes_.size());
    for (auto& [name, r] : classes_) {
        ClassStats cs;
        cs.name             = name;
        cs.lastJitterUs     = r->lastJitterUs.load();
        cs.lastCycleTimeUs  = r->lastCycleTimeUs.load();
        cs.maxCycleTimeUs   = r->maxCycleTimeUs.load();
        cs.tickCount        = r->tickCount.load();
        cs.lastTickStartMs  = r->lastTickStartMs.load();
        cs.memberCount      = static_cast<int>(r->members.size());
        for (auto& m : r->members) cs.moduleIds.push_back(m.moduleId);
        out.push_back(std::move(cs));
    }
    return out;
}

std::string Scheduler::moduleClass(const std::string& moduleId) const {
    std::lock_guard lock(mutex_);
    // Check isolated tasks first (they have a configs_ entry with empty cyclicClass)
    auto cfgIt = configs_.find(moduleId);
    if (cfgIt != configs_.end() && cfgIt->second.isolateThread) return "";
    for (auto& [name, r] : classes_) {
        for (auto& m : r->members) {
            if (m.moduleId == moduleId) return name;
        }
    }
    return "";
}

std::deque<MetricSample> Scheduler::classHistory(const std::string& name) const {
    std::lock_guard lock(mutex_);
    auto it = classes_.find(name);
    if (it == classes_.end()) return {};
    std::lock_guard histLock(it->second->cycleHistoryMx);
    // Convert ring buffer contents to deque for compatibility
    auto vec = it->second->cycleHistory.getAll();
    return std::deque<MetricSample>(vec.begin(), vec.end());
}

bool Scheduler::addClassDef(const ClassDef& def) {
    std::lock_guard lock(mutex_);
    if (classes_.find(def.name) != classes_.end()) return false;  // already exists
    auto runner   = std::make_unique<ClassRunnerState>();
    runner->def   = def;
    classes_[def.name] = std::move(runner);
    schedCfg_.classes.push_back(def);
    // If classes are already running, start this one immediately
    if (classesStarted_) {
        auto& r = *classes_[def.name];
        // No members yet; thread starts on-demand when first module joins
        (void)r;
    }
    spdlog::info("Created class '{}' (period={}µs)", def.name, def.period_us);
    return true;
}

bool Scheduler::updateClassDef(const ClassDef& def) {
    std::lock_guard lock(mutex_);
    auto it = classes_.find(def.name);
    if (it == classes_.end()) return false;
    it->second->def = def;
    // Also update the master config so subsequent classConfigs() reflects the change.
    for (auto& cd : schedCfg_.classes) {
        if (cd.name == def.name) { cd = def; return true; }
    }
    schedCfg_.classes.push_back(def);
    return true;
}

std::vector<ClassDef> Scheduler::classConfigs() const {
    std::lock_guard lock(mutex_);
    std::vector<ClassDef> out;
    out.reserve(classes_.size());
    for (auto& [name, r] : classes_) out.push_back(r->def);
    return out;
}

SchedulerConfig Scheduler::fullConfig() const {
    std::lock_guard lock(mutex_);
    SchedulerConfig cfg;
    cfg.defaultClass = schedCfg_.defaultClass;
    cfg.assignments  = schedCfg_.assignments;
    for (auto& [name, r] : classes_)
        cfg.classes.push_back(r->def);
    return cfg;
}

// ---- Private helpers ------------------------------------------------------------

Scheduler::ClassRunnerState& Scheduler::getOrCreateClass(const std::string& name) {
    auto it = classes_.find(name);
    if (it != classes_.end()) return *it->second;

    // Implicit creation: infer period from built-in names, else use "normal" default.
    ClassDef def;
    def.name      = name;
    def.period_us = (name == "fast") ? 1000 : (name == "slow") ? 100000 : 10000;

    // If user config has a definition for this name, prefer it.
    if (const auto* userDef = findClassDef(schedCfg_, name)) {
        def = *userDef;
    }

    auto runner        = std::make_unique<ClassRunnerState>();
    runner->def        = def;
    auto& ref          = *runner;
    classes_[name]     = std::move(runner);
    spdlog::info("Implicitly created class '{}' (period={}µs)", name, def.period_us);
    return ref;
}

void Scheduler::insertMember(ClassRunnerState& runner, ClassMember member) {
    bool needsPause = runner.running.load();
    if (needsPause) pauseClass(runner);

    runner.members.push_back(std::move(member));
    sortMembers(runner);

    if (needsPause) {
        unpauseClass(runner);
    } else if (classesStarted_ && !runner.running.load()) {
        // Classes system is live but this class never had members at boot — start its thread now.
        runner.running.store(true);
        runner.thread = std::thread([this, r = &runner]() {
            classLoop(*r);
        });
        spdlog::info("Class '{}' started on-demand (period: {}µs)", runner.def.name, runner.def.period_us);
    }
}

void Scheduler::removeMember(ClassRunnerState& runner, const std::string& moduleId) {
    bool needsPause = runner.running.load();
    if (needsPause) pauseClass(runner);

    runner.members.erase(
        std::remove_if(runner.members.begin(), runner.members.end(),
                       [&](const ClassMember& m) { return m.moduleId == moduleId; }),
        runner.members.end());

    if (needsPause) {
        if (runner.members.empty()) {
            // Last member gone — stop the thread so it doesn't spin with no work.
            runner.running.store(false);
            // Unblock the pause-wait so the thread can exit.
            {
                std::lock_guard lk(runner.pauseMx);
                runner.pauseRequested = false;
            }
            runner.pauseCv.notify_all();
            if (runner.thread.joinable()) runner.thread.join();
        } else {
            unpauseClass(runner);
        }
    }
}

void Scheduler::sortMembers(ClassRunnerState& runner) {
    std::stable_sort(runner.members.begin(), runner.members.end(),
                     [](const ClassMember& a, const ClassMember& b) {
                         return a.order < b.order;
                     });
}

void Scheduler::pauseClass(ClassRunnerState& runner) {
    if (!runner.running.load()) return;

    {
        std::lock_guard lk(runner.pauseMx);
        runner.pauseRequested = true;
        runner.pauseAcked     = false;
    }

    // Wait for the class thread to finish its current tick and ack the pause.
    std::unique_lock lk(runner.pauseMx);
    runner.pauseCv.wait(lk, [&] {
        return runner.pauseAcked || !runner.running.load();
    });
}

void Scheduler::unpauseClass(ClassRunnerState& runner) {
    {
        std::lock_guard lk(runner.pauseMx);
        runner.pauseRequested = false;
    }
    runner.pauseCv.notify_all();
}

// ---- Metrics buffering ----------------------------------------------------------

/// Store a cycle/jitter sample in the ring buffer (ultra-fast, non-blocking).
/// Ring buffer is fixed-size preallocated, so O(1) write with no dynamic allocation.
static void storeSample(MetricRingBuffer& buffer, std::mutex& bufferMx,
                        int64_t cycleTimeUs, int64_t jitterUs) {
    // Get timestamp
    int64_t nowMs = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // Non-blocking: if lock isn't immediately available, skip sample
    if (!bufferMx.try_lock()) return;

    std::lock_guard lk(bufferMx, std::adopt_lock);
    buffer.push({ nowMs, cycleTimeUs, jitterUs });
}

// ---- classLoop ------------------------------------------------------------------

void Scheduler::classLoop(ClassRunnerState& runner) {
    const auto& def      = runner.def;
    const auto  periodUs = std::chrono::microseconds(
        static_cast<int64_t>(def.period_us));
    const auto  periodNs = static_cast<int64_t>(def.period_us) * 1000LL;

    spdlog::info("Class '{}' thread started (period: {}µs, priority: {})",
                 def.name, def.period_us, def.priority);

    const uint32_t periodUsUint = static_cast<uint32_t>(periodUs.count());
#ifdef __APPLE__
    applyMachRealtimePolicy(periodUsUint, def.priority);
#elif defined(_WIN32)
    applyWindowsRealtimePolicy(periodUsUint, def.priority);
#elif defined(__linux__)
    if (isPreemptRT())
        spdlog::info("PREEMPT_RT kernel detected for class '{}'", def.name);
    applyLinuxRealtimePolicy(periodUsUint, def.priority);
#endif
    applyAffinityPolicy(def.cpu_affinity);

    const auto kSpinThreshold = std::chrono::microseconds(def.spin_us);
    auto nextWake = std::chrono::steady_clock::now();

    while (runner.running.load()) {
        // --- Spin to hit exact wake time (only if spin_us > 0) ---
        while (kSpinThreshold.count() > 0 && std::chrono::steady_clock::now() < nextWake) {
            // busy-wait the final stretch
        }

        // --- Class-level jitter: how late we actually executed vs. scheduled ---
        auto actualWake  = std::chrono::steady_clock::now();
        auto classJitter = std::chrono::duration_cast<std::chrono::microseconds>(
            actualWake - nextWake);
        // Store signed lateness (negative = early, clamped to 0 for display)
        runner.lastJitterUs.store(classJitter.count() > 0 ? classJitter.count() : 0LL);
        runner.tickCount.fetch_add(1);

        // Record wall-clock tick start (ms since epoch) for timeline visualisation
        runner.lastTickStartMs.store(
            static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()));

        // --- Execute each member sequentially ---
        // NOTE: runner.members is only mutated when the class is paused (below).
        //       Reading it here without a lock is safe.
        auto execStart = std::chrono::steady_clock::now();

        // --- Sweep 1: preCyclic (e.g. read hardware inputs) ---
        for (auto& member : runner.members) {
            if (member.state->faulted.load()) continue;
            member.mod->instance->preCyclicGuarded();
        }

        // --- Sweep 2: cyclic (do work) — timed, sampled ---
        for (auto& member : runner.members) {
            if (member.state->faulted.load()) continue;

            // Per-module jitter: |Δstart − period|
            auto   startNow = std::chrono::steady_clock::now();
            int64_t startNs  = startNow.time_since_epoch().count();
            int64_t prevNs   = member.state->lastCyclicStartNs.load();
            if (prevNs != 0) {
                int64_t deltaNs = startNs - prevNs;
                member.state->lastJitterUs.store(
                    std::abs(deltaNs - periodNs) / 1000LL);
            }
            member.state->lastCyclicStartNs.store(startNs);

            // Execute (cyclicGuarded acquires module's runtimeMutex_ so
            // server/watch threads can't race on runtime_ reads)
            member.mod->instance->cyclicGuarded();

            // Lightweight sampling: use oscilloscope fast-path.
            // A member in runner.members is guaranteed alive — removeMember() pauses
            // the class and waits for ack BEFORE erasing, so no moduleMutex_ needed.
            // We try_lock the module's runtimeMutex_ so we never block the caller;
            // missing one sample tick is acceptable.
            if (oscilloscope_ && dataEngine_) {
                int64_t nowMs = static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
                oscilloscope_->sampleModule(member.moduleId, *dataEngine_,
                                            *member.mod->instance, nowMs);
            }

            auto   endNow = std::chrono::steady_clock::now();
            auto   elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                endNow - startNow);
            member.state->lastCycleTimeUs.store(elapsed.count());
            member.state->cycleCount.fetch_add(1);

            int64_t curMax = member.state->maxCycleTimeUs.load();
            if (elapsed.count() > curMax)
                member.state->maxCycleTimeUs.store(elapsed.count());

            int64_t periodUsInt = static_cast<int64_t>(def.period_us);
            if (elapsed.count() > periodUsInt) {
                member.state->overrunCount.fetch_add(1);
                if (member.state->overrunCount.load() % 100 == 1) {
                    spdlog::warn("Class '{}' module '{}' cyclic overrun: {}µs > {}µs",
                                 def.name, member.moduleId, elapsed.count(), periodUsInt);
                }
            }
        }

        // --- Sweep 2.5: I/O mappings (copy field values between modules) ---
        if (ioMapper_) {
            ioMapper_->executeForClass(def.name);
        }

        // --- Sweep 3: postCyclic (e.g. flush outputs to hardware) ---
        for (auto& member : runner.members) {
            if (member.state->faulted.load()) continue;
            member.mod->instance->postCyclicGuarded();
        }

        // --- Record total class cycle time ---
        {
            auto execEnd   = std::chrono::steady_clock::now();
            auto cycleTime = std::chrono::duration_cast<std::chrono::microseconds>(
                execEnd - execStart).count();
            runner.lastCycleTimeUs.store(cycleTime);
            int64_t curMax = runner.maxCycleTimeUs.load();
            if (cycleTime > curMax) runner.maxCycleTimeUs.store(cycleTime);

            // Store historical sample for charting
            int64_t jitterUs = runner.lastJitterUs.load();
            storeSample(runner.cycleHistory, runner.cycleHistoryMx, cycleTime, jitterUs);
        }

        // --- Pause check (for hot-reassignment and stop) ---
        {
            std::unique_lock lk(runner.pauseMx);
            if (runner.pauseRequested) {
                runner.pauseAcked = true;
                runner.pauseCv.notify_all();
                // Wait until caller releases the pause or the class is being shut down.
                runner.pauseCv.wait(lk, [&] {
                    return !runner.pauseRequested || !runner.running.load();
                });
            }
        }

        // --- Timing: advance deadline and sleep until close to it ---
        nextWake += periodUs;
        auto sleepUntil = nextWake - kSpinThreshold;
        auto now = std::chrono::steady_clock::now();
        if (sleepUntil > now) {
            std::this_thread::sleep_until(sleepUntil);
        }
        // Spin (if any) consumed at top of next iteration
    }

    spdlog::info("Class '{}' thread ended after {} ticks", def.name, runner.tickCount.load());
}

// ---- isolatedLoop ---------------------------------------------------------------

void Scheduler::isolatedLoop(LoadedModule& mod, TaskConfig config, TaskState& state) {
    spdlog::info("Isolated cyclic task started for '{}'", mod.id);

    const auto periodUs = config.cyclePeriod;
    const auto periodNs = static_cast<int64_t>(periodUs.count()) * 1000LL;
    const uint32_t periodUsUint = static_cast<uint32_t>(periodUs.count());

    // Priority 50 (normal default) for isolated threads unless a class def was found.
    int priority = 50;
    int cpuAffinity = -1;
    int spinUs = 0;
    {
        // Use the class def's priority/affinity/spin if the config names a class (even isolated).
        const auto* def = findClassDef(schedCfg_, config.cyclicClass);
        if (def) { priority = def->priority; cpuAffinity = def->cpu_affinity; spinUs = def->spin_us; }
    }

#ifdef __APPLE__
    applyMachRealtimePolicy(periodUsUint, priority);
#elif defined(_WIN32)
    applyWindowsRealtimePolicy(periodUsUint, priority);
#elif defined(__linux__)
    if (isPreemptRT())
        spdlog::info("PREEMPT_RT kernel detected for isolated module '{}'", mod.id);
    applyLinuxRealtimePolicy(periodUsUint, priority);
#endif
    applyAffinityPolicy(cpuAffinity);

    const auto kSpinThreshold = std::chrono::microseconds(spinUs);
    auto nextWake = std::chrono::steady_clock::now();

    while (state.running.load()) {
        auto actualWake = std::chrono::steady_clock::now();
        auto jitter = std::chrono::duration_cast<std::chrono::microseconds>(
            actualWake - nextWake);
        state.lastJitterUs.store(std::abs(jitter.count()));

        // Per-module start tracking (consistent with class loop)
        int64_t startNs = actualWake.time_since_epoch().count();
        int64_t prevNs  = state.lastCyclicStartNs.load();
        if (prevNs != 0) {
            int64_t deltaNs = startNs - prevNs;
            state.lastJitterUs.store(std::abs(deltaNs - periodNs) / 1000LL);
        }
        state.lastCyclicStartNs.store(startNs);

        auto t0 = std::chrono::steady_clock::now();
        mod.instance->cyclicGuarded();
        auto t1 = std::chrono::steady_clock::now();

        // Execute I/O mappings for this isolated module
        if (ioMapper_) {
            ioMapper_->executeForModule(mod.id);
        }

        // Sample this isolated module using oscilloscope fast-path.
        // The isolated thread owns this module exclusively; state.running guards lifetime.
        if (oscilloscope_ && dataEngine_) {
            int64_t nowMs = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            oscilloscope_->sampleModule(mod.id, *dataEngine_, *mod.instance, nowMs);
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0);
        state.lastCycleTimeUs.store(elapsed.count());
        state.cycleCount.fetch_add(1);

        int64_t curMax = state.maxCycleTimeUs.load();
        if (elapsed.count() > curMax) state.maxCycleTimeUs.store(elapsed.count());

        // Store historical sample for charting
        int64_t jitterUs = state.lastJitterUs.load();
        storeSample(state.cycleHistory, state.cycleHistoryMx, elapsed.count(), jitterUs);

        if (elapsed > periodUs) {
            state.overrunCount.fetch_add(1);
            if (state.overrunCount.load() % 100 == 1) {
                spdlog::warn("Module '{}' cyclic overrun: {}µs > {}µs",
                             mod.id, elapsed.count(), periodUs.count());
            }
        }

        nextWake += periodUs;
        auto sleepUntil = nextWake - kSpinThreshold;
        if (sleepUntil > std::chrono::steady_clock::now()) {
            std::this_thread::sleep_until(sleepUntil);
        }
        if (kSpinThreshold.count() > 0) {
            while (std::chrono::steady_clock::now() < nextWake && state.running.load()) {
                /* spin */
            }
        }
    }

    spdlog::info("Isolated cyclic task ended for '{}'", mod.id);
}

} // namespace loom

