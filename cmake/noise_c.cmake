# The noise_c target: noise-c's reference backend for 25519, ChaChaPoly and SHA-256, with OpenSSL 3
# for AES-GCM (the reference AES is table-based and not constant-time).
#
# The esphome-libs fork (the one sendspin-cpp uses, at its pin) ships only an ESP-IDF
# CMakeLists.txt, so it is populated without being added (SOURCE_SUBDIR names a directory that
# does not exist) and built here from a manual source list. sendspin-cpp's host build uses this
# target instead of building its own when a `noise_c` target exists.
#
# Compile definitions:
#   NOISE_USE_REFERENCE_BACKEND=1: the pure-C reference backend
#   NOISE_USE_AES=1:               register the AESGCM suite name
#   NOISE_USE_OPENSSL=1:           AES-GCM from OpenSSL (src/backend/openssl/cipher-aesgcm.c)
#   NOISE_USE_LIBSODIUM=0:         no libsodium backend
#   NOISE_USE_CUSTOM_RAND=0:       the OS RNG (rand_os.c)

if(APPLE AND NOT DEFINED OPENSSL_ROOT_DIR)
    # Homebrew's openssl@3 is keg-only, so FindOpenSSL does not look there by itself
    find_program(SENDSPIN_SERVER_BREW_PROGRAM brew)
    if(SENDSPIN_SERVER_BREW_PROGRAM)
        execute_process(
            COMMAND ${SENDSPIN_SERVER_BREW_PROGRAM} --prefix openssl@3
            OUTPUT_VARIABLE _sendspin_server_openssl_prefix
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _sendspin_server_brew_result
        )
        if(_sendspin_server_brew_result EQUAL 0 AND EXISTS "${_sendspin_server_openssl_prefix}")
            set(OPENSSL_ROOT_DIR "${_sendspin_server_openssl_prefix}")
        endif()
    endif()
endif()
find_package(OpenSSL 3 REQUIRED)

FetchContent_Declare(
    noise_c
    GIT_REPOSITORY https://github.com/esphome-libs/noise-c.git
    GIT_TAG        v0.1.13
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  no-cmake-build
)
FetchContent_MakeAvailable(noise_c)

add_library(noise_c STATIC
    # Protocol state machine
    ${noise_c_SOURCE_DIR}/src/protocol/cipherstate.c
    ${noise_c_SOURCE_DIR}/src/protocol/dhstate.c
    ${noise_c_SOURCE_DIR}/src/protocol/errors.c
    ${noise_c_SOURCE_DIR}/src/protocol/handshakestate.c
    ${noise_c_SOURCE_DIR}/src/protocol/hashstate.c
    ${noise_c_SOURCE_DIR}/src/protocol/internal.c
    ${noise_c_SOURCE_DIR}/src/protocol/names.c
    ${noise_c_SOURCE_DIR}/src/protocol/patterns.c
    ${noise_c_SOURCE_DIR}/src/protocol/rand_os.c
    ${noise_c_SOURCE_DIR}/src/protocol/randstate.c
    ${noise_c_SOURCE_DIR}/src/protocol/signstate.c
    ${noise_c_SOURCE_DIR}/src/protocol/symmetricstate.c
    ${noise_c_SOURCE_DIR}/src/protocol/util.c

    # Reference backend: ChaChaPoly, Curve25519, SHA-256
    ${noise_c_SOURCE_DIR}/src/backend/ref/cipher-chachapoly.c
    ${noise_c_SOURCE_DIR}/src/backend/ref/dh-curve25519.c
    ${noise_c_SOURCE_DIR}/src/backend/ref/hash-sha256.c

    # OpenSSL backend: AES-GCM
    ${noise_c_SOURCE_DIR}/src/backend/openssl/cipher-aesgcm.c

    # Low-level primitives for the reference backend
    ${noise_c_SOURCE_DIR}/src/crypto/chacha/chacha.c
    ${noise_c_SOURCE_DIR}/src/crypto/donna/poly1305-donna.c
    ${noise_c_SOURCE_DIR}/src/crypto/sha2/sha256.c
    ${noise_c_SOURCE_DIR}/src/crypto/x25519/x25519.c
)

target_include_directories(noise_c SYSTEM PUBLIC
    ${noise_c_SOURCE_DIR}/include
    ${noise_c_SOURCE_DIR}/src
)

target_compile_definitions(noise_c PUBLIC
    NOISE_USE_REFERENCE_BACKEND=1
    NOISE_USE_AES=1
    NOISE_USE_OPENSSL=1
    NOISE_USE_LIBSODIUM=0
    NOISE_USE_CUSTOM_RAND=0
)

target_link_libraries(noise_c PUBLIC OpenSSL::Crypto)

set_target_properties(noise_c PROPERTIES C_STANDARD 99)
