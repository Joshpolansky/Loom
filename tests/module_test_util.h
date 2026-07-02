#pragma once

#include <filesystem>
#include <string>

namespace loomtest {

/// Locate a built module plugin by class name, using the platform suffix and
/// the module output directory provided by CMake. Returns {} if not found
/// (caller should GTEST_SKIP — modules must be built first).
inline std::filesystem::path findModule(const std::string& name) {
#ifdef LOOM_MODULE_SUFFIX
    const std::string suffix = LOOM_MODULE_SUFFIX;
#else
    const std::string suffix = ".so";
#endif
#ifdef LOOM_MODULE_DIR
    for (const std::string& fname : {name + suffix, "lib" + name + suffix}) {
        std::filesystem::path p = std::filesystem::path(LOOM_MODULE_DIR) / fname;
        if (std::filesystem::exists(p)) return p;
    }
#endif
    return {};
}

} // namespace loomtest
