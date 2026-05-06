set(LOOM_MODULES_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(LOOM_MODULES_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/modules")

# Build all example modules
add_subdirectory(class_based)
add_subdirectory(ethercat)
add_subdirectory(example_motor)
add_subdirectory(oscilloscope)
add_subdirectory(pneumatic_actuator)
add_subdirectory(sequencer)
add_subdirectory(stack_light)
