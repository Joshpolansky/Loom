add_library(stack_light MODULE
    stack_light.cpp
)

target_link_libraries(stack_light PRIVATE
    loom::sdk
)

target_include_directories(stack_light PUBLIC
    ${LOOM_MODULES_DIR}
)

set_target_properties(stack_light PROPERTIES
    PREFIX ""
    SUFFIX "${LOOM_MODULE_SUFFIX}"  # .dll on Windows, .so elsewhere
    LIBRARY_OUTPUT_DIRECTORY "${LOOM_MODULES_OUTPUT_DIR}"
    CXX_VISIBILITY_PRESET hidden
)
