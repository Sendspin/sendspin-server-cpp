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

/// @file silence_feeder.h
/// @brief A thread that keeps a group playing by writing silence to its input

#pragma once

#include <sendspin/server/server.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace sendspin::server::test {

/// Writes 48 kHz 16-bit stereo silence to input until destroyed, then stops the input, which
/// ends its group's run
class SilenceFeeder {
public:
    explicit SilenceFeeder(std::shared_ptr<GroupInput> input) : input_(std::move(input)) {
        this->thread_ = std::thread([this] {
            constexpr size_t FRAMES = 960;
            const std::vector<int16_t> pcm(2 * FRAMES, 0);
            constexpr int64_t WAIT_US = 20000;
            while (!this->stop_.load()) {
                if (this->input_->write(pcm.data(), FRAMES).frames == 0) {
                    this->input_->wait_writable(WAIT_US);
                }
            }
        });
    }

    ~SilenceFeeder() {
        this->stop_.store(true);
        this->thread_.join();
        this->input_->stop();
    }

    SilenceFeeder(const SilenceFeeder&) = delete;
    SilenceFeeder& operator=(const SilenceFeeder&) = delete;

private:
    std::shared_ptr<GroupInput> input_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace sendspin::server::test
