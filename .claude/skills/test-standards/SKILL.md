---
name: test-standards
description: Review the tests in a sendspin-server-cpp branch or diff against its test-quality standards: extract-for-testability into pure functions, the right harness per layer (pure tables, Session without I/O, loopback transport, the raw test client, in-process sendspin-cpp players), mutation-survivable assertions, Control rows, no filler, no wall-clock pass/fail, no test seams in production, and scaffolding that satisfies production invariants. Use when reviewing new or changed tests, or when asked whether a change is adequately tested.
user-invocable: true
allowed-tools: Read, Grep, Glob, Bash, Edit
---

# Test Standards Review

Review the tests a change adds or modifies, and whether the change's logic is testable at all. A test's job is to fail when the production code is broken; a test that cannot fail that way is filler, and filler has negative value. Report findings only. The normative rules live in `docs/conventions.md` ("Testing"); this checklist applies them to a diff. The only edits this review makes are temporary mutations of production code for the checks under "Mutation survival", and every one of them is reverted before the report is written.

## Scope

Determine the diff: if `$ARGUMENTS` names a commit range or branch, diff that; otherwise `git diff main...HEAD`, falling back to `git diff HEAD`. If the review environment already supplies the diff, review that diff directly instead of computing one. Consider both directions: new tests that are weak, and new logic that ships without a test that could catch its breakage. A test already weak on `main` is out of scope unless the diff touches it or relies on it for coverage.

Back every claim with a command: a "checked and clean" statement, a mutation result, or "the suite passes" rests on a build, run, grep or read performed during this review, with the real `ctest` counts.

## Extract for testability

- Nontrivial pure logic inside a threaded or I/O-coupled path is extracted into a free function or value type with no thread, socket, lock or callback dependencies, and tested directly. `src/sched/`, `discovery_policy.h` (`reconnect_after()`, `dials_on_its_own()`), `playback_policy.h` (`decide_activation()`, `select_player_format()`) and `controller.h` (`judge_command()`) are the shape. Flag new decision-heavy logic reachable only through `ServerCore`, a `Session` or a running server.
- A timing decision takes `now` as an ordinary argument; the production call site reads `now_us()` and nothing becomes swappable. There is no clock injection, and a review does not propose one. A predicate covers a decision, not a sequence over time; a sequence that needs direct coverage is a gap to name, or a seeded property test over pure functions (`tests/test_sched_property.cpp`).

## The right harness

- Pure logic: table-driven tests over the function, seeded where randomized.
- `Session`: driven with messages and deadlines, no I/O (`tests/test_session.cpp`).
- Transport and `SessionDriver`: real loopback sockets (`tests/test_transport.cpp`, `tests/test_session_driver.cpp`).
- Malformed, out-of-order or adversarial peers, and AES-GCM: the raw test client (`tests/raw_client.h`), scripted one step at a time.
- What a player receives and plays: in-process sendspin-cpp clients, asserting on what their listeners and audio output saw (`tests/test_server_players.cpp`).
- A server under test uses `local_config()` (port 0, no advertising or browsing); a peer that cannot report an ephemeral port takes one from `tests/test_ports.h`. Flag a fixed port or a test that touches the network's mDNS.

## No test seams in production

- `src/` and `include/` acquire no friends, test-only hooks, widened visibility, extra template parameters, injectable clocks or transports, or fixture-aware naming for a test. The fix is extraction, a test-side technique, or a named gap.
- Allowed test-side techniques: tests include private headers from `src/` and construct internal classes directly (`HostView`, `GroupAudio`, `Session`); a test file that reaches private members compiles with `-fno-access-control`, set on that file in `tests/CMakeLists.txt` beside a comment naming each member it reaches and why. A test that reaches a private member the comment does not name is a finding.

## Mutation survival

- For every new test, identify the production line or branch its name or comment claims to defend and ask: if that line were deleted or its condition inverted, would this test fail? A test that would still pass is filler. Prefer mutations a person would plausibly make (dropping a reset, inverting a comparison, removing a guard) over line-by-line deletion.
- Reasoning alone may clear a test whose assertion obviously pins its line. A survival finding, or a certification that a subtle test is adequate, requires building and running the mutant after the unmutated build passes, and the report states the command and the result.
- Running a mutant: copy the file to a backup in a temporary directory first, apply the edit, rebuild, run the affected tests (`--gtest_filter` or `ctest -R`), then restore the file from the backup and confirm `git status` and `git diff` show the tree as it was before the review. Never revert with `git checkout --`, which discards uncommitted work in the same file. Build under ASan/UBSan (`build-asan`, `-DSENDSPIN_SERVER_ENABLE_SANITIZERS=ON`); a mutant that changes thread interaction (a dropped lock, a reordered store, a post moved under a lock) is built under ThreadSanitizer (`build-tsan`, `-DSENDSPIN_SERVER_ENABLE_TSAN=ON`) instead. Never run the two suites at once. Check that the build output shows the mutated object recompiling; a stale build passes mutants.
- A surviving mutation is a defect of the test only when it breaks a contract the test claims to cover; otherwise it is a gap. A surviving mutation and an overclaiming comment on the same test are one finding.
- An assertion that claims to cover several failure modes fails for each one on its own; break each path alone to confirm.
- Watch for self-referential tests: asserting on state the test itself set, exercising only the fake or the raw client, or recomputing the expected value with the code under test. The property test's capacity check counts against a brute-force replay of the spec's rule, not against `Ledger`; that is the shape.

## Validation tests

- A family of related rejection cases (one parser's malformed inputs, one policy table's rows, one function's bounds) is one table-driven test whose rows are the branches. Flag a family spelled out as one test per row, and a control written as a separate test: the table carries at least one accepting row marked `Control:` beside the rows it controls. A separate test is warranted only for a branch with its own spec citation or its own failure mode.
- Cover the reject-the-whole-object rule: one bad sibling field rejects the enclosing object, and the test shows the valid neighbors did not survive into the output.
- A protocol violation is covered end to end at least once through the raw client: the server closes the connection with the expected end and reports what the host should hear.

## No filler

- No tests that restate the implementation line by line, duplicate an existing case with cosmetic variation, or exist to inflate a count. Recommending deletion of a weak test is a valid outcome.
- Assertions target what a caller, the host or a peer observes: a returned value, a listener event, a snapshot, a message on the wire. Flag a test that reads private state, queue contents or the thread that ran a step when an observable outcome would distinguish the correct path; where none would, the test says so.
- Log wording is never asserted; a guard whose only effect is a log line is covered through the behavior it protects or not at all.
- Test names and comments describe the behavior under test, not the defect history, closely enough that a red run identifies the break without opening the file.

## Granularity and independence

- One behavior per test: not one assertion per test, and not one test per bug. A test spanning several behaviors is split along them.
- A test's assertions establish what its name and comment claim, no more and no less.
- `ASSERT_*` only where continuing would be meaningless (a parse that must succeed before its result is read); `EXPECT_*` otherwise.
- An assertion whose failure would not explain itself carries a `<<` message with the observed value, so a red run is diagnosable from the log alone.
- No test depends on order, on another test, or on shared mutable global state. A test that fails without a code change is a defect: fix it or delete it, never retry it into passing.

## Scaffolding and test doubles

- Harnesses satisfy production invariants instead of stubbing around them: a `HostView` gets a running `Dispatcher` and the group parameters core would write, a `Session` an `Identity` and `SessionSettings` production would give it.
- Production `assert()` calls are contracts for callers, not behavior under test; the suite has no death tests.
- Doubles are hand-written fakes that record outcomes (`Recorder` and `MemoryProvider` in `tests/test_recorder.h`); the tree uses no gmock. A test that asserts on which calls were made rather than on what resulted is exercising the double.
- Concurrency tests observe real effects, never a counter the thread under test would have been the one to advance. A concurrency finding, or a certification that a threaded test is adequate, is backed by a TSan run with its command. A race TSan reports is real until shown otherwise; the review never proposes a suppression.

## No wall-clock assertions

- Elapsed time is never the pass/fail condition (`EXPECT_LT(elapsed, x)`); the bound is a guess about machine speed that is eventually wrong. If latency is the property, it is a measurement, not a test: name it as a gap.
- An elapsed-time assertion means the call has a second exit (a timeout) and the clock tells the two apart. Remove the second exit: wait with no timeout so completion is the proof (`Recorder::wait_for()` in `tests/test_recorder.h`); assert on a value only the correct path can produce; or rely on a structural failure. Hangs are caught by the per-test watchdog in `tests/main.cpp` and the CTest `TIMEOUT` in `tests/CMakeLists.txt`.
- Timing on the playback path is asserted from the timeline the server computes (`timestamp`, frame positions), never from when the test saw a message. `send_ahead` and `server_transmitted` record when the server got to send, and so does how much has played or been sent by a given moment: on a loaded machine they move like elapsed time, and a bound on them is a wall-clock assertion. Assert only what holds however late a send is (order, no gap but where its last frame had passed by the next message's stamp, no overlap, a timestamp the timeline fixes), or wait for the event that makes a server-side count final (a drain). A player's playout ends its output but does not fix how much it played: a player whose threads fall behind its clock drops what is late, so a player's output is asserted by content (order, bit for bit, each segment present), never by amount; a latency, lead or amount played is a measurement to name as a gap.
- Allowed: a bounded wait that only decides when to sample; a sleep that orders threads only where a late thread yields a false pass, never a false failure; a sleep that lets real time pass a point the server reads from `now_us()`; a bounded window for a "must not happen" check on a monotonic observation. Sleeps are the minimum that reliably orders the threads; a test longer than a few hundred milliseconds carries a comment saying why.

## Honest gaps

- A gap that cannot be closed cheaply is named in the change's description rather than papered over with a test that appears to cover it. Naming the gap is the finding; never recommend a production seam to close it.
- Platform paths: CI runs Linux (ASan, TSan) and macOS (ASan). A finding on `clock.cpp`, a discovery backend or epoll/kqueue ordering in the transport states which platform the test exercised; a mutation result on one says nothing about the other.

## Report format

For each finding: location (file:line), the standard it misses, and the concrete improvement (including "delete this test" where warranted); a mutation finding also states the command and the observed result. Separate sections for weak tests, missing tests, production-testability issues, and named gaps (missing by acknowledged decision; no action requested). Close with the `git status` output showing every mutation was reverted.
