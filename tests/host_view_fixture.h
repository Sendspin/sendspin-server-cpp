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

/// @file host_view_fixture.h
/// @brief HostViewTest, the fixture of a host view of its own, with a recorder and a dispatcher

#pragma once

#include "dispatcher.h"
#include "host_view.h"
#include "test_recorder.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace sendspin::server::test {

/// @brief A host view of its own, made by make(), make_live() or make_slewing()
class HostViewTest : public testing::Test {
public:
    static constexpr uint32_t RATE = 48000;
    static constexpr int64_t LEAD_US = 300000;
    static constexpr int64_t AHEAD_US = 1000000;
    static constexpr uint64_t AHEAD_FRAMES = RATE;
    static constexpr int64_t LIVE_AHEAD_US = 200000;
    /// W_live widened to 3 s, so that one write holds 2 s, which the slew takes enough frames out
    /// of to count
    static constexpr int64_t SLEW_AHEAD_US = 3000000;
    /// A cell of frames
    static constexpr uint64_t CELL = RATE / 50;

protected:
    // A posted outbox task holds its view, and the view the dispatcher: the dispatch thread may
    // still call the recorder, so it finishes before the members go
    ~HostViewTest() override {
        this->dispatcher->finish();
        this->dispatcher->join();
    }

    HostView& make(const GroupParams& params = {LEAD_US, 0, AHEAD_US},
                   const InputFormat& format = {RATE, 2, SampleFormat::S16}) {
        this->view = std::make_shared<HostView>(GroupId{1}, InputId{2}, InputOptions{format},
                                                params, this->dispatcher, &this->recorder);
        return *this->view;
    }

    /// A live input: `W_live` 200 ms, as core writes it
    HostView& make_live() {
        InputOptions options{{RATE, 2, SampleFormat::S16}};
        options.kind = sendspin::server::InputKind::LIVE;
        this->view = std::make_shared<HostView>(GroupId{1}, InputId{2}, options,
                                                GroupParams{LEAD_US, 0, LIVE_AHEAD_US},
                                                this->dispatcher, &this->recorder);
        return *this->view;
    }

    /// A live input with SLEW_AHEAD_US and a least lead
    HostView& make_slewing(int64_t min_lead_us) {
        InputOptions options{{RATE, 2, SampleFormat::S16}};
        options.kind = sendspin::server::InputKind::LIVE;
        this->view = std::make_shared<HostView>(
            GroupId{1}, InputId{2}, options, GroupParams{LEAD_US, 0, SLEW_AHEAD_US},
            this->dispatcher, &this->recorder, sendspin::server::LiveOptions{min_lead_us});
        return *this->view;
    }

    WriteResult write(uint64_t frames) {
        const std::vector<uint8_t> pcm(frames * 4, 1);
        return this->view->write(pcm.data(), frames);
    }

    Recorder recorder;
    std::shared_ptr<sendspin::server::Dispatcher> dispatcher =
        std::make_shared<sendspin::server::Dispatcher>();
    std::shared_ptr<HostView> view;
};

}  // namespace sendspin::server::test
