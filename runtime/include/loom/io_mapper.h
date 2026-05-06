#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <optional>
#include <memory>

namespace loom {

// Forward declarations
class RuntimeCore;
class IModule;

/// Configuration for a single I/O mapping (persisted to JSON)
struct IOMapEntry {
    std::string source;          // e.g. "left_motor.runtime.current_speed" (required)
    std::string target;          // e.g. "controller.runtime.speed_input"   (required)
    bool enabled = true;         // optional, defaults to true
};

/// Cached resolution of a mapping for hot-path execution
struct ResolvedMapping {
    // Pointers (used for scalar fast path)
    void* src_ptr = nullptr;
    void* dst_ptr = nullptr;

    // Module refs (for runtimeLock on cross-class reads)
    IModule* src_mod = nullptr;
    IModule* dst_mod = nullptr;

    // Parsed from dot notation
    std::string src_module_id;
    std::string dst_module_id;
    std::string src_path;        // TagTable path (e.g. "current_speed")
    std::string dst_path;

    // Copy function — type-dispatched at resolve time
    std::function<void(const void*, void*)> copy_fn;

    bool valid = false;
    bool stable = true;          // false if path goes through dynamic container
    bool uses_json = false;       // true for complex same-type (JSON round-trip)
    std::string error;            // human-readable if invalid

    size_t config_index = 0;      // index into entries_ vector
};

/// I/O Mapper: copies field values between modules at runtime.
/// Manages a registry of mappings, resolves them against module instances,
/// caches pointers for hot-path performance, and handles hot-reload gracefully.
class IOMapper {
public:
    IOMapper() = default;
    ~IOMapper() = default;

    /// Load mappings from a JSON file (e.g. data/io_mappings.json).
    /// Format: [{"source": "...", "target": "...", "enabled": true}, ...]
    bool load(const std::string& filepath);

    /// Save mappings to a JSON file.
    bool save(const std::string& filepath) const;

    /// Resolve all mappings against the runtime core.
    /// Call after loading modules or on hot-reload.
    void resolveAll(const RuntimeCore& runtime);

    /// Invalidate mappings associated with a module (before reload).
    void invalidateModule(const std::string& module_id);

    /// Execute all valid mappings for modules in the given class.
    /// Called from Scheduler classLoop() after cyclic, before postCyclic.
    void executeForClass(const std::string& class_name);

    /// Execute all valid mappings for a module (isolated class).
    /// Called from Scheduler isolatedLoop() after cyclicGuarded().
    void executeForModule(const std::string& module_id);

    /// Add a new mapping at runtime.
    /// Returns the index of the new mapping, or size_t(-1) on error.
    size_t addMapping(const std::string& source, const std::string& target, bool enabled = true);

    /// Update a mapping by config index.
    bool updateMapping(size_t index, const std::string& source, const std::string& target, bool enabled);

    /// Remove a mapping by config index.
    bool removeMapping(size_t index);

    /// Enable/disable a mapping by config index (no re-resolve needed).
    bool setEnabled(size_t index, bool enabled);

    /// Get the resolved mapping at a given config index.
    const ResolvedMapping* getMapping(size_t index) const;

    /// Get all resolved mappings.
    const std::vector<ResolvedMapping>& getMappings() const { return resolved_; }

    /// Get the count of config entries.
    size_t entryCount() const { return entries_.size(); }

private:
    std::vector<IOMapEntry>     entries_;       // Config (persisted)
    std::vector<ResolvedMapping> resolved_;     // Cached resolution state

    /// Parse "left_motor.runtime.current_speed" into {module_id, path}
    /// Returns {module_id, path} or {error, ""} on parse failure.
    struct ParsedAddress { std::string module_id; std::string path; bool valid; };
    static ParsedAddress parseAddress(const std::string& address);

    /// Resolve a single mapping against the runtime core.
    void resolveOne(size_t index, const RuntimeCore& runtime);

    /// Build the copy function for a mapping (type dispatch).
    /// Returns nullptr if types are incompatible.
    std::function<void(const void*, void*)> buildCopyFn(
        const std::string& src_type,
        const std::string& dst_type,
        IModule* src_mod,
        IModule* dst_mod,
        const std::string& src_path,
        const std::string& dst_path,
        bool& out_uses_json
    ) const;

    /// Execute a single mapping (with proper locking for cross-class reads).
    void executeMapping(const ResolvedMapping& mapping);
};

} // namespace loom
