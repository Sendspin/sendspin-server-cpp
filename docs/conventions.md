# Design Conventions

The design standards that code in this repository is held to. This document is normative: `docs/internals.md` maps how the current code fits together, and this document says how new code is shaped. `docs/comment-style.md` governs every comment and document. The review checklists in `.claude/skills/` apply these standards to a diff.

## Threading and cross-thread state

- **One owner per piece of state.** Core owns every dial target, connection, `SessionDriver`, `Session` and `Group` (through `ServerCore`), and touches them without a lock. Every piece of state that crosses threads goes through a channel listed in `docs/internals.md` ("Cross-thread channels") with its writer, reader and mechanism; a new one is added to that table in the same change.
- **From the network thread: the inbound ring.** Every socket is read and written on the transport's network thread, which stamps each message when its last byte is in and hands messages and connection events to core only through the `InboundRing` (`inbound_ring.h`, independent of the transport, so every transport hands off the same way); core reaches a connection only by posting to the network thread, and holds its network half weakly. The network thread does no protocol work. On ESP-IDF the transport's tasks take its place, through the same ring.
- **Into core: `EventLoop::post()`.** Host calls and discovery results reach core only as posted handlers. A host method checks what it can without core and returns without waiting.
- **Out to the host: the dispatch thread.** `ServerListener`, `PersistenceProvider`, the message trace and the log sink are called only on the `Dispatcher`'s thread, one call at a time, in posting order. Core posts and never waits for them; the `Dispatcher` FIFO is unbounded so the host sets the rate it drains at. `start()`'s load is the one wait on the dispatch thread, from a host thread.
- **Host reads: snapshots.** `clients()` and `groups()` copy a list core swaps under a mutex (`Snapshot`, latest wins). Core publishes the snapshot before posting the event that reports the change, so a listener sees the change in the snapshot.
- **The host view.** A group input's state shared by host threads, core and the group audio threads lives in its `HostView`, one mutex and a condition variable; each `GroupInput` call is one step under it, but for a live write through the slew, which resamples with the mutex released between the steps that stage it. A callback the step decides goes into the view's outbox under the mutex and is posted to the dispatch thread after the mutex is released; a change the pipelines act on wakes the group's pipeline task and signals its `Wakeup` after release.
- **Pipelines to core.** Pipeline code runs on the group audio threads (`AudioWorkers`) and talks to core only through the `GroupToCoreFifo` (ordered events and frames, one posted drain handler) and the `PipelineSetSlot` (latest wins), and reads the host view; it does not reach into `Group` or `ServerCore`. What a job holds is shared with the group or outlives it (the FIFO's producer, the slot, the view), so core stops a group's task and erases the group without waiting for a job running.
- **Every library lock is a leaf.** A lock is held only to copy or update its own state, never across a listener or provider call, a post, a signal, a send or another library lock, so there is no lock order. A change that would take a second lock under one is a design change, not a local trade-off.
- **Listeners may call back in.** A listener may call any `SendspinServer` or `GroupInput` method except `start()` and the destructor. A method a listener may call never waits for core or the dispatch thread (`stop()` only begins the shutdown).
- **Any task may call in.** On ESP-IDF a host's calls come from tasks that are not pthreads (an application's main task, a task of its own), and `std::this_thread::get_id()` asserts on those; which thread a caller is goes through `current_thread_token()` (`thread_setup.h`).
- **Teardown discards, it does not deliver.** Work posted after its owner is gone runs as a no-op (a `Wakeup` signal after the `Wakeup` or its loop is gone, a FIFO handler after the FIFO, a task posted after `Dispatcher::finish()`), and the owner's declaration says so. A result the host is owed at teardown (a pending drain's `STOPPED`, a client's `SERVER_STOPPED`) is posted before the dispatch thread finishes.

## Real-time and core budget

- **Core handlers have a budget** of about 200 microseconds. Receive stamps are read on the network thread, so an overrun does not move them, but every reply and send behind it waits. Work that can exceed the budget is cut into posted steps, as the FIFO drains at most `GroupToCoreFifo::DRAIN_BUDGET` events per handler before it posts itself again, or moved off core, as the pipelines are. A pipeline job produces at most one frame per instance and builds at most one resampler, so groups sharing the group audio threads take turns.
- **Core never blocks.** No waits, joins, sleeps, blocking I/O, name resolution (Asio's resolver runs on its own thread; browsed clients are dialed at their resolved addresses), listener or provider calls on core. The one exception is shutdown: core calls the discovery provider's `stop()`, which joins a built-in provider's thread.
- **The write invariant:** a write starts only after the transport reported room and carries the messages ready as it starts, each its own WebSocket frame, within `write_batch_has_room()`; a time reply ends it; the next wait starts after the write completes. Nothing waits for more messages to fill a write. A message is built, stamped and encrypted when the pump takes it, with the stamp read immediately before encryption.
- **`write()` is real-time safe:** it never allocates or frees, waits or does I/O. It copies into the staging ring preallocated by `open_input()` and `set_format()`, under the host-view mutex held for the copy and its bookkeeping, and signals core through the preallocated `Wakeup`. Of the `GroupInput` methods only `wait_writable()` waits.
- **A running pipeline instance does not reallocate.** The resampler is fed at most the block size its history buffer is sized for; filter banks and encoders are allocated when an instance is built.
- **Memory is placed.** On ESP-IDF a buffer, a per-connection or per-group object and a task stack name where they live through `memory.h` (`ExternalBytes`, `make_placed_array()`, `make_shared_external()`, `ExternalNew`) or `profile.h`, rather than leaving it to the project's malloc policy; internal RAM only for what must be there (a stack of a task that may write flash, a FreeRTOS object) or is faster there.
- **Capacities are justified numbers.** Queues, rings and caches have a fixed capacity and an intentional overflow behavior, or a stated reason they need none at their declaration (the `Dispatcher` FIFO above). An overflow is observable (a `SessionEnd`, `on_error()`, a `false` return) or is an ignore documented at the cap's declaration (`discovery_limits::MAX_CLIENTS`). A queue is a `Ring` (`ring.h`: allocated with its owner, at its cap where it has one, else growing only past what its owner expects) or, for closures, a `TaskQueue` (`task_queue.h`), never a `std::deque`, which allocates a block as it slides. Related constants are derived from each other (`constexpr`), not restated. Timing constants are named `constexpr` values (`src/sched/constants.h`) or, where the host tunes them, `ServerTiming` fields; never literals at the use site.
- Trivially copyable types are copied, not `std::move`d; heap-backed types are moved.

## Protocol and validation

- **Validate at the edge.** Every inbound message is parsed once, into a typed struct carrying its receive stamp, by the pure parsers in `messages.h`; nothing past the parser reads raw JSON.
- **Fail closed and uniformly.** A malformed or missing required field rejects the whole enclosing object; nothing is silently defaulted, clamped or repaired. Sibling fields validate to the same standard. A part validated on its own (a `client/command`'s role objects, a `client/state` player object) is documented as such at its parser in `messages.h`. A value a newer revision may add (an unknown codec, an unrecognized message type) is skipped or ignored as the spec's ignore rules say, at one documented site.
- **A misbehaving peer is closed, a well-behaved one never is** for the server's own backlog: the session ends with its `SessionEnd` reason. Passing a send queue's cap (`SendQueueCaps`) means the client misbehaves; audio never overflows because cursors pull.
- **Reuse the parse and build helpers** in `messages.cpp` instead of per-field logic. Two paths that build or parse the same structure share one helper.
- **Pure logic is pure.** Scheduler math, accounting, activation and reconnect policy, format selection, controller state and message builders are free functions or value types over plain data, with no I/O, threads, locks or clock reads; a timing decision takes `now` as an argument and its caller reads `now_us()`. `src/sched/`, `discovery_policy.h`, `playback_policy.h` and `controller.h` are the shape.
- **One clock.** Every timestamp comes from `now_us()` (`clock.h`). There is no clock injection.
- Spec-driven behavior cites the spec section it implements (`docs/comment-style.md`).

## Boundaries

- **Beast and Asio stay in the host's transport.** Only `src/transport.cpp`, `src/transport_asio.h` (the host's `EventLoop::Impl`) and `src/beast_separate_compilation.cpp` include them; `transport.h` and every other header name no Asio type. The ESP-IDF transport (`src/esp/`) implements the same interface without them. The rest of the server sees whole messages, write readiness and close events.
- **Platform differences stay in their files:** the clock (`clock.cpp`), the mDNS backends (`src/discovery/`, one per platform, chosen by CMake), the transport (`transport.cpp` and `transport_asio.h`, `src/esp/`), thread placement and identity (`thread_setup_host.cpp`, `src/esp/`), placed allocation (`memory_host.cpp`, `src/esp/memory_esp.cpp`) and the visualizer's FFT (`fft_pffft.cpp`, `src/esp/`). Core sources carry no platform conditionals. A build's resource profile has one site, `src/profile.h`: `SENDSPIN_SERVER_EMBEDDED_PROFILE` (set by `cmake/esp_idf.cmake`, and by the host CMake option of the same name to run the tests with its values) selects its constants there, and every other source reads them. A capability the build declares is tested by its own macro, never by platform (`SENDSPIN_SERVER_ENABLE_OPUS` in `frame_encoder.cpp` and `source_decoder.cpp`), and so is a standard library's (libstdc++'s lock policy in `src/shared.h`). A change to one platform's file keeps the other platform's sources and CMake coherent even when it cannot be compiled locally: CI compiles the ESP-IDF component (`tests/esp_idf/`, for the ESP32-S3 and ESP32-P4) but runs the tests on hosts only.
- `src/crypto/` and `fragmentation.*` are copied or adapted from sendspin-cpp and recorded in `src/ORIGIN.md` with the commit; `third_party/` is re-vendored, never edited (`third_party/micro-resampler/VENDORED.md`).

## Secrets

- **Held in wiping types, wiped after use.** A private key or PSK lives in a type that wipes it when destroyed (`Identity`, `HandshakePsk`, `ClientRecord`, the ephemeral key a `NoiseInitiator` keeps), and a buffer that held one (a blob loaded or encoded for the provider) is wiped with `secure_zero()` once used. A secret is never formatted into a string.
- **Pairing secrets the same way.** A pairing code or token from the host, the CPace run and the values it unwraps are held in wiping types (`PairingAttempt`, `PairingPskToken`, the code's buffer on its way to core), and the buffers a pairing message was parsed from are wiped once it is parsed. The message trace never sees one: `redact_pairing_secrets()` replaces each secret field a client sends before the plaintext is traced.
- **Persisted before use.** A credential the server will reference in later handshakes (a long-term PSK) is referenced only once the provider reports it durable; until then the record as it was applies (`ServerCore::store_long_term_psk()`).
- **Removed at once, written until durable.** A removed long-term PSK or approval (an unpairing, a revocation) stops applying at once, and a record write no caller waits on that the provider fails is written again until it lands or the server stops (`ServerCore::FailedWrites`), so a restart does not restore it once the provider recovers before the stop.

## Logging

- **Through `log.h`** (`SS_LOGE`, `SS_LOGW`, `SS_LOGI`), at the sites where the host would otherwise not learn what happened or why: a failure the server recovers from, a connection's start and end, a dial and the wait before one, a persistence failure. Never in `src/sched/`, the policy files or `GroupInput::write()`'s path.
- **Levels:** `ERROR` for a failure the host must act on (persistence), `WARN` for one the server recovers from (a dial or a dialed connection's handshake, mDNS, a cap reached), `INFO` for the routine lifecycle. A line any peer can repeat at will is `INFO` (an accepted connection's failed handshake) or logged once per run of it (the accept cap).
- **No secrets and no keys:** never a private or public key, a PSK, a pairing code or token, a `client_id` or `server_id`, or a message payload. A client is named by its name and address and a dial by its URL, and a peer's text goes through `printable()`.

## Public API

- **The consumer-facing API is exactly `include/sendspin/server/`.** Internal types and headers stay in `src/`; nothing appears in a public header for tests or internal plumbing. Public headers include only the standard library and each other.
- **Pimpl.** A public class with state holds it behind `Impl` or an internal handle, so internals change without touching the headers.
- **Errors are values** (`StartResult`, `false` for an unknown id, `ServerListener::on_error()`), never exceptions across the API. A result the caller must act on is `[[nodiscard]]`: `StartResult`, `OpenedInput`, `RoutedSource` and every call whose `false` or nullopt means the request was refused; a cancel or a disconnect, which may find nothing to do, is not. A caller that rightly ignores one discards it with `(void)` and says why.
- **A limit the host must respect is a named constant** in the public headers (`MAX_VOLUME`, `MAX_PLAYBACK_SPEED`, `config_limits`), which the library's and the example's C++ use instead of restating its value.
- **Configuration travels in structs** with default member initializers (`ServerConfig`, `ServerTiming`, `InputOptions`), not positional optional parameters.
- **A contract is documented once and enforced by documentation,** not by defensive checks: a documented precondition is not re-checked at each call. A check at one of several equivalent sites is at all of them or none.
- **Threading is documented at the declaration:** which thread may call a method, whether it waits, and on which thread a callback runs.
- **`examples/server_cli` is a consumer:** it uses only the public API.

## Consistency

- Reuse before invention: before writing a helper, guard or pattern, look for the existing one. If the existing pattern is wrong, fix it everywhere rather than diverge.
- A fix to a flagged pattern is complete once the tree is swept for other instances of it.
- A change that substantially rewrites a region conforms the drifted patterns inside it rather than carrying them forward.
- Parallel paths (a join and a move, start and clear, one role's state message and another's) stay structurally identical; a deliberate asymmetry carries a comment at the asymmetric site.
- An invariant that several sites must keep is made to hold by construction: a chokepoint helper, a `std::optional` instead of a flag and a value, a derived constant.

## Testing

- **Host-only and white-box.** Tests live in `tests/`, include private headers from `src/`, and run under ASan/UBSan and, in their own build, ThreadSanitizer; never run the two suites at once.
- **No seams in production.** `src/` and `include/` acquire nothing for tests: no friends, test-only hooks, widened visibility, extra template parameters, injectable clocks or transports, or fixture-aware naming. Hard-to-reach logic is extracted into a pure function and tested directly. A test that must reach a private member compiles with `-fno-access-control`, set on that file in `tests/CMakeLists.txt` beside a comment naming every member it reaches and why.
- **Each layer has its harness:** pure tables for `src/sched/` and the policy files (seeded where randomized, as `test_sched_property.cpp`); `Session` driven with messages and deadlines and no I/O; the transport and `SessionDriver` over real loopback sockets; the raw test client (`tests/raw_client.h`) for malformed, out-of-order and adversarial peers and for AES-GCM; sendspin-cpp clients in process as real players, whose audio output shows what a player received. Servers listen on port 0 with discovery off (`local_config()`); a peer that cannot report an ephemeral port takes one from `tests/test_ports.h`.
- **A test defends a specific line** and fails when that line is deleted or its condition inverted; a test that cannot fail that way is filler and is deleted.
- **The unit of a test is a behavior.** A family of related guards is one table whose rows are the branches, with at least one accepting row marked `Control:` beside them; a guard gets its own test only for its own spec citation or failure mode.
- **Assert on what a caller or peer observes.** Private state, queue contents and which thread ran a step are reached only when no observable outcome distinguishes the correct path, and the test says so. Log wording is never asserted.
- **Elapsed time is never a pass/fail condition.** A blocked call is proven by waiting with no timeout, by a value only the correct path can produce, or by a structural failure; a hang is caught by the per-test watchdog in `tests/main.cpp` and the CTest `TIMEOUT`. Timing assertions on the playback path use the timeline the server computes (`timestamp`, frame positions); `send_ahead`, `server_transmitted` and anything counted at a real-time moment record when the server got to send, so a bound on them is a wall-clock assertion. How much a real player plays is real time even once it has played out, so its output is asserted by content, never by amount.
- **Fixtures satisfy production invariants** rather than stubbing around them. Doubles are hand-written fakes that record outcomes (`tests/test_recorder.h`); the tree uses no gmock.
- Coverage that needs a production seam is named as a gap in the change's description, not approximated.

## Documentation

- Comments and docs follow `docs/comment-style.md` and describe the code as it is.
- **Drift is a defect.** A change is complete when every description of what it changed is updated in the same change:

  | If the change touches... | Update... |
  | --- | --- |
  | Threads, cross-thread channels, run or connection lifecycle, playback, order of work on core | `docs/internals.md` |
  | Public API, configuration, listener and threading contracts | Header doc comments in `include/sendspin/server/`, `README.md` |
  | What the server does, `server_cli` commands, build and test commands | `README.md` |
  | Layout, key sources, build options, dependencies | `CLAUDE.md` |
  | A source copied or adapted from sendspin-cpp | `src/ORIGIN.md` |
  | These standards | `docs/conventions.md` and the skills in `.claude/skills/` |

- `docs/internals.md` holds only facts that span files. Why one function behaves as it does belongs at that function; protocol behavior is cited from the spec, not restated. It names constants rather than quoting their values and names no tests.
- Numeric values quoted in docs and comments match the code. Pruning stale text counts as much as adding new text, including comments in files the change does not otherwise touch.
- Docs never present a workaround as designed behavior; a known limitation is listed under "Known limitations" in `docs/internals.md`.
