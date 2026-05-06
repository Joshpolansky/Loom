# EtherCAT main controller module — self-contained, fetches SOEM from GitHub.
#
# Platform notes:
#   Linux (production): requires CAP_NET_RAW — run as root or:
#       sudo setcap cap_net_raw+ep <binary>
#   macOS (dev/testing): requires libpcap (brew install libpcap) and the user
#       in the access_bpf group:
#       sudo dseditgroup -o edit -a $USER -t user access_bpf

# SOEM has C sources — enable C before adding the subdirectory.
enable_language(C)

# ── SOEM (fetched from GitHub) ────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    soem
    GIT_REPOSITORY https://github.com/OpenEtherCATsociety/SOEM.git
    GIT_TAG        master          # pin to a release tag in production, e.g. v1.4.0
    GIT_SHALLOW    TRUE
)

# Use the old FetchContent_Populate form so we can inject macOS files before
# add_subdirectory. CMP0169 suppresses the deprecation warning on CMake 3.30+.
cmake_policy(SET CMP0169 OLD)
FetchContent_GetProperties(soem)
if(NOT soem_POPULATED)
    FetchContent_Populate(soem)
    if(APPLE)
        # SOEM's main branch has no macOS support. Inject our custom osal/oshw.
        file(COPY_FILE
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/soem-Darwin.cmake"
            "${soem_SOURCE_DIR}/cmake/Darwin.cmake"
            ONLY_IF_DIFFERENT
        )
        file(MAKE_DIRECTORY "${soem_SOURCE_DIR}/osal/macos")
        file(COPY_FILE
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/soem-macos-osal.c"
            "${soem_SOURCE_DIR}/osal/macos/osal.c"
            ONLY_IF_DIFFERENT
        )
        file(COPY_FILE
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/soem-macos-osal_defs.h"
            "${soem_SOURCE_DIR}/osal/macos/osal_defs.h"
            ONLY_IF_DIFFERENT
        )
    endif()
    set(SOEM_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory("${soem_SOURCE_DIR}" "${soem_BINARY_DIR}")
else()
    set(SOEM_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(soem)
endif()

# ── EtherCAT module ───────────────────────────────────────────────────────────
# main.cpp is compiled directly into the module — no separate library target.
add_library(ethercat MODULE
    ethercat.cpp
    main.cpp
)

target_include_directories(ethercat PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LOOM_MODULES_DIR}
)

target_link_libraries(ethercat PRIVATE
    loom::sdk
    soem
    glaze::glaze
)

set_target_properties(ethercat PROPERTIES
    OUTPUT_NAME  "ethercat"
    PREFIX       ""
    SUFFIX       "${LOOM_MODULE_SUFFIX}"
    LIBRARY_OUTPUT_DIRECTORY "${LOOM_MODULES_OUTPUT_DIR}"
    CXX_VISIBILITY_PRESET hidden
)
