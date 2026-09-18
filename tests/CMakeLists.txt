find_package(GTest REQUIRED)

set(TEST_SOURCES
    test_telemetry_proto.cpp
)

add_executable(hft_unit_tests ${TEST_SOURCES})
target_compile_options(hft_unit_tests PRIVATE -Wall -Wextra -Wpedantic -frtti)
target_include_directories(hft_unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/proto
    ${CMAKE_BINARY_DIR}
    ${PROTO_GEN_DIR}
)
target_link_libraries(hft_unit_tests PRIVATE
    GTest::gtest
    GTest::gtest_main
    hft_telemetry_proto
    Threads::Threads
)

include(GoogleTest)
gtest_discover_tests(hft_unit_tests)
