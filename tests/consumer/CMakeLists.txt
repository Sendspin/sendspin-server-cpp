# A project that consumes the library through FetchContent, as a subproject, for CI
#
#   cmake -S tests/consumer -B <build dir> && cmake --build <build dir>

cmake_minimum_required(VERSION 3.25)

project(sendspin_server_cpp_consumer LANGUAGES C CXX)

include(FetchContent)
FetchContent_Declare(sendspin_server_cpp SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../..)
FetchContent_MakeAvailable(sendspin_server_cpp)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE sendspin_server_cpp)
