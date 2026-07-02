set(LOOM_MODULES_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if(EMSCRIPTEN)
    # Separate output dir: CMAKE_RUNTIME_OUTPUT_DIRECTORY is an absolute path
    # outside the build tree (shared across build presets), so a wasm SIDE_MODULE
    # named e.g. class_based.so would otherwise overwrite the native .so of the
    # same name.
    set(LOOM_MODULES_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/wasm-modules")
else()
    set(LOOM_MODULES_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/modules")
endif()

# Build all example modules. EtherCAT needs SOEM + raw-Ethernet access
# (CAP_NET_RAW / libpcap) that doesn't exist under Emscripten, so it's native-only.
add_subdirectory(class_based)
add_subdirectory(command_probe)
add_subdirectory(crasher)
if(NOT EMSCRIPTEN)
    add_subdirectory(ethercat)
endif()
add_subdirectory(example_motor)
add_subdirectory(oscilloscope)
add_subdirectory(pneumatic_actuator)
add_subdirectory(sequencer)
add_subdirectory(stack_light)

if(EMSCRIPTEN)
    set(LOOM_WASM_MODULE_TARGETS class_based command_probe crasher example_motor
                                  oscilloscope pneumatic_actuator sequencer stack_light)

    # Each module above is `add_library(<name> MODULE ...)`; under Emscripten a
    # MODULE library needs -sSIDE_MODULE=1 (compile and link) to produce a
    # dlopen-able wasm side module instead of a native-style shared object.
    foreach(_mod IN LISTS LOOM_WASM_MODULE_TARGETS)
        target_compile_options(${_mod} PRIVATE -sSIDE_MODULE=1)
        target_link_options(${_mod} PRIVATE -sSIDE_MODULE=1)
    endforeach()

    # Stage straight into the frontend's static assets on every build — `cmake
    # --build` alone is then enough, no manual cp. A plain add_custom_target (not
    # add_custom_command(TARGET ... POST_BUILD)) because POST_BUILD must be added
    # in the same directory that created the target; each module target above was
    # created in its own add_subdirectory(), not here.
    if(EXISTS "${CMAKE_SOURCE_DIR}/frontend")
        set(_wasm_module_files "")
        foreach(_mod IN LISTS LOOM_WASM_MODULE_TARGETS)
            list(APPEND _wasm_module_files "$<TARGET_FILE:${_mod}>")
        endforeach()
        add_custom_target(stage_wasm_modules ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/frontend/public"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_wasm_module_files}
                "${CMAKE_SOURCE_DIR}/frontend/public/"
            DEPENDS ${LOOM_WASM_MODULE_TARGETS}
            COMMENT "Staging wasm modules into frontend/public/"
            VERBATIM
        )
    endif()
endif()

# Debug info for these modules comes from the global flags in the root
# CMakeLists (LOOM_WITH_DEBUG_INFO): a .pdb on Windows, embedded DWARF on Linux.
# macOS .dSYM bundles are produced for the distributed binaries in the CI staging
# step (dsymutil) — not here, because add_custom_command(TARGET) can't attach to
# a target created in a module subdirectory. Local macOS dev resolves via the
# debug map in the build tree, so no .dSYM is needed for development.
