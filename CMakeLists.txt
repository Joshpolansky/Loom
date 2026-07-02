cmake_minimum_required(VERSION 3.25)

# Version is the single source of truth — read from VERSION file
file(READ "${CMAKE_SOURCE_DIR}/VERSION" LOOM_VERSION)
string(STRIP "${LOOM_VERSION}" LOOM_VERSION)

project(Loom VERSION ${LOOM_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Position-independent code for shared libraries
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Use the dynamic CRT (/MD, /MDd) for everything on MSVC — never the static CRT
# (/MT). Loom loads module plugins as separate shared libraries and passes
# std::string/std::vector across that boundary; a static CRT gives each binary
# its own heap, so an object allocated by one side and freed by the other
# corrupts the heap. The dynamic CRT lets the runtime EXE and every module DLL
# share one heap. (Debug-vs-Release matching, which the dynamic CRT does NOT fix
# on its own, is enforced at load time by the module build-signature check in
# module_loader.cpp via loom_query_build_info.)
# Requires policy CMP0091 = NEW, which holds via cmake_minimum_required(3.25).
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/output")
set(CMAKE_PDB_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/output")

if(MSVC)
    # Conan's toolchain suppresses CMake's default /debug linker flag for MSVC
    # Debug builds, so no PDB is ever emitted. Re-add it explicitly.
    string(APPEND CMAKE_EXE_LINKER_FLAGS_DEBUG " /DEBUG")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS_DEBUG " /DEBUG")
    string(APPEND CMAKE_MODULE_LINKER_FLAGS_DEBUG " /DEBUG")
endif()

# Module-build helpers (loom_add_module, debug-info + dSYM) — shared with module
# authors via the SDK install. Defines the LOOM_WITH_DEBUG_INFO option.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(LoomModule)

# Emit debug info for crash symbolization in OPTIMIZED configs too (Debug always
# has it), so release/downloaded runtimes still produce readable crash traces.
# Symbols are shipped next to the binaries (install rules + CI staging). This is
# metadata, not code — no effect on runtime speed, only binary/file size.
# Skipped for WASM: there's no in-process symbolization there (no cpptrace), and
# -g would bloat the .wasm by ~10x for no benefit.
if(LOOM_WITH_DEBUG_INFO AND NOT EMSCRIPTEN)
    if(MSVC)
        foreach(cfg RELEASE RELWITHDEBINFO MINSIZEREL)
            string(APPEND CMAKE_C_FLAGS_${cfg}             " /Zi")
            string(APPEND CMAKE_CXX_FLAGS_${cfg}           " /Zi")
            # /OPT:REF,ICF keeps the optimized image lean (the linker turns them
            # off once /DEBUG is present).
            string(APPEND CMAKE_EXE_LINKER_FLAGS_${cfg}    " /DEBUG /OPT:REF /OPT:ICF")
            string(APPEND CMAKE_SHARED_LINKER_FLAGS_${cfg} " /DEBUG /OPT:REF /OPT:ICF")
            string(APPEND CMAKE_MODULE_LINKER_FLAGS_${cfg} " /DEBUG /OPT:REF /OPT:ICF")
        endforeach()
    else()
        # Linux: DWARF embedded in the (unstripped) binary. macOS: -g leaves DWARF
        # in the .o files; a .dSYM is produced per target by loom_target_dsym().
        foreach(cfg RELEASE RELWITHDEBINFO)
            string(APPEND CMAKE_C_FLAGS_${cfg}   " -g")
            string(APPEND CMAKE_CXX_FLAGS_${cfg} " -g")
        endforeach()
    endif()
endif()

# Platform-appropriate suffix for Loom module plugins (.dll on Windows, .so elsewhere)
if(WIN32)
    set(LOOM_MODULE_SUFFIX ".dll")
else()
    set(LOOM_MODULE_SUFFIX ".so")
endif()

# Set default build type
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Dependencies — all from Conan, native and wasm alike (same pinned versions, no
# drift). glaze + spdlog are shared. The native host also needs Crow (HTTP/WS),
# cpptrace (crash symbolization) and GTest; the WASM build is thread-free and
# server-less and skips them (gated in conanfile.py via settings.os).
find_package(glaze REQUIRED)
find_package(spdlog REQUIRED)
if(EMSCRIPTEN)
    # spdlog is header-only here; alias it to the plain name the tree links
    # against. Neutralize fmt's consteval (rejected by emcc's clang).
    if(NOT TARGET spdlog::spdlog AND TARGET spdlog::spdlog_header_only)
        add_library(spdlog::spdlog INTERFACE IMPORTED)
        target_link_libraries(spdlog::spdlog INTERFACE spdlog::spdlog_header_only)
    endif()
    add_compile_definitions(FMT_CONSTEVAL=)
    add_compile_options(-fexceptions)
    add_link_options(-fexceptions)

    # CMake's Emscripten platform defaults TARGET_SUPPORTS_SHARED_LIBS to FALSE,
    # so add_library(MODULE) (used by every bundled module — modules/*/CMakeLists.txt)
    # silently downgrades to a STATIC archive instead of a dlopen-able wasm side
    # module. Override it before modules/ is added.
    set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS TRUE)
else()
    find_package(GTest REQUIRED)
    find_package(Crow REQUIRED)
    find_package(cpptrace REQUIRED)  # runtime-only: crash-report symbolization
endif()

add_subdirectory(sdk)
add_subdirectory(runtime)

# Bundled modules build under both native and WASM (as SIDE_MODULEs — see
# modules/CMakeLists.txt); EtherCAT is the one exception, gated out there.
# Tests stay native-only (need GTest, which conanfile.py doesn't pull for wasm).
add_subdirectory(modules)
if(NOT EMSCRIPTEN)
    enable_testing()
    add_subdirectory(tests)
endif()
