cmake_minimum_required(VERSION 3.20)
project(HftFixEngine 
        VERSION 1.0.0 
        DESCRIPTION "Low-Latency C++20 FIX Engine with Zero-Copy PACKET_MMAP RX Ring Bypass" 
        LANGUAGES CXX)

# Require C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for LSP/IDE integration
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set build output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)

# Set default build type to Release if not specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "No CMAKE_BUILD_TYPE specified. Defaulting to 'Release'.")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build, options are: Debug Release RelWithDebInfo MinSizeRel." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

message(STATUS "Configuring for Build Type: ${CMAKE_BUILD_TYPE}")

# Base flags for all build configurations
add_compile_options(-fno-rtti)

# Per-build-type optimization and debugging flags
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -funroll-loops -fomit-frame-pointer -fdata-sections -ffunction-sections -fvisibility=hidden -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O3 -g -march=native -funroll-loops -fno-omit-frame-pointer -fdata-sections -ffunction-sections -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG -g3 -fno-omit-frame-pointer -fno-inline")
set(CMAKE_CXX_FLAGS_MINSIZEREL "-Os -march=native -fdata-sections -ffunction-sections -DNDEBUG")

# Option to enable or disable NUMA library support (default ON)
option(ENABLE_NUMA "Enable NUMA library support for thread affinity and memory allocation" ON)

# Option to enable or disable AF_XDP driver support (default ON)
option(HFT_ENABLE_AF_XDP "Enable Linux AF_XDP (XDP Sockets) driver support" ON)

# Try to find log4cplus on the host system first
find_package(log4cplus QUIET)

if(NOT log4cplus_FOUND)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(log4cplus_pc QUIET IMPORTED_TARGET log4cplus)
        if(log4cplus_pc_FOUND)
            message(STATUS "Found log4cplus on host system via pkg-config.")
            add_library(log4cplus::log4cplus INTERFACE IMPORTED GLOBAL)
            set_target_properties(log4cplus::log4cplus PROPERTIES
                INTERFACE_LINK_LIBRARIES PkgConfig::log4cplus_pc
            )
            set(log4cplus_FOUND TRUE)
        endif()
    endif()
endif()

if(NOT log4cplus_FOUND)
    message(STATUS "log4cplus not found on host system. Fetching from Git...")
    include(FetchContent)
    FetchContent_Declare(
        log4cplus
        GIT_REPOSITORY https://github.com/log4cplus/log4cplus.git
        GIT_TAG        REL_2_1_1
    )
    FetchContent_MakeAvailable(log4cplus)

    if(NOT TARGET log4cplus::log4cplus)
        add_library(log4cplus::log4cplus ALIAS log4cplus)
    endif()
else()
    message(STATUS "Found log4cplus on host system.")
endif()

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Find threading support
find_package(Threads REQUIRED)

if(ENABLE_NUMA)
    find_path(NUMA_INCLUDE_DIR NAMES numa.h)
    find_library(NUMA_LIBRARY NAMES numa)

    if(NUMA_INCLUDE_DIR AND NUMA_LIBRARY)
        set(NUMA_FOUND TRUE)
    else()
        set(NUMA_FOUND FALSE)
    endif()

    if(NOT NUMA_FOUND)
        message(STATUS "libnuma not found on host system. Fetching numactl from Git...")
        include(ExternalProject)
        set(NUMA_INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/numa_install)
        ExternalProject_Add(
            numactl_external
            GIT_REPOSITORY https://github.com/numactl/numactl.git
            GIT_TAG        v2.0.16
            CONFIGURE_COMMAND <SOURCE_DIR>/autogen.sh && <SOURCE_DIR>/configure --prefix=${NUMA_INSTALL_DIR} --disable-shared
            BUILD_IN_SOURCE 1
            BUILD_COMMAND   make
            INSTALL_COMMAND make install
            BUILD_BYPRODUCTS ${NUMA_INSTALL_DIR}/lib/libnuma.a
        )

        # Create imported target for NUMA
        add_library(numa::numa STATIC IMPORTED GLOBAL)
        set_target_properties(numa::numa PROPERTIES
            IMPORTED_LOCATION "${NUMA_INSTALL_DIR}/lib/libnuma.a"
            INTERFACE_INCLUDE_DIRECTORIES "${NUMA_INSTALL_DIR}/include"
        )
        add_dependencies(numa::numa numactl_external)
    else()
        message(STATUS "Found libnuma on host system.")
        add_library(numa::numa UNKNOWN IMPORTED GLOBAL)
        set_target_properties(numa::numa PROPERTIES
            IMPORTED_LOCATION "${NUMA_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${NUMA_INCLUDE_DIR}"
        )
    endif()
else()
    message(STATUS "NUMA support is DISABLED by user option (ENABLE_NUMA=OFF). Thread affinity will use POSIX/C++20 native handle solution.")
endif()

# AF_XDP Library Detection (libxdp / libbpf)
if(HFT_ENABLE_AF_XDP)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(LIBXDP QUIET libxdp)
        pkg_check_modules(LIBBPF QUIET libbpf)
    endif()

    if(LIBXDP_FOUND AND LIBBPF_FOUND)
        message(STATUS "Found libxdp and libbpf on host system.")
        add_definitions(-DHFT_ENABLE_AF_XDP -DHFT_HAS_LIBXDP)
        include_directories(${LIBXDP_INCLUDE_DIRS} ${LIBBPF_INCLUDE_DIRS})
        link_libraries(${LIBXDP_LIBRARIES} ${LIBBPF_LIBRARIES})
    else()
        message(STATUS "libxdp or libbpf not found via pkg-config. Checking standard headers...")
        find_path(XDP_INCLUDE_DIR NAMES xsk.h PATH_SUFFIXES xdp)
        find_library(XDP_LIBRARY NAMES xdp)
        if(XDP_INCLUDE_DIR AND XDP_LIBRARY)
            message(STATUS "Found xsk.h and libxdp on system.")
            add_definitions(-DHFT_ENABLE_AF_XDP -DHFT_HAS_LIBXDP)
            include_directories(${XDP_INCLUDE_DIR})
            link_libraries(${XDP_LIBRARY} bpf)
        else()
            message(STATUS "AF_XDP headers/libraries not present. Compiling with Generic AF_PACKET Ring Buffer.")
            add_definitions(-DHFT_ENABLE_AF_XDP)
        endif()
    endif()
endif()

# Collect engine source files excluding main entry points
file(GLOB_RECURSE ENGINE_SOURCES
     "src/common/*.cpp"
     "src/protocol/*.cpp"
     "src/networking/*.cpp"
     "src/worker/*.cpp"
     "src/matching/*.cpp"
     "src/monitoring/*.cpp"
     "src/arena/*.cpp"
     "src/order_book_engine/*.cpp"
)

# Main HFT Engine Executable
add_executable(hft_fix_engine ${ENGINE_SOURCES} "src/main.cpp")
target_compile_options(hft_fix_engine PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
target_link_options(hft_fix_engine PRIVATE -Wl,-z,common-page-size=2097152 -Wl,-z,max-page-size=2097152)

# Resilience Test Arena Executable
add_executable(test_arena ${ENGINE_SOURCES} "src/test_arena_main.cpp")
target_compile_options(test_arena PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

# HFT Order Matching Engine Executable
add_executable(hft_matching_engine ${ENGINE_SOURCES} "src/matching_main.cpp")
target_compile_options(hft_matching_engine PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
target_link_options(hft_matching_engine PRIVATE -Wl,-z,common-page-size=2097152 -Wl,-z,max-page-size=2097152)

# Multi-Protocol Benchmark Comparison Arena Executable
add_executable(protocol_arena ${ENGINE_SOURCES} "src/protocol_arena_main.cpp")
target_compile_options(protocol_arena PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

# Capstone C++20 HFT Order Book Engine Executable
add_executable(hft_order_book_engine ${ENGINE_SOURCES} "src/order_book_engine_main.cpp")
target_compile_options(hft_order_book_engine PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
target_link_options(hft_order_book_engine PRIVATE -Wl,-z,common-page-size=2097152 -Wl,-z,max-page-size=2097152)

# Google Protocol Buffers Configuration
set(protobuf_MODULE_COMPATIBLE ON)
find_package(Protobuf CONFIG REQUIRED)
set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS proto/telemetry.proto)

add_library(hft_telemetry_proto STATIC ${PROTO_SRCS} ${PROTO_HDRS})
target_include_directories(hft_telemetry_proto PUBLIC 
    ${Protobuf_INCLUDE_DIRS}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${PROTO_GEN_DIR}
)
target_link_libraries(hft_telemetry_proto PUBLIC protobuf::libprotobuf)
target_compile_options(hft_telemetry_proto PRIVATE -frtti)

# Automated Google Test / Mock Executable
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/hackerrank_simulator_gtest.cpp")
    find_package(GTest QUIET)
    if(GTEST_FOUND)
        add_executable(hackerrank_gtest ${ENGINE_SOURCES} "src/hackerrank_simulator_gtest.cpp")
        target_compile_options(hackerrank_gtest PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
        target_link_libraries(hackerrank_gtest PRIVATE GTest::gtest GTest::gtest_main GTest::gmock hft_telemetry_proto)
        if(ENABLE_NUMA)
            target_compile_definitions(hackerrank_gtest PRIVATE HFT_ENABLE_NUMA)
            target_link_libraries(hackerrank_gtest PRIVATE numa::numa log4cplus::log4cplus Threads::Threads)
        else()
            target_link_libraries(hackerrank_gtest PRIVATE log4cplus::log4cplus Threads::Threads)
        endif()
    endif()
endif()

if(ENABLE_NUMA)
    target_compile_definitions(hft_fix_engine PRIVATE HFT_ENABLE_NUMA)
    target_compile_definitions(test_arena PRIVATE HFT_ENABLE_NUMA)
    target_compile_definitions(hft_matching_engine PRIVATE HFT_ENABLE_NUMA)
    target_compile_definitions(protocol_arena PRIVATE HFT_ENABLE_NUMA)
    target_compile_definitions(hft_order_book_engine PRIVATE HFT_ENABLE_NUMA)
    target_link_libraries(hft_fix_engine PRIVATE Threads::Threads numa::numa log4cplus::log4cplus hft_telemetry_proto)
    target_link_libraries(test_arena PRIVATE Threads::Threads numa::numa log4cplus::log4cplus hft_telemetry_proto)
    target_link_libraries(hft_matching_engine PRIVATE Threads::Threads numa::numa log4cplus::log4cplus hft_telemetry_proto)
    target_link_libraries(protocol_arena PRIVATE Threads::Threads numa::numa log4cplus::log4cplus hft_telemetry_proto)
    target_link_libraries(hft_order_book_engine PRIVATE Threads::Threads numa::numa log4cplus::log4cplus hft_telemetry_proto)
else()
    target_link_libraries(hft_fix_engine PRIVATE Threads::Threads log4cplus::log4cplus hft_telemetry_proto)
    target_link_libraries(test_arena PRIVATE Threads::Threads log4cplus::log4cplus hft_telemetry_proto)
    target_link_libraries(hft_matching_engine PRIVATE Threads::Threads log4cplus::log4cplus hft_telemetry_proto)
    target_link_libraries(protocol_arena PRIVATE Threads::Threads log4cplus::log4cplus hft_telemetry_proto)
    target_link_libraries(hft_order_book_engine PRIVATE Threads::Threads log4cplus::log4cplus hft_telemetry_proto)
endif()

# Enable Testing & Unit Tests Subdirectory
enable_testing()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

# Target for Doxygen documentation generation
find_package(Doxygen QUIET)
if(DOXYGEN_FOUND)
    add_custom_target(doc
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Generating Doxygen API documentation..."
        VERBATIM
    )
endif()
