# sendspin-server-cpp

C++ library implementing the server side of the Sendspin synchronized audio streaming protocol, for macOS and Linux (it also builds as an ESP-IDF component). `SendspinServer` (`include/sendspin/server/server.h`) advertises and browses over mDNS, accepts and dials clients, completes the Noise handshake, pairs and approves clients, keeps them time-synced and in groups, and streams each group's host input to its players as PCM, FLAC or Opus, with the controller, metadata, artwork, color, visualizer and source roles. `README.md` has the feature list and the commands.

## Documents

- `docs/internals.md`: the threads, the cross-thread channels, a connection's lifecycle, a group's playback and the order of work on the core thread. Keep it in step with any change to them.
- `docs/comment-style.md`: the rules for every comment, doc comment and document.
- `docs/conventions.md`: the normative design standards. The review checklists that apply them to a diff are in `.claude/skills/`: `docs-sync`, `house-patterns`, `realtime-review` and `test-standards`.

## Project layout

```text
include/sendspin/server/  - Public API headers (server.h, config.h, types.h, discovery.h, persistence_keys.h)
src/                      - Library sources and private headers (Key sources below)
src/crypto/               - Keys, peer ids, the Sentinel PSK, base64url, SHA-256 and SHA-512, CPace, pairing codes and tokens, PSK wrapping
src/discovery/            - platform_discovery(): dns_sd (macOS), avahi-client (Linux), or none
src/esp/                  - The ESP-IDF build's platform files: the native transport (event loop on FreeRTOS and esp_timer, dials on lwIP's sockets, accepts on esp_http_server), thread placement and the task token, the esp-dsp FFT, the noise-c hooks (AES-GCM, X25519, RNG), and placed allocation, the internal heap reading and the pthread init wraps (`memory_esp.cpp`)
src/sched/                - The send path's pure core: timeline, leads, ledger, packing, admission, markers, selection
src/ORIGIN.md             - The sources copied or adapted from sendspin-cpp, with the commit
cmake/                    - dependencies.cmake, noise_c.cmake, test_dependencies.cmake, embed_asset.cmake (a file as a C++ byte array, for the dashboard page), sources.cmake (the source lists the host and ESP-IDF builds share), esp_idf.cmake (the ESP-IDF component)
tests/                    - Host unit tests (GoogleTest) behind the main.cpp hang watchdog, and the opt-in soak (soak.cpp)
tests/esp_idf/            - A minimal ESP-IDF project that builds the component, for CI (esp32s3, and esp32p4 with PSRAM and memory statistics)
tests/consumer/           - A project that consumes the library through FetchContent, for CI
examples/server_cli/      - The reference host: a REPL on the public API that plays, queues and loops WAV and FLAC files (read as they play) and plays live PCM from standard input, with file-backed persistence, --opus, --one-group, a timing log and a browser dashboard (--web: web_ui.* on Beast, web_commands.*, web_assets/dashboard.html, web_protocol.md)
docs/conventions.md       - The normative design standards
docs/comment-style.md     - The rules for every comment, doc comment and document
docs/internals.md         - Threads, cross-thread channels, a connection's lifecycle, a group's playback, core thread order
third_party/              - Vendored micro-resampler and stb (each with its VENDORED.md); re-vendor, never edit
NOTICES.md                - The licenses of the vendored and fetched dependencies
.claude/skills/           - Review checklists applying docs/conventions.md to a diff (docs-sync, house-patterns, realtime-review, test-standards)
script/clang-tidy.sh      - clang-tidy over src/ using build/compile_commands.json
```

## Key sources

- `include/sendspin/server/server.h`: `SendspinServer`, `ServerListener` and `PersistenceProvider`, with the threading contract; `config.h` holds `ServerConfig`, the message trace types and the log level and sink; `discovery.h` the mDNS `DiscoveryProvider` a host may supply and `platform_discovery()`; `persistence_keys.h` the keys and blob sizes the provider sees (the server key and the client records, with their layout).
- `server.cpp`: `SendspinServer`'s run lifecycle (start, stop, joins) and the server key's load or creation.
- `server_core.h`: `ServerCore`, the core thread's listener, dial targets (URLs and clients found by browsing), connections, groups, client records, approvals and the PSKs handshakes reference (with a long-term PSK used only once saved), re-handshakes, pairings and unpairing (`server_core_pairing.cpp`; `server_core_client.h` holds a connection's `ServerCore::Client` and the scans of a client_id's connections and targets), departed members held for their rejoin grace, the host's groups (`create_group()`, `move_client()`, `rename_group()`), the host's player formats (`set_player_format()`), controller commands and the caps on connections in their handshake and on activated accepted ones (`connection_limits`), which reports to the host through the dispatch thread and publishes the `clients()` and `groups()` snapshots.
- `discovery_policy.h`: dial intent and the first and later activations' `playback`, `DiscoveryEntry`, the reconnect policy per connection end, when a target is dialed on the server's own, and the mDNS values advertised and kept, as pure functions.
- `dispatcher.h`: the dispatch thread's FIFO.
- `ring.h`: `Ring`, a FIFO of fixed-size records in one placed array, fixed at a cap or growing only past its first capacity; `task_queue.h`: `TaskQueue`, a FIFO of closures each moved into placed blocks behind a small header, which `EventLoop::post()` and `Dispatcher::post()` fill without an allocation of their own.
- `log.h`: `Logger` and the `SS_LOGE`/`SS_LOGW`/`SS_LOGI` macros: lines formatted where logged and handed to `ServerConfig::log` on the dispatch thread.
- `send_queues.h`: a connection's time queue and control FIFO (messages, activations and desired-state markers) and their caps; selection goes through `sched::select_next()`.
- `group.h`: `Group`, one group's members, its input, its pipelines' events, frame rings and drain on core, and its controller and metadata state; `GroupMember` is the view of a client it needs.
- `host_view.h`: `HostView`, the host side of one input under one mutex: its kind (buffered or live), staging ring and its input segments, runs and epochs, anchoring and the anchor's shifts, drain, stop latch, the group parameters (`GroupParams`, in `group_params.h`) and a live input's latency slew.
- `live_slew.h`: `LiveSlew`, a live input played slightly fast while its raised latency comes down (`sched/lowering.h` decides how fast): micro-resampler's ASRC on a host, single frames dropped on the embedded profile.
- `group_audio.h`: `GroupAudio`, a group's pipelines and visualizer analyses, one job at a time, with the group-to-core FIFO and the pipeline-set slot.
- `audio_workers.h`: `AudioWorkers`, the group audio threads: a fixed pool that runs each group's pipeline jobs off core, a group's jobs one at a time, groups taking turns.
- `player_stream.h`: `PlayerStream`, one member's player stream: markers, cursor, ledger, chunk packing and the switch to another format in place, over a `StreamSource` (`stream_source.h`).
- `playback_policy.h`: a session's activation (`decide_activation()`), a player's stream format, the group parameters and what a host's move does with a client (`move_action()`), as pure functions.
- `pairing.h`: one pairing attempt: which pairing message fits its step (`judge_pairing_message()`), the activation's code format, the code an operator types, as pure functions, and `PairingAttempt`, the attempt's state and CPace run as the server.
- `trust_policy.h`: the PSK a handshake references (`select_handshake_psk()`), the session's matched PSK (`PskMatch`) and the activity sets a matched PSK allows, as pure functions.
- `client_records.h`: the client records (each client's approval, long-term PSK, last name and unrecognized identifiers) and their fixed-size blob codec and slots (`persistence_keys.h`).
- `source_stream.h`: `SourceStream`, one session's source role: the start and stop commands, decided at the marker, and the input stream the client opens, replaces and ends, with what each message does. `server_core_source.cpp`: `ServerCore`'s routes, the connection bound to each and what the host hears.
- `source_feed.h`: `SourceFeed`, the bounded FIFO a routed source's stream items cross from core to its decoder, and its faults back.
- `source_decoder.h`: `SourceDecoder`, a route's job on the group audio threads (PCM, FLAC with micro-flac, Opus with libopus), and `source_input_format()`.
- `source_sink.h`: `SourcePcmSink`, the sink seam, and `LiveSourceSink`, which writes the decoded PCM into the group's live input, correcting the source's clock, with `judge_source_chunk()` (stale backlog, long gaps) as a pure function.
- `source_clock.h`: `SourceClock`, a source's phase loop against its capture times (a pure value type), and its two corrections, `SampleSlip` (single frames dropped or repeated, the embedded profile's) and `SourceAsrc` (micro-resampler's ASRC, a host's).
- `controller.h`: a group's controller state, the group volume algorithm, command filtering, the seek checks and the switch cycle, as pure functions.
- `metadata.h`: `MetadataTimeline`, a group's current metadata and scheduled update, and the states a pause and a resume send.
- `scheduled_updates.h`: `ScheduledUpdates`, a current state and at most one scheduled update, sent at most 20 s ahead; the metadata and color schedules.
- `artwork.h`: the artwork role's wire messages, channel declarations, the letterbox fit, `EncodedImage` and `ArtworkTimeline` (a group's current and scheduled image per source), as pure functions and values.
- `artwork_stream.h`: `ArtworkStream`, one session's artwork queue: the stream's lifecycle, each channel's announces, parts, cancels and clears, pacing and aging.
- `artwork_cache.h`: `ArtworkCache`, the run's encodings and palettes keyed by image hash, format and size, posting misses to the worker.
- `artwork_worker.h`: `ArtworkWorker`, the thread that decodes, fits and encodes images and derives palettes, and `run_artwork_job()`.
- `image_codec.h`: JPEG and PNG probing (header limits, JPEG scan count), decoding within a memory budget and the fitted, letterboxed encoding (stb); with `stb_implementation.cpp`, the only sources that include stb. `stb_budget.h` is the per-thread budgeted allocator stb_image allocates through.
- `visualizer.h`: the visualizer role's values: a client's request, the stream configuration the server derives from it (`negotiate_visualizer()`) and its key, the binary messages' sizes.
- `visualizer_analysis.h`: the visualizer's features (loudness, `f_peak`, the spectrum's display bins, onset peaks) from PCM in single precision, the analysis instants and window, the onsets' time-domain A-weighting (`AWeightingFilter`), and `VisualizerAnalysis`, one configuration's analysis state, which `GroupAudio` runs as an analysis instance.
- `visualizer_stream.h`: `VisualizerStream`, one session's visualizer stream: its markers' part, cursor over the group's analysis frames, ledger and configuration change in place, over a `StreamSource`.
- `fft.h`: `RealFft`, the visualizer's FFT seam; `fft_pffft.cpp` is the host's implementation (pffft), and another platform supplies its own source for it.
- `colors.h`: WCAG contrast, `with_contrast()` (the color role's guarantees), the median-cut palette and the colors derived from it.
- `frame_encoder.h`: `FrameEncoder`, a pipeline instance's quantize stage and its PCM, FLAC (micro-flac) or Opus (libopus, from micro-opus) encoder; an output format's shape, `codec_header` and encoder delay.
- `pipeline_stages.h`: a pipeline instance's stages before the quantize stage: the channel mix's fixed matrix and `Resampler`, micro-resampler as the pipelines configure it.
- `pcm_convert.h`: the quantize stage: the host's PCM at a player's bit depth; and the host's PCM as floats for the stages before it.
- `backoff.h`: the handshake backoff and reconnect wait per dial target, and how a connection's end counts toward it and is reported (`is_handshake_failure()`, `error_before_activation()`, `disconnect_reason()`).
- `transport.h`: the WebSocket transport (`Connection`, `Listener`, `dial()`), the core thread's `EventLoop`, its `Timer` and `Wakeup`, with no Asio in the interface (`message_kind.h` holds `MessageKind`); on a host `transport.cpp` implements it on Asio and Beast, with every socket on its network thread, and `transport_asio.h` holds the host's `EventLoop::Impl` (its io_context); with `beast_separate_compilation.cpp`, the only sources that include Asio or Beast. On ESP-IDF `src/esp/` implements it natively (`esp_loop.h` the `EventLoop::Impl`, `ws_link.*` a connection's tasks, `connection_esp.*`, `dial_esp.cpp`, `listener_esp.cpp`), writing each write's frames from a `WsBatch` (`ws_batch.h`), framed, parsed and assembled into messages (`WsAssembly`) by `ws_frame.h` (a dialed connection's masked, its keys read off a ChaCha20 keystream, `WsMaskKeys`), with a dial's opening handshake from `ws_handshake.h`, all three platform-free and host-tested; `transport_url.cpp` (`parse_ws_url()`), `url_text.h` (the IPv4, IPv6 and zone text a dial URL's bracketed host and a browsed address are checked against, with no platform parser) and `ws_host_value()` (the Host header, in `ws_handshake.h`) are shared.
- `inbound_hold.h`: `InboundHold`, a connection's message held for the inbound ring by a transport whose receive task reads for several connections (the ESP-IDF one): who pushes it, the pause tied to it, the close after it; platform-free and host-tested.
- `inbound_ring.h`: `InboundRing`, how a transport's network threads hand received messages, with their receive stamps, and connection events to core: bounded, a message it refuses held back in the transport, independent of any transport.
- `clock.h`: `now_us()`, the clock every timestamp is taken from.
- `profile.h`: the build's resource profile (write-ahead, staging ring, connection caps, the internal RAM a new connection needs, inbound ring, FLAC LPC, a source's clock correction and a live input's slew, audio threads, where each task's stack lives), the one place `SENDSPIN_SERVER_EMBEDDED_PROFILE` changes the library's limits.
- `memory.h`: placed allocation, each naming the memory it prefers (`MemoryLocation`: internal RAM or PSRAM, the other as the fallback): `placed_malloc()`, `PlacedAllocator` and `ExternalBytes` for containers, `make_placed_array()`, `make_shared_external()`, the `ExternalNew` base, and `memory_stats()`; `src/esp/memory_esp.cpp` on ESP-IDF (heap_caps, whatever the project's malloc policy), `memory_host.cpp` on the host (one heap).
- `shared.h`: `SharedPtr`, `WeakPtr` and `EnableSharedFromThis`, the transport's shared pointers: with the atomic lock policy where libstdc++ is built without it (the ESP-IDF riscv32 toolchain), `std::shared_ptr` elsewhere.
- `thread_setup.h`: `start_thread()`, each library thread's name and placement, and `current_thread_token()`, the calling thread's identity from any task; `thread_setup_host.cpp` on the host, `src/esp/thread_setup_esp.cpp` on ESP-IDF (core, priority, and the stack in internal RAM or PSRAM as `profile.h` places it; the token is the FreeRTOS task handle), with the connections' writer tasks.
- `session.h`: `Session`, one connection's protocol state from `client/init` through activation, time sync and re-handshakes, with no I/O: it takes inbound messages and deadlines and yields outbound messages, stamped and encrypted when taken (`SessionPhase`, `SessionTrace`, `CredentialSource`); `session_end.h` holds `SessionEnd` and `SessionOutcome`, the value a session's end is reported by.
- `session_driver.h`: `SessionDriver`, which runs a `Session` over a `Connection` under the write invariant and reports `on_ready` and `on_closed` to a `SessionObserver`.
- `noise.h`: `NoiseInitiator` (the server's side of KKpsk2, with the Sentinel Fallback's replay of message 1) and `TransportCiphers`; `noise_types.h` holds the cipher suite and PSK category names, `HandshakePsk` and the handshake hash.
- `fragmentation.h`: inbound `Reassembler` and outbound `write_fragment()`.
- `byte_order.h`: `put_be()`, `append_be()` and `read_be()`, the binary messages' big-endian fields.
- `sched/`: the send path's decisions as value types and free functions over plain data, each taking `now`; `docs/internals.md` lists its files.
- `messages.h`: builders for what the server sends (`server/state` and `server/command` included) and validating parsers for what a client sends (`client/command` and `client/leave` included), as pure functions over typed structs; `rule_for_unhandled()` applies the ignore rules.

## Build

- Library target `sendspin_server_cpp` (alias `sendspin_server_cpp::sendspin_server_cpp`); CMake options are prefixed `SENDSPIN_SERVER_`. `SENDSPIN_SERVER_ENABLE_WERROR` (default off) makes warnings in this project's targets errors; CI builds with it on, so local builds should too. `SENDSPIN_SERVER_EMBEDDED_PROFILE` (default off) builds with `src/profile.h`'s embedded limits, as the ESP-IDF component (`cmake/esp_idf.cmake`) always does.
- Plain: `cmake -B build && cmake --build build`, which builds `examples/` too (`SENDSPIN_SERVER_BUILD_EXAMPLES`, default on when top-level). Uses ccache when installed (`SENDSPIN_SERVER_USE_CCACHE`, default on when top-level). `SENDSPIN_SERVER_ENABLE_DISCOVERY` (default on) builds the mDNS backend: dns_sd on macOS, avahi-client through pkg-config on Linux (`libavahi-client-dev`). `SENDSPIN_SERVER_ENABLE_OPUS` (default on) builds the Opus encoder and the source role's Opus decoder; off compiles them out (`src/frame_encoder.cpp` and `src/source_decoder.cpp` are the places it is guarded) and is refused with the tests on.
- Tests never use the network's mDNS: `tests/test_recorder.h` `local_config()` turns advertising and browsing off and listens on a port the system picks, and `test_server_discovery.cpp` supplies its own `DiscoveryProvider` through `ServerConfig::discovery`.
- Tests: `cmake -B build-asan -DSENDSPIN_SERVER_ENABLE_WERROR=ON -DSENDSPIN_SERVER_BUILD_TESTS=ON -DSENDSPIN_SERVER_ENABLE_SANITIZERS=ON`, then `cmake --build build-asan` and `ctest --test-dir build-asan --output-on-failure`. The tests also build `server_cli`'s WAV and FLAC readers, feeders, `--one-group`, timing figures and log and `--web` dashboard; `tests/temp_dir.h` gives a test a temporary directory. `tests/test_ports.h` hands out loopback ports for test peers; `tests/raw_client.h` is the raw test client (noise-c and Beast, either suite, scriptable misbehavior), `tests/silent_listener.h` a listener that holds the server's dials in their upgrade or handshake, `tests/test_recorder.h` a recording listener and an in-memory provider, `tests/flac_reader.h` decodes a FLAC stream as a player gets it, `tests/opus_reader.h` an Opus stream, with the test tones and an SNR to compare lossy audio by, `tests/player_sink.h` holds the test signal and a real-time audio output for sendspin-cpp players, `tests/peer.h` what each in-process sendspin-cpp client shares (its network provider and blob store, its config and the URL the server dials, its main loop run on the test thread, the client_id at a URL), and `tests/trust_fixtures.h` distinct PSKs and the client record a server saved.
- Soak: `tests/soak.cpp` (its trace checker in `tests/soak_wire_checker.h`, tested in the default suite), target `sendspin_server_cpp_soak` with its own entry point, registered as the CTest test `Soak` (label `soak`), which skips itself unless `SENDSPIN_SERVER_SOAK_SECONDS` is set (`SENDSPIN_SERVER_SOAK_SEED` picks the script); README.md has the command.
- CI also builds the library with Opus and discovery off and memory statistics on; as a subproject (`tests/consumer/`); and as the ESP-IDF component (`tests/esp_idf/`: `idf.py -C tests/esp_idf set-target esp32s3`, then `idf.py -C tests/esp_idf build`).
- Memory statistics: `SENDSPIN_SERVER_MEMORY_STATS` (default off; `CONFIG_SENDSPIN_SERVER_MEMORY_STATS` in the component's `Kconfig`) logs the heaps and the tasks' stacks at the server's start (on the thread calling `start()`) and, on the core thread, at a connection's start and end and a group's playback (`profile::MEMORY_STATS`); the tests count log lines and expect it off.
- Embedded profile: `cmake -B build-embedded -DSENDSPIN_SERVER_ENABLE_WERROR=ON -DSENDSPIN_SERVER_BUILD_TESTS=ON -DSENDSPIN_SERVER_ENABLE_SANITIZERS=ON -DSENDSPIN_SERVER_EMBEDDED_PROFILE=ON`, then `cmake --build build-embedded` and `ctest --test-dir build-embedded --output-on-failure`: the host suite with `src/profile.h`'s embedded limits, as the ESP-IDF build has them (CI runs it on Linux). A test that depends on a profile limit derives its expectation from `profile.h` (or the constant that reads it), so the suite passes in both profiles.
- ThreadSanitizer: `-DSENDSPIN_SERVER_ENABLE_TSAN=ON` in its own build directory (applied to every target, including fetched dependencies; exclusive with `SENDSPIN_SERVER_ENABLE_SANITIZERS`); run with `TSAN_OPTIONS=halt_on_error=1`. CI runs it on Linux. Never run the ASan and TSan suites concurrently.

## Dependencies

All pinned; see `cmake/`.

- Boost.Beast and Boost.Asio, host only: the `-cmake` release tarball (URL and SHA256), or a system Boost at or above the pin. Compiled once in `src/beast_separate_compilation.cpp`; any translation unit that includes them links `sendspin_server_cpp_beast` for the separate-compilation definitions. Within the library, only that file, `src/transport.cpp` and `src/transport_asio.h` include Asio or Beast; `src/transport.h` names no Asio type. Tests that run Asio objects on the core thread reach its io_context through `transport_asio.h`. `server_cli`'s dashboard (`web_ui.cpp`) includes them too, with ArduinoJson for its messages.
- ESP-IDF: its `lwip` and `esp_http_server` for the transport.
- ArduinoJson, at the sendspin-cpp pin.
- noise-c (esphome-libs fork, the sendspin-cpp pin) built as target `noise_c` from a manual source list: reference backend plus OpenSSL 3 AES-GCM. Initialized with `noise_init_framework()`. On ESP-IDF it is the `esphome/noise-c` component (libsodium-backed) instead, with its name tables and the AESGCM id turned on by `cmake/esp_idf.cmake` and weak hooks in `src/esp/noise_esp.cpp`. A `noise_c` target a parent project defined first is used instead, and must define `NOISE_USE_AES=1`.
- micro-flac at its v0.3.0 release, declared before sendspin-cpp so its declaration wins; `server_cli` links it too, to decode FLAC files.
- micro-opus `v0.3.5`, the sendspin-cpp pin, declared before sendspin-cpp too: the server encodes with the libopus it builds (fixed point, a pseudostack per thread), and the test peer decodes with it.
- micro-resampler, vendored; its tests and native tuning are forced off and its QUAD precision path is compiled out. A `micro-resampler` target a parent project defined first is kept.
- pffft (marton78/pffft, pinned by commit), the visualizer's FFT on the host: its single-precision real FFT only, built from its two sources as target `sendspin_server_cpp_pffft` with its warnings off; only `src/fft_pffft.cpp` includes it.
- stb (`stb_image.h`, `stb_image_resize2.h`, `stb_image_write.h`), vendored; compiled once in `src/stb_implementation.cpp` as target `sendspin_server_cpp_stb`, with its warnings off and JPEG and PNG only; stb_image allocates through `sendspin_server_cpp_stb_budget` (`src/stb_budget.cpp`).
- mDNS: the system's dns_sd on macOS; avahi-client on Linux, found with pkg-config (not fetched).
- Tests only: GoogleTest and the sendspin-cpp test peer (target `sendspin`, with its Opus decoder), pinned by commit; override with `-DFETCHCONTENT_SOURCE_DIR_SENDSPIN_CPP=<path>`. Under `SENDSPIN_SERVER_ENABLE_SANITIZERS` the peer's IXWebSocket is instrumented too.

## Coding conventions

- C++20, namespace `sendspin::server`.
- `docs/comment-style.md` for every comment; ASCII punctuation only (no em or en dashes).
- Apache 2.0 license header on every source file.
- clang-format (v18, `.clang-format`), clang-tidy (`.clang-tidy`), markdownlint (`.markdownlint.yaml`); hooks in `.pre-commit-config.yaml`.
- Tests are host-only and white-box. A test file that reaches private members compiles with `-fno-access-control`, set per file in `tests/CMakeLists.txt` with the reason; never add a seam to production code.
