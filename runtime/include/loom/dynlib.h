#pragma once

/// dynlib.h — thin platform abstraction over dynamic library loading.
///
/// Wraps dlopen/dlsym/dlclose (POSIX) and LoadLibrary/GetProcAddress/FreeLibrary (Windows)
/// behind a uniform API so the rest of the runtime is platform-agnostic.

#include <string>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>

namespace loom::dynlib {

using Handle = HMODULE;
static constexpr Handle kNull = nullptr;

inline Handle open(const char* path) {
    return LoadLibraryA(path);
}

inline void* sym(Handle h, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(h, name));
}

inline bool close(Handle h) {
    return FreeLibrary(h) != 0;
}

inline std::string lastError() {
    DWORD err = GetLastError();
    if (err == 0) return {};
    char buf[256]{};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, buf, sizeof(buf), nullptr);
    return buf;
}

/// Platform-native shared module extension.
static constexpr const char* kExtension = ".dll";

} // namespace loom::dynlib

#else // POSIX (Linux, macOS)
#  include <dlfcn.h>

namespace loom::dynlib {

using Handle = void*;
static constexpr Handle kNull = nullptr;

inline Handle open(const char* path) {
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

inline void* sym(Handle h, const char* name) {
    return dlsym(h, name);
}

inline bool close(Handle h) {
    return dlclose(h) == 0;
}

inline std::string lastError() {
    const char* e = dlerror();
    return e ? e : "";
}

#  if defined(__APPLE__)
/// Modules are intentionally built with .so suffix on macOS for cross-platform
/// consistency (matching Linux); use .so here to match the build output.
static constexpr const char* kExtension = ".so";
#  else
static constexpr const char* kExtension = ".so";
#  endif

} // namespace loom::dynlib

#endif // platform
