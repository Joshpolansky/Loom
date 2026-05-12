#pragma once

#include <cstdint>
#include <string>
#include "loom/version.h"

namespace loom {

/// API version for ABI compatibility checks between runtime and modules.
///
/// Bump on any change to the ModuleHeader layout or to the exported
/// loom_module_create / loom_module_destroy contract. Modules whose
/// `hdr->api_version` doesn't match are rejected before the runtime
/// reads any header fields, so introducing a new trailing field is
/// safe — a module built against the older header will be rejected
/// rather than have its header read past the end.
///
/// v1 → v2: added ModuleHeader::source_file.
inline constexpr uint32_t kApiVersion = 2;

/// Describes a loaded module to the runtime.
struct ModuleHeader {
    uint32_t    api_version = kApiVersion;
    const char* name        = "unnamed";
    const char* version     = "0.0.0";
    /// SDK version string baked in at module compile time (from VERSION file).
    /// The runtime rejects modules whose sdk_version differs from its own.
    const char* sdk_version = kSdkVersion;
    /// Source file the LOOM_MODULE_HEADER macro was invoked from (__FILE__).
    /// Lets tooling jump to source. Null for modules built before this field.
    const char* source_file = nullptr;
};

/// Current state of a module in the runtime lifecycle.
enum class ModuleState : uint8_t {
    Unloaded,
    Loaded,
    Initialized,
    Running,
    Stopping,
    Error,
};

/// Identifies which data section is being accessed.
enum class DataSection : uint8_t {
    Config,
    Recipe,
    Runtime,
    Summary,  ///< Optional read-only state summary shown on the dashboard
};

/// Error codes returned from module operations.
enum class ModuleError : int32_t {
    Ok = 0,
    InitFailed = -1,
    CyclicFailed = -2,
    ExitFailed = -3,
    InvalidConfig = -4,
    InvalidRecipe = -5,
};

/// Reason why init() is being called — passed via InitContext.
enum class InitReason : uint8_t {
    Boot,      ///< First load at runtime startup
    Reload,    ///< Warm-restart triggered via API or tooling
    Recovery,  ///< Restart after a fault or watchdog trip (future use)
};

/// Context passed to init(). Use this to branch behaviour on why init is running.
/// Additional fields will be added here as the runtime evolves (e.g. prior version,
/// whether a runtime snapshot was restored) without changing the init() signature.
struct InitContext {
    InitReason reason = InitReason::Boot;

    /// True when a prior runtime snapshot was found and applied after this init().
    /// Module authors can check this to know whether runtime_ reflects a previous state.
    bool runtimeRestored = false;

    /// The previous module version string, if this is a Reload and the .so was swapped.
    /// Empty string on Boot or if the version is unchanged.
    std::string previousVersion;
};

/// Scheduling hint declared by a module author.
/// The runtime uses this as a fallback when no explicit assignment exists in scheduler.json.
/// The runtime may override any or all of these values.
struct TaskHint {
    std::string cyclicClass{};  ///< Preferred class name. "" = accept system default.
    int         order = 0;      ///< Preferred execution order within the class.
    bool        isolate = false; ///< Request a dedicated thread (opt-out of class pooling).
};

} // namespace loom
