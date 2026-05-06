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
# Static library — all runtime logic, exported for consumers to link against.
# Consumers add a thin main.cpp that calls loom::run(argc, argv).
# ---------------------------------------------------------------------------
add_library(loom_runtime STATIC
    src/run.cpp
    src/runtime_core.cpp
    src/module_loader.cpp
    src/scheduler.cpp
    src/scheduler_config.cpp
    src/data_engine.cpp
    src/data_store.cpp
    src/io_mapper.cpp
    src/server.cpp
    src/oscilloscope.cpp
    src/module_watcher.cpp
)

target_include_directories(loom_runtime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(loom_runtime PUBLIC
    loom::sdk
    spdlog::spdlog
    Crow::Crow
    ${CMAKE_DL_LIBS}
)

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

install(TARGETS loom_runtime
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(TARGETS loom
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY "${CMAKE_SOURCE_DIR}/frontend/dist/"
    DESTINATION data/UI
    OPTIONAL
)

