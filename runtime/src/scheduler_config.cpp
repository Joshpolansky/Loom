#include "loom/scheduler_config.h"

#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>

#include <fstream>

namespace loom {

const ClassDef* findClassDef(const SchedulerConfig& cfg, const std::string& name) {
    for (const auto& c : cfg.classes) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

SchedulerConfig defaultSchedulerConfig() {
    SchedulerConfig cfg;
    cfg.defaultClass = "normal";
    cfg.classes = {
        ClassDef{ "fast",   1000,   0, 90 },
        ClassDef{ "normal", 10000,  -1, 50 },
        ClassDef{ "slow",   100000, -1, 30 },
    };
    return cfg;
}

SchedulerConfig loadSchedulerConfig(const std::filesystem::path& path) {
    // Always start with defaults so built-in classes are never absent.
    SchedulerConfig cfg = defaultSchedulerConfig();

    if (!std::filesystem::exists(path)) {
        spdlog::info("scheduler.json not found at '{}', using defaults", path.string());
        return cfg;
    }

    std::ifstream f(path);
    if (!f.is_open()) {
        spdlog::warn("Could not open '{}', using default scheduler config", path.string());
        return cfg;
    }

    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    // Read into a temporary so a parse failure leaves cfg (defaults) intact.
    SchedulerConfig parsed = defaultSchedulerConfig();
    auto err = glz::read_json(parsed, content);
    if (err) {
        spdlog::warn("Failed to parse '{}', using defaults. Error: {}",
                     path.string(), glz::format_error(err, content));
        return cfg;  // return defaults
    }

    return parsed;
}

void saveSchedulerConfig(const SchedulerConfig& cfg, const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());

    auto result = glz::write_json(cfg);
    if (!result) {
        spdlog::error("Failed to serialize scheduler config");
        return;
    }

    std::ofstream f(path);
    if (!f.is_open()) {
        spdlog::error("Could not write scheduler config to '{}'", path.string());
        return;
    }

    f << glz::prettify_json(*result);
    spdlog::info("Saved scheduler config to '{}'", path.string());

    // Write JSON Schema alongside for tooling / IDE support.
    auto schemaPath = path.parent_path() / "scheduler.schema.json";
    auto schema = glz::write_json_schema<SchedulerConfig>().value_or("{}");
    if (std::ofstream sf(schemaPath); sf.is_open())
        sf << glz::prettify_json(schema);
}

} // namespace loom
