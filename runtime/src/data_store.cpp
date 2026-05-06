#include "loom/data_store.h"

#include <glaze/glaze.hpp>
#include <fstream>
#include <spdlog/spdlog.h>

namespace loom {

DataStore::DataStore(const std::filesystem::path& rootDir)
    : rootDir_(rootDir) {
    std::filesystem::create_directories(rootDir_);
}

bool DataStore::loadConfig(const std::string& moduleId, DataEngine& engine) {
    auto path = configPath(moduleId);
    if (!std::filesystem::exists(path)) {
        spdlog::info("DataStore: no config file for '{}', writing defaults to disk", moduleId);
        saveConfig(moduleId, engine);
        return false;
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        spdlog::error("DataStore: failed to open config file: {}", path.string());
        return false;
    }

    std::string json((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());

    if (engine.writeSection(moduleId, DataSection::Config, json)) {
        spdlog::info("DataStore: loaded config for '{}' from {}", moduleId, path.string());
        return true;
    }

    spdlog::error("DataStore: failed to parse config for '{}'", moduleId);
    return false;
}

bool DataStore::saveConfig(const std::string& moduleId, const DataEngine& engine) {
    auto path = configPath(moduleId);
    std::filesystem::create_directories(path.parent_path());

    auto json = engine.readSection(moduleId, DataSection::Config);

    std::ofstream file(path);
    if (!file.is_open()) {
        spdlog::error("DataStore: failed to write config file: {}", path.string());
        return false;
    }

    file << glz::prettify_json(json);
    spdlog::info("DataStore: saved config for '{}' to {}", moduleId, path.string());

    // Write schema alongside
    auto schemaPath = path.parent_path() / "config.schema.json";
    auto schema = engine.schemaSection(moduleId, DataSection::Config);
    if (std::ofstream sf(schemaPath); sf.is_open())
        sf << glz::prettify_json(schema);

    return true;
}

bool DataStore::loadRecipe(const std::string& moduleId, const std::string& recipeName, DataEngine& engine) {
    auto path = recipePath(moduleId, recipeName);
    if (!std::filesystem::exists(path)) {
        spdlog::info("DataStore: no recipe '{}' for '{}', writing defaults to disk", recipeName, moduleId);
        saveRecipe(moduleId, recipeName, engine);
        return false;
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        spdlog::error("DataStore: failed to open recipe file: {}", path.string());
        return false;
    }

    std::string json((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());

    if (engine.writeSection(moduleId, DataSection::Recipe, json)) {
        spdlog::info("DataStore: loaded recipe '{}' for '{}'", recipeName, moduleId);
        return true;
    }

    spdlog::error("DataStore: failed to parse recipe '{}' for '{}'", recipeName, moduleId);
    return false;
}

bool DataStore::saveRecipe(const std::string& moduleId, const std::string& recipeName, const DataEngine& engine) {
    auto path = recipePath(moduleId, recipeName);
    std::filesystem::create_directories(path.parent_path());

    auto json = engine.readSection(moduleId, DataSection::Recipe);

    std::ofstream file(path);
    if (!file.is_open()) {
        spdlog::error("DataStore: failed to write recipe file: {}", path.string());
        return false;
    }

    file << glz::prettify_json(json);
    spdlog::info("DataStore: saved recipe '{}' for '{}'", recipeName, moduleId);

    // Write schema alongside
    auto schemaPath = path.parent_path() / (recipeName + ".schema.json");
    auto schema = engine.schemaSection(moduleId, DataSection::Recipe);
    if (std::ofstream sf(schemaPath); sf.is_open())
        sf << glz::prettify_json(schema);

    return true;
}

std::vector<std::string> DataStore::listRecipes(const std::string& moduleId) const {
    std::vector<std::string> names;
    auto dir = modulePath(moduleId) / "recipes";

    if (!std::filesystem::is_directory(dir)) {
        return names;
    }

    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto path = entry.path();
        if (path.extension() != ".json") continue;
        // Skip schema files (e.g. "fast.schema.json")
        auto stem = path.stem().string();
        if (stem.size() > 7 && stem.substr(stem.size() - 7) == ".schema") continue;
        names.push_back(stem);
    }

    std::sort(names.begin(), names.end());
    return names;
}

std::filesystem::path DataStore::modulePath(const std::string& moduleId) const {
    return rootDir_ / moduleId;
}

std::filesystem::path DataStore::configPath(const std::string& moduleId) const {
    return modulePath(moduleId) / "config.json";
}

std::filesystem::path DataStore::recipePath(const std::string& moduleId, const std::string& recipeName) const {
    return modulePath(moduleId) / "recipes" / (recipeName + ".json");
}

} // namespace loom
