# The library as an ESP-IDF component
#
# Included by CMakeLists.txt when ESP_PLATFORM is set. The sources are cmake/sources.cmake's, shared
# with the host build. ArduinoJson comes from a source tree the project names, as a normal variable
# set before project() or an environment variable of the same name:
#   SENDSPIN_SERVER_ARDUINOJSON_SOURCE_DIR: ArduinoJson
# The IDF components the project adds: esphome/noise-c (with its esphome/libsodium),
# espressif/esp-dsp, espressif/esp_websocket_client, micro-flac (the v0.3.0 tag, with the encoder)
# and micro-resampler (third_party/micro-resampler). The component's Kconfig turns on the httpd and
# esp_websocket_client options the transport needs.

include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

set(_priv_requires lwip pthread esp_timer esp_hw_support heap freertos mbedtls esp_http_server
    tcp_transport esp-tls espressif__esp_websocket_client esphome__noise-c esphome__libsodium
    espressif__esp-dsp micro-flac micro-resampler)

if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register(PRIV_REQUIRES ${_priv_requires})
    return()
endif()

if(NOT DEFINED SENDSPIN_SERVER_ARDUINOJSON_SOURCE_DIR AND DEFINED ENV{SENDSPIN_SERVER_ARDUINOJSON_SOURCE_DIR})
    set(SENDSPIN_SERVER_ARDUINOJSON_SOURCE_DIR $ENV{SENDSPIN_SERVER_ARDUINOJSON_SOURCE_DIR})
endif()
if(NOT SENDSPIN_SERVER_ARDUINOJSON_SOURCE_DIR)
    message(FATAL_ERROR "sendspin-server-cpp: set SENDSPIN_SERVER_ARDUINOJSON_SOURCE_DIR (cmake/esp_idf.cmake)")
endif()

set(_root ${CMAKE_CURRENT_LIST_DIR}/..)
set(_srcs ${SENDSPIN_SERVER_SOURCES} ${SENDSPIN_SERVER_STB_SOURCES} ${SENDSPIN_SERVER_ESP_SOURCES})
list(TRANSFORM _srcs PREPEND ${_root}/)

# WHOLE_ARCHIVE: src/esp/noise_esp.cpp defines noise-c's weak hooks (noise_rand_bytes(),
# noise_aesgcm_new()), which the esphome__noise-c archive references but nothing in this one does
# where the linker scans it; --gc-sections still drops what is unused. esphome__libsodium is
# noise-c's backend, and noise_esp.cpp's X25519 for CPace.
idf_component_register(
    SRCS ${_srcs}
    INCLUDE_DIRS ${_root}/include
    PRIV_INCLUDE_DIRS ${_root}/src ${_root}/src/esp ${_root}/third_party/stb
                      ${SENDSPIN_SERVER_ARDUINOJSON_SOURCE_DIR}/src
    PRIV_REQUIRES ${_priv_requires}
    WHOLE_ARCHIVE
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    # src/profile.h's embedded profile
    SENDSPIN_SERVER_EMBEDDED_PROFILE=1
    # No Opus encoder or source decoder on the embedded profile (src/frame_encoder.cpp,
    # src/source_decoder.cpp)
    SENDSPIN_SERVER_ENABLE_OPUS=0
    # noise-c's headers as the library uses them: the AESGCM cipher id (noise_esp.cpp supplies the
    # cipher, sendspin_server_cpp_configure_noise_c() below registers its name)
    NOISE_USE_AES=1
    ARDUINOJSON_ENABLE_STD_STRING=1
    ARDUINOJSON_USE_LONG_LONG=1
    STBI_ONLY_JPEG STBI_ONLY_PNG STBI_NO_STDIO STBI_WRITE_NO_STDIO
)
# src/profile.h's memory statistics (Kconfig)
if(CONFIG_SENDSPIN_SERVER_MEMORY_STATS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SENDSPIN_SERVER_MEMORY_STATS=1)
endif()
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers -Wno-unused-parameter)
# The whole application's pthread_mutex_init() and pthread_cond_init() go through
# src/esp/memory_esp.cpp's wraps, which leave a mutex or condition variable they failed to create
# initializable again rather than garbage (libstdc++ ignores the failure). WHOLE_ARCHIVE above
# links the wraps.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=pthread_mutex_init"
                      "-Wl,--wrap=pthread_cond_init")
set_source_files_properties(${_root}/src/stb_implementation.cpp PROPERTIES COMPILE_OPTIONS "-w")

# src/esp/dial_esp.cpp gives its socket transport the socket getter esp_transport_set_func() does
# not take, through tcp_transport's private header (the same struct on ESP-IDF 5.5.5 and 6.0.2)
idf_component_get_property(_tcp_transport_dir tcp_transport COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${_tcp_transport_dir}/private_include")

# esphome__noise-c is built with one pattern (NNpsk0), no name tables and no AES-GCM. The server
# looks its Noise_KKpsk2 protocols up by name and must offer both of the spec's suites (spec
# connection.md "Cipher Suites"), so the name tables go back on and the AESGCM name and cipher id
# are compiled into the two files that use them, without the component's own AES-GCM backends:
# noise_aesgcm_new() comes from src/esp/noise_esp.cpp. Deferred to the end of configuration: the
# component's target may not exist when this file runs.
function(sendspin_server_cpp_configure_noise_c)
    idf_component_get_property(_noise_lib esphome__noise-c COMPONENT_LIB)
    idf_component_get_property(_noise_dir esphome__noise-c COMPONENT_DIR)
    get_target_property(_noise_defs ${_noise_lib} COMPILE_DEFINITIONS)
    if(NOT _noise_defs)
        set(_noise_defs "")
    endif()
    list(REMOVE_ITEM _noise_defs "NOISE_USE_PROTOCOL_NAME_TABLE=0")
    if(NOT "NOISE_USE_PROTOCOL_NAME_TABLE=1" IN_LIST _noise_defs)
        list(APPEND _noise_defs "NOISE_USE_PROTOCOL_NAME_TABLE=1")
    endif()
    set_target_properties(${_noise_lib} PROPERTIES COMPILE_DEFINITIONS "${_noise_defs}")
    set_source_files_properties(${_noise_dir}/src/protocol/cipherstate.c
                                ${_noise_dir}/src/protocol/names.c
                                TARGET_DIRECTORY ${_noise_lib}
                                PROPERTIES COMPILE_DEFINITIONS NOISE_USE_AES=1)
endfunction()
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL sendspin_server_cpp_configure_noise_c)
