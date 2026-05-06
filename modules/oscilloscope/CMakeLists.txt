add_library(oscilloscope MODULE
    oscilloscope.cpp
)

target_link_libraries(oscilloscope PRIVATE
    loom::sdk
)

target_include_directories(oscilloscope PUBLIC
    ${LOOM_MODULES_DIR}
)

set_target_properties(oscilloscope PROPERTIES
    PREFIX ""
    SUFFIX "${LOOM_MODULE_SUFFIX}"
    LIBRARY_OUTPUT_DIRECTORY "${LOOM_MODULES_OUTPUT_DIR}"
    CXX_VISIBILITY_PRESET hidden
)
