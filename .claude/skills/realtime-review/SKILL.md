---
name: realtime-review
description: Review a sendspin-server-cpp branch or diff for real-time and latency suitability: the core thread's handler budget and blocking calls, the write invariant and stamp-at-take, GroupInput::write()'s real-time contract, allocation per cell, frame or chunk, bounded queues, lock scope, and work on the dispatch thread. This is a distinct review axis from correctness; use it before committing any change to the transport, the session, the send pump, the pipelines, the host view or a cross-thread channel.
user-invocable: true
allowed-tools: Read, Grep, Glob, Bash
---

# Real-Time Review

Review the change for what it costs the threads that keep players in sync. This axis is independent of correctness: code can be correct and still delay receive stamps, stall every connection behind one handler, or make a host's audio callback wait. Report findings only; do not edit files. The normative rules live in `docs/conventions.md` ("Threading and cross-thread state", "Real-time and core budget"); this checklist applies them to a diff.

## Scope

Determine the diff: if `$ARGUMENTS` names a commit range or branch, diff that; otherwise `git diff main...HEAD`, falling back to `git diff HEAD`. If the review environment already supplies the diff, review that diff directly instead of computing one. Read the full definitions of changed functions, not just the hunks, and map each one to the thread that runs it (host thread, core, group audio, dispatch, discovery) by tracing its callers; a cost is judged against the thread that pays it.

## Core handler budget

- Every core handler stays within about 200 microseconds: AEAD over one message, one X25519 step, a few KB of JSON. Receive stamps are read on the network thread, so a long handler does not move them, but it holds every time reply and send behind it. Flag new work on core that scales with the number of connections, members, frames or queued items without a bound per handler.
- Work that can exceed the budget is cut into posted steps, as `GroupToCoreFifo` drains (`DRAIN_BUDGET` events per run of its `Wakeup`, then it signals itself again), or moved off core, as the pipeline jobs are (to the group audio threads, `AudioWorkers`). Flag a loop that drains a whole queue in one handler, and encoding or resampling on core.
- A pipeline job stays one frame per instance with at most one resampler built, so groups sharing the group audio threads take turns. Flag a job that loops until its input runs out.
- When the verdict depends on a number, say what to measure, in a `RelWithDebInfo` build.

## Blocking

- The network thread does socket I/O and the receive stamps only: no protocol work, decryption or parsing, and no lock held across a call out (it takes the inbound ring's, the handler pool's, the network thread's stop mutex and Asio's own when it posts; on ESP-IDF the transport's tasks take `InboundHold`'s, `LoopCore`'s and `WsLink`'s). A message it cannot hand off waits in its connection's read buffer (on ESP-IDF, its `InboundHold`), never in a queue of its own.
- Nothing on core waits: no `join()` (a group audio thread's included: a group's task is stopped, never waited for), sleep, condition-variable wait, blocking socket call, `getaddrinfo`, file I/O, or listener, provider or trace call. Listener and provider calls are posted to the `Dispatcher`. The exceptions are shutdown's: core calls the discovery provider's `stop()`, which joins a built-in provider's thread; after its loop ends it waits up to the grace for the transport to end (on a host, joining the network thread) and joins the artwork worker as `ServerCore` is destroyed.
- A `SendspinServer` or `GroupInput` method a listener may call returns without waiting for core or the dispatch thread; only `start()`, the destructor and `GroupInput::wait_writable()` wait. Flag a new wait in any other method, and any wait on the dispatch thread for core.
- A listener call holds the dispatch thread: library code run there does only the call and its bookkeeping.

## The send path

- The write invariant: no write starts unless the transport reported room; a write carries the messages ready as it starts, each its own WebSocket frame, within `write_batch_has_room()`, and a time reply ends it; the next wait starts only after the write completes. Flag a path that writes outside the pump, queues a second write, holds a message back to fill a write, or builds a message after a time reply in the same write.
- A message is built, stamped and encrypted when `Session::take_message()` takes it, with the stamp read immediately before encryption. Flag a stamp read earlier (at queueing, before packing) or a message encrypted ahead of its turn.
- Selection order is `sched::select_next()`'s; a control or time message is never held behind audio. Flag a new message kind that bypasses the queues.

## `GroupInput::write()` and the host view

- `write()` never allocates or frees, waits or does I/O. Trace every function the diff adds to its path: the host-view step, `sched::decide_write()`, the copy into the preallocated staging ring, the `Wakeup` signal and the pipeline task's wake (`AudioWorkers::Task::wake()`). A `std::vector` growth, `std::function` construction, the destruction of an object that owns memory, `Dispatcher::post()` or log in that path is a finding.
- The host-view mutex is held for one step and its bookkeeping. Callbacks the step decides go into the outbox and are posted after release; the `Wakeup` is signalled, and the pipeline task woken, after release.

## Allocation on hot paths

- A running pipeline instance does not reallocate: the resampler is fed at most its block size, and filter banks and encoders are allocated when an instance is built. Flag allocation per cell inside a stage that owns a buffer it could reuse.
- Per frame, per chunk and per message allocation on core is judged against the handler budget: flag a new allocation per item where the surrounding code reuses a buffer, and a copy of a frame or message the existing path avoids.

## Bounded resources

- Every queue, ring and cache has a fixed capacity and an intentional overflow behavior (`SendQueueCaps`, the reassembly cap, `discovery_limits::MAX_CLIENTS`), or a reason it needs none stated at its declaration. A new unbounded container fed by a peer is a finding: a misbehaving peer is closed, never allowed to grow server memory.
- An overflow is observable (`SessionEnd`, `on_error()`, a `false` return) or is an ignore documented at the cap's declaration (`discovery_limits::MAX_CLIENTS`); an undocumented silent drop is a finding.
- A queue is a `Ring` (`ring.h`), for variable-length bytes a `ByteRing` (`byte_ring.h`), or, for closures, a `TaskQueue` (`task_queue.h`), allocated with its owner: a `std::deque` or `std::list` in `src/` allocates as it slides and is a finding. A growing `Ring` names at its declaration what bounds it and why its first capacity; a fixed one is allocated at its cap.
- Latest-wins state (`PipelineSetSlot`, the snapshots) is a slot, never an ordered queue a flood could grow.

## Memory placement

- On ESP-IDF a new buffer, per-connection or per-group object or task names its memory through `memory.h` or `profile.h` (`docs/conventions.md` "Memory is placed"). Flag a large or per-connection allocation left to `malloc()`, internal RAM chosen without a reason (a task that may write flash, a FreeRTOS object, a hot loop), and a stack in PSRAM for a task that can reach a flash write (a provider or listener call). A new `std::shared_ptr`, `std::mutex` or `weak_ptr::lock()` per connection or per message costs a FreeRTOS mutex in internal RAM on the riscv32 toolchain.

## Lock scope

- Every library lock is a leaf: held only to copy or update its own state, never across a listener or provider call, a post, a `Wakeup` signal, a send or another library lock. Flag a lock held while calling out, and a destructor that may run under a lock (release shared pointers outside it, as `GroupInputs` does).
- Every cross-thread member names its writer and reader at its declaration, or appears in the channel table of `docs/internals.md`.

## Budget justification

- A grown cap, timeout, budget or ring size comes with a derivation; "it failed with the old value" is a reason to measure, not a justification.
- Constants that depend on other constants are derived from them (`constexpr`), never restated as a second literal. Timing constants are named `constexpr` values (`src/sched/constants.h`) or, where the host tunes them, `ServerTiming` fields; never literals at the use site.
- Trivially copyable types are copied, not moved; heap-backed types are moved.

## Report format

For each finding: location (file:line), the thread that pays and what it strains (handler budget, stamp accuracy, `write()` latency, memory bound, lock scope), why it matters, and the suggested fix, preferring a pattern this codebase already uses (file:line). Order by severity. If a finding depends on a measurement you cannot perform, say what to measure.
