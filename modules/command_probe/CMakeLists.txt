add_library(command_probe MODULE
    command_probe.cpp
)

target_link_libraries(command_probe PRIVATE
    loom::sdk
)
target_include_directories(command_probe PUBLIC
        ${LOOM_MODULES_DIR}
)

set_target_properties(command_probe PROPERTIES
    PREFIX ""
    SUFFIX "${LOOM_MODULE_SUFFIX}"
    LIBRARY_OUTPUT_DIRECTORY "${LOOM_MODULES_OUTPUT_DIR}"
    CXX_VISIBILITY_PRESET hidden
)
