#pragma once

#include "loom/module.h"
#include "loom/types.h"
#include "loom/dynlib.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace loom {

/// Holds a loaded module: its shared library handle, factory pointers, and instance.
struct LoadedModule {
    std::string id;                        // Unique identifier (assigned at instantiation)
    std::string className;                 // Class name from header.name (may differ from id for multi-instance)
    std::filesystem::path path;            // Path to the module file (original, not shadow)
    std::filesystem::path shadowPath;      // Windows only: shadow copy loaded in place of path
    dynlib::Handle handle = dynlib::kNull; // Platform-native library handle
    ModuleHeader header{};                 // Cached module header (const char* valid only while .so is loaded)
    std::string nameStr;                   // Owned copy of header.name — safe after dlclose
    std::string versionStr;               // Owned copy of header.version — safe after dlclose
    std::unique_ptr<IModule, std::function<void(IModule*)>> instance; // Module instance with custom deleter
    ModuleState state = ModuleState::Unloaded;
};

/// Brief metadata read from a .so without instantiating it.
struct AvailableModule {
    std::string filename;   ///< Filename only (e.g. "libexample_motor.so")
    std::string className;  ///< header.name
    std::string version;    ///< header.version
};

/// Loads and manages .so module plugins.
class ModuleLoader {
public:
    ModuleLoader() = default;
    ~ModuleLoader();

    // Non-copyable
    ModuleLoader(const ModuleLoader&) = delete;
    ModuleLoader& operator=(const ModuleLoader&) = delete;

    /// Load a single module from a .so file path.
    /// If instanceId is non-empty it becomes the module's ID (allows multiple instances of
    /// the same class with different IDs). Otherwise the header.name is used as the ID.
    /// Returns the module ID on success, empty string on failure.
    std::string load(const std::filesystem::path& soPath,
                     const std::string& instanceId = {});

    /// Unload a module by ID. Calls exit() first if initialized.
    bool unload(const std::string& id);

    /// Warm-restart: unload then reload from the same (or new) path.
    bool reload(const std::string& id, const std::filesystem::path& newPath = {});

    /// Discover and load all .so files in a directory.
    std::vector<std::string> loadDirectory(const std::filesystem::path& dir);

    /// Scan a directory for .so files and return their metadata without loading them.
    static std::vector<AvailableModule> queryAvailable(const std::filesystem::path& dir);

    /// Access a loaded module by ID.
    LoadedModule* get(const std::string& id);
    const LoadedModule* get(const std::string& id) const;

    /// Iterate all loaded modules.
    const std::unordered_map<std::string, LoadedModule>& modules() const { return modules_; }

private:
    std::unordered_map<std::string, LoadedModule> modules_;
};

} // namespace loom
