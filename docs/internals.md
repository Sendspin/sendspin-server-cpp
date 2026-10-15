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

Asio's resolver runs host name lookups for `connect_url()` on a thread of its own. A client found by browsing is dialed at the literal addresses its resolve reported, never through the resolver.

## Cross-thread channels

| Channel | Writer | Reader | Mechanism |
| --- | --- | --- | --- |
| Host calls (`connect_url()`, `connect()`, an input's attachment, the pairing and group calls, `stop()`) | Host threads | Core | `EventLoop::post()` |
| Browse results and mDNS availability | Discovery | Core | The run's `DiscoverySink`, which posts each to `ServerCore` |
| Discovery's stop | Core (or the host thread in `start()`) | Discovery | The provider's `stop()`, which wakes and joins a built-in provider's thread |
| Received messages with their receive stamps, and connection events (room, a write's completion, a close) | Network | Core | `InboundRing` (`inbound_ring.h`, independent of the transport): a bounded FIFO under one mutex. A message the ring refuses is held by its connection, which reads nothing more until the ring has room and the connections it refused push again: on a host, in the order they were refused. On ESP-IDF the connection's `InboundHold` (`inbound_hold.h`) holds it, or one that came before core started the connection, and the connection alone receives nothing more until core resumes it: once the ring has room (`LoopCore`'s parked connections, in the order they parked) or as it starts the connection. Core offers an accepted connection's held message to the ring again then; a dialed connection's reader task offers its own later |
| Commands to a connection (start, wait for room, write, close), a dial's start and cancel, the listener's close | Core | Network | Posts to the network `io_context` (a connection's commands and a dial's cancel hold its network half weakly). On ESP-IDF, a connection's start is its `InboundHold`'s (above); the other commands are request bits the connection's writer task is notified of (`WsLink`), a dial's cancel among them as its close; a dial's start is its writer task's start; the listener's close is `ListenerState`'s flags, and a task of its own stops httpd once its last session closes |
| Accepted connections (the admission, the upgrade's end, an ESP-IDF session's open and close) and a dial's outcome | Network | Core | On a host, a post to core's loop (`to_core()`), and core's admission answer is posted back. On ESP-IDF, httpd's task posts a session's open, upgrade and close (`LoopCore::post()`); a dial's opening is the link's open flag and a signalled `Wakeup`, its failure the ring's close |
| A connection's frames, socket and state, on ESP-IDF | Its writer task, its receive task (a dialed connection's reader task, httpd's task for an accepted one), core | The same | `WsLink` (`ws_link.h`): a frame lock and a socket mutex; core reads the connection's open, close and last receive through atomics |
| The transport's end at shutdown | Network | Core | On a host, the network thread's stop under a mutex and condition variable, which core waits for until its deadline; on ESP-IDF, `LoopCore`'s count of the transport tasks still running, which the shutdown's grace waits for |
| Listener events, traced messages and log lines | Core; log lines from any thread (`Logger`) | Dispatch | `Dispatcher` FIFO, unbounded: the host sets the rate it drains at, and core never waits for it |
| Server key and client record load at `start()` | Host thread | Dispatch | A task on the `Dispatcher`; `start()` waits for its result |
| Client record saves, and their durability | Core, then dispatch | Dispatch, then core | A task on the `Dispatcher` per record change, in order; the outcome is posted back to core, and a failed write no caller waits on stays on the dispatch thread, written again before the next and at shutdown (`ServerCore::FailedWrites`) |
| `clients()`, `groups()` | Core | Host threads | `ClientSnapshot`, `GroupSnapshot`: a vector swapped in and copied out under a mutex (latest wins) |
| The current run | Host threads (`start()`, `stop()`) | Host threads (every `SendspinServer` call) | `SendspinServer`'s mutex: the run's shared pointer, copied out under it, and the runs stopped and not yet joined |
| A group input's state: staged PCM and its segments, run and epoch, anchor and its shifts, drain and stop latch, group parameters | Host threads, core, group audio | Host threads, core, group audio | `HostView`: one mutex and a condition variable; each `GroupInput` call is one step under it, but for a live write through the slew: it owns the view's `LiveSlew` while `slewing_`, resamples without the mutex and stages each block under it. The slew a write replaces with `set_format()`'s stays in the view until core frees it on a later wakeup, or the next `set_format()` takes it back |
| The inputs opened and not yet attached | Host threads (`open_input()`), core (the attachment) | Core (shutdown) | `InputRegistry` (`host_view.h`): a vector under a mutex; at shutdown core closes every view still in it, and it refuses later ones |
| The input each group holds | Host threads (`open_input()`) | Host threads | `GroupInputs` (`host_view.h`): a map of weak pointers under a mutex; an input stops holding its group once it is `gone()` |
| Process-wide initialization (noise-c, an FFT backend's global tables) | Host threads (`start()`), group audio | Dispatch and core (noise-c), group audio (the FFT tables) | `std::call_once` |
| Group ids, and the input, run, epoch and drain ids | Host threads, core (group ids), group audio (a routed source's writes) | The same | Atomic counters: the run's group ids, which `create_group()` and core take alike; the others process-wide, so ids from two inputs never compare equal |
| An input's `on_drained()` and `on_writable()` | Host threads and core, under the host-view mutex | Dispatch | The view's outbox, posted after the mutex is released |
| Host view changes the pipelines act on | Host threads, core | Group audio, core | The group's pipeline task (`AudioWorkers::Task::wake()`) and its `Wakeup`, preallocated, so `write()` never allocates |
| Group-to-core events and frames | The group's pipelines | Core | `GroupToCoreFifo`: an ordered FIFO with one posted drain handler |
| The pipeline set | Core | The group's pipelines | `PipelineSetSlot`, latest wins |
| Artwork images | Host threads (`set_artwork()`) | Core | `EventLoop::post()`; the image is copied, checked and hashed on the host thread |
| Artwork jobs and results | Core, then the artwork worker | The artwork worker, then core | `ArtworkWorker`'s bounded FIFO; results come back with `EventLoop::post()` |
| A routed source's stream items | Core | The route's decoder | `SourceFeed` (`source_feed.h`), bounded; faults come back through its fault list and the route's `Wakeup` |

Every library lock is a leaf: each is held only to copy or update its own state, never across a listener or provider call, a post, a signal, a send or another library lock, so there is no lock order. The one exception is an ESP-IDF connection's frame lock, held across its sends and the socket mutex they take.

## Run lifecycle

- `start()` initializes noise-c, starts the dispatch thread, loads or creates the server key and loads the client records there, builds the `ServerCore`, binds `ServerConfig::listen_port` (which starts the network thread), starts the discovery provider when the server advertises or browses, and starts the core thread, stopping the provider again when that thread cannot start. A built-in discovery provider that cannot start is reported as `ErrorKind::DISCOVERY_UNAVAILABLE` and the server runs without it.
- `stop()` posts the provider's `stop()` and `ServerCore::shutdown()` to core and never waits, so a listener may call it. Shutdown reports every started source, then every connected client and pairing, as stopped (`post_event()` reports nothing after them), closes the listener, every connection and dial and every input (so pending drains report `STOPPED`), puts back a long-term PSK still being saved, writes once more each record write that failed (`ServerCore::FailedWrites`), ends the `Dispatcher` after the events already posted, and ends the loop.
- The destructor, and the next `start()`, join the threads of every run `stop()` ended: the core thread, then the group audio threads, then the dispatch thread.

## A connection

1. A connection starts from `connect_url()` (one dial target per URL), from browsing (one target per mDNS instance) or from a client that connects to the listener. The caps in `connection_limits` bound the dials, the accepted connections in their handshake and the activated accepted connections.
2. A target is dialed when the operator asks (`connect_url()`, `connect()`) and when `dials_on_its_own()` (`discovery_policy.h`) holds. Failed dials and handshakes wait out the target's `HandshakeBackoff` (`backoff.h`).
3. A completed dial or accept becomes a `SessionDriver` over the `Connection`. The `Session` runs the Noise handshake through `client/hello`, with the server the initiator either way.
4. At `READY_FOR_ACTIVATION` `ServerCore` closes an accepted connection past `MAX_ACCEPTED_CLIENTS`, or past `MAX_ACCEPTED_UNAPPROVED` when it may not play, with no activation (`admit_activation()`). It places any other client in a group and has the session queue `server/activate` from `decide_activation()` (`playback_policy.h`), over the PSK the session matched and the client's approval. A client that may not play gets no activities or roles, and the host hears `on_approval_needed()`, `on_pairing_required()` or `on_credential_mismatch()`. The client counts as connected at its first `client/state` (or at once when the activation carries no roles); only then is the snapshot published and `on_client_connected()` posted.
5. Later activations declare `playback` while the client's group plays and `[]` otherwise.
6. When the session ends or the transport fails, the client leaves its group and is reported with `on_client_disconnected()`. A client that went away without a goodbye keeps its place in its group for `ServerTiming::group_rejoin_grace_us`.
7. A dialed connection's end decides whether its target is dialed again (`reconnect_after()`): a `restart`, a `concurrent_attempt` or a drop sets it reconnecting with a growing wait; any other goodbye, and the host's `disconnect()` or `cancel_connect_url()`, stops it.

Core publishes the snapshot before posting the event that reports a change, so a listener sees the change in `clients()` and `groups()`.

### Trust

At `client/init` the session asks which PSK message 1 references (`select_handshake_psk()`, `trust_policy.h`): the client record's long-term PSK, else a pairing PSK, else the Sentinel. When message 2 fails under a long-term or pairing PSK, the `NoiseInitiator` replays message 1 under the Sentinel with the same ephemeral key; success is the credential-mismatch signal. A long-term PSK is referenced only once the provider reports its save durable (`ServerCore::store_long_term_psk()`). `ServerCore::rekey()` re-handshakes an activated connection to another PSK.

### Pairing

`begin_pairing()` posts to core, which keeps one `ServerCore::Pairing` per client with its `PairingAttempt` (`pairing.h`). A client that is not connected is dialed for the pairing. A connection under another PSK than the method's is re-handshaken to it first, and the attempt drops the leftovers sent before that completes. `judge_pairing_message()` decides each pairing message against the attempt's step:

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
2. `write()` copies into the view's staging ring; the write that begins a run anchors it. `set_format()` starts an input segment at the head, so the timeline continues at the new rate. A live input's write also measures how much later its first sample lands than a fresh anchor would place it, against `G_next` or the input's least lead where higher (`HostView::lead_next()`). Its writes fall in consecutive windows of `LOWERING_WINDOW_US` from the epoch's first; once every write of a window kept such an excess, the window's least is owed, never more than a later write's own excess, and the view plays the writes that follow `LOWERING_SLEW_PPM` fast until it is paid (`sched::live_slew_ppm()`). `LiveSlew` (`live_slew.h`) plays them: micro-resampler's ASRC where `profile::SOURCE_ASRC`, started from the frames staged before it and run on until the epoch ends, so its output stays continuous, else single frames dropped (`SampleSlip`). The write runs the slew with the view's mutex released and stages each block's output under it, so core and the group audio threads never wait on the resampling; a flush, stop, pause, drain, close, release, server stop, new epoch or format meanwhile ends its staging, and a write made meanwhile takes nothing (`InputState::FULL`). A write then stages fewer positions than it was given and reports those, and `G_eff` falls by the time taken out (`sched::record_slew()`), never below `G_next` or the least lead, nor does the lead, as the rate leaves a frame of the debt for the part frame a frame drop carries: the lead keeps its distance from the stall rebase's threshold and from admission's horizon, so no frame goes late and no rebase follows. A joiner's raise during the slew lowers the debt to the excess it leaves; a stall's shift is measured by the windows after it. A latency a joiner, a stall or a fast clock raised thus comes back down within the epoch, starting one to two windows after it rose; an excess that straddles a window's start waits for the next.
3. A `GroupAudio` job reads the view, posts the epoch events, shifts the timeline when a frame's lead falls too low (a rebase), produces at most one frame per pipeline instance and lets the view drop staged PCM no instance needs. Core's FIFO handler appends frames to the frame rings and pumps the members.
4. Each member's `PlayerStream` reads the group's frames through a cursor, packs chunks within its ledger, and emits `stream/start`, `stream/clear` and `stream/end` from markers at the head of its control FIFO. A `VisualizerStream` runs beside it on the same markers.
5. A drain completes once no member's stream is active; core ends the run before the view releases `on_drained`.

### Formats and pipelines

Each member's stream format comes from `select_player_format()` (`playback_policy.h`): the operator's format (`set_player_format()`), then the player's preference, when producible, else its first producible format at the input's rate, else its first producible format. `Group::recompute()` keeps one pipeline instance per distinct format, each with its own frame ring. An instance runs, as its format needs: the channel mix (`pipeline_stages.h`), the resampler (micro-resampler, `Resampler`), and the quantize stage and encoder (`FrameEncoder`: PCM, FLAC through micro-flac, Opus through libopus). Every instance cuts the same cells (`sched::CELL_US`), so a cell index carries the same timestamp in every format. A changed preference switches an active stream in place at a frame boundary of both formats (`sched::plan_switch()`). An instance added while an epoch plays catches up from the input the staging ring still holds.

### Artwork, colors and visualizer

`Group` holds an `ArtworkTimeline` (`artwork.h`) per image source and its colors (`ScheduledUpdates<ColorState>`). Encodings come from the run's `ArtworkCache`, which posts misses to the artwork worker; each session's `ArtworkStream` is its artwork queue. Each distinct visualizer configuration is an analysis in the pipeline set, run by `GroupAudio` beside the frame instances (`VisualizerAnalysis`, its FFT behind `RealFft`).

### Routed sources

`route_source()` opens a live input on the group and posts the route to core (`server_core_source.cpp`). The bound connection's `SourceStream` (`source_stream.h`) decides the source's `start` and `stop` and what each of the stream's messages does, and its `Session` pushes the items it builds from them into the route's `SourceFeed`, which core hands it with `Session::set_source_route()`. Core pushes a stream's end itself when it unbinds a connection whose stream is open. The route's `SourceDecoder`, on the group audio threads, decodes the items and writes the PCM through a `LiveSourceSink` into the input.

The sink keeps each sample a fixed latency after its capture. The run's anchor sets that latency (the group's lead past the write of its first chunk, or the route's least latency, `SourceRouteOptions::min_latency_us`, where higher); the source's clock is then held to the server's: `SourceClock` (`source_clock.h`) compares each chunk's capture time with the time its first frame plays at its corrected position, and once their difference drifts past `SOURCE_CLOCK_ENGAGE_US` a PI loop steers the correction, `SourceAsrc` (micro-resampler's ASRC; the input then takes floats) where `profile::SOURCE_ASRC`, else `SampleSlip`. Until then the audio passes unaltered. The input's own rebase and lowering move the latency as for any live input, the lowering never below the least latency; the loop measures only the corrected stream, before the view's slew, so neither disturbs it.

## Work on the core thread

Receive: the network thread stamps a message when its last byte is read and pushes it into the inbound ring; core's drain delivers it to the driver. `Session` checks the phase, decrypts, traces the plaintext, reassembles fragments and dispatches.

Send: after every event the driver runs its pump. With a message to send and no wait or write outstanding, it asks the transport for room. When room is reported, the driver takes every message ready, while the write has room (`write_batch_has_room()`, `transport.h`): `Session::take_message()` selects through `sched::select_next()` (the rest of a fragmented message, then the time queue, then the control FIFO, then an aged artwork message, then the player and visualizer cursors by earliest deadline, then artwork), builds the message, reads the stamp, encrypts and returns it. A time reply ends the write, so no message is built between its stamp and its write. The messages go out as one write, each its own WebSocket frame: in one send on ESP-IDF (`WsBatch`), one after another on a host, where Beast's write lock, which its pongs and closing handshake share, holds one message at a time. The next wait starts only after the write completes. The transport closes a connection whose wait or write stays outstanding for the write-stall time; a send queue that passes its cap (`SendQueueCaps`) ends the session.

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

On ESP-IDF the library is a component (`cmake/esp_idf.cmake`, sources from `cmake/sources.cmake`) built with the embedded resource profile, `src/profile.h`: smaller write-ahead, staging ring and connection caps, a smaller cap on a reassembled message, one group audio thread, a source's clock corrected and a live input's latency slewed by single-frame slips rather than the ASRC, and no Opus. The host runs its whole test suite with the same values (`-DSENDSPIN_SERVER_EMBEDDED_PROFILE=ON`). The transport is native (`src/esp/`): dials on lwIP's sockets, with a reader task per dialed connection, accepts on esp_http_server, whose one task receives for every accepted connection, and a writer task per connection; every receive task hands its messages to core through the same inbound ring. `src/esp/memory_esp.cpp` wraps the whole application's `pthread_mutex_init()` and `pthread_cond_init()` at link time, so another component that wraps either symbol will not link with it.

## Known limitations

- **A live input's raised latency comes down slowly.** It plays only `LOWERING_SLEW_PPM` fast, so a tenth of a second of excess takes minutes to come off.
- **A host's live input keeps its own clock.** Only a routed source's clock is corrected, from its capture times: on a host the ASRC slews it inaudibly; on the embedded profile single frames are dropped or repeated (spec player/v1.md "Sample deletion and insertion"). A host's live input with a slow clock gets a rebase gap each time it falls behind; one with a fast clock gains latency the lowering slews back down; past the slew's rate its lead climbs to `G_eff + W_live`, where `write()` refuses the excess and the audio skips.
- **A routed source's clock is corrected within `SOURCE_CLOCK_MAX_PPM` of the server's.** Beyond it, a fast source's latency climbs until its writes are refused and the audio skips; a slow one gets a rebase gap each time it falls behind.
- **A live input's silence is one gap in its run.** A live input that receives no writes keeps its run, and its next write moves the timeline forward by the silence; a host whose source can go silent should call `GroupInput::stop()` and let its next samples start a new run.
- **An Opus stream starts with silence**, the encoder's lookahead at each new Opus instance.
- **A group's fifth visualizer configuration gets no stream.** Past `MAX_GROUP_ANALYSES`, a configuration fails for the rest of the run.
- **One peer can keep client-initiated connections refused** by holding `connection_limits::MAX_ACCEPTED_HANDSHAKES` accepted connections in their handshake. Dialed clients are unaffected.
- **One peer can keep client-initiated connections refused under `ServerConfig::approve_all_clients`** by holding `connection_limits::MAX_ACCEPTED_CLIENTS` accepted connections activated, since every client may play there. Dialed clients are unaffected.
- **A client reconnecting while its earlier accepted connection is still open counts both,** so at `connection_limits::MAX_ACCEPTED_CLIENTS` its new connection is refused until the transport ends the old one.
- **One peer can keep new unapproved clients from connecting** by holding `connection_limits::MAX_ACCEPTED_UNAPPROVED` accepted connections activated with fresh keys, so the operator does not see a new client to approve or pair until one of them closes. Dialed clients and clients that may play are unaffected.
- **A client that ignores `server/unpair` keeps its connection** until it closes it.
- **A connection waiting for room in the inbound ring is not read,** so the messages behind a held-back message are stamped late, and its pings go unanswered while core is behind.
