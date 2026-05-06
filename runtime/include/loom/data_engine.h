#pragma once

#include "loom/module.h"
#include "loom/types.h"

#include <glaze/glaze.hpp>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace loom {

/// Describes a single field in a reflected data section.
struct FieldDescriptor {
    std::string name;
    std::string type;     // Human-readable type name
    size_t offset = 0;    // Byte offset within the data section
    size_t size = 0;      // Size in bytes
};

/// Describes one data section (Config, Recipe, or Runtime) of a module.
struct SectionDescriptor {
    DataSection section;
    std::vector<FieldDescriptor> fields;

    // Type-erased read/write via JSON
    std::function<std::string()> readJson;
    std::function<bool(std::string_view)> writeJson;
    std::function<bool(std::string_view, std::string_view)> writeField; ///< ptr, valueJson → in-place field update
    std::function<std::string()> readSchema; ///< JSON Schema for this section
};

/// Describes all data sections for one module.
struct ModuleDescriptor {
    std::string moduleId;
    SectionDescriptor config;
    SectionDescriptor recipe;
    SectionDescriptor runtime;
    SectionDescriptor summary;
};

/// Central registry of module data descriptors.
/// Uses glaze reflection to build field descriptors at registration time.
class DataEngine {
public:
    /// Register a module's data sections. Called by the runtime after loading a module.
    void registerModule(const std::string& moduleId, IModule* module);

    /// Unregister a module (on unload).
    void unregisterModule(const std::string& moduleId);

    /// Read a data section as JSON.
    std::string readSection(const std::string& moduleId, DataSection section) const;

    /// Write a data section from JSON.
    bool writeSection(const std::string& moduleId, DataSection section, std::string_view json);

    /// Read-modify-write a single field within a section.
    /// @param ptr   JSON Pointer (RFC 6901) to the field, e.g. "/speeds/2" or "/enabled"
    /// @param valueJson  JSON-encoded value to set at that path, e.g. "42.0" or "true" or "\"hello\""
    bool patchSection(const std::string& moduleId, DataSection section,
                      std::string_view ptr, std::string_view valueJson);

    /// Read the JSON Schema for a section (Config or Recipe only).
    std::string schemaSection(const std::string& moduleId, DataSection section) const;

    /// Get the descriptor for a module (for introspection / frontend schema).
    const ModuleDescriptor* descriptor(const std::string& moduleId) const;

    /// Get all registered module descriptors.
    const std::unordered_map<std::string, ModuleDescriptor>& descriptors() const { return descriptors_; }

private:
    std::unordered_map<std::string, ModuleDescriptor> descriptors_;
};

} // namespace loom
