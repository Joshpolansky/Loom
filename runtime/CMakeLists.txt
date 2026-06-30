cmake_minimum_required(VERSION 3.25)

# When invoked directly by Conan (standalone), behave as a top-level project.
# When included via add_subdirectory() from the root, skip and use parent-provided targets.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # LOOM_RUNTIME_VERSION is injected by conanfile.py via CMakeToolchain variables.
    if(NOT DEFINED LOOM_RUNTIME_VERSION)
        message(FATAL_ERROR "LOOM_RUNTIME_VERSION must be set when building runtime standalone (set by conanfile.py)")
    endif()
    project(loom-runtime VERSION ${LOOM_RUNTIME_VERSION} LANGUAGES CXX)

    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    find_package(loom REQUIRED)
    find_package(spdlog REQUIRED)
    find_package(Crow REQUIRED)
endif()

# ---------------------------------------------------------------------------
# loom_core — portable engine. No networking, signals, or process lifecycle:
# the scheduler, data engine/store, module loader, IO mapper, oscilloscope and
# the portable diagnostics. Depends only on the SDK, spdlog, glaze (via the SDK)
# and dlopen. This is the layer every host links against — the native host
# below, and (out of tree, on branch wasm-spike) an Emscripten/WASM host.
#
# NB: keep this list free of any TU that needs sockets (Crow), signals, or
# in-process stack symbolization (cpptrace) — those belong in loom_runtime.
# ---------------------------------------------------------------------------
add_library(loom_core STATIC
    src/runtime_core.cpp
    src/runtime_heap.cpp
    src/module_loader.cpp
    src/scheduler.cpp
    src/scheduler_config.cpp
    src/data_engine.cpp
    src/data_store.cpp
    src/io_mapper.cpp
    src/oscilloscope.cpp
    src/module_watcher.cpp
    src/diag/breadcrumb.cpp
    src/diag/fault_report.cpp
    src/diag/fault_store.cpp
    src/diag/runtime_fault_sink.cpp
)

target_include_directories(loom_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Build type stamped into crash reports (works for single- and multi-config).
target_compile_definitions(loom_core PRIVATE LOOM_BUILD_TYPE="$<CONFIG>")

target_link_libraries(loom_core PUBLIC
    loom::sdk
    spdlog::spdlog
    ${CMAKE_DL_LIBS}
)

# ---------------------------------------------------------------------------
# loom_runtime — native host layer over loom_core: the HTTP/WebSocket and
# OPC-UA servers, process lifecycle / signal handling, and in-process crash
# capture. It aggregates loom_core (PUBLIC), so the installed and conan-consumed
# `loom_runtime` target keeps providing the full runtime exactly as before.
# Consumers still add a thin main.cpp that calls loom::run(argc, argv).
# ---------------------------------------------------------------------------
add_library(loom_runtime STATIC
    src/run.cpp
    src/server.cpp
    src/opcua_rest_server.cpp
    src/diag/crash_handler.cpp
    src/diag/symbolizer.cpp
)

# Build type stamped into crash reports (works for single- and multi-config).
target_compile_definitions(loom_runtime PRIVATE LOOM_BUILD_TYPE="$<CONFIG>")

target_link_libraries(loom_runtime PUBLIC
    loom_core
    Crow::Crow
)

# Symbolization for crash reports. PRIVATE: confined to the diag symbolizer TU,
# never exposed in loom_runtime's public headers (SDK/consumers stay clean of it).
target_link_libraries(loom_runtime PRIVATE cpptrace::cpptrace)

# ---------------------------------------------------------------------------
# Thin executable — just calls loom::run(argc, argv).
# ---------------------------------------------------------------------------
add_executable(loom src/main.cpp)
target_link_libraries(loom PRIVATE loom_runtime)

# ---------------------------------------------------------------------------
# Frontend UI — symlink data/UI → frontend/dist for live development editing.
# On install, the dist contents are copied instead (see install rule below).
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_SOURCE_DIR}/frontend")
    add_custom_target(link_frontend_ui ALL
        COMMAND ${CMAKE_COMMAND} -E remove_directory "${CMAKE_SOURCE_DIR}/data/UI"
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            "${CMAKE_SOURCE_DIR}/frontend/dist"
            "${CMAKE_SOURCE_DIR}/data/UI"
        COMMENT "Symlinking frontend/dist -> data/UI"
        VERBATIM
    )
    add_dependencies(loom link_frontend_ui)
endif()

# Set RPATH so the executable can find modules relative to itself at install time.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_target_properties(loom PROPERTIES
        BUILD_RPATH           "$ORIGIN/modules"
        INSTALL_RPATH         "$ORIGIN/modules"
        INSTALL_RPATH_USE_LINK_PATH FALSE
    )
elseif(APPLE)
    set_target_properties(loom PROPERTIES
        BUILD_RPATH           "@loader_path/modules"
        INSTALL_RPATH         "@loader_path/modules"
        INSTALL_RPATH_USE_LINK_PATH FALSE
    )
endif()

# ---------------------------------------------------------------------------
# Install — library, headers, and thin exe.
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS loom_runtime loom_core
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(TARGETS loom
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Ship the Windows PDB next to the binary so crash reports symbolize in-process
# on the downloaded runtime. OPTIONAL: absent when LOOM_WITH_DEBUG_INFO is OFF.
# Linux DWARF is embedded in the unstripped binary; the macOS .dSYM is produced
# from the binary in the CI staging step (dsymutil).
if(MSVC)
    install(FILES $<TARGET_PDB_FILE:loom>
        DESTINATION ${CMAKE_INSTALL_BINDIR} OPTIONAL)
endif()

install(DIRECTORY "${CMAKE_SOURCE_DIR}/frontend/dist/"
    DESTINATION data/UI
    OPTIONAL
)

# Loom monitoring UI, served always at /_loom. Installed to a dedicated location
# (<prefix>/share/loom/ui) so the runtime can find it relative to the binary
# regardless of --data-dir (which may host a user app). resolveLoomUiDir() in
# run.cpp looks here first via <exe>/../share/loom/ui.
install(DIRECTORY "${CMAKE_SOURCE_DIR}/frontend/dist/"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/loom/ui
    OPTIONAL
)

