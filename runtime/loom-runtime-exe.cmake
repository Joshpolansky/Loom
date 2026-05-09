# Included by CMakeDeps via cmake_build_modules.
# Sets LOOM_RUNTIME_EXECUTABLE as a CMake cache variable pointing at the
# packaged runtime binary. Consumers can reference it in launch.json as:
#   "program": "${cmake:LOOM_RUNTIME_EXECUTABLE}"
get_filename_component(_loom_pkg_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if(CMAKE_HOST_WIN32)
    set(_loom_exe "${_loom_pkg_root}/bin/loom.exe")
else()
    set(_loom_exe "${_loom_pkg_root}/bin/loom")
endif()

if(EXISTS "${_loom_exe}")
    set(LOOM_RUNTIME_EXECUTABLE "${_loom_exe}" CACHE FILEPATH
        "Path to the loom runtime executable (set by loom-runtime Conan package)" FORCE)
    message(STATUS "Loom runtime: ${LOOM_RUNTIME_EXECUTABLE}")
else()
    message(WARNING "Loom runtime executable not found at ${_loom_exe}")
endif()

unset(_loom_exe)
unset(_loom_pkg_root)
