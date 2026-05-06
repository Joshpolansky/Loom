#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace loom {

/// Defines one cyclic class: a shared thread that runs at a fixed rate.
/// All fields are plain types so glaze can auto-reflect this struct for JSON I/O.
struct ClassDef {
    std::string name         = "normal"; ///< Unique class identifier
    int         period_us    = 10000;    ///< Cycle period in microseconds
    int         cpu_affinity = -1;       ///< CPU core to pin the class thread to (-1 = no pinning)
    int         priority     = 50;       ///< RT scheduling priority 1–99.
                                         ///  Linux:   SCHED_FIFO priority level.
                                         ///  macOS:   scales Mach time-constraint computation fraction.
                                         ///  Windows: maps to Win32 THREAD_PRIORITY_* level.
    int         spin_us      = 0;        ///< Busy-spin duration before the scheduled wake (µs).
                                         ///  0 = sleep-only (default). Non-zero values trade CPU
                                         ///  for sub-µs wake accuracy (useful for fast RT classes).
};

/// Scheduling assignment for a single module (key = moduleId in SchedulerConfig.assignments).
struct ModuleAssignment {
    std::string classId = "normal"; ///< Target class name
    int         order   = 0;        ///< Execution order within class (ascending, ties use load order)
    bool        isolate = false;    ///< Force a dedicated thread (bypass class pooling)
};

/// Top-level scheduler configuration — loaded from scheduler.json, UI-editable.
///
/// Assignment resolution priority (highest to lowest):
///   1. Explicit entry in SchedulerConfig.assignments (keyed by moduleId)
///   2. Module's IModule::taskHint() return value
///   3. defaultClass
struct SchedulerConfig {
    std::vector<ClassDef>                             classes;
    std::unordered_map<std::string, ModuleAssignment> assignments; ///< moduleId → assignment
    std::string                                       defaultClass = "normal";
};

// ---- Free-function helpers --------------------------------------------------------

/// Returns true if the config has an explicit assignment for the given moduleId.
inline bool hasAssignment(const SchedulerConfig& cfg, const std::string& moduleId) {
    return cfg.assignments.count(moduleId) > 0;
}

/// Find a class definition by name. Returns nullptr if not found.
const ClassDef* findClassDef(const SchedulerConfig& cfg, const std::string& name);

/// Returns a config pre-populated with the three built-in classes:
///   fast (1 ms, priority 90), normal (10 ms, priority 50), slow (100 ms, priority 30).
SchedulerConfig defaultSchedulerConfig();

/// Load from a JSON file. Returns defaultSchedulerConfig() if the file is missing or malformed.
/// Built-in classes are always merged in so they are never absent.
SchedulerConfig loadSchedulerConfig(const std::filesystem::path& path);

/// Persist to a JSON file (pretty-printed). Creates parent directories as needed.
void saveSchedulerConfig(const SchedulerConfig& cfg, const std::filesystem::path& path);

} // namespace loom
