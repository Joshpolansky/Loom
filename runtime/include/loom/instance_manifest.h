#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>

namespace loom {

/// One entry in instances.json — describes a module instance to load at boot.
struct InstanceEntry {
    std::string id;         ///< Instance ID (bus namespace, data dir key)
    std::string so;         ///< .so filename, relative to moduleDir
    std::string className;  ///< header.name (informational; runtime re-reads from .so)
};

/// Load instances.json from disk. Returns empty vector if missing or malformed.
inline std::vector<InstanceEntry> loadInstanceManifest(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return {};

    std::ifstream f(path);
    if (!f.is_open()) {
        spdlog::error("InstanceManifest: cannot open '{}'", path.string());
        return {};
    }

    std::string json((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());

    std::vector<InstanceEntry> entries;
    auto err = glz::read_json(entries, json);
    if (err) {
        spdlog::error("InstanceManifest: parse error in '{}': {}",
                      path.string(), glz::format_error(err, json));
        return {};
    }

    return entries;
}

/// Write instances.json to disk (pretty-printed).
inline bool saveInstanceManifest(const std::filesystem::path& path,
                                  const std::vector<InstanceEntry>& entries) {
    std::filesystem::create_directories(path.parent_path());
    auto json = glz::write_json(entries);
    if (!json) {
        spdlog::error("InstanceManifest: serialization failed");
        return false;
    }
    std::ofstream f(path);
    if (!f.is_open()) {
        spdlog::error("InstanceManifest: cannot write '{}'", path.string());
        return false;
    }
    f << glz::prettify_json(*json);
    return true;
}

} // namespace loom
