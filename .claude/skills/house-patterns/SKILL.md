---
name: house-patterns
description: Review a sendspin-server-cpp branch or diff for fit with its design conventions: helper reuse, thread ownership and the cross-thread channels (EventLoop::post, the Dispatcher, snapshots, the host view, the group-to-core FIFO), validation posture, pure logic kept pure, Beast confined to the transport, header visibility, and public API shape. Use to check whether functionally correct code is shaped the way this library shapes it.
user-invocable: true
allowed-tools: Read, Grep, Glob, Bash
---

# House Patterns Review

Review the change for fit with this library's design conventions. The premise: the code is assumed functionally correct; the question is whether it is shaped the way sendspin-server-cpp shapes it. Report findings only; do not edit files. The normative rules live in `docs/conventions.md`; this checklist applies them to a diff.

## Scope

Determine the diff: if `$ARGUMENTS` names a commit range or branch, diff that; otherwise `git diff main...HEAD`, falling back to `git diff HEAD`. If the review environment already supplies the diff, review that diff directly instead of computing one. For each area below, read the sibling code the diff should resemble before judging the diff.

Back every claim with a command. A "checked and clean" statement must rest on a grep, diff or file read performed during this review, never on recollection.

## Reuse before invention

- New parsing or building uses the helpers in `messages.cpp` (`read_u32()`, `read_positive_u32()`, `read_flags()`, `parse_format()`, `try_serialize()`) rather than per-field logic.
- A new queue is a `Ring` (`ring.h`), for variable-length bytes a `ByteRing` (`byte_ring.h`), or, for closures, a `TaskQueue` (`task_queue.h`), placed as `memory.h` names, not a standard container that allocates as it slides (`std::deque`, `std::list`).
- A new decision about timing, admission, selection, leads or the stream lifecycle uses or extends `src/sched/`; a new policy decision belongs beside its siblings in `discovery_policy.h`, `playback_policy.h` or `controller.h`.
- Before accepting a new helper, guard or utility, search the tree for one that already does the job. Duplicating an existing mechanism is a finding even when the duplicate is correct.
- A new role or feature copies the structure of the sibling that already solves the same problem (the controller and metadata roles for state roles, `PlayerStream` and its markers for a stream). Drift from the sibling is a defect: conform, or fix all siblings.
- When a diff substantially rewrites a region, drifted patterns inside it are conformed in the same change; "the old code did it this way" excuses a one-line addition, not a rewrite.

## Threads and channels

Check against `docs/conventions.md` ("Threading and cross-thread state") and the tables in `docs/internals.md`:

- First, map each changed function to the thread(s) that run it (host thread, core, group audio, dispatch, discovery) by tracing its callers; every rule below is judged against that mapping, and a function whose thread you have not identified is not yet reviewed.
- Core state (targets, connections, sessions, groups, records in memory) is touched only on core. Another thread reaches it only through `EventLoop::post()`.
- Listener, provider and trace calls happen only on the dispatch thread, posted through the `Dispatcher`. Core publishes the `clients()`/`groups()` snapshot before posting the event that reports the change.
- Host-view state changes in one step under its mutex; a callback goes through the outbox and a wake through the `Wakeup` and the pipeline task, all after release.
- Pipeline code runs on the group audio threads and reaches core only through `GroupToCoreFifo` and `PipelineSetSlot`, never `Group` or `ServerCore` directly; what a job holds is shared with the group or outlives it, so the group stops its task without waiting.
- Every lock is a leaf: a second library lock taken under one, or a lock held across a call out, is a finding. A new cross-thread member, or a new channel, appears in the channel table of `docs/internals.md`.
- Work posted after its owner is gone runs as a no-op, and the owner's declaration says so.

## Validation posture

- Inbound JSON is parsed once into a typed struct with its receive stamp by a parser in `messages.h`; code past the parser does not read JSON.
- Fail closed and uniformly: a malformed required field rejects the whole enclosing object; no silent defaulting, clamping or repair; sibling fields validate to the same standard. A value a newer revision may add is skipped or ignored only as the spec's ignore rules say, at one documented site.
- A misbehaving peer ends the session with a `SessionEnd` reason; flag a path that keeps a connection after a protocol violation, or closes a well-behaved one for the server's own backlog.
- Two paths that build or parse the same structure share an extracted helper.
- Spec-driven behavior cites the spec section (`docs/comment-style.md`).

## Pure logic and boundaries

- Decision-heavy logic is a free function or value type over plain data, with no I/O, threads, locks or clock reads; a timing decision takes `now` as an argument and its caller reads `now_us()`. Flag a clock read, lock or callback inside `src/sched/` or a policy file, and a decision buried in `ServerCore` or `Session` that a pure function would hold.
- Every timestamp comes from `now_us()`; flag `std::chrono::steady_clock` or `system_clock` in `src/`.
- Only `src/transport.cpp`, `src/transport_asio.h` and `src/beast_separate_compilation.cpp` include Asio or Beast, and the ESP-IDF build compiles none of them; `transport.h` and every other header name no Asio type. Verify with a grep for `boost/` includes.
- Platform conditionals appear only in `clock.cpp` and in `src/esp/` (an ESP-IDF version, a dependency's capability); platform differences otherwise live in their own files: the backends in `src/discovery/` (chosen by CMake), `src/profile.h` (the resource profile, keyed on `SENDSPIN_SERVER_EMBEDDED_PROFILE`), `thread_setup_host.cpp` and `src/esp/thread_setup_esp.cpp` (thread placement and the calling thread's token, `thread_setup.h`), `fft_pffft.cpp` and `src/esp/fft_esp_dsp.cpp` (the visualizer's FFT, `fft.h`), and the rest of `src/esp/`. A capability the build declares is tested by its own macro (`SENDSPIN_SERVER_ENABLE_OPUS` in `frame_encoder.cpp` and `source_decoder.cpp`), and so is a standard library's (libstdc++'s lock policy in `src/shared.h`). A change to one platform's file keeps the other platform's sources and CMake coherent even when it cannot be compiled locally: CI compiles the ESP-IDF component (`tests/esp_idf/`, for the ESP32-S3 and ESP32-P4) but runs the tests on hosts only.
- A source copied from sendspin-cpp is recorded in `src/ORIGIN.md`; `third_party/` is not edited.

## Logging

- A log line goes through the `log.h` macros at a site `docs/conventions.md` ("Logging") names, at its level; flag a log in `src/sched/`, a policy file or `GroupInput::write()`'s path, a direct `stderr` write in `src/`, and a line a flood of peers can repeat without bound.
- Flag a line that formats a key, a PSK, a pairing secret, a `client_id`, a `server_id` or a payload, and a peer's text logged without its control characters replaced.

## Secrets

- A private key or PSK held outside a wiping type (`Identity`, `HandshakePsk`, `ClientRecord`), a blob that held one left unwiped after the provider call, and a secret formatted into a string are findings (`docs/conventions.md` "Secrets").
- A credential referenced in a handshake before the provider reported it durable is a finding.
- A record write that drops a credential (a long-term PSK, an approval) bypassing `write_record()`, so a failed write is not written again, is a finding.

## Header visibility and API shape

- Only consumer-facing API in `include/sendspin/server/`; internal types and headers stay in `src/`. Nothing appears in a public header for tests or internal plumbing, and public headers include only the standard library and each other.
- A public class with state keeps it behind `Impl` or an internal handle.
- Errors are values or `on_error()`, never exceptions across the API.
- Configuration travels in structs with default member initializers, not positional optional parameters.
- Contracts are enforced by documentation, not defensive code: flag a check against a documented precondition, and a check present at one of several equivalent sites.
- Public declarations state which thread may call them, whether they wait, and on which thread a callback runs.
- `examples/server_cli` uses only the public API; verify with a grep of its includes.

## Consistency completion

- A fixed pattern is swept across the tree: grep for other instances of the same shape before calling the fix complete.
- Parallel paths (a join and a move, approval and revocation, start and clear) stay structurally identical; deliberate asymmetry carries a comment at the asymmetric site.
- An invariant that several sites must keep is restructured to hold by construction: a chokepoint helper, `std::optional` instead of a flag and a value, a derived constant.

## Report format

For each finding: location (file:line), the convention violated (cite the `docs/conventions.md` section), the sibling code or helper that shows the expected shape (file:line), and the suggested change. Distinguish "must conform" findings from "sibling pattern itself may deserve fixing" findings.
