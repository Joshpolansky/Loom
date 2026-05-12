#include "loom/module_loader.h"
#include "loom/dynlib.h"

#include <spdlog/spdlog.h>
#include <atomic>

namespace loom {

#if defined(_WIN32)
static std::atomic<uint64_t> gShadowLoadCounter{0};
#endif

// Function pointer types for the exported C interface
using CreateFn = IModule* (*)();
using DestroyFn = void (*)(IModule*);
using QueryHeaderFn = const ModuleHeader* (*)();

ModuleLoader::~ModuleLoader() {
    // Unload all modules in reverse order
    std::vector<std::string> ids;
    ids.reserve(modules_.size());
    for (auto& [id, _] : modules_) {
        ids.push_back(id);
    }
    for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
        unload(*it);
    }
}

std::string ModuleLoader::load(const std::filesystem::path& soPath,
                              const std::string& instanceId) {
    if (!std::filesystem::exists(soPath)) {
        spdlog::error("Module file not found: {}", soPath.string());
        return {};
    }

#if defined(_WIN32)
    // On Windows, never load the original DLL directly. Loading it even once can
    // cause the debugger to bind and lock the build-side PDB, breaking hot-rebuild.
    std::string shadowKey = instanceId.empty() ? soPath.stem().string() : instanceId;
    auto shadowSeq = ++gShadowLoadCounter;
    std::filesystem::path shadowDir =
        soPath.parent_path() / ".shadow" / shadowKey / std::to_string(shadowSeq);
    std::filesystem::path shadowPath = shadowDir / soPath.filename();

    auto cleanupShadowDir = [&]() {
        std::error_code cleanupEc;
        std::filesystem::remove_all(shadowDir, cleanupEc);
    };

    std::error_code ec;
    std::filesystem::create_directories(shadowDir, ec);
    if (ec) {
        spdlog::error("Failed to create shadow dir '{}': {}", shadowDir.string(), ec.message());
        return {};
    }

    std::filesystem::copy_file(soPath, shadowPath,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        spdlog::error("Shadow copy failed for '{}': {}", soPath.string(), ec.message());
        cleanupShadowDir();
        return {};
    }

    // Best-effort shadow-copy of debug symbols next to the shadow DLL.
    auto sourcePdbPath = soPath;
    sourcePdbPath.replace_extension(".pdb");
    if (!std::filesystem::exists(sourcePdbPath)) {
        sourcePdbPath = soPath.parent_path() / (soPath.stem().string() + "_build.pdb");
    }
    if (std::filesystem::exists(sourcePdbPath)) {
        auto shadowPdbPath = shadowDir / sourcePdbPath.filename();
        std::error_code pdbEc;
        std::filesystem::copy_file(sourcePdbPath, shadowPdbPath,
                                   std::filesystem::copy_options::overwrite_existing, pdbEc);
        if (pdbEc) {
            spdlog::warn("Shadow PDB copy failed for '{}': {}", sourcePdbPath.string(), pdbEc.message());
        }
    }

    auto loadPath = shadowPath;
#else
    auto loadPath = soPath;
#endif

    // Open the shared library
    dynlib::Handle handle = dynlib::open(loadPath.string().c_str());
    if (!handle) {
        spdlog::error("dynlib::open failed for {}: {}", loadPath.string(), dynlib::lastError());
#if defined(_WIN32)
        cleanupShadowDir();
#endif
        return {};
    }

    // Clear any existing error (POSIX only — harmless on Windows)
    dynlib::lastError();

    // Resolve the header query function first (version check)
    auto queryHeader = reinterpret_cast<QueryHeaderFn>(dynlib::sym(handle, "loom_query_header"));
    if (!queryHeader) {
        spdlog::error("Missing loom_query_header in {}: {}", loadPath.string(), dynlib::lastError());
        dynlib::close(handle);
    #if defined(_WIN32)
        cleanupShadowDir();
    #endif
        return {};
    }

    const ModuleHeader* hdr = queryHeader();
    if (!hdr) {
        spdlog::error("loom_query_header returned null in {}", loadPath.string());
        dynlib::close(handle);
    #if defined(_WIN32)
        cleanupShadowDir();
    #endif
        return {};
    }

    // API version check
    if (hdr->api_version != kApiVersion) {
        spdlog::error("API version mismatch for {} (module: {}, runtime: {})",
                       loadPath.string(), hdr->api_version, kApiVersion);
        dynlib::close(handle);
#if defined(_WIN32)
        cleanupShadowDir();
#endif
        return {};
    }

    // SDK version check: module must have been built against the same SDK version.
    // hdr->sdk_version may be null for modules built before this field was added —
    // treat that as a mismatch rather than silently loading a potentially incompatible module.
    const char* modSdkVer = hdr->sdk_version ? hdr->sdk_version : "<unknown>";
    if (std::string_view(modSdkVer) != std::string_view(kSdkVersion)) {
        spdlog::error("SDK version mismatch for {} (module built with: {}, runtime SDK: {}). "
                      "Rebuild the module against SDK {}.",
                      loadPath.string(), modSdkVer, kSdkVersion, kSdkVersion);
        dynlib::close(handle);
#if defined(_WIN32)
        cleanupShadowDir();
#endif
        return {};
    }

    // Resolve factory functions
    auto createFn = reinterpret_cast<CreateFn>(dynlib::sym(handle, "loom_module_create"));
    if (!createFn) {
        spdlog::error("Missing loom_module_create in {}: {}", loadPath.string(), dynlib::lastError());
        dynlib::close(handle);
    #if defined(_WIN32)
        cleanupShadowDir();
    #endif
        return {};
    }

    auto destroyFn = reinterpret_cast<DestroyFn>(dynlib::sym(handle, "loom_module_destroy"));
    if (!destroyFn) {
        spdlog::error("Missing loom_module_destroy in {}: {}", loadPath.string(), dynlib::lastError());
        dynlib::close(handle);
    #if defined(_WIN32)
        cleanupShadowDir();
    #endif
        return {};
    }

    // Create the module instance with custom deleter
    IModule* rawInstance = createFn();
    if (!rawInstance) {
        spdlog::error("loom_module_create returned null in {}", loadPath.string());
        dynlib::close(handle);
    #if defined(_WIN32)
        cleanupShadowDir();
    #endif
        return {};
    }

    // Resolve the instance ID: use the caller-supplied one if present,
    // fall back to header.name for backwards-compatible single-instance loads.
    std::string id = instanceId.empty() ? std::string(hdr->name) : instanceId;
    if (modules_.contains(id)) {
        spdlog::error("Module instance '{}' already loaded (class: {})", id, hdr->name);
        destroyFn(rawInstance);
        dynlib::close(handle);
#if defined(_WIN32)
        cleanupShadowDir();
#endif
        return {};
    }

    // Keep ownership of string fields (ModuleHeader stores const char*).
    std::string classNameStr   = hdr->name        ? hdr->name        : "";
    std::string versionStr     = hdr->version     ? hdr->version     : "";
    std::string sourceFileStr  = hdr->source_file ? hdr->source_file : "";

    LoadedModule mod;
    mod.id        = id;
    mod.className = classNameStr;
    mod.path      = soPath;
#if defined(_WIN32)
    mod.shadowPath = loadPath;
#endif
    mod.handle        = handle;
    mod.header        = *hdr;
    mod.nameStr       = classNameStr;
    mod.versionStr    = versionStr;
    mod.sourceFileStr = sourceFileStr;
    mod.instance = std::unique_ptr<IModule, std::function<void(IModule*)>>(
        rawInstance, destroyFn);
    mod.state = ModuleState::Loaded;

    spdlog::info("Loaded module '{}' v{} from {}", id, versionStr, soPath.string());

    modules_.emplace(id, std::move(mod));
    return id;
}

bool ModuleLoader::unload(const std::string& id) {
    auto it = modules_.find(id);
    if (it == modules_.end()) {
        spdlog::warn("Cannot unload '{}': not found", id);
        return false;
    }

    auto& mod = it->second;

    // Call exit() if the module was initialized/running
    if (mod.state == ModuleState::Running || mod.state == ModuleState::Initialized) {
        spdlog::info("Calling exit() on module '{}'", id);
        mod.instance->exit();
    }

    // Destroy the instance (calls destroyFn via custom deleter)
    mod.instance.reset();

    // Close the shared library
    if (mod.handle) {
        if (!dynlib::close(mod.handle)) {
            spdlog::error("dynlib::close failed for '{}': {}", id, dynlib::lastError());
        }
        mod.handle = nullptr;
    }

    mod.state = ModuleState::Unloaded;
    spdlog::info("Unloaded module '{}'", id);

#if defined(_WIN32)
    if (!mod.shadowPath.empty()) {
        std::error_code ec;
        auto shadowDir = mod.shadowPath.parent_path();
        std::filesystem::remove_all(shadowDir, ec);
        if (ec) spdlog::warn("Failed to remove shadow dir '{}': {}", shadowDir.string(), ec.message());
    }
#endif

    modules_.erase(it);
    return true;
}

bool ModuleLoader::reload(const std::string& id, const std::filesystem::path& newPath) {
    auto it = modules_.find(id);
    if (it == modules_.end()) {
        spdlog::warn("Cannot reload '{}': not found", id);
        return false;
    }

    std::filesystem::path path = newPath.empty() ? it->second.path : newPath;

    spdlog::info("Reloading module '{}' from {}", id, path.string());
    unload(id);
    return !load(path, id).empty();  // preserve the original instance ID
}

std::vector<std::string> ModuleLoader::loadDirectory(const std::filesystem::path& dir) {
    std::vector<std::string> loaded;

    if (!std::filesystem::is_directory(dir)) {
        spdlog::error("Module directory not found: {}", dir.string());
        return loaded;
    }

    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            auto ext = entry.path().extension().string();
            if (ext == dynlib::kExtension) {
                auto id = load(entry.path());
                if (!id.empty()) {
                    loaded.push_back(std::move(id));
                }
            }
        }
    }

    spdlog::info("Loaded {} module(s) from {}", loaded.size(), dir.string());
    return loaded;
}

LoadedModule* ModuleLoader::get(const std::string& id) {
    auto it = modules_.find(id);
    return it != modules_.end() ? &it->second : nullptr;
}

const LoadedModule* ModuleLoader::get(const std::string& id) const {
    auto it = modules_.find(id);
    return it != modules_.end() ? &it->second : nullptr;
}

std::vector<AvailableModule> ModuleLoader::queryAvailable(const std::filesystem::path& dir) {
    std::vector<AvailableModule> result;
    if (!std::filesystem::is_directory(dir)) return result;

    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension().string() != dynlib::kExtension) continue;

        auto handle = dynlib::open(entry.path().string().c_str());
        if (!handle) continue;

        auto queryHeader = reinterpret_cast<QueryHeaderFn>(dynlib::sym(handle, "loom_query_header"));
        if (queryHeader) {
            const ModuleHeader* hdr = queryHeader();
            if (hdr && hdr->name) {
                AvailableModule am;
                am.filename  = entry.path().filename().string();
                am.className = hdr->name;
                am.version   = hdr->version ? hdr->version : "";
                result.push_back(std::move(am));
            }
        }

        dynlib::close(handle);
    }

    return result;
}

} // namespace loom
