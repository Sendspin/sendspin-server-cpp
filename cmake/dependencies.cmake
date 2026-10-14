# Library dependencies for sendspin-server-cpp
#
# Every dependency is pinned. Sub-project options that build more than a library (tests,
# examples) are set off before it is made available. They are normal variables: every dependency
# here and in test_dependencies.cmake sets policy CMP0077, so its option() takes them.

include(FetchContent)

find_package(Threads REQUIRED)

# ==============================================================================
# Boost.Beast and Boost.Asio
#
# The `-cmake` release tarball, which carries a CMake build for every library, restricted to Beast
# and its dependencies. A system Boost at or above the pin is used instead when one is installed.
# Asio and Beast are compiled once, in src/beast_separate_compilation.cpp; every translation unit
# that includes them links sendspin_server_cpp_beast, which carries the matching definitions.
# ==============================================================================

set(BOOST_INCLUDE_LIBRARIES beast)
set(BOOST_BEAST_BUILD_TESTS OFF)
set(BOOST_BEAST_BUILD_EXAMPLES OFF)
set(BOOST_BEAST_BUILD_FUZZERS OFF)
FetchContent_Declare(
    Boost
    URL      https://github.com/boostorg/boost/releases/download/boost-1.90.0/boost-1.90.0-cmake.tar.xz
    URL_HASH SHA256=aca59f889f0f32028ad88ba6764582b63c916ce5f77b31289ad19421a96c555f
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM
    FIND_PACKAGE_ARGS 1.90.0 CONFIG
)
FetchContent_MakeAvailable(Boost)

add_library(sendspin_server_cpp_beast INTERFACE)
# A system Boost's CMake package defines no per-library targets for header-only libraries
if(TARGET Boost::beast)
    target_link_libraries(sendspin_server_cpp_beast INTERFACE Boost::beast)
else()
    target_link_libraries(sendspin_server_cpp_beast INTERFACE Boost::headers)
endif()
target_link_libraries(sendspin_server_cpp_beast INTERFACE Threads::Threads)
target_compile_definitions(sendspin_server_cpp_beast INTERFACE
    BOOST_ASIO_SEPARATE_COMPILATION
    BOOST_BEAST_SEPARATE_COMPILATION
    BOOST_ASIO_NO_DEPRECATED
)

# ==============================================================================
# ArduinoJson (header-only), at the sendspin-cpp pin
# ==============================================================================

FetchContent_Declare(
    ArduinoJson
    GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
    GIT_TAG        v7.4.1
    GIT_SHALLOW    TRUE
    SYSTEM
)
FetchContent_MakeAvailable(ArduinoJson)
# 64-bit integers and std::string, matching the test peer; set on the target so every consumer in
# this build agrees on it.
target_compile_definitions(ArduinoJson INTERFACE
    ARDUINOJSON_ENABLE_STD_STRING=1
    ARDUINOJSON_USE_LONG_LONG=1
)

# ==============================================================================
# noise-c with OpenSSL AES-GCM
# ==============================================================================

include(${CMAKE_CURRENT_LIST_DIR}/noise_c.cmake)

# ==============================================================================
# micro-flac: the v0.3.0 release, the first with the FLAC encoder; its decoder API
# is the one sendspin-cpp builds against. Declared before the test peer, which declares micro_flac
# at its own pin: FetchContent uses the first declaration of a name.
# ==============================================================================

FetchContent_Declare(
    micro_flac
    GIT_REPOSITORY https://github.com/esphome-libs/micro-flac.git
    GIT_TAG        f9c27d4d0166021db977d7fc37d205b21d81e9f2  # v0.3.0
    GIT_SUBMODULES "lib/micro-ogg-demuxer"
    SYSTEM
)
FetchContent_MakeAvailable(micro_flac)

# ==============================================================================
# micro-opus at the sendspin-cpp pin, for libopus's encoder, which it builds in fixed point with
# its pseudostack per thread; only with SENDSPIN_SERVER_ENABLE_OPUS. Declared before the test peer,
# whose Opus decoder uses the same declaration.
# ==============================================================================

if(SENDSPIN_SERVER_ENABLE_OPUS)
    FetchContent_Declare(
        micro_opus
        GIT_REPOSITORY https://github.com/esphome-libs/micro-opus.git
        GIT_TAG        v0.3.5
        GIT_SUBMODULES "lib/opus" "lib/micro-ogg-demuxer"
        SYSTEM
    )
    FetchContent_MakeAvailable(micro_opus)
endif()

# ==============================================================================
# micro-resampler, vendored (third_party/micro-resampler/VENDORED.md)
# ==============================================================================

# A consumer that defined a micro-resampler target first keeps its own
if(NOT TARGET micro-resampler)
    set(MICRO_RESAMPLER_BUILD_TESTS OFF)
    set(MICRO_RESAMPLER_NATIVE_TUNING OFF)
    add_subdirectory(${PROJECT_SOURCE_DIR}/third_party/micro-resampler
                     ${PROJECT_BINARY_DIR}/third_party/micro-resampler SYSTEM)
    # Its QUAD coefficient path compiles in whenever Boost.Multiprecision headers happen to be on
    # its include path. The server never requests QUAD, so keep the build independent of which
    # Boost is installed.
    target_compile_definitions(micro-resampler PRIVATE RESAMPLER_DISABLE_QUAD_PRECISION)
endif()

# ==============================================================================
# pffft, the visualizer's FFT on the host (src/fft_pffft.cpp): its single-precision real FFT only,
# built here from its two sources as target sendspin_server_cpp_pffft, with its warnings off. Populated
# without being added (SOURCE_SUBDIR names a directory that does not exist): its own CMake build
# adds the double-precision FFT, its DSP helpers, tests and benchmarks. Another platform's FFT
# replaces src/fft_pffft.cpp and this target.
# ==============================================================================

FetchContent_Declare(
    pffft
    GIT_REPOSITORY https://github.com/marton78/pffft.git
    GIT_TAG        aa16fd3db58de4ba5dae8b0438440bb9da46b6fa  # master
    SOURCE_SUBDIR  no-cmake-build
)
FetchContent_MakeAvailable(pffft)
add_library(sendspin_server_cpp_pffft STATIC
    ${pffft_SOURCE_DIR}/src/pffft.c
    ${pffft_SOURCE_DIR}/src/pffft_common.c
)
set_target_properties(sendspin_server_cpp_pffft PROPERTIES C_STANDARD 99 C_EXTENSIONS OFF)
target_compile_options(sendspin_server_cpp_pffft PRIVATE -w)
target_compile_definitions(sendspin_server_cpp_pffft PRIVATE _USE_MATH_DEFINES PFFFT_SCALVEC_ENABLED=1)
target_include_directories(sendspin_server_cpp_pffft SYSTEM PUBLIC ${pffft_SOURCE_DIR}/include/pffft)
if(NOT APPLE AND NOT WIN32)
    target_link_libraries(sendspin_server_cpp_pffft PRIVATE m)
endif()
if(SENDSPIN_SERVER_ENABLE_SANITIZERS)
    target_compile_options(sendspin_server_cpp_pffft PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(sendspin_server_cpp_pffft PUBLIC -fsanitize=address)
endif()
