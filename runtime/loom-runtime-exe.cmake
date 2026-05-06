# Included by CMakeDeps via cmake_build_modules.
# Sets LOOM_RUNTIME_EXECUTABLE as a CMake cache variable pointing at the
# packaged runtime binary. Consumers can reference it in launch.json as:
#   "program": "${cmake:LOOM_RUNTIME_EXECUTABLE}"
get_filename_component(_crt_pkg_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if(CMAKE_HOST_WIN32)
    set(_crt_exe "${_crt_pkg_root}/bin/loom.exe")
else()
    set(_crt_exe "${_crt_pkg_root}/bin/loom")
endif()

if(EXISTS "${_crt_exe}")
    set(LOOM_RUNTIME_EXECUTABLE "${_crt_exe}" CACHE FILEPATH
        "Path to the loom runtime executable (set by loom-runtime Conan package)" FORCE)
    message(STATUS "Loom runtime: ${LOOM_RUNTIME_EXECUTABLE}")
else()
    message(WARNING "Loom runtime executable not found at ${_crt_exe}")
endif()

unset(_crt_exe)
unset(_crt_pkg_root)
