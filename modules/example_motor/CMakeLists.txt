add_library(example_motor MODULE
    motor_module.cpp
)

target_link_libraries(example_motor PRIVATE
    loom::sdk
)
target_include_directories(example_motor PUBLIC
        ${LOOM_MODULES_DIR}
)

# Module output settings
set_target_properties(example_motor PROPERTIES
    PREFIX ""                              # No "lib" prefix
    SUFFIX "${LOOM_MODULE_SUFFIX}"  # .dll on Windows, .so elsewhere
    LIBRARY_OUTPUT_DIRECTORY "${LOOM_MODULES_OUTPUT_DIR}"
    CXX_VISIBILITY_PRESET hidden           # Hide symbols by default
)

