#include "loom/runtime_core.h"
#include "loom/scheduler_config.h"

#include <filesystem>
#include <fstream>
#include <shared_mutex>
#include <spdlog/spdlog.h>

namespace loom {

namespace {

// Normalize a module filename to the platform-native shared library extension.
// If an extension exists, replace it; if none exists, append the correct one.
std::filesystem::path normalizeModuleFilename(std::string_view name) {
    auto p = std::filesystem::path(name);
    p.replace_extension(dynlib::kExtension);
    return p;
}

} // namespace

RuntimeCore::RuntimeCore(const RuntimeConfig& config)
    : config_(config), dataStore_(config.dataDir),
      watcher_(config.moduleDir) {
    // Load scheduler.json from the data directory.
    auto schedPath = config.dataDir / "scheduler.json";
    bool schedExisted = std::filesystem::exists(schedPath);
    schedCfg_ = loadSchedulerConfig(schedPath);
    scheduler_.configure(schedCfg_);
    // Provide scheduler with pointers needed for cycle-aligned oscilloscope sampling.
    scheduler_.setSamplingTargets(&oscilloscope_, &dataEngine_, &loader_, &moduleMutex_);
    scheduler_.setIOMapper(&ioMapper_);
    if (!schedExisted) {
        loom::saveSchedulerConfig(schedCfg_, schedPath);
        spdlog::info("Wrote default scheduler config to '{}'", schedPath.string());
    }
    spdlog::info("Scheduler config loaded from '{}' ({} classes, {} assignments)",
                 schedPath.string(),
                 static_cast<int>(schedCfg_.classes.size()),
                 static_cast<int>(schedCfg_.assignments.size()));

    // Load I/O mappings (initialize empty if file doesn't exist)
    auto ioMapPath = config.dataDir / "io_mappings.json";
    if (std::filesystem::exists(ioMapPath)) {
        if (!ioMapper_.load(ioMapPath.string())) {
            spdlog::warn("Failed to load I/O mappings from '{}'", ioMapPath.string());
        }
    }
}

TaskConfig RuntimeCore::resolveTaskConfig(const std::string& moduleId,
                                          const IModule* mod) const {
    TaskConfig cfg;

    // 1. Check explicit scheduler.json assignment (highest priority).
    if (hasAssignment(schedCfg_, moduleId)) {
        const auto& asg = schedCfg_.assignments.at(moduleId);
        cfg.cyclicClass   = asg.classId.empty() ? schedCfg_.defaultClass : asg.classId;
        cfg.order         = asg.order;
        cfg.isolateThread = asg.isolate;
    } else {
        // 2. Fall back to module's own taskHint().
        auto hint = mod->taskHint();
        if (!hint.cyclicClass.empty()) {
            cfg.cyclicClass   = hint.cyclicClass;
            cfg.order         = hint.order;
            cfg.isolateThread = hint.isolate;
        } else {
            // 3. Use system defaultClass.
            cfg.cyclicClass   = schedCfg_.defaultClass;
            cfg.order         = 0;
            cfg.isolateThread = false;
        }
    }

    // For isolated threads, use the class's configured period if available.
    if (cfg.isolateThread || cfg.cyclicClass.empty()) {
        if (const auto* def = findClassDef(schedCfg_, cfg.cyclicClass)) {
            cfg.cyclePeriod = std::chrono::microseconds(def->period_us);
        } else {
            cfg.cyclePeriod = config_.defaultCyclePeriod;
        }
    }

    return cfg;
}

std::vector<std::string> RuntimeCore::loadModules() {
#if defined(_WIN32)
    // Clean up stale shadow copies from previous crashes before loading modules.
    {
        std::error_code ec;
        auto shadowRoot = config_.moduleDir / ".shadow";
        if (std::filesystem::exists(shadowRoot, ec)) {
            auto removed = std::filesystem::remove_all(shadowRoot, ec);
            if (ec) {
                spdlog::warn("Failed to remove module shadow root '{}': {}",
                             shadowRoot.string(), ec.message());
            } else if (removed > 0) {
                spdlog::info("Removed {} stale shadow file(s)/dir(s) from '{}'",
                             removed, shadowRoot.string());
            }
        }
    }
#endif

    auto manifestPath = config_.dataDir / "instances.json";
    auto entries = loadInstanceManifest(manifestPath);

    InitContext ctx;
    ctx.reason = InitReason::Boot;

    if (!entries.empty()) {
        // Boot from instances.json — load each entry with its assigned ID.
        spdlog::info("Loading {} instance(s) from instances.json", entries.size());
        std::vector<std::string> ids;
        for (auto& e : entries) {
            auto soFile = normalizeModuleFilename(e.so);
            auto soPath = config_.moduleDir / soFile;
            auto id = loader_.load(soPath, e.id);
            if (id.empty()) {
                spdlog::error("loadModules: failed to load instance '{}' from '{}'", e.id, e.so);
                continue;
            }
            startModule(id, ctx);
            ids.push_back(id);
        }
        if (ids.empty()) {
            spdlog::warn("No modules loaded from instances.json");
            return ids;
        }
        scheduler_.startClasses();
        ioMapper_.resolveAll(*this);
        setupWatcher();
        return ids;
    }

    // First boot — scan each configured module directory, primary first
    // then any additional read-only ones (e.g. the system examples dir
    // shipped in the Loom release).
    auto ids = loader_.loadDirectory(config_.moduleDir);
    for (const auto& extra : config_.additionalModuleDirs) {
        auto more = loader_.loadDirectory(extra);
        ids.insert(ids.end(), more.begin(), more.end());
    }
    if (ids.empty()) {
        spdlog::warn("No modules found in {} or any additional module dir", config_.moduleDir.string());
        return ids;
    }

    for (auto& id : ids) {
        startModule(id, ctx);
    }

    // Persist discovered instances so future boots use the manifest.
    std::vector<InstanceEntry> discovered;
    {
        for (auto& id : ids) {
            auto* mod = loader_.get(id);
            if (!mod) continue;
            InstanceEntry e;
            e.id        = id;
            e.so        = mod->path.stem().string();
            e.className = mod->className;
            discovered.push_back(std::move(e));
        }
    }
    if (loom::saveInstanceManifest(manifestPath, discovered)) {
        spdlog::info("Wrote instances.json with {} entry(ies)", discovered.size());
    }

    scheduler_.startClasses();
    ioMapper_.resolveAll(*this);
    setupWatcher();
    return ids;
}
 

bool RuntimeCore::reloadModule(const std::string& id) {
    // Phase 1: snapshot what we need under a non-exclusive read lock.
    std::string previousVersion;
    std::string runtimeSnapshot;
    {
        std::shared_lock<std::shared_mutex> rlock(moduleMutex_);
        auto* mod = loader_.get(id);
        if (!mod) {
            spdlog::error("reloadModule: module '{}' not found", id);
            return false;
        }
        previousVersion = mod->header.version ? mod->header.version : "";
        runtimeSnapshot = dataEngine_.readSection(id, DataSection::Runtime);
    }

    // Phase 2: stop the cyclic task WITHOUT holding moduleMutex_.
    // classLoop takes shared_lock(moduleMutex_) on every tick for oscilloscope
    // sampling; holding unique_lock here would deadlock with pauseClass().
    scheduler_.stop(id);

    // Phase 3: exclusive lock for the .so swap + re-init.
    // At this point the class thread has either paused (and is blocked in
    // pauseCv.wait) or exited, so it won't contend on moduleMutex_.
    std::unique_lock<std::shared_mutex> lock(moduleMutex_);

    auto* mod = loader_.get(id);
    if (!mod) {
        spdlog::error("reloadModule: module '{}' vanished before exclusive lock", id);
        return false;
    }

    // 3a. Persist config while the old instance is still alive
    dataStore_.saveConfig(id, dataEngine_);

    // 3b. Unregister from engine + bus
    dataEngine_.unregisterModule(id);
    mod->instance->cleanupSubscriptions();
    bus_.unregisterServicesByPrefix(id + "/");
    bus_.unsubscribeByPrefix(id + "/");
    ioMapper_.invalidateModule(id);

    // 3c. Reload the .so
    if (!loader_.reload(id)) {
        spdlog::error("reloadModule: loader failed for '{}'", id);
        return false;
    }

    mod = loader_.get(id);
    if (!mod || !mod->instance) {
        spdlog::error("reloadModule: missing instance after reload for '{}'", id);
        return false;
    }

    // 3d. Re-wire and init
    InitContext ctx;
    ctx.reason = InitReason::Reload;
    ctx.previousVersion = previousVersion;

    if (!startModule(id, ctx)) {
        return false;
    }

    ioMapper_.resolveAll(*this);

    // 3e. Best-effort restore runtime snapshot (after init so init runs clean)
    bool restored = dataEngine_.writeSection(id, DataSection::Runtime, runtimeSnapshot);
    spdlog::info("reloadModule: '{}' runtime snapshot restore {}",
                 id, restored ? "succeeded" : "failed (fields may have changed)");

    return true;
}

void RuntimeCore::shutdown() {
    watcher_.stop();

    std::unique_lock<std::shared_mutex> lock(moduleMutex_);

    scheduler_.stopAll();

    for (auto& [id, mod] : loader_.modules()) {
        if (!mod.instance) continue;
        spdlog::info("Calling exit() on module '{}'", id);
        mod.instance->exit();
        dataStore_.saveConfig(id, dataEngine_);
    }

    ioMapper_.save((config_.dataDir / "io_mappings.json").string());
}

void RuntimeCore::saveSchedulerConfig() {
    auto schedPath = config_.dataDir / "scheduler.json";
    loom::saveSchedulerConfig(scheduler_.fullConfig(), schedPath);
}

IModule* RuntimeCore::findModule(std::string_view id) {
    auto* mod = loader_.get(std::string(id));
    if (!mod || !mod->instance) return nullptr;
    return mod->instance.get();
}

bool RuntimeCore::startModule(const std::string& id, const InitContext& ctx) {
    auto* mod = loader_.get(id);
    if (!mod || !mod->instance) return false;

    dataEngine_.registerModule(id, mod->instance.get());
    mod->instance->setBus(&bus_, id);
    mod->instance->setRegistry(this);
    dataStore_.loadConfig(id, dataEngine_);

    TaskConfig taskCfg = resolveTaskConfig(id, mod->instance.get());
    taskCfg.enableLongRunning = true;  // TODO: configurable per-module

    spdlog::info("Module '{}' → class '{}' (order={}, isolated={})",
                 id, taskCfg.cyclicClass.empty() ? "isolated" : taskCfg.cyclicClass,
                 taskCfg.order, taskCfg.isolateThread);

    if (!scheduler_.start(*mod, taskCfg, ctx)) {
        spdlog::error("startModule: scheduler failed to start '{}'", id);
        return false;
    }
    return true;
}

void RuntimeCore::setupWatcher() {
    watcher_.baseline();
    watcher_.onChanged([this](const std::string& fileStem) {
        // Collect all instance IDs whose .so path stem matches the changed file.
        // A single .so can back multiple instances (multi-instance feature).
        std::vector<std::string> affected;
        {
            std::shared_lock<std::shared_mutex> lock(moduleMutex_);
            for (auto& [id, mod] : loader_.modules()) {
                auto stem = mod.path.stem().string();
                if (stem.starts_with("lib")) stem = stem.substr(3);
                if (stem == fileStem) affected.push_back(id);
            }
        }
        if (affected.empty()) {
            spdlog::warn("ModuleWatcher: no loaded instance found for file stem '{}', skipping", fileStem);
            return;
        }
        for (auto& id : affected) {
            spdlog::info("ModuleWatcher: auto-reloading '{}' (file stem: '{}')", id, fileStem);
            reloadModule(id);
        }
    });
    watcher_.start();
}

void RuntimeCore::saveInstanceManifest() {
    auto manifestPath = config_.dataDir / "instances.json";
    std::vector<InstanceEntry> entries;
    for (auto& [id, mod] : loader_.modules()) {
        InstanceEntry e;
        e.id        = id;
        e.so        = mod.path.stem().string();
        e.className = mod.className;
        entries.push_back(std::move(e));
    }
    loom::saveInstanceManifest(manifestPath, entries);
}

std::string RuntimeCore::instantiateModule(const std::string& soFilename,
                                            const std::string& instanceId) {
    auto normalized = normalizeModuleFilename(soFilename);

    // Path-traversal guard. `soFilename` arrives over HTTP; reject anything
    // that isn't a plain filename so that joining with moduleDir can't
    // escape into the rest of the filesystem (absolute paths replace the
    // base when joined; "..", separators, drive specs, etc. would let a
    // caller dlopen arbitrary shared libraries).
    std::filesystem::path nPath(normalized);
    if (normalized.empty()
        || nPath.is_absolute()
        || nPath.has_parent_path()
        || nPath != nPath.filename()) {
        spdlog::error("instantiateModule: rejecting non-filename '{}' (must be a bare .so/.dylib/.dll name)",
                      soFilename);
        return {};
    }

    std::filesystem::path soPath = config_.moduleDir / normalized;
    if (!std::filesystem::exists(soPath)) {
        // Fall back to any additional read-only module dirs (system examples
        // shipped in the install, etc.). The .so is loaded from wherever
        // it's found — no copy.
        bool found = false;
        for (const auto& extra : config_.additionalModuleDirs) {
            auto candidate = extra / normalized;
            if (std::filesystem::exists(candidate)) {
                soPath = candidate;
                found = true;
                break;
            }
        }
        if (!found) {
            spdlog::error("instantiateModule: '{}' not found in any module directory", soFilename);
            return {};
        }
    }

    std::unique_lock<std::shared_mutex> lock(moduleMutex_);

    if (loader_.get(instanceId)) {
        spdlog::error("instantiateModule: instance '{}' already exists", instanceId);
        return {};
    }

    auto id = loader_.load(soPath, instanceId);
    if (id.empty()) {
        spdlog::error("instantiateModule: loader failed for '{}' as '{}'", soFilename, instanceId);
        return {};
    }

    InitContext ctx;
    ctx.reason = InitReason::Boot;
    if (!startModule(id, ctx)) {
        loader_.unload(id);
        return {};
    }

    scheduler_.startClasses();
    ioMapper_.resolveAll(*this);
    saveInstanceManifest();
    return id;
}

bool RuntimeCore::removeInstance(const std::string& id) {
    std::unique_lock<std::shared_mutex> lock(moduleMutex_);

    auto* mod = loader_.get(id);
    if (!mod) {
        spdlog::error("removeInstance: '{}' not found", id);
        return false;
    }

    lock.unlock();
    scheduler_.stop(id);
    lock.lock();

    dataStore_.saveConfig(id, dataEngine_);
    dataEngine_.unregisterModule(id);
    mod->instance->cleanupSubscriptions();
    bus_.unregisterServicesByPrefix(id + "/");
    bus_.unsubscribeByPrefix(id + "/");
    ioMapper_.invalidateModule(id);
    loader_.unload(id);

    saveInstanceManifest();
    spdlog::info("Removed instance '{}'", id);
    return true;
}

std::string RuntimeCore::uploadModule(const std::filesystem::path& srcPath) {
    // Derive the module ID from the filename stem, stripping a leading "lib" prefix.
    std::string stem = srcPath.stem().string();
    if (stem.starts_with("lib")) stem = stem.substr(3);
    const std::string id = stem;

    // Copy the .so into the module directory (atomic overwrite via temp + rename).
    std::error_code ec;
    std::filesystem::create_directories(config_.moduleDir, ec);
    auto destPath = config_.moduleDir / srcPath.filename();
    auto tmpPath  = destPath;
    tmpPath.replace_extension(".tmp");

    std::filesystem::copy_file(srcPath, tmpPath,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        spdlog::error("uploadModule: copy failed for '{}': {}", srcPath.string(), ec.message());
        return {};
    }
    std::filesystem::rename(tmpPath, destPath, ec);
    if (ec) {
        spdlog::error("uploadModule: rename failed: {}", ec.message());
        std::filesystem::remove(tmpPath, ec);
        return {};
    }

    // Reload if already loaded, otherwise load fresh.
    {
        std::unique_lock<std::shared_mutex> lock(moduleMutex_);
        if (loader_.get(id)) {
            // Delegate to the existing reload path (already holds the lock — call internals directly).
            lock.unlock();
            if (!reloadModule(id)) return {};
            return id;
        }

        // New module — load and start.
        auto loadedId = loader_.load(destPath);
        if (loadedId.empty()) {
            spdlog::error("uploadModule: loader failed for '{}'", destPath.string());
            return {};
        }

        InitContext ctx;
        ctx.reason = InitReason::Boot;
        if (!startModule(loadedId, ctx)) {
            return {};
        }
        scheduler_.startClasses();
        ioMapper_.resolveAll(*this);
        saveInstanceManifest();
    }

    return id;
}

} // namespace loom

