cmake_minimum_required(VERSION 3.29)

project(spz
    VERSION 1.0.0
    LANGUAGES CXX
    DESCRIPTION "SPZ Library and Tools"
)

set(PYTHON_PACKAGE_BUILD $ENV{PYTHON_PACKAGE_BUILD} CACHE BOOL "" FORCE)

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

if(MSVC)
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|ARM64)")
        add_compile_options(/arch:AVX2)
    endif()

    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
    endif()
    add_compile_definitions(_USE_MATH_DEFINES)
endif()

# Determine if building for WebAssembly
set(IS_WASM OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    set(IS_WASM ON)
    message(STATUS "Configuring for WebAssembly using Emscripten")
    # Disable features that are not relevant for WebAssembly:
    set(BUILD_PYTHON_BINDINGS OFF CACHE BOOL "Disable Python bindings for wasm" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Prefer static linking for wasm" FORCE)
endif()

include(FetchContent)
include(CheckIPOSupported)

# Python bindings setup
if(BUILD_PYTHON_BINDINGS)
    find_package(Python3 COMPONENTS Interpreter Development.Module REQUIRED)

    FetchContent_Declare(
        pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG        v2.13.6
    )
    FetchContent_MakeAvailable(pybind11)
endif()

# Check for LTO (Link Time Optimization) support
check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)

# ZSTD setup via FetchContent
FetchContent_Declare(
    zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG v1.5.7
)
FetchContent_MakeAvailable(zstd)

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "BUILD PROGRAMS" FORCE)
# Configure ZSTD build options
if(IS_WASM)
  set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "Enable multithreading in Zstd" FORCE)
  message(STATUS "Disabling ZSTD multithreading for WASM")
else()
  set(ZSTD_MULTITHREAD_SUPPORT ON CACHE BOOL "Enable multithreading in Zstd" FORCE)
endif()

if(${USE_STATIC_ZSTD})
  set(ZSTD_BUILD_STATIC ${USE_STATIC_ZSTD} CACHE BOOL "Build static Zstd library" FORCE)
  set(ZSTD_BUILD_SHARED OFF CACHE BOOL "Build shared Zstd library" FORCE)
else()
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

set(LIBRARY_SOURCES
    src/cc/load-spz.cc
    src/cc/splat-c-types.cc
    src/cc/splat-types.cc
    src/cc/shared.cc
)

set(EXECUTABLE_SOURCES
    src/cc/main.cc
)

# Shared library target
if(NOT IS_WASM)
    add_library(spz_shared SHARED ${LIBRARY_SOURCES})
    set_target_properties(spz_shared PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )
endif()

# Static library target
add_library(spz_static STATIC ${LIBRARY_SOURCES})
set_target_properties(spz_static PROPERTIES
    VERSION ${PROJECT_VERSION}
    POSITION_INDEPENDENT_CODE ON
)

# Common include directories for both libraries
if(NOT IS_WASM)
    set(SPZ_LIBS spz_shared spz_static)
else()
    set(SPZ_LIBS spz_static)
endif()

foreach(target ${SPZ_LIBS})
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            src/cc
    )
    target_link_libraries(${target} PRIVATE zstd::zstd)
endforeach()

# Executable target (skipped for Emscripten builds)
if(NOT IS_WASM)
    add_executable(spz_exe ${EXECUTABLE_SOURCES})
    set_target_properties(spz_exe PROPERTIES OUTPUT_NAME "spz")
    target_include_directories(spz_exe PRIVATE src src/cc)
    target_link_libraries(spz_exe PRIVATE spz_static zstd::zstd)

    # Sanitizer configuration (only for traditional builds)
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
else()
    message(STATUS "Skipping executable target 'spz_exe' for WebAssembly build.")
endif()

# Add wasm.cc target for Emscripten builds
if(IS_WASM)
    add_executable(spz_wasm src/cc/shared.cc)
    target_link_libraries(spz_wasm PRIVATE spz_static)

    target_compile_options(spz_wasm PRIVATE
        "-fwasm-exceptions"
    )

    target_link_options(spz_wasm PRIVATE
        "-sWASM=1"
        "-sMODULARIZE=0"
        "-sEXPORTED_FUNCTIONS=['_compress_spz','_decompress_spz','_malloc','_free']"
        "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','getValue','HEAPU8']"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=268435456" # 256 MB initial memory
        "-fwasm-exceptions"
    )

    # Copy HTML file for experiments
    add_custom_target(copy_html ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/src/html/index.html
            ${CMAKE_BINARY_DIR}/index.html
        COMMENT "Copying index.html from src/html/ to build directory"
    )
endif()

# Python bindings target (only if enabled and not building for Emscripten)
if(BUILD_PYTHON_BINDINGS)
    pybind11_add_module(pyspz src/cc/python/bindings.cc)
    target_include_directories(pyspz PRIVATE src src/cc)
    target_link_libraries(pyspz PRIVATE spz_static zstd::zstd)
    set_target_properties(pyspz PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        C_VISIBILITY_PRESET hidden
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/pyspz"
    )
endif()

if(CMAKE_BUILD_TYPE MATCHES "Release")
    # Enable LTO if supported
    if(IPO_SUPPORTED)
        message(STATUS "Enabling Link Time Optimization (LTO)")
    else()
        message(STATUS "LTO not supported: ${IPO_ERROR}")
    endif()
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ${IPO_SUPPORTED})

    # Collect flags
    set(_extra_release_flags)
    if(IS_WASM)
        message(STATUS "Enabling SIMD128 for WebAssembly")
        list(APPEND _extra_release_flags "-msimd128")
    elseif(USE_MARCH_NATIVE)
        message(STATUS "Setting -march=native")
        list(APPEND _extra_release_flags "-march=native")
    elseif(NOT MSVC)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "86")
            list(APPEND _extra_release_flags "-march=x86-64-v3")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "(arm|aarch)")
            list(APPEND _extra_release_flags "-march=armv8.2-a")
        else()
            message(WARNING "Unsupported architecture: -march option not set.")
        endif()
    endif()
    if(_extra_release_flags)
        set(CMAKE_CXX_FLAGS_RELEASE
            "${CMAKE_CXX_FLAGS_RELEASE} ${_extra_release_flags}"
        )
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
if(NOT PYTHON_PACKAGE_BUILD)
  if(NOT IS_WASM)
    install(TARGETS spz_shared spz_static
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
    )
  else()
    install(TARGETS spz_static
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
    )
  endif()
endif()

if(BUILD_PYTHON_BINDINGS)
    install(TARGETS pyspz
        LIBRARY DESTINATION .
    )
endif()
