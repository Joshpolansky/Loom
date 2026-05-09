cmake_minimum_required(VERSION 3.25)

# When invoked directly by Conan (standalone), behave as a top-level project.
# When included via add_subdirectory() from the root, skip and inherit the parent version.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # LOOM_SDK_VERSION is injected by conanfile.py via CMakeToolchain variables.
    if(NOT DEFINED LOOM_SDK_VERSION)
        message(FATAL_ERROR "LOOM_SDK_VERSION must be set when building sdk standalone (set by conanfile.py)")
    endif()
    project(loom-sdk VERSION ${LOOM_SDK_VERSION} LANGUAGES CXX)
    find_package(glaze REQUIRED)
else()
    # Subdirectory mode: inherit version from the root project
    set(LOOM_SDK_VERSION ${PROJECT_VERSION})
endif()

# Generate version.h from the template so every module built against this SDK
# has the exact version string baked in at compile time.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/loom/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/loom/version.h
    @ONLY
)

add_library(sdk INTERFACE)
add_library(loom::sdk ALIAS sdk)

target_include_directories(sdk INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(sdk INTERFACE
    glaze::glaze
)

target_compile_features(sdk INTERFACE cxx_std_23)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
    DESTINATION include)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/loom/version.h
    DESTINATION include/loom)

install(TARGETS sdk
    EXPORT LoomSdkTargets
)

install(EXPORT LoomSdkTargets
        FILE LoomSdkTargets.cmake
        NAMESPACE loom::
        DESTINATION lib/cmake/loom)

include(CMakePackageConfigHelpers)
set(CONFIG_INSTALL_DIR lib/cmake/loom)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/loom-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/loomConfig.cmake
    INSTALL_DESTINATION ${CONFIG_INSTALL_DIR}
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/loomConfigVersion.cmake
    VERSION ${LOOM_SDK_VERSION}
    COMPATIBILITY AnyNewerVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/loomConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/loomConfigVersion.cmake
    DESTINATION ${CONFIG_INSTALL_DIR}
)

# Conan packaging is handled outside of CMake (use `just conan-create` task)

