#pragma once

#include "loom/data_engine.h"
#include "loom/types.h"

#include <filesystem>
#include <string>

namespace loom {

/// Persists Config and Recipe data sections to JSON files on disk.
///
/// Directory layout:
///   <root>/
///     <moduleId>/
///       config.json
///       recipes/
///         <recipeName>.json
///
class DataStore {
public:
    explicit DataStore(const std::filesystem::path& rootDir);

    /// Load the persisted config for a module into the data engine.
    /// Returns true if a config file existed and was loaded.
    bool loadConfig(const std::string& moduleId, DataEngine& engine);

    /// Save the current config from the data engine to disk.
    bool saveConfig(const std::string& moduleId, const DataEngine& engine);

    /// Load a named recipe into the module's recipe section.
    bool loadRecipe(const std::string& moduleId, const std::string& recipeName, DataEngine& engine);

    /// Save the current recipe to a named file.
    bool saveRecipe(const std::string& moduleId, const std::string& recipeName, const DataEngine& engine);

    /// List all available recipe names for a module.
    std::vector<std::string> listRecipes(const std::string& moduleId) const;

    const std::filesystem::path& rootDir() const { return rootDir_; }

private:
    std::filesystem::path modulePath(const std::string& moduleId) const;
    std::filesystem::path configPath(const std::string& moduleId) const;
    std::filesystem::path recipePath(const std::string& moduleId, const std::string& recipeName) const;

    std::filesystem::path rootDir_;
};

} // namespace loom
