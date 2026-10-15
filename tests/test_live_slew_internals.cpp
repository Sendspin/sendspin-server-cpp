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

/// @file test_live_slew_internals.cpp
/// @brief A live slew whose ASRC refuses a block keeps it. Compiled with -fno-access-control
/// (tests/CMakeLists.txt).

#include "live_slew.h"
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using sendspin::server::LiveSlew;
using sendspin::server::ResamplerQuality;
using sendspin::server::SampleFormat;

constexpr uint32_t RATE = 48000;
constexpr size_t BLOCK = RATE / 100;

// An ASRC that refuses a block falls the slew back to dropped frames until it is prepared again,
// restarted or not, and the slew keeps it, so the write it refused in frees nothing.
TEST(LiveSlew, FallsBackToDroppedFramesKeepingTheAsrc) {
    // A block of a mono float ramp
    std::vector<uint8_t> in(BLOCK * sizeof(float));
    for (size_t k = 0; k < BLOCK; ++k) {
        const float sample = (static_cast<float>(k) / static_cast<float>(BLOCK)) - 0.5F;
        std::memcpy(&in[k * sizeof(float)], &sample, sizeof(float));
    }
    LiveSlew slew;
    slew.prepare({RATE, 1, SampleFormat::F32}, true, ResamplerQuality::MEDIUM);
    slew.start(0);
    ASSERT_TRUE(slew.engaged());
    // An empty first block cannot prime it
    EXPECT_EQ(slew.process(in.data(), 0, 300, 1000000).written, 0U);
    EXPECT_FALSE(slew.engaged());
    EXPECT_NE(slew.asrc_, nullptr);
    slew.restart();
    EXPECT_FALSE(slew.awaits_start());
    // At no slew a block passes as it came
    const LiveSlew::Block block = slew.process(in.data(), BLOCK, 0, 1000000);
    ASSERT_EQ(block.written, BLOCK);
    EXPECT_EQ(std::memcmp(slew.output(), in.data(), in.size()), 0);
    // Control: prepared again, it waits for start()
    slew.prepare({RATE, 1, SampleFormat::F32}, true, ResamplerQuality::MEDIUM);
    EXPECT_TRUE(slew.awaits_start());
}

}  // namespace
