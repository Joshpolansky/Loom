add_executable(loom_tests
    test_module_loader.cpp
    test_data_engine.cpp
    test_data_store.cpp
    test_bus.cpp
    test_trace_cache.cpp
    test_tag_table.cpp
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
)

include(GoogleTest)
gtest_discover_tests(loom_tests
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Ensure example_motor is built before tests (tests load it as a plugin)
add_dependencies(loom_tests example_motor)

# Tell tests where modules are built
target_compile_definitions(loom_tests PRIVATE
    LOOM_MODULE_DIR="${CMAKE_BINARY_DIR}/modules"
)
