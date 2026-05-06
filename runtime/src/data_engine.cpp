#include "loom/data_engine.h"

#include <spdlog/spdlog.h>

namespace loom {

void DataEngine::registerModule(const std::string& moduleId, IModule* module) {
    if (descriptors_.contains(moduleId)) {
        spdlog::warn("DataEngine: module '{}' already registered, overwriting", moduleId);
    }

    ModuleDescriptor desc;
    desc.moduleId = moduleId;

    // Wire up type-erased JSON read/write through the IModule interface.
    // The actual glaze reflection happens inside Module<C,R,Rt>::readSection/writeSection.
    desc.config.section = DataSection::Config;
    desc.config.readJson    = [module]() { return module->readSection(DataSection::Config); };
    desc.config.writeJson   = [module](std::string_view json) { return module->writeSection(DataSection::Config, json); };
    desc.config.writeField  = [module](std::string_view ptr, std::string_view val) { return module->writeField(DataSection::Config, ptr, val); };
    desc.config.readSchema  = [module]() { return module->schemaSection(DataSection::Config); };

    desc.recipe.section = DataSection::Recipe;
    desc.recipe.readJson    = [module]() { return module->readSection(DataSection::Recipe); };
    desc.recipe.writeJson   = [module](std::string_view json) { return module->writeSection(DataSection::Recipe, json); };
    desc.recipe.writeField  = [module](std::string_view ptr, std::string_view val) { return module->writeField(DataSection::Recipe, ptr, val); };
    desc.recipe.readSchema  = [module]() { return module->schemaSection(DataSection::Recipe); };

    desc.runtime.section = DataSection::Runtime;
    desc.runtime.readJson   = [module]() { return module->readSection(DataSection::Runtime); };
    desc.runtime.writeJson  = [module](std::string_view json) { return module->writeSection(DataSection::Runtime, json); };
    desc.runtime.writeField = [module](std::string_view ptr, std::string_view val) { return module->writeField(DataSection::Runtime, ptr, val); };
    desc.runtime.readSchema = [module]() { return module->schemaSection(DataSection::Runtime); };

    desc.summary.section = DataSection::Summary;
    desc.summary.readJson   = [module]() { return module->readSection(DataSection::Summary); };
    desc.summary.writeJson  = [](std::string_view) { return false; }; // read-only
    desc.summary.readSchema = [module]() { return module->schemaSection(DataSection::Summary); };

    descriptors_.emplace(moduleId, std::move(desc));
    spdlog::info("DataEngine: registered module '{}'", moduleId);
}

void DataEngine::unregisterModule(const std::string& moduleId) {
    if (descriptors_.erase(moduleId)) {
        spdlog::info("DataEngine: unregistered module '{}'", moduleId);
    }
}

std::string DataEngine::readSection(const std::string& moduleId, DataSection section) const {
    auto it = descriptors_.find(moduleId);
    if (it == descriptors_.end()) {
        spdlog::warn("DataEngine: module '{}' not found", moduleId);
        return "{}";
    }

    const auto& desc = it->second;
    switch (section) {
        case DataSection::Config:  return desc.config.readJson();
        case DataSection::Recipe:  return desc.recipe.readJson();
        case DataSection::Runtime: return desc.runtime.readJson();
        case DataSection::Summary: return desc.summary.readJson();
    }
    return "{}";
}

bool DataEngine::writeSection(const std::string& moduleId, DataSection section, std::string_view json) {
    auto it = descriptors_.find(moduleId);
    if (it == descriptors_.end()) {
        spdlog::warn("DataEngine: module '{}' not found for write", moduleId);
        return false;
    }

    auto& desc = it->second;
    switch (section) {
        case DataSection::Config:  return desc.config.writeJson(json);
        case DataSection::Recipe:  return desc.recipe.writeJson(json);
        case DataSection::Runtime: return desc.runtime.writeJson(json);
        case DataSection::Summary: return false; // read-only
    }
    return false;
}

bool DataEngine::patchSection(const std::string& moduleId, DataSection section,
                              std::string_view ptr, std::string_view valueJson) {
    auto it = descriptors_.find(moduleId);
    if (it == descriptors_.end()) {
        spdlog::warn("DataEngine: patchSection: module '{}' not found", moduleId);
        return false;
    }
    // Delegate directly to the module's TagTable — no full section read/write needed.
    switch (section) {
        case DataSection::Config:  return it->second.config.writeField  ? it->second.config.writeField(ptr, valueJson)  : false;
        case DataSection::Recipe:  return it->second.recipe.writeField  ? it->second.recipe.writeField(ptr, valueJson)  : false;
        case DataSection::Runtime: return it->second.runtime.writeField ? it->second.runtime.writeField(ptr, valueJson) : false;
        case DataSection::Summary: return false;
    }
    return false;
}

const ModuleDescriptor* DataEngine::descriptor(const std::string& moduleId) const {
    auto it = descriptors_.find(moduleId);
    return it != descriptors_.end() ? &it->second : nullptr;
}

std::string DataEngine::schemaSection(const std::string& moduleId, DataSection section) const {
    auto it = descriptors_.find(moduleId);
    if (it == descriptors_.end()) return "{}";
    const auto& desc = it->second;
    switch (section) {
        case DataSection::Config:
            return desc.config.readSchema ? desc.config.readSchema() : "{}";
        case DataSection::Recipe:
            return desc.recipe.readSchema ? desc.recipe.readSchema() : "{}";
        case DataSection::Runtime:
            return desc.runtime.readSchema ? desc.runtime.readSchema() : "{}";
        case DataSection::Summary:
            return desc.summary.readSchema ? desc.summary.readSchema() : "{}";            
        default:
            return "{}";
    }
}

} // namespace loom
