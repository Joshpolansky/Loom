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
# CMakeLists (LOOM_WITH_DEBUG_INFO): a .pdb on Windows, embedded DWARF on Linux.
# macOS .dSYM bundles are produced for the distributed binaries in the CI staging
# step (dsymutil) — not here, because add_custom_command(TARGET) can't attach to
# a target created in a module subdirectory. Local macOS dev resolves via the
# debug map in the build tree, so no .dSYM is needed for development.
