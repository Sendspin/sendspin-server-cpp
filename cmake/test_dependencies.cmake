# Test-only dependencies for sendspin-server-cpp: GoogleTest and the sendspin-cpp test peer

include(FetchContent)

# ==============================================================================
# GoogleTest
# ==============================================================================

set(INSTALL_GTEST OFF)
set(BUILD_GMOCK OFF)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
    SYSTEM
)
FetchContent_MakeAvailable(googletest)

# ==============================================================================
# sendspin-cpp, the client library the tests run against (target `sendspin`)
#
# Pinned by commit. To build against a local checkout instead, configure with
#   -DFETCHCONTENT_SOURCE_DIR_SENDSPIN_CPP=/path/to/sendspin-cpp
#
# Its host build uses this project's noise_c target (cmake/noise_c.cmake) and the micro_flac,
# micro_opus and ArduinoJson declarations made in cmake/dependencies.cmake, which come first. Its
# Opus decoder is on (SENDSPIN_ENABLE_OPUS, the peer's default), so test players can list Opus.
# ==============================================================================

set(BUILD_EXAMPLES OFF)
set(SENDSPIN_BUILD_TESTS OFF)
set(SENDSPIN_USE_CCACHE OFF)
# Instrument the peer as this project's targets are; ThreadSanitizer already reaches it through
# the global flags.
set(ENABLE_SANITIZERS ${SENDSPIN_SERVER_ENABLE_SANITIZERS})
FetchContent_Declare(
    sendspin_cpp
    GIT_REPOSITORY https://github.com/Sendspin/sendspin-cpp.git
    GIT_TAG        f8ef7ece80f219b0ccc2f443c46f850ab2a6d967  # main
    GIT_SUBMODULES ""
    SYSTEM
)
FetchContent_MakeAvailable(sendspin_cpp)
# The peer's WebSocket library shares std::vector instantiations with the instrumented peer; left
# uninstrumented, the two disagree on ASan's container annotations and ASan reports overflows that
# are not there.
if(SENDSPIN_SERVER_ENABLE_SANITIZERS AND TARGET ixwebsocket)
    target_compile_options(ixwebsocket PRIVATE -fsanitize=address)
endif()
