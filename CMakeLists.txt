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

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/output")

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

# Conan-provided dependencies
find_package(glaze REQUIRED)
find_package(spdlog REQUIRED)
find_package(GTest REQUIRED)
find_package(Crow REQUIRED)

add_subdirectory(sdk)
add_subdirectory(runtime)
add_subdirectory(modules)

enable_testing()
add_subdirectory(tests)
