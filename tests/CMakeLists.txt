add_executable(loom_tests
    test_module_loader.cpp
    test_data_engine.cpp
    test_data_store.cpp
    test_bus.cpp
    test_trace_cache.cpp
    test_tag_table.cpp
    test_runtime_heap.cpp
    test_command_integration.cpp
    test_command_fb.cpp
    test_port.cpp
    test_diag.cpp
)

target_include_directories(loom_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/runtime/include
)

target_link_libraries(loom_tests PRIVATE
    loom::sdk
    spdlog::spdlog
    GTest::gtest
    GTest::gtest_main
    ${CMAKE_DL_LIBS}
)

# Link against runtime sources (not the executable, just the .cpp files)
target_sources(loom_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/runtime/src/module_loader.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/data_engine.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/data_store.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/io_mapper.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/scheduler.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/scheduler_config.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/runtime_heap.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/diag/breadcrumb.cpp  # scheduler.cpp references diag::tlsBreadcrumb
)

include(GoogleTest)
gtest_discover_tests(loom_tests
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Ensure the plugin modules the tests load are built first.
add_dependencies(loom_tests example_motor command_probe)

# Tell tests where modules are built and the platform plugin suffix.
target_compile_definitions(loom_tests PRIVATE
    LOOM_MODULE_DIR="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/modules"
    LOOM_MODULE_SUFFIX="${LOOM_MODULE_SUFFIX}"
)
