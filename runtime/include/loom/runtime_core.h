#pragma once

#include "loom/bus.h"
#include "loom/data_engine.h"
#include "loom/data_store.h"
#include "loom/instance_manifest.h"
#include "loom/io_mapper.h"
#include "loom/module.h"
#include "loom/module_loader.h"
#include "loom/module_watcher.h"
#include "loom/oscilloscope.h"
#include "loom/scheduler.h"
#include "loom/scheduler_config.h"
#include "loom/types.h"

#include <chrono>
#include <filesystem>
#include <shared_mutex>
#include <string>
#include <vector>

namespace loom {

/// Configuration for the RuntimeCore.
struct RuntimeConfig {
    /// Primary module directory — writable, watched for hot-reload, and
    /// the target for /api/modules/upload + new instance copies. Pass
    /// the user's workspace output dir here when running under LoomUI.
    std::filesystem::path moduleDir;
    /// Additional read-only module directories searched after moduleDir
    /// when loading on startup, listing /api/modules/available, and
    /// resolving a .so filename for instantiate. Pass the install's
    /// bundled examples directory here for the typical workflow.
    std::vector<std::filesystem::path> additionalModuleDirs;
    std::filesystem::path dataDir         = "./data";
    std::chrono::milliseconds defaultCyclePeriod{100};
};

/// Central runtime controller. Owns all subsystems and module lifecycle operations.
///
/// The Server is a thin translation layer that delegates to this class.
/// main.cpp creates a RuntimeCore, constructs a Server around it, and calls run().
class RuntimeCore : public IModuleRegistry {
public:
    explicit RuntimeCore(const RuntimeConfig& config);
    ~RuntimeCore() = default;

    // Non-copyable
    RuntimeCore(const RuntimeCore&) = delete;
    RuntimeCore& operator=(const RuntimeCore&) = delete;

    /// Discover and load all modules from the configured directory.
    /// Returns list of loaded module IDs.
    std::vector<std::string> loadModules();

    /// Warm-restart a module by ID. Snapshots runtime, reloads .so, restores runtime.
    /// Returns false if the module was not found or reload failed.
    bool reloadModule(const std::string& id);

    /// Load a new module from a .so file, copy it to moduleDir, and start it.
    /// If a module with the same ID is already loaded, it will be reloaded instead.
    /// Returns the module ID on success, empty string on failure.
    std::string uploadModule(const std::filesystem::path& srcPath);

    /// Create a new instance of a module class from a .so filename already in moduleDir.
    /// instanceId becomes the bus namespace and data-dir key; it must not already be loaded.
    /// The instance is persisted to instances.json so it survives restarts.
    /// Returns the instance ID on success, empty string on failure.
    std::string instantiateModule(const std::string& soFilename,
                                  const std::string& instanceId);

    /// Stop and unload a running instance by ID.
    /// Also removes the instance from instances.json.
    /// Returns false if the instance was not found.
    bool removeInstance(const std::string& id);

    /// Save all configs and shut everything down cleanly.
    void shutdown();

    /// Persist the current scheduler configuration to data/scheduler.json.
    void saveSchedulerConfig();

    /// IModuleRegistry implementation — used by modules to look up siblings.
    IModule* findModule(std::string_view id) override;

    // --- Accessors for the Server translation layer ---
    ModuleLoader& loader()          { return loader_; }
    DataEngine& dataEngine()        { return dataEngine_; }
    DataStore& dataStore()          { return dataStore_; }
    Scheduler& scheduler()          { return scheduler_; }
    Bus& bus()                      { return bus_; }
    Oscilloscope& oscilloscope()    { return oscilloscope_; }
    IOMapper& ioMapper()            { return ioMapper_; }
    const RuntimeConfig& config() const { return config_; }
    const SchedulerConfig& schedulerConfig() const { return schedCfg_; }

    const ModuleLoader& loader()    const { return loader_; }
    const DataEngine& dataEngine()  const { return dataEngine_; }
    const Scheduler& scheduler()    const { return scheduler_; }
    const Bus& bus()                const { return bus_; }
    const Oscilloscope& oscilloscope() const { return oscilloscope_; }
    const IOMapper& ioMapper()      const { return ioMapper_; }

    /// Mutex protecting the module map and all module lifecycle operations.
    /// Server read paths take a shared_lock; reloadModule/shutdown take a unique_lock.
    std::shared_mutex& moduleMutex() { return moduleMutex_; }

private:
    /// Resolve a TaskConfig for a module based on scheduler.json, module hints, and defaults.
    TaskConfig resolveTaskConfig(const std::string& moduleId, const IModule* mod) const;

    /// Wire up a single module: register with DataEngine, inject Bus, load persisted
    /// config, then start scheduling with the given InitContext.
    bool startModule(const std::string& id, const InitContext& ctx);

    /// Start the file watcher and wire up the auto-reload callback.
    void setupWatcher();

    /// Update instances.json to reflect the current set of loaded modules.
    void saveInstanceManifest();

    RuntimeConfig   config_;
    SchedulerConfig schedCfg_;

    ModuleLoader  loader_;
    DataEngine    dataEngine_;
    DataStore     dataStore_;
    Scheduler     scheduler_;
    Bus           bus_;
    Oscilloscope  oscilloscope_;
    IOMapper      ioMapper_;
    ModuleWatcher watcher_;

    std::shared_mutex moduleMutex_;
};

/// Parse argv, set up signal handlers, construct RuntimeCore + Server, and run
/// until SIGINT/SIGTERM. Returns an exit code suitable for returning from main().
///
/// Usage:
///   #include <loom/runtime_core.h>
///   int main(int argc, char* argv[]) { return loom::run(argc, argv); }
int run(int argc, char* argv[]);

} // namespace loom

