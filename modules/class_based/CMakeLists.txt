add_library(class_based MODULE
    class_module.cpp
)

target_link_libraries(class_based PRIVATE
    loom::sdk
)

# Module output settings
set_target_properties(class_based PROPERTIES
    PREFIX ""                              # No "lib" prefix
    SUFFIX "${LOOM_MODULE_SUFFIX}"  # .dll on Windows, .so elsewhere
    LIBRARY_OUTPUT_DIRECTORY "${LOOM_MODULES_OUTPUT_DIR}"
    CXX_VISIBILITY_PRESET hidden           # Hide symbols by default
)

target_include_directories(class_based PUBLIC
    ${LOOM_MODULES_DIR}
)
