cmake_minimum_required(VERSION 3.29)

project(spz
    VERSION 1.0.0
    LANGUAGES CXX
    DESCRIPTION "SPZ Library and Tools"
)

# Build options
option(BUILD_SHARED_LIBS "Build shared libraries" ON)
option(BUILD_PYTHON_BINDINGS "Build Python bindings" ON)
option(USE_MARCH_NATIVE "Enable -march=native for release builds" OFF)
option(USE_STATIC_ZSTD "Build and link against static zstd library" ON)

# Debug options
option(ENABLE_ASAN "Enable Address Sanitizer" OFF)
option(ENABLE_UBSAN "Enable Undefined Behavior Sanitizer" OFF)

# Core settings
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Include external content manager
include(FetchContent)
include(CheckIPOSupported)

# Python bindings setup
if(BUILD_PYTHON_BINDINGS)
    find_package(Python COMPONENTS Interpreter Development REQUIRED)

    FetchContent_Declare(
        pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG        v2.13.6
    )
    FetchContent_MakeAvailable(pybind11)
endif()

# ZSTD setup
FetchContent_Declare(
    zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG v1.5.6
)
FetchContent_MakeAvailable(zstd)

# Configure ZSTD build options
set(ZSTD_MULTITHREAD ON CACHE BOOL "Enable multithreading in Zstd" FORCE)
if (${USE_STATIC_ZSTD})
  set(ZSTD_BUILD_STATIC ${USE_STATIC_ZSTD} CACHE BOOL "Build static Zstd library" FORCE)
elseif()
  set(ZSTD_BUILD_SHARED ON CACHE BOOL "Build shared Zstd library" FORCE)
endif()

# Add ZSTD CMake configuration
add_subdirectory(${CMAKE_CURRENT_BINARY_DIR}/_deps/zstd-src/build/cmake zstd)

# Set up ZSTD target alias based on linking preference
if(USE_STATIC_ZSTD)
    add_library(zstd::zstd ALIAS libzstd_static)
else()
    add_library(zstd::zstd ALIAS libzstd_shared)
endif()

# Check for LTO support
check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)

# Define source files
set(LIBRARY_SOURCES
    src/cc/load-spz.cc
    src/cc/splat-c-types.cc
    src/cc/splat-types.cc
)

set(EXECUTABLE_SOURCES
    src/cc/main.cc
)

# Shared library target
add_library(spz_shared SHARED ${LIBRARY_SOURCES})
target_compile_definitions(spz_shared PRIVATE SPZ_BUILDING_SHARED)
set_target_properties(spz_shared PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME "spz"
)

# Static library target
add_library(spz_static STATIC ${LIBRARY_SOURCES})
target_compile_definitions(spz_static
    PRIVATE
        SPZ_BUILDING_STATIC
)
set_target_properties(spz_static PROPERTIES
    OUTPUT_NAME "spz"
    VERSION ${PROJECT_VERSION}
    POSITION_INDEPENDENT_CODE ON
)

# Common include directories for both libraries
foreach(target spz_shared spz_static)
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            src/cc
    )
    target_link_libraries(${target} PRIVATE zstd::zstd)
endforeach()

# Executable target
add_executable(spz_exe ${EXECUTABLE_SOURCES})
set_target_properties(spz_exe PROPERTIES OUTPUT_NAME "spz")
target_include_directories(spz_exe PRIVATE src src/cc)
target_link_libraries(spz_exe PRIVATE spz_static zstd::zstd)

# Sanitizer configuration
if(ENABLE_ASAN OR ENABLE_UBSAN)
    set(SANITIZER_FLAGS "")

    if(ENABLE_ASAN)
        message(STATUS "Address Sanitizer (ASAN) enabled for executable")
        list(APPEND SANITIZER_FLAGS "-fsanitize=address")
    endif()

    if(ENABLE_UBSAN)
        message(STATUS "Undefined Behavior Sanitizer (UBSAN) enabled for executable")
        list(APPEND SANITIZER_FLAGS "-fsanitize=undefined")
    endif()

    target_compile_options(spz_exe PRIVATE ${SANITIZER_FLAGS})
    target_link_options(spz_exe PRIVATE ${SANITIZER_FLAGS})
endif()

# Python bindings configuration
if(BUILD_PYTHON_BINDINGS)
    pybind11_add_module(pyspz src/cc/python/bindings.cc)
    target_include_directories(pyspz PRIVATE src src/cc)
    target_link_libraries(pyspz PRIVATE spz_static zstd::zstd)

    set_target_properties(pyspz PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/pyspz"
        PREFIX ""
        SUFFIX "${PYTHON_MODULE_EXTENSION}"
    )
endif()

# Release build optimizations
if(CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo")
    # Enable LTO if supported
    if(IPO_SUPPORTED)
        message(STATUS "Enabling Link Time Optimization (LTO)")
        foreach(target spz_shared spz_static spz_exe)
            set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        endforeach()
    else()
        message(STATUS "LTO not supported: ${IPO_ERROR}")
    endif()

    # Configure architecture-specific optimizations
    if(USE_MARCH_NATIVE)
        message(STATUS "Setting -march=native")
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -march=native")
    else()
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "86")
            # For x86/x64 processors
            if(NOT CMAKE_CXX_FLAGS_RELEASE MATCHES "march=")
                set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -march=x86-64-v3")
            endif()
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm" OR CMAKE_SYSTEM_PROCESSOR MATCHES "aarch")
            # For ARM processors
            if(NOT CMAKE_CXX_FLAGS_RELEASE MATCHES "march=")
                set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -march=armv8.2-a")
            endif()
        else()
            message(WARNING "Unsupported architecture: -march option not set.")
        endif()
    endif()

    # Generate build information
    file(WRITE ${CMAKE_BINARY_DIR}/build_info.txt
        "Build Configuration:\n"
        "CMake Version: ${CMAKE_VERSION}\n"
        "Build Type: ${CMAKE_BUILD_TYPE}\n"
        "\n"
        "Compiler Information:\n"
        "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\n"
        "Compiler Path: ${CMAKE_CXX_COMPILER}\n"
        "CMAKE_CXX_FLAGS_RELEASE: ${CMAKE_CXX_FLAGS_RELEASE}\n"
        "\n"
        "Linker Information:\n"
        "Linker: ${CMAKE_LINKER}\n"
        "Linker Flags: ${CMAKE_EXE_LINKER_FLAGS}\n"
        "Static ZSTD: ${USE_STATIC_ZSTD}\n"
        "LTO Enabled: ${IPO_SUPPORTED}\n"
        "\n"
    )
endif()

# Installation configuration
install(TARGETS spz_shared spz_static
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

if(BUILD_PYTHON_BINDINGS)
    install(TARGETS pyspz
        LIBRARY DESTINATION .
    )
endif()
