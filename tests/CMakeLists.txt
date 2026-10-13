# Host unit tests for sendspin-server-cpp
#
# Enabled from the top-level CMakeLists.txt with -DSENDSPIN_SERVER_BUILD_TESTS=ON, which also
# fetches GoogleTest and the sendspin-cpp test peer (cmake/test_dependencies.cmake).

# One executable aggregating every test_*.cpp file behind the main.cpp watchdog entry point.
# gtest_discover_tests still registers the individual TEST() cases with CTest so failures are
# reported per case.
add_executable(sendspin_server_cpp_tests
    main.cpp
    # The raw test client (raw_client.h), shared by the tests that drive the server as a client
    raw_client.cpp
    test_arduinojson.cpp
    test_artwork.cpp
    test_audio_source.cpp
    test_audio_workers.cpp
    test_backoff.cpp
    test_beast.cpp
    test_client_records.cpp
    test_clock.cpp
    test_colors.cpp
    test_controller.cpp
    test_crypto.cpp
    test_dispatcher.cpp
    test_discovery_policy.cpp
    test_feeder.cpp
    test_fragmentation.cpp
    test_frame_encoder.cpp
    test_group_audio.cpp
    test_host_view.cpp
    test_inbound_hold.cpp
    test_inbound_ring.cpp
    test_live_feeder.cpp
    test_messages.cpp
    test_memory.cpp
    test_metadata.cpp
    test_micro_flac.cpp
    test_micro_resampler.cpp
    test_noise.cpp
    test_noise_initiator.cpp
    test_one_group.cpp
    test_pairing.cpp
    test_pairing_crypto.cpp
    test_pcm_convert.cpp
    test_peer.cpp
    test_pipeline_stages.cpp
    test_playback_policy.cpp
    test_player_stream.cpp
    test_ring.cpp
    test_sched_admission.cpp
    test_sched_chunk.cpp
    test_sched_frames.cpp
    test_sched_leads.cpp
    test_sched_ledger.cpp
    test_sched_lifecycle.cpp
    test_sched_lowering.cpp
    test_sched_property.cpp
    test_sched_selection.cpp
    test_sched_timeline.cpp
    test_send_queues.cpp
    test_server.cpp
    test_server_accept.cpp
    test_server_artwork.cpp
    test_server_core.cpp
    test_server_discovery.cpp
    test_server_limits.cpp
    test_server_log.cpp
    test_server_pairing.cpp
    test_server_pairing_wire.cpp
    test_server_playback.cpp
    test_server_players.cpp
    test_server_raw_client.cpp
    test_server_roles.cpp
    test_server_source.cpp
    test_server_trust.cpp
    test_server_visualizer.cpp
    test_session.cpp
    test_session_driver.cpp
    test_source_clock.cpp
    test_source_decoder.cpp
    test_source_sink.cpp
    test_source_stream.cpp
    test_soak_wire_checker.cpp
    test_task_queue.cpp
    test_timer.cpp
    test_timing_log.cpp
    test_timing_stats.cpp
    test_transport.cpp
    test_trust_policy.cpp
    test_visualizer.cpp
    test_visualizer_stream.cpp
    test_wav_reader.cpp
    test_web_commands.cpp
    test_web_ui.cpp
    # server_cli's file readers, feeders, --one-group, timing figures and --web dashboard, tested
    # beside the library
    ${PROJECT_SOURCE_DIR}/examples/server_cli/audio_source.cpp
    ${PROJECT_SOURCE_DIR}/examples/server_cli/feeder.cpp
    ${PROJECT_SOURCE_DIR}/examples/server_cli/flac_file.cpp
    ${PROJECT_SOURCE_DIR}/examples/server_cli/live_feeder.cpp
    ${PROJECT_SOURCE_DIR}/examples/server_cli/one_group.cpp
    ${PROJECT_SOURCE_DIR}/examples/server_cli/timing_log.cpp
    ${PROJECT_SOURCE_DIR}/examples/server_cli/timing_stats.cpp
    ${PROJECT_SOURCE_DIR}/examples/server_cli/wav_reader.cpp
    ${PROJECT_SOURCE_DIR}/examples/server_cli/web_commands.cpp
    ${PROJECT_SOURCE_DIR}/examples/server_cli/web_ui.cpp
)
sendspin_server_cpp_configure_target(sendspin_server_cpp_tests)
include(${PROJECT_SOURCE_DIR}/cmake/embed_asset.cmake)
sendspin_server_cpp_embed_asset(sendspin_server_cpp_tests
    ${PROJECT_SOURCE_DIR}/examples/server_cli/web_assets/dashboard.html DASHBOARD_HTML
    dashboard_html.h)
# GCC reports ArduinoJson's CollectionIterator::nextId_ as maybe uninitialized where its object
# lookups inline, which most test files do through the raw client and the recorder; it is set on
# every path
target_compile_options(sendspin_server_cpp_tests PRIVATE
                       $<$<CXX_COMPILER_ID:GNU>:-Wno-maybe-uninitialized>)

target_include_directories(sendspin_server_cpp_tests PRIVATE ${PROJECT_SOURCE_DIR}/src
                                                         ${PROJECT_SOURCE_DIR}/examples/server_cli)

# White-box: test_dispatcher.cpp reads Dispatcher::token_ once the thread has ended: a later
# thread given the ended thread's token would show it, but a host need not hand a token out again.
set_source_files_properties(test_dispatcher.cpp PROPERTIES COMPILE_OPTIONS -fno-access-control)
# White-box: test_noise_initiator.cpp fixes the initiator's ephemeral key for the cacophony
# vectors through NoiseInitiator::state_, the only holder of the handshake state, and flips a byte
# of NoiseInitiator::payload_1_ or message_1_ to show the replay refusing a changed message 1.
set_source_files_properties(test_noise_initiator.cpp PROPERTIES COMPILE_OPTIONS -fno-access-control)
# White-box: test_pairing_crypto.cpp fixes a CPace run's scalar through CPace::scalar_ and
# public_share_ to reach the known answers, since start() samples the scalar from the CSPRNG.
set_source_files_properties(test_pairing_crypto.cpp PROPERTIES COMPILE_OPTIONS -fno-access-control)
# White-box: test_host_view.cpp leaves the outbox as a host call does between deciding a callback and posting
# it, so that close() racing that host call can be checked without a race. It sets
# HostView::lowering_ to a window of live writes already behind the input, so a latency cut is
# checked without waiting out the window in real time.
set_source_files_properties(test_host_view.cpp PROPERTIES COMPILE_OPTIONS -fno-access-control)
# White-box: test_group_audio.cpp swaps an instance's encoder (GroupAudio::instances_) for one that
# refuses, the only way to show a pipeline failure: no format the server selects makes an encoder
# or the resampler refuse. It calls GroupAudio::cut_to() on its own to extend a short stretch,
# which the stages never give, and sets HostView::p_pause_ to resume from a chosen position, which
# pause() takes from the time. It reads HostView::rebase_latency_us_, the `L_p` the pipelines
# publish for admission, which only shows in admission's bounds.
set_source_files_properties(test_group_audio.cpp PROPERTIES COMPILE_OPTIONS -fno-access-control)
# White-box: test_session.cpp queues a control message behind the initial server/activate through
# Session::queue_json(), since the server queues no other control message there.
# test_server_core.cpp reads ServerCore::targets_ to show a dial waiting out its backoff, and a
# lost instance's target gone with its wait, and a dial that waited not started once no longer due,
# which only the absence of a dial would otherwise show, calls ServerCore::prune_targets() with a chosen
# time, and shortens HandshakeBackoff::next_dial_us_ so the deferred dial needs no full wait; it
# also wraps a group's pipeline job (Group::audio_, AudioWorkers::Task::job_, replaced under the
# pool's mutex between two jobs) with a step that swaps its encoder (GroupAudio::instances_) for
# one that refuses on the group audio thread, to show core's handling of a pipeline failure.
# test_session_driver.cpp reads SessionDriver::writing_ and waiting_ and the session's fragment
# progress to send a goodbye while a write is stalled, and writing_ again at the close to tell
# which end closed, states nothing else shows.
# test_server_trust.cpp sets ServerCore::pairing_psks_, the state an operator-started Pairing PSK
# pairing sets, calls ServerCore::rekey() and handshake_psk(), which pairing drives, and reads
# ServerCore::records_ for a record, which no listener call carries.
# test_audio_workers.cpp wraps a group's pipeline job the same way, to hold it running while core
# erases the group, and reads AudioWorkers::Pool::stopping to let a job finish only once a stop
# has begun, which no observable state shows. Its stall tests set the state an epoch's start sets
# on a Group with no input (playing_, epoch_, anchor_, rings_, pipelines_.analyses), call its
# on_frame(), on_anchor_shift() and report_stalls() as the group-to-core FIFO and its timer do, and
# set Group::stalls_.reported_us to a chosen last report, so a held-back report is due within the
# test rather than after 10 s.
set_source_files_properties(test_session.cpp test_server_core.cpp test_session_driver.cpp
                            test_server_trust.cpp test_audio_workers.cpp
                            PROPERTIES COMPILE_OPTIONS -fno-access-control)

# sendspin_server_cpp_beast last: a system Boost's include directory (e.g. /opt/homebrew/include) must
# not shadow the fetched dependencies' headers.
target_link_libraries(sendspin_server_cpp_tests PRIVATE
    sendspin_server_cpp
    ArduinoJson
    noise_c
    micro_flac
    micro_opus
    micro-resampler
    sendspin
    # stb_image_write makes the test images
    sendspin_server_cpp_stb
    GTest::gtest
    sendspin_server_cpp_beast
)

# The watchdog in main.cpp prints a backtrace on a hang; glibc needs exported symbols to name
# the frames.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(sendspin_server_cpp_tests PRIVATE -rdynamic)
endif()

include(GoogleTest)
# Backstop above the 55 s watchdog in main.cpp, which fires first and prints the hung test's
# stack.
gtest_discover_tests(sendspin_server_cpp_tests PROPERTIES TIMEOUT 60)

# The soak (tests/soak.cpp): its own executable and entry point, whose watchdog follows the
# requested length. It skips itself unless SENDSPIN_SERVER_SOAK_SECONDS is set, so a plain ctest
# run reports it as skipped; `ctest -L soak` with the variable set runs it, with no CTest timeout.
add_executable(sendspin_server_cpp_soak soak.cpp raw_client.cpp)
sendspin_server_cpp_configure_target(sendspin_server_cpp_soak)
target_compile_options(sendspin_server_cpp_soak PRIVATE
                       $<$<CXX_COMPILER_ID:GNU>:-Wno-maybe-uninitialized>)
target_include_directories(sendspin_server_cpp_soak PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(sendspin_server_cpp_soak PRIVATE
    sendspin_server_cpp
    ArduinoJson
    noise_c
    micro_flac
    micro_opus
    sendspin
    sendspin_server_cpp_stb
    GTest::gtest
    sendspin_server_cpp_beast
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(sendspin_server_cpp_soak PRIVATE -rdynamic)
endif()
add_test(NAME Soak COMMAND sendspin_server_cpp_soak)
set_tests_properties(Soak PROPERTIES LABELS soak TIMEOUT 0
                     SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]")
