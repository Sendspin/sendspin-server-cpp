# A minimal ESP-IDF project that builds the library as a component (cmake/esp_idf.cmake), for CI
#
#   idf.py -C tests/esp_idf set-target esp32s3
#   idf.py -C tests/esp_idf build
#
# The component's dependencies: esphome/noise-c and espressif/esp-dsp from the component registry
# (main/idf_component.yml), micro-resampler from third_party/, and micro-flac and ArduinoJson
# fetched below at cmake/dependencies.cmake's pins.

cmake_minimum_required(VERSION 3.16)

set(_root ${CMAKE_CURRENT_LIST_DIR}/../..)

# Fetched before project(): micro-flac is a component directory, and cmake/esp_idf.cmake reads
# ArduinoJson's source tree
include(FetchContent)
FetchContent_Populate(
    micro_flac
    GIT_REPOSITORY https://github.com/esphome-libs/micro-flac.git
    GIT_TAG        f9c27d4d0166021db977d7fc37d205b21d81e9f2  # v0.3.0
    GIT_SUBMODULES "lib/micro-ogg-demuxer"
    # The directory's name is the component's
    SOURCE_DIR     ${CMAKE_BINARY_DIR}/_deps/micro-flac
    BINARY_DIR     ${CMAKE_BINARY_DIR}/_deps/micro-flac-build
    SUBBUILD_DIR   ${CMAKE_BINARY_DIR}/_deps/micro-flac-subbuild
)
FetchContent_Populate(
    arduinojson
    GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
    GIT_TAG        v7.4.1
    GIT_SHALLOW    TRUE
    SOURCE_DIR     ${CMAKE_BINARY_DIR}/_deps/arduinojson-src
    BINARY_DIR     ${CMAKE_BINARY_DIR}/_deps/arduinojson-build
    SUBBUILD_DIR   ${CMAKE_BINARY_DIR}/_deps/arduinojson-subbuild
)
set(SENDSPIN_SERVER_ARDUINOJSON_SOURCE_DIR ${CMAKE_BINARY_DIR}/_deps/arduinojson-src)

set(EXTRA_COMPONENT_DIRS
    ${CMAKE_BINARY_DIR}/_deps/micro-flac
    ${_root}/third_party/micro-resampler
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sendspin_server_esp_idf_build)
