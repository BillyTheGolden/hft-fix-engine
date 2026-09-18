cmake_minimum_required(VERSION 3.20)
project(hft_monitoring LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Qt6 modules
find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ------------------------------------------------------------------------------
# Google Protocol Buffers Configuration (Standalone or Subdirectory)
# ------------------------------------------------------------------------------
if(NOT TARGET hft_telemetry_proto)
    set(protobuf_MODULE_COMPATIBLE ON)
    find_package(Protobuf CONFIG REQUIRED)

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/proto/telemetry.proto")
        set(MONITORING_PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
    else()
        set(MONITORING_PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../proto")
    endif()

    set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
    file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

    # Locate protoc
    if(TARGET protobuf::protoc)
        get_target_property(PROTOC_PATH protobuf::protoc IMPORTED_LOCATION_RELEASE)
        if(NOT PROTOC_PATH)
            get_target_property(PROTOC_PATH protobuf::protoc IMPORTED_LOCATION)
        endif()
    endif()
    if(NOT PROTOC_PATH)
        set(PROTOC_PATH ${Protobuf_PROTOC_EXECUTABLE})
    endif()
    if(NOT PROTOC_PATH)
        find_program(PROTOC_PATH protoc REQUIRED)
    endif()

    # Pre-generate telemetry.pb.h at configure-time for Qt Creator / Clangd / IDEs
    execute_process(
        COMMAND ${PROTOC_PATH}
                --proto_path=${MONITORING_PROTO_DIR}
                --cpp_out=${PROTO_GEN_DIR}
                ${MONITORING_PROTO_DIR}/telemetry.proto
        RESULT_VARIABLE PROTOC_RES
    )
    if(PROTOC_RES EQUAL 0)
        message(STATUS "[monitoring_app] Protobuf telemetry bindings pre-generated in: ${PROTO_GEN_DIR}")
        # Also generate into binary dir and proto dir
        execute_process(
            COMMAND ${PROTOC_PATH}
                    --proto_path=${MONITORING_PROTO_DIR}
                    --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
                    ${MONITORING_PROTO_DIR}/telemetry.proto
        )
        execute_process(
            COMMAND ${PROTOC_PATH}
                    --proto_path=${MONITORING_PROTO_DIR}
                    --cpp_out=${MONITORING_PROTO_DIR}
                    ${MONITORING_PROTO_DIR}/telemetry.proto
        )
    endif()

    # Build-time incremental generation rule
    protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${MONITORING_PROTO_DIR}/telemetry.proto)

    add_library(hft_telemetry_proto STATIC ${PROTO_SRCS} ${PROTO_HDRS})
    target_include_directories(hft_telemetry_proto PUBLIC
        ${Protobuf_INCLUDE_DIRS}
        ${CMAKE_CURRENT_BINARY_DIR}
        ${PROTO_GEN_DIR}
        ${MONITORING_PROTO_DIR}
    )
    target_link_libraries(hft_telemetry_proto PUBLIC protobuf::libprotobuf)
    target_compile_options(hft_telemetry_proto PRIVATE -frtti)
endif()

set(MONITORING_SOURCES
    src/main.cpp
    qml.qrc
)

add_executable(hft_monitoring ${MONITORING_SOURCES}
    include/TelemetryDataPoint.hpp
    include/TelemetrySession.hpp
    src/TelemetrySession.cpp
    include/TelemetryViewModel.hpp
    include/TelemetryReceiver.hpp
    src/TelemetryReceiver.cpp)

target_include_directories(hft_monitoring PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/proto
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../proto
    ${CMAKE_CURRENT_BINARY_DIR}
    ${PROTO_GEN_DIR}
)

target_compile_options(hft_monitoring PRIVATE -Wall -Wextra -Wpedantic -frtti)

target_link_libraries(hft_monitoring PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    hft_telemetry_proto
)

set_target_properties(hft_monitoring PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin
)
