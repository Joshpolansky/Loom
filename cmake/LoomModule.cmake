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

# Target-scoped half of Emscripten wasm SIDE_MODULE support (see
# loom_add_module below for the GLOBAL-property half, which — unlike these —
# MUST be set before add_library() runs to take effect for that target):
#   1. -sSIDE_MODULE=1 (compile + link) is what actually produces a dlopen-able
#      wasm binary (a `dylink.0` section) instead of a normal static archive.
#   2. -fexceptions is required so real C++ exceptions work — the module ABI
#      boundary (dlopen'd module <-> the loom_wasm MAIN_MODULE host) needs
#      matching exception support on both sides, and every Loom module's SDK
#      surface (guard(), command dispatch) relies on real exception unwinding.
#      De-risked standalone (cross-module throw/catch across a real worker
#      thread) in the loom repo's spike/phaseC-pthread-dlopen/.
#   3. FMT_CONSTEVAL= neutralizes fmt's consteval format-string checks, which
#      emcc's clang rejects; harmless if a module never includes spdlog/fmt.
# All apply per-module (not globally), so a consumer's own CMakeLists.txt needs
# zero Emscripten-specific configuration for modules built via loom_add_module.
function(loom_target_wasm_module_support target)
    if(NOT EMSCRIPTEN)
        return()
    endif()
    target_compile_options(${target} PRIVATE -sSIDE_MODULE=1 -fexceptions)
    target_link_options(${target} PRIVATE -sSIDE_MODULE=1 -fexceptions)
    target_compile_definitions(${target} PRIVATE FMT_CONSTEVAL=)
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
# On Emscripten, produces a dlopen-able wasm SIDE_MODULE instead (see
# loom_target_wasm_module_support above) — no extra configuration needed by
# the caller; the SAME loom_add_module() call works for native and wasm.
function(loom_add_module name)
    cmake_parse_arguments(ARG "" "OUTPUT_DIRECTORY" "SOURCES;LINK;INCLUDE" ${ARGN})
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "loom_add_module(${name}): SOURCES is required")
    endif()

    # Must run BEFORE add_library(MODULE): TARGET_SUPPORTS_SHARED_LIBS is read
    # at add_library() time, so setting it any later has no effect on this call.
    if(EMSCRIPTEN)
        set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS TRUE)
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
    loom_target_wasm_module_support(${name})
endfunction()
