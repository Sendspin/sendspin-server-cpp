# Example hosts for sendspin-server-cpp, built with -DSENDSPIN_SERVER_BUILD_EXAMPLES=ON (the default
# when this is the top-level project). They use the public API only.

# server_cli: a REPL that dials clients by URL, plays WAV and FLAC files or live PCM from standard
# input to them, and prints the server's events and message trace
add_executable(server_cli
    server_cli/main.cpp
    server_cli/audio_source.cpp
    server_cli/feeder.cpp
    server_cli/file_persistence_provider.cpp
    server_cli/flac_file.cpp
    server_cli/live_feeder.cpp
    server_cli/one_group.cpp
    server_cli/timing_log.cpp
    server_cli/timing_stats.cpp
    server_cli/wav_reader.cpp
    # The --web dashboard: its HTTP and WebSocket server, and its commands and state
    server_cli/web_commands.cpp
    server_cli/web_ui.cpp
)
sendspin_server_cpp_configure_target(server_cli)
# GCC reports ArduinoJson's CollectionIterator::nextId_ as maybe uninitialized where its object
# lookups inline; it is set on every path
target_compile_options(server_cli PRIVATE $<$<CXX_COMPILER_ID:GNU>:-Wno-maybe-uninitialized>)
include(${PROJECT_SOURCE_DIR}/cmake/embed_asset.cmake)
sendspin_server_cpp_embed_asset(server_cli
    ${CMAKE_CURRENT_SOURCE_DIR}/server_cli/web_assets/dashboard.html DASHBOARD_HTML dashboard_html.h)
# micro-flac decodes FLAC files (flac_file.cpp); Beast serves the dashboard and ArduinoJson carries
# its messages. sendspin_server_cpp_beast last, as the library links it.
target_link_libraries(server_cli PRIVATE sendspin_server_cpp micro_flac ArduinoJson sendspin_server_cpp_beast)
