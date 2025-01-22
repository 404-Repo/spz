cmake_minimum_required(VERSION 3.20)
project(spz VERSION 1.0.0 LANGUAGES CXX)

option(USE_MARCH_NATIVE "Enable -march=native for release builds" OFF)
option(BUILD_SHARED_LIBS "Build shared libraries" ON)
option(BUILD_PYTHON_BINDINGS "Build Python bindings" ON)

option(ENABLE_ASAN "Enable Address Sanitizer" OFF)
option(ENABLE_UBSAN "Enable Undefined Behavior Sanitizer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Configure third-party libraries
find_package(zstd REQUIRED)
include_directories(${ZSTD_INCLUDE_DIRS})

if(BUILD_PYTHON_BINDINGS)
    find_package(Python COMPONENTS Interpreter Development REQUIRED)

    # Find or fetch pybind11
    include(FetchContent)
    FetchContent_Declare(
        pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG        v2.13.6
    )
    FetchContent_MakeAvailable(pybind11)
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT error)

set(LIBRARY_SOURCES
    src/cc/load-spz.cc
    src/cc/splat-c-types.cc
    src/cc/splat-types.cc
)

set(EXECUTABLE_SOURCES
    src/cc/main.cc
)

# Define shared library target
add_library(spz_shared SHARED ${LIBRARY_SOURCES})
target_compile_definitions(spz_shared PRIVATE SPZ_BUILDING_SHARED)
set_target_properties(spz_shared PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
    OUTPUT_NAME "spz"
)
target_include_directories(spz_shared
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        src/cc
)
target_link_libraries(spz_shared PRIVATE zstd)

# Define static library target with PIC
add_library(spz_static STATIC ${LIBRARY_SOURCES})
target_compile_definitions(spz_static PRIVATE SPZ_BUILDING_STATIC)
set_target_properties(spz_static PROPERTIES
    OUTPUT_NAME "spz"
    VERSION 1.0.0
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(spz_static
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        src/cc
)
target_link_libraries(spz_static PRIVATE zstd)

add_executable(spz_exe ${EXECUTABLE_SOURCES})
set_target_properties(spz_exe PROPERTIES OUTPUT_NAME "spz")
target_include_directories(spz_exe PRIVATE src src/cc)
target_link_libraries(spz_exe PRIVATE spz_static zstd)

if(ENABLE_ASAN OR ENABLE_UBSAN)
    set(SANITIZER_FLAGS)

    if(ENABLE_ASAN)
        message(STATUS "Address Sanitizer (ASAN) enabled for executable")
        list(APPEND SANITIZER_FLAGS "-fsanitize=address")
    endif()

    if(ENABLE_UBSAN)
        message(STATUS "Undefined Behavior Sanitizer (UBSAN) enabled for executable")
        list(APPEND SANITIZER_FLAGS "-fsanitize=undefined")
    endif()

    # Apply sanitizer flags to the executable target
    if(SANITIZER_FLAGS)
        target_compile_options(spz_exe PRIVATE ${SANITIZER_FLAGS})
        target_link_options(spz_exe PRIVATE ${SANITIZER_FLAGS})
    endif()
endif()

if(BUILD_PYTHON_BINDINGS)
    pybind11_add_module(pyspz src/cc/python/bindings.cc)
    target_include_directories(pyspz PRIVATE src src/cc)

    target_link_libraries(pyspz PRIVATE spz_static zstd)

    set_target_properties(pyspz PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/pyspz"
        PREFIX ""
        SUFFIX "${PYTHON_MODULE_EXTENSION}"
    )
endif()

# Configure build settings for Release, ReleaseWithDebugInfo is used for profiling
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "ReleaseWithDebugInfo")
    if (ipo_supported)
        message(STATUS "IPO / LTO enabled")
        set_property(TARGET spz_shared PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET spz_static PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET spz_exe PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(STATUS "IPO / LTO not supported: <${error}>")
    endif()

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
                set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -march=armv8-a")
            endif()
        else()
            message(WARNING "Unsupported architecture: -march option not set.")
        endif()
    endif()

    # Write compile flags
    file(WRITE "${CMAKE_BINARY_DIR}/compile_flags.txt" "CXXFLAGS: ${CMAKE_CXX_FLAGS_RELEASE}\n")
    file(APPEND "${CMAKE_BINARY_DIR}/compile_flags.txt" "LINKER_FLAGS: ${CMAKE_EXE_LINKER_FLAGS}\n")
endif()

target_include_directories(spz_shared
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        src/cc
)

target_include_directories(spz_static
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        src/cc
)

if(BUILD_PYTHON_BINDINGS)
    install(TARGETS pyspz
        LIBRARY DESTINATION .
    )
endif()
