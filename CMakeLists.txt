# CMakeLists.txt for sendspin-server-cpp
#
# Structure:
#   cmake/sources.cmake            - The source lists, shared with the ESP-IDF component
#   cmake/esp_idf.cmake            - The library as an ESP-IDF component
#   cmake/dependencies.cmake       - Library dependencies (Boost, ArduinoJson, noise-c, micro-flac,
#                                    micro-opus, pffft, micro-resampler)
#   cmake/noise_c.cmake            - The noise_c target, built from a manual source list
#   cmake/test_dependencies.cmake  - GoogleTest and the sendspin-cpp test peer

# On ESP-IDF the library is a component (cmake/esp_idf.cmake); nothing below applies there
if(ESP_PLATFORM)
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/esp_idf.cmake)
    return()
endif()

# 3.25: FetchContent FIND_PACKAGE_ARGS (3.24) and SYSTEM (3.25), add_subdirectory SYSTEM (3.25)
cmake_minimum_required(VERSION 3.25)

project(sendspin_server_cpp LANGUAGES C CXX)

set(_sendspin_server_is_top_level OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_sendspin_server_is_top_level ON)
endif()

# ==============================================================================
# Options
# ==============================================================================

option(SENDSPIN_SERVER_BUILD_TESTS "Build the host unit tests (fetches GoogleTest and sendspin-cpp)" OFF)
option(SENDSPIN_SERVER_BUILD_EXAMPLES "Build the example hosts in examples/" ${_sendspin_server_is_top_level})
option(SENDSPIN_SERVER_USE_CCACHE "Compile through ccache when it is installed" ${_sendspin_server_is_top_level})
option(SENDSPIN_SERVER_ENABLE_WERROR "Treat warnings in this project's targets as errors" OFF)
option(SENDSPIN_SERVER_ENABLE_DISCOVERY "Build the mDNS discovery backend: dns_sd on macOS, avahi-client on Linux" ON)
option(SENDSPIN_SERVER_ENABLE_OPUS "Build the Opus encoder and source decoder (libopus through micro-opus); OFF leaves Opus out, and ServerConfig::enable_opus then offers none" ON)
option(SENDSPIN_SERVER_EMBEDDED_PROFILE "Build with the embedded resource profile (src/profile.h), as ESP-IDF does, to run the host tests with its limits" OFF)
option(SENDSPIN_SERVER_MEMORY_STATS "Log the heaps and the tasks' stacks at the server's start, as each connection is ready and closes and as a group starts playing; a host reports none, and the host tests, which count log lines, expect it off" OFF)
option(SENDSPIN_SERVER_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
# ThreadSanitizer is a separate option because it cannot be combined with ASan/UBSan.
option(SENDSPIN_SERVER_ENABLE_TSAN "Enable ThreadSanitizer (mutually exclusive with SENDSPIN_SERVER_ENABLE_SANITIZERS)" OFF)
if(SENDSPIN_SERVER_ENABLE_TSAN AND SENDSPIN_SERVER_ENABLE_SANITIZERS)
    message(FATAL_ERROR "SENDSPIN_SERVER_ENABLE_TSAN and SENDSPIN_SERVER_ENABLE_SANITIZERS are mutually exclusive")
endif()
# The tests stream Opus to the test peer and decode it
if(SENDSPIN_SERVER_BUILD_TESTS AND NOT SENDSPIN_SERVER_ENABLE_OPUS)
    message(FATAL_ERROR "SENDSPIN_SERVER_BUILD_TESTS needs SENDSPIN_SERVER_ENABLE_OPUS")
endif()

if(_sendspin_server_is_top_level)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    get_property(_sendspin_server_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(NOT _sendspin_server_multi_config AND NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    endif()
endif()

# ccache keys compiles on content, so branch switches reuse earlier builds. Set before any target
# so the FetchContent dependencies are cached too.
if(SENDSPIN_SERVER_USE_CCACHE)
    find_program(SENDSPIN_SERVER_CCACHE_PROGRAM ccache)
    if(SENDSPIN_SERVER_CCACHE_PROGRAM)
        foreach(_lang C CXX)
            if(NOT DEFINED CMAKE_${_lang}_COMPILER_LAUNCHER)
                set(CMAKE_${_lang}_COMPILER_LAUNCHER ${SENDSPIN_SERVER_CCACHE_PROGRAM})
            endif()
        endforeach()
        message(STATUS "Using ccache: ${SENDSPIN_SERVER_CCACHE_PROGRAM}")
    endif()
endif()

# ThreadSanitizer is applied globally, before any dependency is declared, so every fetched
# dependency (Boost.Asio, GoogleTest, the sendspin-cpp test peer) is instrumented too: TSan cannot
# see the atomic accesses of uninstrumented code.
if(SENDSPIN_SERVER_ENABLE_TSAN)
    string(APPEND CMAKE_C_FLAGS " -fsanitize=thread -fno-omit-frame-pointer -g")
    string(APPEND CMAKE_CXX_FLAGS " -fsanitize=thread -fno-omit-frame-pointer -g")
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -fsanitize=thread")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS " -fsanitize=thread")
    string(APPEND CMAKE_MODULE_LINKER_FLAGS " -fsanitize=thread")
endif()

# Compile options for this project's own targets (the library and the tests), never for
# dependencies.
function(sendspin_server_cpp_configure_target TARGET)
    target_compile_features(${TARGET} PUBLIC cxx_std_20)
    target_compile_options(${TARGET} PRIVATE -Wall -Wextra -Wpedantic)
    if(SENDSPIN_SERVER_ENABLE_WERROR)
        target_compile_options(${TARGET} PRIVATE -Werror)
    endif()
    if(SENDSPIN_SERVER_ENABLE_SANITIZERS)
        target_compile_options(${TARGET} PRIVATE -fsanitize=address,undefined
                               -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
        target_link_options(${TARGET} PUBLIC -fsanitize=address,undefined
                            -fno-sanitize-recover=undefined)
    endif()
endfunction()

# ==============================================================================
# Dependencies
# ==============================================================================

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/dependencies.cmake)

# ==============================================================================
# Library
# ==============================================================================

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/sources.cmake)

# stb_image, stb_image_resize2 and stb_image_write (third_party/stb/VENDORED.md), compiled once in
# src/stb_implementation.cpp with their warnings off, ASan applied but not UBSan; JPEG and PNG
# only, from memory only
# stb_image allocates through src/stb_budget.cpp, built with this project's warnings
add_library(sendspin_server_cpp_stb_budget STATIC src/stb_budget.cpp)
sendspin_server_cpp_configure_target(sendspin_server_cpp_stb_budget)
add_library(sendspin_server_cpp_stb STATIC src/stb_implementation.cpp)
target_link_libraries(sendspin_server_cpp_stb PUBLIC sendspin_server_cpp_stb_budget)
target_compile_features(sendspin_server_cpp_stb PUBLIC cxx_std_20)
target_compile_options(sendspin_server_cpp_stb PRIVATE -w)
if(SENDSPIN_SERVER_ENABLE_SANITIZERS)
    target_compile_options(sendspin_server_cpp_stb PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(sendspin_server_cpp_stb PUBLIC -fsanitize=address)
endif()
target_include_directories(sendspin_server_cpp_stb SYSTEM PUBLIC
                           ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
target_compile_definitions(sendspin_server_cpp_stb PUBLIC
    STBI_ONLY_JPEG
    STBI_ONLY_PNG
    STBI_NO_STDIO
    STBI_WRITE_NO_STDIO
)

list(TRANSFORM SENDSPIN_SERVER_SOURCES PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
list(TRANSFORM SENDSPIN_SERVER_HOST_SOURCES PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
add_library(sendspin_server_cpp STATIC ${SENDSPIN_SERVER_SOURCES} ${SENDSPIN_SERVER_HOST_SOURCES})
sendspin_server_cpp_configure_target(sendspin_server_cpp)
# GCC reports ArduinoJson's CollectionIterator::nextId_ as maybe uninitialized where its object
# lookups inline; it is set on every path
set_property(SOURCE src/messages.cpp APPEND PROPERTY COMPILE_OPTIONS
             $<$<CXX_COMPILER_ID:GNU>:-Wno-maybe-uninitialized>)

target_include_directories(sendspin_server_cpp
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
# src/profile.h: the tests that include the library's private headers see the same
# profile
if(SENDSPIN_SERVER_EMBEDDED_PROFILE)
    target_compile_definitions(sendspin_server_cpp PUBLIC SENDSPIN_SERVER_EMBEDDED_PROFILE=1)
endif()
if(SENDSPIN_SERVER_MEMORY_STATS)
    target_compile_definitions(sendspin_server_cpp PUBLIC SENDSPIN_SERVER_MEMORY_STATS=1)
endif()

# platform_discovery(): one backend per platform, each with a thread of its own
if(SENDSPIN_SERVER_ENABLE_DISCOVERY AND APPLE)
    # dns_sd is part of the system library
    target_sources(sendspin_server_cpp PRIVATE src/discovery/dns_sd_discovery.cpp)
elseif(SENDSPIN_SERVER_ENABLE_DISCOVERY AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Native avahi-client: the avahi-compat dns_sd layer cannot resolve addresses
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SENDSPIN_SERVER_AVAHI REQUIRED IMPORTED_TARGET avahi-client)
    target_sources(sendspin_server_cpp PRIVATE src/discovery/avahi_discovery.cpp)
    target_link_libraries(sendspin_server_cpp PRIVATE PkgConfig::SENDSPIN_SERVER_AVAHI)
else()
    target_sources(sendspin_server_cpp PRIVATE src/discovery/no_discovery.cpp)
endif()

# sendspin_server_cpp_beast last: a system Boost's include directory (e.g. /opt/homebrew/include) must
# not shadow the fetched dependencies' headers.
target_link_libraries(sendspin_server_cpp PRIVATE
    ArduinoJson
    noise_c
    micro_flac
    micro-resampler
    sendspin_server_cpp_pffft
    sendspin_server_cpp_stb
    sendspin_server_cpp_beast
)
if(SENDSPIN_SERVER_ENABLE_OPUS)
    target_link_libraries(sendspin_server_cpp PRIVATE micro_opus)
    target_compile_definitions(sendspin_server_cpp PRIVATE SENDSPIN_SERVER_ENABLE_OPUS=1)
endif()

# ==============================================================================
# Tests
# ==============================================================================

if(SENDSPIN_SERVER_BUILD_TESTS)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_dependencies.cmake)
    enable_testing()
    add_subdirectory(tests)
endif()

# ==============================================================================
# Examples
# ==============================================================================

if(SENDSPIN_SERVER_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
