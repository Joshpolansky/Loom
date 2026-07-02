add_library(crasher MODULE
    crasher.cpp
)

target_link_libraries(crasher PRIVATE
    loom::sdk
)
target_include_directories(crasher PUBLIC
        ${LOOM_MODULES_DIR}
)

set_target_properties(crasher PROPERTIES
    PREFIX ""
    SUFFIX "${LOOM_MODULE_SUFFIX}"
    LIBRARY_OUTPUT_DIRECTORY "${LOOM_MODULES_OUTPUT_DIR}"
    CXX_VISIBILITY_PRESET hidden
)
