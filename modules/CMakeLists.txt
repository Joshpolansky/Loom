set(LOOM_MODULES_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(LOOM_MODULES_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/modules")

# Build all example modules
add_subdirectory(class_based)
add_subdirectory(command_probe)
add_subdirectory(crasher)
add_subdirectory(ethercat)
add_subdirectory(example_motor)
add_subdirectory(oscilloscope)
add_subdirectory(pneumatic_actuator)
add_subdirectory(sequencer)
add_subdirectory(stack_light)

# Debug info for these modules comes from the global flags in the root
# CMakeLists (LOOM_WITH_DEBUG_INFO). On macOS, additionally emit a .dSYM next to
# each module binary so crash frames in module code resolve to file:line
# (no-op on Linux/Windows). Listed explicitly so we don't dsymutil third-party
# targets (e.g. SOEM under ethercat).
if(COMMAND loom_target_dsym)
    foreach(_loom_mod class_based command_probe crasher ethercat example_motor
                      oscilloscope pneumatic_actuator sequencer stack_light)
        if(TARGET ${_loom_mod})
            loom_target_dsym(${_loom_mod})
        endif()
    endforeach()
endif()
