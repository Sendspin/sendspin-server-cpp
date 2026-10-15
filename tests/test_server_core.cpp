// Copyright 2026 Sendspin Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/// @file test_server_core.cpp
/// @brief ServerCore on its own core thread: a dial toward a URL whose dials keep failing waits
/// out the URL's backoff, and pruning keeps a failed target; a lost instance's target goes with
/// its wait for a dial, and a dial that waited checks again before it starts. White-box: reaches
/// ServerCore's targets_ and prune_targets() and HandshakeBackoff::next_dial_us_ (see
/// tests/CMakeLists.txt). Shutdown closes an input whose attachment has not run. A pipeline
/// failure, forced by swapping a group's encoder, ends the stream on its format and leaves a drain
/// to complete. A member whose leaving or stopping its group overflows its control FIFO, its cap
/// lowered through Session::queues_, closes inside that step, and so does a client's second
/// connection inside the host's move of its first.

#include "clock.h"
#include "crypto/keys.h"
#include "dispatcher.h"
#include "frame_encoder.h"
#include "host_view.h"
#include "raw_client.h"
#include "sched/constants.h"
#include "server_core.h"
#include "server_core_client.h"
#include "test_ports.h"
#include "test_recorder.h"
#include "transport.h"
#include <gtest/gtest.h>
#include <noise/protocol.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using sendspin::server::ServerError;

class ErrorCounter : public sendspin::server::ServerListener {
public:
    void on_error(const ServerError& /*error*/) override {
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            ++this->errors_;
        }
        this->changed_.notify_all();
    }

    /// Waits with no timeout; a regression hangs into the suite watchdog
    void wait_for(int errors) {
        std::unique_lock<std::mutex> lock(this->mutex_);
        this->changed_.wait(lock, [&] { return this->errors_ >= errors; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    int errors_{0};
};

struct TargetState {
    bool exists{false};
    bool dialing{false};
    bool retry_pending{false};
    uint32_t failures{0};
    bool waiting{false};  // next_dial_us is in the future
};

// messaging.md "client/goodbye": backoff on repeated failures toward one target
TEST(ServerCoreBackoff, ADialAfterAFailureWaitsOutTheBackoff) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    sendspin::server::EventLoop loop;
    sendspin::server::AudioWorkers workers(1);
    // Outlives the dispatcher, whose thread calls it until joined
    ErrorCounter listener;
    sendspin::server::Dispatcher dispatcher;
    const sendspin::server::Identity identity = *sendspin::server::Identity::generate();
    const sendspin::server::CoreSettings settings;
    sendspin::server::ClientSnapshot snapshot;
    sendspin::server::GroupSnapshot groups;
    sendspin::server::InputRegistry inputs;
    std::atomic<uint64_t> group_ids{1};
    sendspin::server::ServerCore core(loop, workers, dispatcher, &listener, identity, settings,
                                      snapshot, groups, inputs, group_ids);
    std::thread core_thread([&] { loop.run(); });

    const sendspin::server::test::UnlistenedPort unlistened;
    const std::string url = "ws://127.0.0.1:" + std::to_string(unlistened.port()) + "/sendspin";
    const sendspin::server::DialTarget target = *sendspin::server::parse_ws_url(url);
    const auto connect_and_inspect = [&] {
        std::promise<TargetState> inspected;
        loop.post([&] {
            core.connect_url(url, target);
            TargetState state;
            const auto it = core.targets_.find(url);
            if (it != core.targets_.end()) {
                state = {true, it->second.dial.has_value(), it->second.retry_pending,
                         it->second.backoff.failures(),
                         it->second.backoff.next_dial_us() > sendspin::server::now_us()};
            }
            inspected.set_value(state);
        });
        return inspected.get_future().get();
    };

    // Control: the first dial starts at once
    const TargetState first = connect_and_inspect();
    EXPECT_TRUE(first.dialing);
    EXPECT_FALSE(first.retry_pending);
    listener.wait_for(1);

    // On the core thread: a failed target outlives a prune just after its wait ends, so a host
    // retrying then still escalates the backoff. Its wait is then cut short, so the deferred dial
    // below comes soon.
    std::promise<std::pair<bool, uint32_t>> pruned;
    loop.post([&] {
        sendspin::server::HandshakeBackoff& backoff = core.targets_.at(url).backoff;
        core.prune_targets(backoff.next_dial_us() + 1);
        const auto it = core.targets_.find(url);
        pruned.set_value({it != core.targets_.end(),
                          it != core.targets_.end() ? it->second.backoff.failures() : 0U});
        if (it != core.targets_.end()) {
            constexpr int64_t SHORT_WAIT_US = 50000;
            it->second.backoff.next_dial_us_ = sendspin::server::now_us() + SHORT_WAIT_US;
        }
    });
    const std::pair<bool, uint32_t> after_prune = pruned.get_future().get();
    EXPECT_TRUE(after_prune.first);
    EXPECT_EQ(after_prune.second, 1U);

    // Within the backoff the dial is deferred, not started
    const TargetState second = connect_and_inspect();
    ASSERT_TRUE(second.exists);
    EXPECT_EQ(second.failures, 1U);
    EXPECT_TRUE(second.waiting);
    EXPECT_FALSE(second.dialing);
    EXPECT_TRUE(second.retry_pending);

    // The deferred dial runs when the backoff ends, and its failure counts again
    listener.wait_for(2);
    std::promise<std::pair<uint32_t, bool>> last;
    loop.post([&] {
        const sendspin::server::HandshakeBackoff backoff = core.targets_.at(url).backoff;
        // Control: a full maximum wait after its last wait ended, the idle target goes
        core.prune_targets(backoff.next_dial_us() + settings.backoff.max_us);
        last.set_value({backoff.failures(), core.targets_.count(url) == 0});
    });
    const std::pair<uint32_t, bool> final_state = last.get_future().get();
    EXPECT_EQ(final_state.first, 2U);
    EXPECT_TRUE(final_state.second);

    loop.post([&] { core.shutdown(0); });
    core_thread.join();
}

// ConnectOptions::retry_until_connected: a URL target whose dial failed is dialed again on the
// server's own after its backoff (a retry scheduled), and cancel_connect_url() withdraws it;
// without the option a failed dial ends the operator's request
TEST(ServerCoreBackoff, RetriesAUrlUntilConnectedUntilCancelled) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    struct Row {
        const char* name;
        bool retry;
        bool scheduled;
    };
    const Row rows[] = {
        {"Control: a plain dial", false, false},
        {"retry until connected", true, true},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        sendspin::server::EventLoop loop;
        sendspin::server::AudioWorkers workers(1);
        // Outlives the dispatcher, whose thread calls it until joined
        ErrorCounter listener;
        sendspin::server::Dispatcher dispatcher;
        const sendspin::server::Identity identity = *sendspin::server::Identity::generate();
        const sendspin::server::CoreSettings settings;
        sendspin::server::ClientSnapshot snapshot;
        sendspin::server::GroupSnapshot groups;
        sendspin::server::InputRegistry inputs;
        std::atomic<uint64_t> group_ids{1};
        sendspin::server::ServerCore core(loop, workers, dispatcher, &listener, identity, settings,
                                          snapshot, groups, inputs, group_ids);
        std::thread core_thread([&] { loop.run(); });

        const sendspin::server::test::UnlistenedPort unlistened;
        const std::string url = "ws://127.0.0.1:" + std::to_string(unlistened.port()) + "/sendspin";
        loop.post([&] { core.connect_url(url, *sendspin::server::parse_ws_url(url), row.retry); });
        listener.wait_for(1);
        const auto inspect = [&] {
            std::promise<TargetState> inspected;
            loop.post([&] {
                TargetState state;
                const auto it = core.targets_.find(url);
                if (it != core.targets_.end()) {
                    state = {true, it->second.dial.has_value(), it->second.retry_pending,
                             it->second.backoff.failures(),
                             it->second.backoff.next_dial_us() > sendspin::server::now_us()};
                }
                inspected.set_value(state);
            });
            return inspected.get_future().get();
        };
        const TargetState failed = inspect();
        ASSERT_TRUE(failed.exists);
        EXPECT_EQ(failed.failures, 1U);
        EXPECT_EQ(failed.retry_pending, row.scheduled);

        loop.post([&] { core.cancel_connect_url(url); });
        const TargetState cancelled = inspect();
        EXPECT_FALSE(cancelled.retry_pending);
        EXPECT_FALSE(cancelled.dialing);

        loop.post([&] { core.shutdown(0); });
        core_thread.join();
    }
}

// A client found by browsing whose dial failed waits out its backoff; once its instance is lost the
// target goes, which cancels the wait, so it is not dialed again
TEST(ServerCoreDiscovery, ALostInstanceCancelsItsDialWait) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    sendspin::server::EventLoop loop;
    sendspin::server::AudioWorkers workers(1);
    // Outlives the dispatcher, whose thread calls it until joined
    ErrorCounter listener;
    sendspin::server::Dispatcher dispatcher;
    const sendspin::server::Identity identity = *sendspin::server::Identity::generate();
    const sendspin::server::CoreSettings settings;
    sendspin::server::ClientSnapshot snapshot;
    sendspin::server::GroupSnapshot groups;
    sendspin::server::InputRegistry inputs;
    std::atomic<uint64_t> group_ids{1};
    sendspin::server::ServerCore core(loop, workers, dispatcher, &listener, identity, settings,
                                      snapshot, groups, inputs, group_ids);
    std::thread core_thread([&] { loop.run(); });

    const sendspin::server::test::UnlistenedPort unlistened;
    sendspin::server::DiscoveredClient found;
    found.instance = "Gone";
    found.addresses = {"127.0.0.1"};
    found.port = unlistened.port();
    found.path = "/sendspin";
    loop.post([&] { core.on_client_found(found); });
    listener.wait_for(1);
    const std::string key = "mdns:Gone";
    std::promise<std::pair<bool, bool>> waiting;
    loop.post([&] {
        const auto it = core.targets_.find(key);
        waiting.set_value(
            {it != core.targets_.end(), it != core.targets_.end() && it->second.retry_pending});
    });
    const std::pair<bool, bool> before = waiting.get_future().get();
    EXPECT_TRUE(before.first);
    EXPECT_TRUE(before.second);

    std::promise<bool> after;
    loop.post([&] {
        core.on_client_lost("Gone");
        after.set_value(core.targets_.count(key) == 0);
    });
    EXPECT_TRUE(after.get_future().get());
    EXPECT_TRUE(snapshot.read().empty());

    loop.post([&] { core.shutdown(0); });
    core_thread.join();
}

// A dial on the server's own that waited out its backoff checks again before it starts, so one no
// longer due (here a URL target that stopped reconnecting) is not made
TEST(ServerCoreDiscovery, ADialThatWaitedChecksAgainBeforeItStarts) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    sendspin::server::EventLoop loop;
    sendspin::server::AudioWorkers workers(1);
    // Outlives the dispatcher, whose thread calls it until joined
    ErrorCounter listener;
    sendspin::server::Dispatcher dispatcher;
    const sendspin::server::Identity identity = *sendspin::server::Identity::generate();
    const sendspin::server::CoreSettings settings;
    sendspin::server::ClientSnapshot snapshot;
    sendspin::server::GroupSnapshot groups;
    sendspin::server::InputRegistry inputs;
    std::atomic<uint64_t> group_ids{1};
    sendspin::server::ServerCore core(loop, workers, dispatcher, &listener, identity, settings,
                                      snapshot, groups, inputs, group_ids);
    std::thread core_thread([&] { loop.run(); });

    const sendspin::server::test::UnlistenedPort unlistened;
    const std::string url = "ws://127.0.0.1:" + std::to_string(unlistened.port()) + "/sendspin";
    const sendspin::server::DialTarget target = *sendspin::server::parse_ws_url(url);
    loop.post([&] { core.connect_url(url, target); });
    listener.wait_for(1);
    // Reconnecting, so the failure's backoff schedules a dial on the server's own; then no longer
    // reconnecting before the short wait ends
    loop.post([&] {
        auto& entry = core.targets_.at(url);
        entry.reconnecting = true;
        constexpr int64_t SHORT_WAIT_US = 50000;
        entry.backoff.next_dial_us_ = sendspin::server::now_us() + SHORT_WAIT_US;
        core.dial_if_due(entry);
        entry.reconnecting = false;
    });
    struct State {
        bool retry_pending;
        bool dialing;
        uint32_t failures;
    };
    State state{true, false, 0};
    while (state.retry_pending) {
        std::promise<State> read;
        loop.post([&] {
            const auto& entry = core.targets_.at(url);
            read.set_value({entry.retry_pending, entry.dial.has_value(), entry.backoff.failures()});
        });
        state = read.get_future().get();
    }
    EXPECT_FALSE(state.dialing);
    EXPECT_EQ(state.failures, 1U);

    loop.post([&] { core.shutdown(0); });
    core_thread.join();
}

// An input open_input() registered whose attachment is still queued, or lands after the loop ended,
// is closed by the stop, so its pending drain resolves as STOPPED before the dispatch thread
// finishes, and no input is registered after it
TEST(ServerCoreShutdown, ClosesAnInputWhoseAttachmentHasNotRun) {
    using sendspin::server::DrainOutcome;
    using sendspin::server::InputState;
    using sendspin::server::test::Recorder;
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    sendspin::server::EventLoop loop;
    sendspin::server::AudioWorkers workers(1);
    // Outlives the dispatcher, whose thread calls it until joined
    Recorder recorder;
    auto dispatcher = std::make_shared<sendspin::server::Dispatcher>();
    const sendspin::server::Identity identity = *sendspin::server::Identity::generate();
    const sendspin::server::CoreSettings settings;
    sendspin::server::ClientSnapshot snapshot;
    sendspin::server::GroupSnapshot groups;
    sendspin::server::InputRegistry inputs;
    std::atomic<uint64_t> group_ids{1};
    sendspin::server::ServerCore core(loop, workers, *dispatcher, &recorder, identity, settings,
                                      snapshot, groups, inputs, group_ids);
    std::thread core_thread([&] { loop.run(); });

    // A view with room, so a drain can be staged without its group
    const sendspin::server::InputId id{5};
    auto view = std::make_shared<sendspin::server::HostView>(
        sendspin::server::GroupId{1}, id,
        sendspin::server::InputOptions{
            sendspin::server::InputFormat{48000, 2, sendspin::server::SampleFormat::S16}},
        sendspin::server::GroupParams{300000, 0, 1000000}, dispatcher, &recorder);
    const std::vector<uint8_t> pcm(4 * 960, 0);
    ASSERT_EQ(view->write(pcm.data(), 960).frames, 960U);
    view->drain();
    ASSERT_TRUE(inputs.add(view));

    loop.post([&] { core.shutdown(0); });
    core_thread.join();
    dispatcher->join();
    EXPECT_EQ(recorder.count(Recorder::drained(id, DrainOutcome::STOPPED)), 1U);
    EXPECT_EQ(view->write(pcm.data(), 1).state, InputState::CLOSED);
    EXPECT_FALSE(inputs.add(view));
}

// A group still holding an input the host released, whose detach has not run, takes the next
// input; doing both in one core handler shows it without a race
TEST(ServerCoreInputs, AttachesAnInputOverOneTheHostReleased) {
    using sendspin::server::HostView;
    using sendspin::server::InputState;
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    sendspin::server::EventLoop loop;
    sendspin::server::AudioWorkers workers(1);
    auto dispatcher = std::make_shared<sendspin::server::Dispatcher>();
    const sendspin::server::Identity identity = *sendspin::server::Identity::generate();
    const sendspin::server::CoreSettings settings;
    sendspin::server::ClientSnapshot snapshot;
    sendspin::server::GroupSnapshot groups;
    sendspin::server::InputRegistry inputs;
    std::atomic<uint64_t> group_ids{1};
    sendspin::server::ServerCore core(loop, workers, *dispatcher, nullptr, identity, settings,
                                      snapshot, groups, inputs, group_ids);
    std::thread core_thread([&] { loop.run(); });

    const auto view = [&](uint64_t id) {
        return std::make_shared<HostView>(
            core.default_group(), sendspin::server::InputId{id},
            sendspin::server::InputOptions{
                sendspin::server::InputFormat{48000, 2, sendspin::server::SampleFormat::S16}},
            sendspin::server::GroupParams{300000, 0, 1000000}, dispatcher, nullptr);
    };
    const std::shared_ptr<HostView> released = view(1);
    const std::shared_ptr<HostView> next = view(2);
    const std::shared_ptr<HostView> refused = view(3);
    std::promise<void> attached;
    loop.post([&] {
        core.attach_input(released);
        released->release();
        core.attach_input(next);
        // Control: an input not released still refuses the next
        core.attach_input(refused);
        attached.set_value();
    });
    attached.get_future().get();
    EXPECT_EQ(released->state(), InputState::CLOSED);
    EXPECT_NE(next->state(), InputState::CLOSED);
    EXPECT_EQ(refused->state(), InputState::CLOSED);

    loop.post([&] { core.shutdown(0); });
    core_thread.join();
    dispatcher->finish();
    dispatcher->join();
}

// A pipeline that fails mid-stream (here its encoder, swapped on the core thread for one that
// refuses its frames) excludes its format for the run: on_error names the player and the format,
// and the player streaming it gets stream/end, then a join stream/start on the other format it
// lists, which needs the resampler, and whose chunks follow from an instance that catches up at
// once. A drain then completes as PLAYED. Control: a member with no playable format is reported
// once, not again when the failure's exclusion ends with the run.
TEST(ServerCorePipelines, MovesAPlayerOffAFailedFormatAndStillCompletesADrain) {
    using sendspin::server::DrainOutcome;
    using sendspin::server::ErrorKind;
    using sendspin::server::StreamCodec;
    using sendspin::server::StreamFormat;
    using sendspin::server::test::RawClient;
    using sendspin::server::test::Recorder;
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    sendspin::server::EventLoop loop;
    sendspin::server::AudioWorkers workers(1);
    // Outlives the dispatcher, whose thread calls it until joined
    Recorder recorder;
    auto dispatcher = std::make_shared<sendspin::server::Dispatcher>();
    const sendspin::server::Identity identity = *sendspin::server::Identity::generate();
    sendspin::server::CoreSettings settings;
    settings.approve_all_clients = true;
    sendspin::server::ClientSnapshot snapshot;
    sendspin::server::GroupSnapshot groups;
    sendspin::server::InputRegistry inputs;
    std::atomic<uint64_t> group_ids{1};
    sendspin::server::ServerCore core(loop, workers, *dispatcher, &recorder, identity, settings,
                                      snapshot, groups, inputs, group_ids);
    std::thread core_thread([&] { loop.run(); });

    // A player of a format at the input's rate and one at another, and one of none the input
    // gives
    RawClient client;
    RawClient unplayable;
    const std::string url = client.url();
    const std::string unplayable_url = unplayable.url();
    loop.post([&] {
        core.connect_url(url, *sendspin::server::parse_ws_url(url));
        core.connect_url(unplayable_url, *sendspin::server::parse_ws_url(unplayable_url));
    });
    client.activate(sendspin::server::test::player_hello_json(
        "Failing",
        R"([{"codec":"pcm","sample_rate":44100,"channels":2,"bit_depth":16},)"
        R"({"codec":"pcm","sample_rate":48000,"channels":2,"bit_depth":24}])",
        200000));
    client.send_json(sendspin::server::test::client_state_json(
        true, sendspin::server::test::player_state_json(150, 300, 0)));
    unplayable.activate(sendspin::server::test::player_hello_json(
        "Unplayable", R"([{"codec":"pcm","sample_rate":44101,"channels":2,"bit_depth":16}])",
        200000));
    unplayable.send_json(sendspin::server::test::client_state_json(
        true, sendspin::server::test::player_state_json(150, 300, 0)));
    constexpr uint32_t IN_RATE = 44100;
    const auto view = std::make_shared<sendspin::server::HostView>(
        core.default_group(), sendspin::server::InputId{9},
        sendspin::server::InputOptions{
            sendspin::server::InputFormat{IN_RATE, 2, sendspin::server::SampleFormat::S16}},
        sendspin::server::GroupParams{250000, 0, 0}, dispatcher, &recorder);
    ASSERT_TRUE(inputs.add(view));
    loop.post([&] { core.attach_input(view); });
    const std::vector<uint8_t> pcm(size_t{IN_RATE} * 4, 1);
    // Written once the group has attached the input and set its write-ahead
    const auto write = [&](uint64_t frames) {
        uint64_t written = 0;
        while (written < frames) {
            written += view->write(pcm.data(), frames - written).frames;
            if (written < frames) {
                view->wait_writable(INT64_MAX);
            }
        }
    };
    size_t starts = 0;
    std::string json;
    /// Reads messages until the one wanted, counting the stream/start messages
    const auto read_until = [&](const std::string& wanted) {
        for (;;) {
            const std::optional<std::vector<uint8_t>> plaintext = client.receive_plaintext();
            if (!plaintext) {
                return false;
            }
            if (plaintext->empty() || plaintext->front() != 0) {
                if (wanted == "chunk" && !plaintext->empty() && plaintext->front() == 4) {
                    return true;
                }
                continue;
            }
            json.assign(plaintext->begin() + 1, plaintext->end());
            starts += json.find(R"("type":"stream/start")") != std::string::npos ? 1 : 0;
            if (json.find(R"("type":")" + wanted + '"') != std::string::npos) {
                return true;
            }
        }
    };
    write(IN_RATE / 5);
    ASSERT_TRUE(read_until("stream/start"));
    ASSERT_TRUE(read_until("chunk"));

    // White-box: an encoder whose frame is shorter refuses the next 44.1 kHz frame. The group's
    // job gets a first step that swaps it on the group audio thread, put in place under the pool's
    // mutex between two jobs, so nothing races the swap.
    std::promise<std::shared_ptr<sendspin::server::AudioWorkers::Task>> task_of;
    loop.post([&] { task_of.set_value(core.groups_.at(core.default_group())->audio_); });
    const std::shared_ptr<sendspin::server::AudioWorkers::Task> task = task_of.get_future().get();
    std::promise<size_t> swapped;
    for (bool wrapped = false; !wrapped;) {
        const std::lock_guard<std::mutex> lock(task->pool_->mutex);
        if (task->running_) {
            continue;
        }
        task->job_ = [inner = std::move(task->job_), &swapped, first = true]() mutable {
            if (std::exchange(first, false)) {
                size_t count = 0;
                using Job = sendspin::server::Group::PipelineJob;
                for (auto& instance : inner.target<Job>()->audio->instances_) {
                    instance.encoder = sendspin::server::FrameEncoder::create(
                        {sendspin::server::sched::Codec::FLAC, 8000, 2, 16});
                    ++count;
                }
                swapped.set_value(count);
            }
            return inner();
        };
        wrapped = true;
    }
    task->wake();
    ASSERT_EQ(swapped.get_future().get(), 1U);
    write(IN_RATE / 5);
    ASSERT_TRUE(read_until("stream/end"));
    const sendspin::server::ServerError error = recorder.wait_for_error(url);
    EXPECT_EQ(error.kind, ErrorKind::PIPELINE_FAILED);
    EXPECT_FALSE(error.client_id.empty());
    EXPECT_EQ(error.format, (StreamFormat{StreamCodec::PCM, IN_RATE, 2, 16}));
    ASSERT_TRUE(read_until("stream/start"));
    EXPECT_NE(json.find(R"("sample_rate":48000)"), std::string::npos) << json;
    write(IN_RATE / 2);
    ASSERT_TRUE(read_until("chunk"));
    view->drain();
    // The run's end reaches the player after anything sent before it. Read before waiting for the
    // drain, which needs the stream/end written, and an unread socket would stall that write.
    ASSERT_TRUE(read_until("group/update"));
    recorder.wait_for(Recorder::drained(sendspin::server::InputId{9}, DrainOutcome::PLAYED));
    EXPECT_EQ(
        recorder.count("error " + std::to_string(static_cast<int>(ErrorKind::PIPELINE_FAILED)) +
                       " " + url),
        1U);
    EXPECT_EQ(starts, 2U);
    const std::string no_format =
        "error " + std::to_string(static_cast<int>(ErrorKind::NO_PLAYABLE_FORMAT)) + " ";
    EXPECT_EQ(recorder.count(no_format + unplayable_url), 1U);
    EXPECT_EQ(recorder.count(no_format + url), 0U);

    loop.post([&] { core.shutdown(0); });
    core_thread.join();
    dispatcher->finish();
    dispatcher->join();
}

sendspin::server::CoreSettings approving_settings() {
    sendspin::server::CoreSettings settings;
    settings.approve_all_clients = true;
    return settings;
}

/// A ServerCore approving every client on its own core thread, shut down when destroyed
struct RunningCore {
    RunningCore() : thread([this] { this->loop.run(); }) {}

    ~RunningCore() {
        this->loop.post([this] { this->core.shutdown(0); });
        this->thread.join();
        this->dispatcher->finish();
        this->dispatcher->join();
    }

    RunningCore(const RunningCore&) = delete;
    RunningCore& operator=(const RunningCore&) = delete;

    /// Dials client, which takes the connection with hello and state
    void dial(sendspin::server::test::RawClient& client, const std::string& hello,
              const std::string& state) {
        const std::string url = client.url();
        this->loop.post(
            [this, url] { this->core.connect_url(url, *sendspin::server::parse_ws_url(url)); });
        ASSERT_NO_FATAL_FAILURE(client.activate(hello));
        client.send_json(state);
    }

    /// Dials client, which takes the connection with hello and state; the host hears it connected
    void connect(sendspin::server::test::RawClient& client, const std::string& hello,
                 const std::string& state = sendspin::server::test::client_state_json(true)) {
        ASSERT_NO_FATAL_FAILURE(this->dial(client, hello, state));
        this->recorder.wait_for("connected " + client.noise.identity().peer_id());
    }

    /// Runs fn on the core thread and waits for it
    template <typename Fn>
    auto on_core(Fn fn) {
        std::promise<decltype(fn())> done;
        this->loop.post([&] {
            if constexpr (std::is_void_v<decltype(fn())>) {
                fn();
                done.set_value();
            } else {
                done.set_value(fn());
            }
        });
        return done.get_future().get();
    }

    /// The group as the groups() snapshot shows it
    sendspin::server::GroupInfo group(sendspin::server::GroupId id) const {
        for (const sendspin::server::GroupInfo& info : this->groups.read()) {
            if (info.id == id) {
                return info;
            }
        }
        return {};
    }

    sendspin::server::GroupInfo server_group() const {
        return this->group(this->core.default_group());
    }

    /// The connection dialed to client; core thread only
    sendspin::server::ServerCore::Client* client_of(
        const sendspin::server::test::RawClient& client) {
        for (const auto& [id, connection] : this->core.clients_) {
            if (connection->url == client.url()) {
                return connection.get();
            }
        }
        return nullptr;
    }

    /// White-box: once client's connection has no write or wait outstanding and nothing to send,
    /// so that its session's end closes it at once, caps its control FIFO at the bytes it holds,
    /// so the next message queued overflows it
    void fill_control(const sendspin::server::test::RawClient& client) {
        while (!this->on_core([&] {
            sendspin::server::SessionDriver& driver = *this->client_of(client)->driver;
            if (driver.writing_ || driver.waiting_ || driver.session().has_message()) {
                return false;
            }
            sendspin::server::SendQueues& queues = driver.session().queues_;
            queues.caps_.control_bytes = queues.control_bytes_;
            return true;
        })) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    sendspin::server::EventLoop loop;
    sendspin::server::AudioWorkers workers{1};
    // Outlives the dispatcher, whose thread calls it until joined
    sendspin::server::test::Recorder recorder;
    std::shared_ptr<sendspin::server::Dispatcher> dispatcher =
        std::make_shared<sendspin::server::Dispatcher>();
    const sendspin::server::Identity identity = *sendspin::server::Identity::generate();
    const sendspin::server::CoreSettings settings = approving_settings();
    sendspin::server::ClientSnapshot snapshot;
    sendspin::server::GroupSnapshot groups;
    sendspin::server::InputRegistry inputs;
    std::atomic<uint64_t> group_ids{1};
    sendspin::server::ServerCore core{loop,     workers,  *dispatcher, &recorder, identity,
                                      settings, snapshot, groups,      inputs,    group_ids};
    std::thread thread;
};

// messaging.md "Server behavior when a client becomes unavailable", "client/leave": a member of a
// shared group that turns unavailable or leaves moves into a group of its own, and the solo
// group's join messages can pass its control FIFO's cap. The pump closes the connection inside
// the move, which clears its group; the core thread goes on and the other member is untouched.
// Control: with room in the FIFO the member stays connected.
TEST(ServerCoreGroups, ClosesAMemberWhoseMoveOverflowsItsQueue) {
    using sendspin::server::test::RawClient;
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    struct Row {
        const char* name;
        const char* message;
        bool full;
    };
    const Row rows[] = {
        {"Control: client/state unavailable with room",
         R"({"type":"client/state","payload":{"available":false}})", false},
        {"client/state unavailable", R"({"type":"client/state","payload":{"available":false}})",
         true},
        {"client/leave", R"({"type":"client/leave","payload":{}})", true},
    };
    const std::string player = sendspin::server::test::player_hello_json(
        "Player", R"([{"codec":"pcm","sample_rate":48000,"channels":2,"bit_depth":16}])", 200000);
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        RunningCore c;
        RawClient leaving;
        RawClient staying;
        ASSERT_NO_FATAL_FAILURE(c.connect(leaving, player));
        ASSERT_NO_FATAL_FAILURE(c.connect(staying, player));
        ASSERT_EQ(c.server_group().client_ids.size(), 2U);
        if (row.full) {
            c.fill_control(leaving);
        }
        leaving.send_json(row.message);
        if (row.full) {
            leaving.read_until_closed();
            const std::vector<sendspin::server::ClientDisconnect> disconnects =
                c.recorder.wait_for_disconnects(1);
            EXPECT_EQ(disconnects.front().reason,
                      sendspin::server::DisconnectReason::CLOSED_BY_SERVER);
        } else {
            // The solo group's group/update follows the one of the first activation
            EXPECT_FALSE(leaving.receive_until("group/update").empty());
            leaving.send_json(sendspin::server::test::client_time_json(1));
            EXPECT_FALSE(leaving.receive_until("server/time").empty());
        }
        staying.send_json(sendspin::server::test::client_time_json(1));
        EXPECT_FALSE(staying.receive_until("server/time").empty());
        EXPECT_EQ(c.server_group().client_ids,
                  std::vector<std::string>{staying.noise.identity().peer_id()});
    }
}

// messaging.md "Server behavior when a client becomes unavailable": the only member of a playing
// group that turns unavailable stops its playback, and the stop's group/update can pass its
// control FIFO's cap. The pump closes the connection inside the stop, which clears its group, and
// the core thread goes on. Control: with room in the FIFO the member gets the stopped
// group/update.
TEST(ServerCoreGroups, ClosesALoneMemberWhoseStopOverflowsItsQueue) {
    using sendspin::server::test::RawClient;
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    for (const bool full : {false, true}) {
        SCOPED_TRACE(full ? "full" : "Control: with room");
        RunningCore c;
        // No player, so the server's group plays with no stream to it
        RawClient client;
        ASSERT_NO_FATAL_FAILURE(c.connect(client, sendspin::server::test::client_hello_json()));
        const sendspin::server::GroupId group = c.core.default_group();
        ASSERT_EQ(c.server_group().client_ids.size(), 1U);
        constexpr uint32_t RATE = 48000;
        const auto view = std::make_shared<sendspin::server::HostView>(
            group, sendspin::server::InputId{3},
            sendspin::server::InputOptions{
                sendspin::server::InputFormat{RATE, 2, sendspin::server::SampleFormat::S16}},
            sendspin::server::GroupParams{250000, 0, 0}, c.dispatcher, &c.recorder);
        ASSERT_TRUE(c.inputs.add(view));
        c.loop.post([&] { c.core.attach_input(view); });
        // Past the write-ahead a group with no player takes, so the run starts; the last writes
        // wait for room as the run plays
        constexpr uint64_t FRAMES =
            (sendspin::server::sched::timing_defaults::WRITE_AHEAD_MIN_US + 200'000) * RATE /
            1'000'000;
        const std::vector<uint8_t> pcm(FRAMES * 4, 0);
        for (uint64_t written = 0; written < FRAMES;) {
            written += view->write(pcm.data(), FRAMES - written).frames;
            if (written < FRAMES) {
                view->wait_writable(INT64_MAX);
            }
        }
        for (;;) {
            const std::string update = client.receive_until("group/update");
            ASSERT_FALSE(update.empty());
            if (update.find(R"("playback_state":"playing")") != std::string::npos) {
                break;
            }
        }
        if (full) {
            c.fill_control(client);
        }
        client.send_json(R"({"type":"client/state","payload":{"available":false}})");
        if (full) {
            client.read_until_closed();
            const std::vector<sendspin::server::ClientDisconnect> disconnects =
                c.recorder.wait_for_disconnects(1);
            EXPECT_EQ(disconnects.front().reason,
                      sendspin::server::DisconnectReason::CLOSED_BY_SERVER);
            // The core thread still runs its handlers, and the group stopped
            c.on_core([] {});
            EXPECT_EQ(c.server_group().playback_state, sendspin::server::PlaybackState::STOPPED);
            EXPECT_TRUE(c.server_group().client_ids.empty());
        } else {
            for (;;) {
                const std::string update = client.receive_until("group/update");
                ASSERT_FALSE(update.empty());
                if (update.find(R"("playback_state":"stopped")") != std::string::npos) {
                    break;
                }
            }
        }
        view->release();
    }
}

// A host's move judges each connection of a client in turn (move_action()). Moving the first out
// of the server's group sends the controller state it changes to the members left, and when the
// second connection's control FIFO cannot take it, that connection closes inside the first one's
// move; the move goes on without it. Control: with room both connections move.
TEST(ServerCoreGroups, MovesPastAConnectionAnEarlierMoveClosed) {
    using sendspin::server::Identity;
    using sendspin::server::test::RawClient;
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    const std::string hello =
        R"({"type":"client/hello","payload":{"name":"Two","supported_roles":["player@v1",)"
        R"("controller@v1"],"player@v1_support":{"supported_formats":[{"codec":"pcm",)"
        R"("sample_rate":48000,"channels":2,"bit_depth":16}],"buffer_capacity":200000},)"
        R"("supported_pair_methods":{"pairing_psk":{}},"unpaired_access":{"enabled":true}}})";
    const auto state = [](uint32_t volume) {
        return sendspin::server::test::client_state_json(
            true, R"({"output_delay_ms":0,"required_lead_time_ms":0,"min_buffer_ms":0,)"
                  R"("supported_commands":["volume"],"volume":)" +
                      std::to_string(volume) + "}");
    };
    for (const bool full : {false, true}) {
        SCOPED_TRACE(full ? "full" : "Control: with room");
        RunningCore c;
        const Identity identity = *Identity::generate();
        RawClient first(sendspin::server::CipherSuite::CHACHAPOLY_SHA256,
                        *Identity::from_private_bytes(identity.private_bytes));
        RawClient second(sendspin::server::CipherSuite::CHACHAPOLY_SHA256,
                         *Identity::from_private_bytes(identity.private_bytes));
        const std::string id = identity.peer_id();
        ASSERT_NO_FATAL_FAILURE(c.connect(first, hello, state(30)));
        // A second connection of a client already reported is not reported again
        ASSERT_NO_FATAL_FAILURE(c.dial(second, hello, state(70)));
        second.send_json(sendspin::server::test::client_time_json(1));
        ASSERT_FALSE(second.receive_until("server/time").empty());
        ASSERT_EQ(c.server_group().client_ids, (std::vector<std::string>{id, id}));
        if (full) {
            c.fill_control(second);
        }
        const sendspin::server::GroupId moved{c.group_ids.fetch_add(1)};
        c.on_core([&] { c.core.create_group(moved, "Moved", id); });
        if (full) {
            second.read_until_closed();
            EXPECT_EQ(c.group(moved).client_ids, std::vector<std::string>{id});
        } else {
            EXPECT_EQ(c.group(moved).client_ids, (std::vector<std::string>{id, id}));
        }
        EXPECT_TRUE(c.server_group().client_ids.empty());
        first.send_json(sendspin::server::test::client_time_json(1));
        EXPECT_FALSE(first.receive_until("server/time").empty());
    }
}

}  // namespace
