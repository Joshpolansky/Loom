# LoomModule.cmake — helpers for building Loom module plugins with the debug
# info needed for in-process crash symbolization.
#
# Installed with the SDK and pulled in by loomConfig.cmake, so module authors who
# `find_package(loom CONFIG REQUIRED)` get loom_add_module() with symbols handled
# the same way the bundled modules are. Also included by the in-repo build.
#
# Crash reports are symbolized in-process via cpptrace, which needs debug info:
#   • Windows : a .pdb next to the binary (/Zi + /DEBUG, kept lean with /OPT)
#   • Linux   : DWARF embedded in the unstripped binary (-g)
#   • macOS   : a .dSYM bundle next to the binary (dsymutil, since the linker
#               leaves DWARF in the .o files which don't ship)

if(NOT DEFINED LOOM_WITH_DEBUG_INFO)
    option(LOOM_WITH_DEBUG_INFO "Emit debug info (PDB/DWARF/dSYM) for crash symbolization" ON)
endif()

# Platform-appropriate module suffix (.dll on Windows, .so elsewhere — matching
# the runtime's dlopen/LoadLibrary lookup). Honors a value already set by the
# in-repo build.
if(NOT DEFINED LOOM_MODULE_SUFFIX)
    if(WIN32)
        set(LOOM_MODULE_SUFFIX ".dll")
    else()
        set(LOOM_MODULE_SUFFIX ".so")
    endif()
endif()

# Add debug-info flags to a target for optimized (non-Debug) configs. Debug
# already carries full debug info. No-op when LOOM_WITH_DEBUG_INFO is OFF.
function(loom_target_debug_info target)
    if(NOT LOOM_WITH_DEBUG_INFO)
        return()
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE $<$<NOT:$<CONFIG:Debug>>:/Zi>)
        # /DEBUG so a PDB is emitted; /OPT:REF,ICF to keep the optimized image
        # lean (the linker disables those by default once /DEBUG is present).
        target_link_options(${target} PRIVATE
            $<$<NOT:$<CONFIG:Debug>>:/DEBUG>
            $<$<NOT:$<CONFIG:Debug>>:/OPT:REF>
            $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>)
    else()
        target_compile_options(${target} PRIVATE $<$<NOT:$<CONFIG:Debug>>:-g>)
    endif()
endfunction()

# On macOS, produce a <binary>.dSYM bundle next to the target after build so
# cpptrace can resolve file:line from a distributed (away-from-build-tree) binary.
function(loom_target_dsym target)
    if(APPLE AND LOOM_WITH_DEBUG_INFO)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND dsymutil $<TARGET_FILE:${target}> -o $<TARGET_FILE:${target}>.dSYM
            COMMENT "dsymutil ${target} → .dSYM (crash symbols)"
            VERBATIM)
    endif()
endfunction()

# loom_add_module(<name>
#     SOURCES  <src...>
#     [LINK    <libs...>]
#     [INCLUDE <dirs...>]
#     [OUTPUT_DIRECTORY <dir>])
#
# Builds a Loom module plugin: a MODULE library with no "lib" prefix, the
# platform module suffix, hidden default visibility, linked against loom::sdk,
# and with crash-symbolization debug info (PDB/DWARF/dSYM) emitted next to it.
function(loom_add_module name)
    cmake_parse_arguments(ARG "" "OUTPUT_DIRECTORY" "SOURCES;LINK;INCLUDE" ${ARGN})
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "loom_add_module(${name}): SOURCES is required")
    endif()

    add_library(${name} MODULE ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE loom::sdk ${ARG_LINK})
    if(ARG_INCLUDE)
        target_include_directories(${name} PRIVATE ${ARG_INCLUDE})
    endif()

    set_target_properties(${name} PROPERTIES
        PREFIX ""
        SUFFIX "${LOOM_MODULE_SUFFIX}"
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)

    if(ARG_OUTPUT_DIRECTORY)
        set_target_properties(${name} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${ARG_OUTPUT_DIRECTORY}")
    elseif(DEFINED LOOM_MODULES_OUTPUT_DIR)
        set_target_properties(${name} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${LOOM_MODULES_OUTPUT_DIR}")
    endif()

    loom_target_debug_info(${name})
    loom_target_dsym(${name})
endfunction()
