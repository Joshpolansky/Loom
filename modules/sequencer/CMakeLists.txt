add_library(sequencer MODULE
    sequencer.cpp
)

target_link_libraries(sequencer PRIVATE
    loom::sdk
)

set_target_properties(sequencer PROPERTIES
    PREFIX ""
    SUFFIX "${LOOM_MODULE_SUFFIX}"  # .dll on Windows, .so elsewhere
    LIBRARY_OUTPUT_DIRECTORY "${LOOM_MODULES_OUTPUT_DIR}"
    CXX_VISIBILITY_PRESET hidden
)

target_include_directories(sequencer PUBLIC
    ${LOOM_MODULES_DIR}
)
