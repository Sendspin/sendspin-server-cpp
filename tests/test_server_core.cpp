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
/// @brief ServerCore on its own core thread: shutdown closes an input whose attachment has not
/// run, and a group takes an input over one the host released whose detach has not run.

#include "crypto/keys.h"
#include "dispatcher.h"
#include "host_view.h"
#include "server_core.h"
#include "test_recorder.h"
#include "transport.h"
#include <gtest/gtest.h>
#include <noise/protocol.h>

#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <thread>
#include <vector>

namespace {

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

}  // namespace
