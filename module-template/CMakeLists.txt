cmake_minimum_required(VERSION 3.18)
project(module_template LANGUAGES CXX)

find_package(loom CONFIG REQUIRED)
find_package(loom-runtime REQUIRED)

# The loom executable — built from your own main.cpp that calls loom::run().
# CMake Tools sees this as a native launch target; select it in the status bar
# and press F5 to run or debug with your modules loaded.
add_executable(loom main.cpp)
target_link_libraries(loom PRIVATE loom::runtime)

option(BUILD_MODULE_TEMPLATE "Build example module template alongside test executable" ON)

if(BUILD_MODULE_TEMPLATE)
    if(NOT TARGET example_module)
        add_library(example_module MODULE foo.cpp)
        target_link_libraries(example_module PRIVATE loom::sdk)
        if(WIN32)
            set(_mod_suffix ".dll")
        else()
            set(_mod_suffix ".so")
        endif()
        set_target_properties(example_module PROPERTIES
            PREFIX ""
            SUFFIX "${_mod_suffix}"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/modules"
        )
    endif()
endif()

