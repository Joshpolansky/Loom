add_library(pneumatic_actuator MODULE
    pneumatic_actuator.cpp
)

target_link_libraries(pneumatic_actuator PRIVATE
    loom::sdk
)

target_include_directories(pneumatic_actuator PUBLIC
    ${LOOM_MODULES_DIR}
)

set_target_properties(pneumatic_actuator PROPERTIES
    PREFIX ""
    SUFFIX "${LOOM_MODULE_SUFFIX}"  # .dll on Windows, .so elsewhere
    LIBRARY_OUTPUT_DIRECTORY "${LOOM_MODULES_OUTPUT_DIR}"
    CXX_VISIBILITY_PRESET hidden
)
