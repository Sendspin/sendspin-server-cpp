# The library's source lists, shared by the host build (CMakeLists.txt) and the ESP-IDF component
# (cmake/esp_idf.cmake). Paths are relative to the repository root; each build prepends its own.
#
#   SENDSPIN_SERVER_SOURCES       - every platform: the library itself
#   SENDSPIN_SERVER_STB_SOURCES   - stb_image's allocator and its implementation (built as their
#                                   own targets on a host, with stb's warnings off)
#   SENDSPIN_SERVER_HOST_SOURCES  - a host's platform files: the transport on Asio and Beast, the
#                                   pffft FFT, plain std::threads, one heap
#   SENDSPIN_SERVER_ESP_SOURCES   - ESP-IDF's platform files (src/esp/): the transport on
#                                   esp_websocket_client and esp_http_server among them, and no
#                                   mDNS backend; the host's mDNS backends are chosen in
#                                   CMakeLists.txt

set(SENDSPIN_SERVER_SOURCES
    # Artwork and colors: the role's values, the per-session stream, core's cache of encodings,
    # the worker thread that decodes and encodes, and the color palettes
    src/artwork.cpp
    src/artwork_cache.cpp
    src/artwork_stream.cpp
    src/artwork_worker.cpp
    src/audio_workers.cpp
    src/backoff.cpp
    src/client_records.cpp
    src/clock.cpp
    src/colors.cpp
    src/controller.cpp
    src/crypto/base64url.cpp
    src/crypto/cpace.cpp
    src/crypto/keys.cpp
    src/crypto/pairing_code.cpp
    src/crypto/pairing_token.cpp
    src/crypto/psk_wrap.cpp
    src/discovery_policy.cpp
    src/dispatcher.cpp
    src/fragmentation.cpp
    src/frame_encoder.cpp
    # A group on core, its pipelines and visualizer analyses (run on the group audio threads as
    # budgeted jobs) and its input's host view
    src/group.cpp
    src/group_audio.cpp
    src/host_view.cpp
    src/image_codec.cpp
    # How the transport's network threads hand messages and events to core
    src/inbound_ring.cpp
    # A connection's message held for the inbound ring, for a transport whose receive task reads
    # for several connections
    src/inbound_hold.cpp
    # A live input played fast while its latency comes down
    src/live_slew.cpp
    src/log.cpp
    src/messages.cpp
    src/metadata.cpp
    src/noise.cpp
    # One pairing attempt: the decisions as pure functions, and its state and crypto
    src/pairing.cpp
    src/pcm_convert.cpp
    src/pipeline_stages.cpp
    src/playback_policy.cpp
    src/player_stream.cpp
    # The send path's pure core: value types and functions over plain data, no I/O or threads
    src/sched/admission.cpp
    src/sched/chunk.cpp
    src/sched/frames.cpp
    src/sched/leads.cpp
    src/sched/ledger.cpp
    src/sched/lifecycle.cpp
    src/sched/lowering.cpp
    src/sched/run_view.cpp
    src/sched/selection.cpp
    src/sched/timeline.cpp
    src/send_queues.cpp
    src/server.cpp
    src/server_core.cpp
    src/server_core_pairing.cpp
    src/server_core_source.cpp
    src/session.cpp
    src/session_driver.cpp
    # The source role: a session's commands and input stream, the feed to a routed source's
    # decoder on the group audio threads, and the sink that corrects its clock and writes its PCM
    # into a group's input
    src/source_clock.cpp
    src/source_decoder.cpp
    src/source_feed.cpp
    src/source_sink.cpp
    src/source_stream.cpp
    src/trust_policy.cpp
    # The visualizer role: its values, the features computed from the group's PCM, and the
    # per-session stream
    src/visualizer.cpp
    src/visualizer_analysis.cpp
    src/visualizer_stream.cpp
    # parse_ws_url(), which every transport shares
    src/transport_url.cpp
    # WebSocket framing, one write's messages as frames back to back and a dial's opening
    # handshake, for a transport that frames and dials itself (the ESP-IDF one)
    src/ws_batch.cpp
    src/ws_frame.cpp
    src/ws_handshake.cpp
)

set(SENDSPIN_SERVER_STB_SOURCES
    src/stb_budget.cpp
    src/stb_implementation.cpp
)

set(SENDSPIN_SERVER_HOST_SOURCES
    # The WebSocket transport on Asio and Beast: with beast_separate_compilation.cpp, the only
    # sources that include them
    src/transport.cpp
    # The one translation unit that compiles Boost.Asio and Boost.Beast
    src/beast_separate_compilation.cpp
    # The visualizer's FFT seam on pffft
    src/fft_pffft.cpp
    # The library's threads as std::thread starts them
    src/thread_setup_host.cpp
    # Placed allocation on one heap
    src/memory_host.cpp
)

set(SENDSPIN_SERVER_ESP_SOURCES
    # The transport: the core task's loop on FreeRTOS and esp_timer, a connection's core half and
    # the link its receive and writer tasks share, dials on esp_websocket_client and the listener
    # on esp_http_server
    src/esp/connection_esp.cpp
    src/esp/dial_esp.cpp
    src/esp/event_loop_esp.cpp
    src/esp/listener_esp.cpp
    src/esp/ws_link.cpp
    # The visualizer's FFT seam on esp-dsp
    src/esp/fft_esp_dsp.cpp
    # Placed allocation through heap_caps, the memory statistics, the internal heap a new
    # connection needs, and the pthread init wraps (cmake/esp_idf.cmake)
    src/esp/memory_esp.cpp
    # noise-c's RNG hook and its AES-GCM cipher on mbedtls
    src/esp/noise_esp.cpp
    # Each library thread's stack, core and priority
    src/esp/thread_setup_esp.cpp
    src/discovery/no_discovery.cpp
)
