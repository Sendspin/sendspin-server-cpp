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

/// @file test_source_sink_internals.cpp
/// @brief A LiveSourceSink whose ASRC refuses a chunk. Compiled with -fno-access-control
/// (tests/CMakeLists.txt).

#include "profile.h"
#include "source_clock.h"
#include "source_sink_rig.h"
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace {

using sendspin::server::SampleFormat;
using sendspin::server::test::CHUNK;
using sendspin::server::test::RATE;
using sendspin::server::test::SinkRig;
using sendspin::server::test::tone;
using sendspin::server::test::TONE_STEP;

/// A source's tone at position as the input stages it, unaltered
float staged_tone(uint64_t position) {
    return static_cast<float>(tone(position)) / 32768.0F;
}

/// A source whose clock runs 2,000 ppm fast, writing 20 ms chunks into a SinkRig, whose ASRC
/// refuses a chunk on request. The refusal comes from a block size of zero, which takes nothing: no
/// input the server receives makes micro-resampler refuse.
class RefusingSource {
public:
    RefusingSource() {
        EXPECT_TRUE(this->rig.sink->begin({RATE, 1, SampleFormat::S16}));
    }

    void next() {
        this->rig.write(this->position, CHUNK, this->capture);
        this->position += CHUNK;
        this->capture += std::llround(20000.0 / 1.002);
    }

    /// One chunk the ASRC refuses, staged unaltered
    void refused() {
        const size_t refused_at = this->rig.staged.size();
        const uint64_t refused_position = this->position;
        const size_t max_block = this->rig.sink->asrc_->max_block_;
        this->rig.sink->asrc_->max_block_ = 0;
        this->next();
        this->rig.sink->asrc_->max_block_ = max_block;
        ASSERT_EQ(this->rig.staged.size(), refused_at + CHUNK);
        for (size_t i = 0; i < CHUNK; ++i) {
            EXPECT_EQ(this->rig.staged[refused_at + i], staged_tone(refused_position + i)) << i;
        }
    }

    /// Chunks until the correction has started, then 50 more
    void engage() {
        for (int k = 0; k < 750 && !this->rig.sink->clock_.engaged(); ++k) {
            this->next();
        }
        ASSERT_TRUE(this->rig.sink->clock_.engaged());
        for (int k = 0; k < 50; ++k) {
            this->next();
        }
    }

    SinkRig rig;
    uint64_t position{0};
    int64_t capture{1000000};
};

// A chunk the ASRC refuses once the correction has started is staged unaltered, and the
// correction starts over from the next chunk, whose tone then continues it rather than being cut
// as staged already
TEST(LiveSourceSink, StartsTheCorrectionOverAfterTheAsrcRefuses) {
    if (!sendspin::server::profile::SOURCE_ASRC) {
        GTEST_SKIP() << "the build corrects by slipping frames";
    }
    RefusingSource source;
    source.engage();
    source.refused();

    // The next chunks continue the tone, at most the ASRC's hold behind
    const size_t resumed_at = source.rig.staged.size();
    const uint64_t resumed_position = source.position;
    constexpr int AFTER = 10;
    for (int k = 0; k < AFTER; ++k) {
        source.next();
    }
    EXPECT_GE(source.rig.staged.size(), resumed_at + (AFTER * CHUNK) - 64);
    ASSERT_GT(source.rig.staged.size(), resumed_at + 200);
    for (size_t i = 0; i < 200; ++i) {
        // 200 frames at the clamp move the source's position by a tenth of a frame
        EXPECT_NEAR(source.rig.staged[resumed_at + i], staged_tone(resumed_position + i),
                    (0.1 * TONE_STEP) + 0.002)
            << i;
    }
}

// A chunk the ASRC refuses before the correction starts leaves nothing to cut where it starts: the
// input stages every frame but what the correction drops, at most SOURCE_CLOCK_MAX_PPM of them,
// and what the ASRC holds
TEST(LiveSourceSink, StartsTheCorrectionOverAfterTheAsrcRefusesBeforeIt) {
    if (!sendspin::server::profile::SOURCE_ASRC) {
        GTEST_SKIP() << "the build corrects by slipping frames";
    }
    RefusingSource source;
    for (int k = 0; k < 25; ++k) {
        source.next();
    }
    ASSERT_FALSE(source.rig.sink->clock_.engaged());
    source.refused();
    source.engage();
    const auto written = static_cast<double>(source.position);
    const auto most_dropped =
        static_cast<size_t>(std::ceil(written * sendspin::server::SOURCE_CLOCK_MAX_PPM / 1e6) + 64);
    EXPECT_GE(source.rig.staged.size() + most_dropped, source.position);
}

}  // namespace
