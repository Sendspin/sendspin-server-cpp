# Internals

How the parts of the server fit together: its threads, the channels between them, a connection's lifecycle, a group's playback and the order of work on the core thread. Facts that belong to one class are documented at that class.

## Threads

| Thread | Owns | Does |
| --- | --- | --- |
| Host threads | Nothing of the server's | Call `SendspinServer` and `GroupInput`; every call but `start()`, the destructor and `GroupInput::wait_writable()` returns without waiting |
| Core | The `EventLoop`, and through `ServerCore` every dial target, connection, `SessionDriver`, `Session` and `Group` | The messages and connection events the network thread hands over, decryption, protocol logic, frame rings, the send pump, stamps at take, encryption, timers |
| Network | The sockets of the run: the listener, the upgrades, the dials and the connections (`transport.cpp`) | All socket I/O, the HTTP upgrade and the WebSocket and closing handshakes, the receive stamps and the write stall; no protocol work. On ESP-IDF the transport's FreeRTOS tasks (`src/esp/`) do this |
| Group audio | Each group's pipelines (`GroupAudio`) and each routed source's decoder (`SourceDecoder`) | A fixed pool of threads (`AudioWorkers`) running the groups' pipeline jobs and the routed sources' decode jobs; a task's jobs run one at a time, each producing at most one frame per instance, and a task with more work goes behind the others |
| Dispatch | Nothing protocol-related | `ServerListener` and `PersistenceProvider` calls, the message trace and the log sink, one at a time, in posting order (`Dispatcher`) |
| Artwork worker | Its job queue | Decodes, fits, letterboxes and encodes artwork images and derives an album image's palette (`ArtworkWorker`), so core never decodes or encodes an image |
| Discovery | The `DiscoveryProvider`'s mDNS handles | Advertises `_sendspin-server._tcp`, browses `_sendspin._tcp` and resolves each instance; the built-in provider (`platform_discovery()`) runs dns_sd on macOS and avahi-client on Linux |

Asio's resolver runs host name lookups for `connect_url()` on a thread of its own. A client found by browsing is dialed at the addresses its resolve reported, never through the resolver.

## Cross-thread channels

| Channel | Writer | Reader | Mechanism |
| --- | --- | --- | --- |
| Host calls (`connect_url()`, `connect()`, an input's attachment, the pairing and group calls, `stop()`) | Host threads | Core | `EventLoop::post()` |
| Browse results and mDNS availability | Discovery | Core | The run's `DiscoverySink`, which posts each to `ServerCore` |
| Received messages with their receive stamps, and connection events (room, a write's completion, a close) | Network | Core | `InboundRing` (`inbound_ring.h`, independent of the transport): a bounded FIFO under one mutex. A message the ring refuses stays in its connection's read buffer, which is not read again until there is room |
| Commands to a connection (wait for room, write, close), a dial's start and cancel | Core | Network | Posts to the network `io_context`, holding the network half weakly |
| Listener events, traced messages and log lines | Core | Dispatch | `Dispatcher` FIFO, unbounded: the host sets the rate it drains at, and core never waits for it |
| Server key and client record load at `start()` | Host thread | Dispatch | A task on the `Dispatcher`; `start()` waits for its result |
| Client record saves, and their durability | Core, then dispatch | Dispatch, then core | A task on the `Dispatcher` per record change, in order; the outcome is posted back to core |
| `clients()`, `groups()` | Core | Host threads | `ClientSnapshot`, `GroupSnapshot`: a vector swapped in and copied out under a mutex (latest wins) |
| A group input's state: staged PCM and its segments, run and epoch, anchor and its shifts, drain and stop latch, group parameters | Host threads, core, group audio | Host threads, core, group audio | `HostView`: one mutex and a condition variable; each `GroupInput` call is one step under it |
| An input's `on_drained()` and `on_writable()` | Host threads and core, under the host-view mutex | Dispatch | The view's outbox, posted after the mutex is released |
| Host view changes the pipelines act on | Host threads, core | Group audio, core | The group's pipeline task (`AudioWorkers::Task::wake()`) and its `Wakeup`, preallocated, so `write()` never allocates |
| Group-to-core events and frames | The group's pipelines | Core | `GroupToCoreFifo`: an ordered FIFO with one posted drain handler |
| The pipeline set | Core | The group's pipelines | `PipelineSetSlot`, latest wins |
| Artwork images | Host threads (`set_artwork()`) | Core | `EventLoop::post()`; the image is copied, checked and hashed on the host thread |
| Artwork jobs and results | Core, then the artwork worker | The artwork worker, then core | `ArtworkWorker`'s bounded FIFO; results come back with `EventLoop::post()` |
| A routed source's stream items | Core | The route's decoder | `SourceFeed` (`source_feed.h`), bounded; faults come back through its fault list and the route's `Wakeup` |

Every library lock is a leaf: each is held only to copy or update its own state, never across a listener or provider call, a post, a signal, a send or another library lock, so there is no lock order.

## Run lifecycle

- `start()` initializes noise-c, starts the dispatch thread, loads or creates the server key and loads the client records there, builds the `ServerCore`, binds `ServerConfig::listen_port` (which starts the network thread), starts the discovery provider when the server advertises or browses, and starts the core thread. A built-in discovery provider that cannot start is reported as `ErrorKind::DISCOVERY_UNAVAILABLE` and the server runs without it.
- `stop()` posts the provider's `stop()` and `ServerCore::shutdown()` to core and never waits, so a listener may call it. Shutdown reports every connected client as stopped, closes the listener, every connection and dial and every input (so pending drains report `STOPPED`), ends the `Dispatcher` after the events already posted, and ends the loop.
- The destructor, and the next `start()`, join the threads of every run `stop()` ended: the core thread, then the group audio threads, then the dispatch thread.

## A connection

1. A connection starts from `connect_url()` (one dial target per URL), from browsing (one target per mDNS instance) or from a client that connects to the listener. The caps in `connection_limits` bound the dials and the accepted connections in their handshake.
2. A target is dialed when the operator asks (`connect_url()`, `connect()`) and when `dials_on_its_own()` (`discovery_policy.h`) holds. Failed dials and handshakes wait out the target's `HandshakeBackoff` (`backoff.h`).
3. A completed dial or accept becomes a `SessionDriver` over the `Connection`. The `Session` runs the Noise handshake through `client/hello`, with the server the initiator either way.
4. At `READY_FOR_ACTIVATION` `ServerCore` places the client in a group and has the session queue `server/activate` from `decide_activation()` (`playback_policy.h`), over the PSK the session matched and the client's approval. A client that may not play gets no activities or roles, and the host hears `on_approval_needed()`, `on_pairing_required()` or `on_credential_mismatch()`. The client counts as connected at its first `client/state` (or at once when the activation carries no roles); only then is the snapshot published and `on_client_connected()` posted.
5. Later activations declare `playback` while the client's group plays and `[]` otherwise.
6. When the session ends or the transport fails, the client leaves its group and is reported with `on_client_disconnected()`. A client that went away without a goodbye keeps its place in its group for `ServerTiming::group_rejoin_grace_us`.
7. A dialed connection's end decides whether its target is dialed again (`reconnect_after()`): a `restart`, a `concurrent_attempt` or a drop sets it reconnecting with a growing wait; any other goodbye, and the host's `disconnect()` or `cancel_connect_url()`, stops it.

Core publishes the snapshot before posting the event that reports a change, so a listener sees the change in `clients()` and `groups()`.

### Trust

At `client/init` the session asks which PSK message 1 references (`select_handshake_psk()`, `trust_policy.h`): the client record's long-term PSK, else a pairing PSK, else the Sentinel. When message 2 fails under a long-term or pairing PSK, the `NoiseInitiator` replays message 1 under the Sentinel with the same ephemeral key; success is the credential-mismatch signal. A long-term PSK is referenced only once the provider reports its save durable (`ServerCore::store_long_term_psk()`). `ServerCore::rekey()` re-handshakes an activated connection to another PSK.

### Pairing

`begin_pairing()` posts to core, which keeps one `ServerCore::Pairing` per client with its `PairingAttempt` (`pairing.h`). A client that is not connected is dialed for the pairing. A connection under another PSK than the method's is re-handshaken to it first. `judge_pairing_message()` decides each pairing message against the attempt's step:

| Step | Next accepted | Then |
| --- | --- | --- |
| `AWAIT_INIT` | `client/pair-pending`, `client/pair-init` | Pairing PSK: `AWAIT_FINALIZE`. Static code: `AWAIT_CODE`. Dynamic code: `server/pair-init` with `nonce_A`, `AWAIT_CODE` |
| `AWAIT_CODE` | The operator's code (`submit_pairing_code()`) | CPace as role A, `server/pair-auth`, `AWAIT_AUTH` |
| `AWAIT_AUTH` | `client/pair-auth` | `server/pair-confirm`, `AWAIT_CONFIRM` |
| `AWAIT_CONFIRM` | `client/pair-confirm`; `client/pair-retry` (dynamic code) | The key confirmation and the code's binding checked; a retry asks for the code again |
| `AWAIT_FINALIZE` | `client/pair-finalize` | The long-term PSK goes to `store_long_term_psk()`: `PERSISTING` |
| `PERSISTING` | `pair/abort` | Durable: `server/pair-finalize`, `PAIRED`, and a re-handshake to the new PSK. Otherwise nothing persists |

`unpair()` drops the client's long-term PSK and sends `server/unpair` to each session under it.

## A group's playback

1. `open_input()` creates the input's `HostView` on the host thread and posts its attachment to core. Attached, the group hands its `GroupAudio` to the group audio threads and writes its parameters (group lead, write-ahead) into the view as its members change them.
2. `write()` copies into the view's staging ring; the write that begins a run anchors it. `set_format()` starts an input segment at the head, so the timeline continues at the new rate. A live input's write also measures how much later its first sample lands than a fresh anchor would place it; once every write of a window kept such an excess, the view cuts it from the writes that follow and lowers `G_eff` with it (`sched/lowering.h`), so a latency a joiner, a stall or a fast clock raised comes back down within the epoch.
3. A `GroupAudio` job reads the view, posts the epoch events, shifts the timeline when a frame's lead falls too low (a rebase), produces at most one frame per pipeline instance and lets the view drop staged PCM no instance needs. Core's FIFO handler appends frames to the frame rings and pumps the members.
4. Each member's `PlayerStream` reads the group's frames through a cursor, packs chunks within its ledger, and emits `stream/start`, `stream/clear` and `stream/end` from markers at the head of its control FIFO. A `VisualizerStream` runs beside it on the same markers.
5. A drain completes once no member's stream is active; core ends the run before the view releases `on_drained`.

### Formats and pipelines

Each member's stream format comes from `select_player_format()` (`playback_policy.h`): the operator's format (`set_player_format()`), then the player's preference, when producible, else its first producible format at the input's rate, else its first producible format. `Group::recompute()` keeps one pipeline instance per distinct format, each with its own frame ring. An instance runs, as its format needs: the channel mix (`pipeline_stages.h`), the resampler (micro-resampler, `Resampler`), and the quantize stage and encoder (`FrameEncoder`: PCM, FLAC through micro-flac, Opus through libopus). Every instance cuts the same cells (`sched::CELL_US`), so a cell index carries the same timestamp in every format. A changed preference switches an active stream in place at a frame boundary of both formats (`sched::plan_switch()`). An instance added while an epoch plays catches up from the input the staging ring still holds.

### Artwork, colors and visualizer

`Group` holds an `ArtworkTimeline` (`artwork.h`) per image source and its colors (`ScheduledUpdates<ColorState>`). Encodings come from the run's `ArtworkCache`, which posts misses to the artwork worker; each session's `ArtworkStream` is its artwork queue. Each distinct visualizer configuration is an analysis in the pipeline set, run by `GroupAudio` beside the frame instances (`VisualizerAnalysis`, its FFT behind `RealFft`).

### Routed sources

`route_source()` opens a live input on the group and posts the route to core (`server_core_source.cpp`). The bound connection's `SourceStream` (`source_stream.h`) emits the source's `start` and `stop` and pushes the stream's items into the route's `SourceFeed`; the route's `SourceDecoder`, on the group audio threads, decodes them and writes the PCM through a `LiveSourceSink` into the input.

## Work on the core thread

Receive: the network thread stamps a message when its last byte is read and pushes it into the inbound ring; core's drain delivers it to the driver. `Session` checks the phase, decrypts, traces the plaintext, reassembles fragments and dispatches.

Send: after every event the driver runs its pump. With a message to send and no wait or write outstanding, it asks the transport for room. When room is reported, `Session::take_message()` selects through `sched::select_next()` (the rest of a fragmented message, then the time queue, then the control FIFO, then an aged artwork message, then the player and visualizer cursors by earliest deadline, then artwork), builds the message, reads the stamp, encrypts and returns it; the next wait starts only after the write completes. The transport closes a connection whose wait or write stays outstanding for the write-stall time; a send queue that passes its cap (`SendQueueCaps`) ends the session.

## The send-path core

`src/sched/` (namespace `sendspin::server::sched`) holds the send path's decisions as value types and free functions over plain data, with no I/O, threads, clock reads or locks: each takes `now` as an argument.

| File | Holds |
| --- | --- |
| `constants.h` | The send path's constants and the tunable `SchedTiming` defaults |
| `types.h` | Run, epoch and instance ids, formats, roles, and the input, run and drain states |
| `frames.h` | The frame grid (`frame_layout()`), the `FrameRing`, the `Cursor` and its switch point (`plan_switch()`) |
| `timeline.h` | The `Anchor` with its segments and shifts: timestamps, positions, the stall rebase and the pause position |
| `leads.h` | Per-player leads, pipeline latency, the group lead, the startup bound and the write-ahead |
| `ledger.h` | `Ledger`, a session's byte accounting for one stream role |
| `chunk.h` | The chunk header and `send_ahead`, producibility, format selection and `pack_chunk()` |
| `admission.h` | Write admission and readiness |
| `lifecycle.h` | Desired state, markers and the drain predicates |
| `lowering.h` | Bringing a live input's raised latency back down within an epoch |
| `run_view.h` | The host view's run, stop latch and staged drain |
| `selection.h` | The pump's `select_next()`, `item_ready()`, `next_wake()` and the drain watch |

## ESP-IDF

On ESP-IDF the library is a component (`cmake/esp_idf.cmake`, sources from `cmake/sources.cmake`) built with the embedded resource profile, `src/profile.h`: smaller write-ahead, staging ring and connection caps, one group audio thread and no Opus. The host runs its whole test suite with the same values (`-DSENDSPIN_SERVER_EMBEDDED_PROFILE=ON`). The transport is native (`src/esp/`): dials on esp_websocket_client, accepts on esp_http_server, a writer task per connection, all handing received messages to core through the same inbound ring. `src/esp/memory_esp.cpp` wraps the whole application's `pthread_mutex_init()` and `pthread_cond_init()` at link time, so another component that wraps either symbol will not link with it.

## Known limitations

- **A routed source plays on the server's timeline.** Its audio goes into an ordinary live input and plays the group's lead after it arrives; its capture times are used only to drop a stall's backlog and to end the run at a long gap, and nothing corrects the source's clock against the server's.
- **A live input's latency falls in one cut.** The audio it drops is heard as a skip, as a rise is heard as a gap.
- **A live input's silence is one gap in its run.** A live input that receives no writes keeps its run, and its next write moves the timeline forward by the silence; a host whose source can go silent should call `GroupInput::stop()` and let its next samples start a new run.
- **An Opus stream starts with silence**, the encoder's lookahead at each new Opus instance.
- **A group's fifth visualizer configuration gets no stream.** Past `MAX_GROUP_ANALYSES`, a configuration fails for the rest of the run.
- **One peer can keep client-initiated connections refused** by holding `connection_limits::MAX_ACCEPTED_HANDSHAKES` accepted connections in their handshake. Dialed clients are unaffected.
- **A client that ignores `server/unpair` keeps its connection** until it closes it.
- **A connection waiting for room in the inbound ring is not read,** so the messages behind a held-back message are stamped late, and its pings go unanswered while core is behind.
