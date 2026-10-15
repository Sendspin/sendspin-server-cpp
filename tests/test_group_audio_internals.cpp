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

/// @file test_group_audio_internals.cpp
/// @brief A group's pipelines on their own, reached inside: a resume from a paused position the
/// test sets, a stretch cut or extended directly, the resamplers a job builds, an instance whose
/// encoder is swapped for one that refuses, and the `L_p` a live rebase publishes. Compiled with
/// -fno-access-control (tests/CMakeLists.txt).

#include "clock.h"
#include "frame_encoder.h"
#include "group_audio.h"
#include "group_pipelines.h"
#include "host_view.h"
#include "pipeline_stages.h"
#include "sched/leads.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <variant>
#include <vector>

namespace {

using sendspin::server::EpochStarted;
using sendspin::server::GroupEvent;
using sendspin::server::InputFormat;
using sendspin::server::now_us;
using sendspin::server::PipelineFailed;
using sendspin::server::SampleFormat;
using sendspin::server::test::concatenated;
using sendspin::server::test::drained_last_cell;
using sendspin::server::test::expect_resampled;
using sendspin::server::test::final_indices;
using sendspin::server::test::frames_by_instance;
using sendspin::server::test::live_input;
using sendspin::server::test::Pipelines;
using sendspin::server::test::resample_offline;
using sendspin::server::test::run_all;
using sendspin::server::test::Tone;
namespace sched = sendspin::server::sched;
using Bytes = std::vector<uint8_t>;

constexpr int64_t LEAD_US = Pipelines::LEAD_US;
/// A live input's write jitter `J_in`
constexpr int64_t JITTER_US = 20000;

// A resume whose paused position lies in a later segment starts the new epoch in that segment's
// format: one inside the 48 kHz segment, and one exactly on its first position. The 48 kHz
// instance's frames are the input from the paused position, bit for bit; on the boundary the 44.1
// kHz instance has no history in the segment, so it is micro-resampler's output over the segment
// from its start, primed, sample for sample
TEST(GroupAudioPipelines, ResumesInALaterSegment) {
    struct Row {
        const char* name;
        uint64_t paused;
    };
    const std::array<Row, 2> rows{{{"inside the segment", 44100 + 9600}, {"on its start", 44100}}};
    const Tone first(44100, 2, 44100);
    const Tone second(48000, 2, 48000, 48000);
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        Pipelines p(InputFormat{44100, 2, SampleFormat::S16}, 10000000);
        const sched::InstanceId at48{190};
        const sched::InstanceId at44{191};
        p.slot->publish({{at48, {}, {sched::Codec::PCM, 48000, 2, 16}, 0},
                         {at44, {}, {sched::Codec::PCM, 44100, 2, 16}, 0}});
        ASSERT_EQ(p.view->write(first.pcm.data(), 44100).frames, 44100U);
        ASSERT_TRUE(p.view->set_format({48000, 2, SampleFormat::S16}));
        ASSERT_EQ(p.view->write(second.pcm.data(), 48000).frames, 48000U);
        run_all(p);
        p.view->pause();
        // White-box: the paused position, which pause() takes from the time
        p.view->p_pause_ = row.paused;
        run_all(p);
        p.view->resume();
        p.view->drain();
        const std::vector<GroupEvent> events = run_all(p);
        const auto started = std::find_if(events.begin(), events.end(), [](const GroupEvent& e) {
            return std::holds_alternative<EpochStarted>(e);
        });
        ASSERT_NE(started, events.end());
        const sched::Anchor& anchor = std::get<EpochStarted>(*started).anchor;
        EXPECT_EQ(anchor.segments.front().position, row.paused);
        EXPECT_EQ(anchor.segments.front().rate, 48000U);
        std::map<sched::InstanceId, std::vector<sched::Frame>> frames = frames_by_instance(events);

        const Bytes out48 = concatenated(frames[at48]);
        const size_t skipped = (row.paused - 44100) * 4;
        ASSERT_EQ(out48.size(), second.pcm.size() - skipped);
        EXPECT_TRUE(std::equal(out48.begin(), out48.end(), second.pcm.begin() + skipped));
        if (row.paused == 44100) {
            const Bytes out44 = concatenated(frames[at44]);
            const std::vector<float> reference =
                resample_offline(second.floats, 2, 48000, 44100, 960);
            ASSERT_EQ(out44.size() / 2, reference.size());
            expect_resampled(out44, 0, reference);
        }
    }
}

// A stretch's output is cut or extended with its last sample to the boundary's output index; a
// warm-up's samples, below the next cell's first, are counted but never kept; an extension past two
// samples, which no stage gives, is refused and changes nothing. The stages give no short stretch
// through the property test above, so the extension is reached directly
TEST(GroupAudioPipelines, CutsOrExtendsAStretchToItsBoundaryCount) {
    struct Row {
        const char* name;
        int64_t produced;
        size_t pending;
        int64_t total;
        bool accepted;
        size_t kept;
        size_t extended;
    };
    // Mono 16-bit samples of 1 and a last sample of 7; cell 1's first output sample is 882
    const std::array<Row, 5> rows{{
        {"cut", 900, 18, 895, true, 13, 0},
        {"extended", 900, 18, 902, true, 18, 2},
        {"extended past a warm-up", 881, 0, 883, true, 0, 1},
        {"refused", 900, 18, 903, false, 18, 0},
        // Control: at the boundary already
        {"exact", 900, 18, 900, true, 18, 0},
    }};
    Pipelines p(InputFormat{44100, 1, SampleFormat::S16});
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        // White-box: an instance with only what the cut reads
        sendspin::server::GroupAudio::Instance instance;
        instance.cells = 1;
        instance.frame_samples = 882;
        instance.sample_bytes = 2;
        instance.next = 1;
        instance.produced = row.produced;
        instance.pending.assign(row.pending * 2, 0);
        for (size_t i = 0; i < row.pending; ++i) {
            instance.pending[2 * i] = 1;
        }
        instance.last_sample = {7, 0};
        EXPECT_EQ(p.audio.cut_to(instance, row.total), row.accepted);
        EXPECT_EQ(instance.produced, row.accepted ? row.total : row.produced);
        ASSERT_EQ(instance.pending.size(), (row.kept + row.extended) * 2);
        for (size_t i = 0; i < row.kept + row.extended; ++i) {
            EXPECT_EQ(instance.pending[2 * i], i < row.kept ? 1 : 7) << i;
        }
    }
}

// A job builds at most one resampler, whose filter bank is the costly allocation, and reports more
// work while another waits; an instance with none, listed after one that waits, is built at once
TEST(GroupAudioPipelines, BuildsOneResamplerPerJob) {
    Pipelines p(InputFormat{44100, 2, SampleFormat::S16});
    p.slot->publish({{sched::InstanceId{120}, {}, {sched::Codec::PCM, 48000, 2, 16}, 0},
                     {sched::InstanceId{122}, {}, {sched::Codec::PCM, 96000, 2, 16}, 0},
                     {sched::InstanceId{121}, {}, {sched::Codec::PCM, 44100, 2, 24}, 0}});
    const auto built = [&] {
        return std::count_if(p.audio.instances_.begin(), p.audio.instances_.end(),
                             [](const auto& instance) { return instance.configured; });
    };
    // White-box: one job at a time, before any input
    EXPECT_TRUE(p.audio.run_job(now_us()));
    EXPECT_EQ(built(), 2);
    EXPECT_FALSE(p.audio.run_job(now_us()));
    EXPECT_EQ(built(), 3);
}

// An encoder that refuses fails its instance once, which then produces nothing and no longer holds
// a drain back; the group's other instance carries on through the drain
TEST(GroupAudioPipelines, FailsAnInstanceWhoseEncoderRefuses) {
    Pipelines p(InputFormat{44100, 2, SampleFormat::S16});
    const sched::InstanceId native{80};
    const sched::InstanceId resampled{81};
    p.slot->publish({{native, {}, {sched::Codec::PCM, 44100, 2, 16}, 0},
                     {resampled, {}, {sched::Codec::PCM, 48000, 2, 16}, 0}});
    const Bytes pcm(6 * 882 * 4, 3);
    ASSERT_EQ(p.view->write(pcm.data(), 2 * 882).frames, 2 * 882U);
    std::map<sched::InstanceId, std::vector<sched::Frame>> frames = frames_by_instance(p.run());
    ASSERT_EQ(frames[resampled].size(), 1U);
    // Cell 1 waits for the resampler's lookahead in both instances
    ASSERT_EQ(frames[native].size(), 1U);

    // White-box: an encoder whose frame is shorter refuses a 48 kHz frame
    auto& instances = p.audio.instances_;
    const auto it = std::find_if(instances.begin(), instances.end(),
                                 [&](const auto& instance) { return instance.id == resampled; });
    ASSERT_NE(it, instances.end());
    it->encoder = sendspin::server::FrameEncoder::create({sched::Codec::FLAC, 8000, 2, 16});
    ASSERT_TRUE(it->encoder);
    ASSERT_EQ(p.view->write(pcm.data(), 4 * 882).frames, 4 * 882U);
    p.view->drain();
    const std::vector<GroupEvent> events = p.run();
    size_t failures = 0;
    for (const GroupEvent& event : events) {
        if (const auto* failed = std::get_if<PipelineFailed>(&event)) {
            EXPECT_EQ(failed->instance, resampled);
            ++failures;
        }
    }
    EXPECT_EQ(failures, 1U);
    frames = frames_by_instance(events);
    EXPECT_TRUE(frames[resampled].empty());
    EXPECT_EQ(frames[native].size(), 5U);
    EXPECT_EQ(drained_last_cell(events), 5);
    std::map<sched::InstanceId, int64_t> finals = final_indices(events);
    EXPECT_EQ(finals.count(resampled), 0U);
    EXPECT_EQ(finals[native], 5);
    EXPECT_TRUE(p.run().empty());
}

// A live rebase takes the largest `L_p` over the frame instances. From 44.1 kHz, a 48 kHz
// instance's resampler reads 53 input frames, 1202 us, past each cell, which a native 44.1 kHz
// instance does not, so the late frame shifts to G - (20000 + 1202 + J) + J in both, and admission
// is published that `L_p`
TEST(GroupAudioLive, RebasesByTheResampledInstancesLookahead) {
    constexpr uint32_t IN_RATE = 44100;
    constexpr uint64_t IN_CELL = IN_RATE / 50;
    constexpr int64_t RESAMPLED_LATENCY_US = 20000 + 1202 + JITTER_US;
    ASSERT_EQ(
        sched::pipeline_latency_us({1,
                                    sendspin::server::Resampler::filter_length(
                                        IN_RATE, 48000, sendspin::server::ResamplerQuality::MEDIUM),
                                    IN_RATE},
                                   sendspin::server::InputKind::LIVE, JITTER_US),
        RESAMPLED_LATENCY_US);
    Pipelines p(live_input({IN_RATE, 1, SampleFormat::S16}, JITTER_US), 200000);
    const sched::InstanceId resampled{120};
    const sched::InstanceId native{121};
    p.slot->publish({{resampled, {}, {sched::Codec::PCM, 48000, 1, 24}, 0},
                     {native, {}, {sched::Codec::PCM, IN_RATE, 1, 24}, 0}});
    const Bytes cells(5 * IN_CELL * 2, 1);
    ASSERT_EQ(p.view->write(cells.data(), 2 * IN_CELL).frames, 2 * IN_CELL);
    std::vector<GroupEvent> events = p.run();
    ASSERT_FALSE(events.empty());
    const int64_t t0 = std::get<EpochStarted>(events[0]).anchor.t0_us;

    // Cells 2 to 4 come 190 ms late. The shift index is 1, which the resampled instance could not
    // make without the input past cell 1, and the native one holds at until the rebase is decided
    constexpr int64_t LATE_US = 190000;
    ASSERT_EQ(p.view->write(cells.data(), 3 * IN_CELL).frames, 3 * IN_CELL);
    const int64_t before = now_us() + LATE_US;
    events = p.run(LATE_US);
    const int64_t after = now_us() + LATE_US;
    std::optional<sendspin::server::AnchorShift> shift;
    for (const GroupEvent& event : events) {
        if (const auto* found = std::get_if<sendspin::server::AnchorShift>(&event)) {
            ASSERT_FALSE(shift) << "a second shift";
            shift = *found;
        }
    }
    ASSERT_TRUE(shift);
    ASSERT_EQ(shift->index, 1);
    const int64_t at = t0 + 20000 + shift->delta_us;
    EXPECT_GE(at, before + LEAD_US - RESAMPLED_LATENCY_US + JITTER_US);
    EXPECT_LE(at, after + LEAD_US - RESAMPLED_LATENCY_US + JITTER_US);
    std::map<sched::InstanceId, std::vector<sched::Frame>> frames = frames_by_instance(events);
    for (const sched::InstanceId instance : {resampled, native}) {
        const auto frame = std::find_if(frames[instance].begin(), frames[instance].end(),
                                        [](const sched::Frame& f) { return f.index == 1; });
        ASSERT_NE(frame, frames[instance].end()) << instance.value;
        EXPECT_EQ(frame->ts_us, at) << instance.value;
    }
    EXPECT_EQ(p.view->rebase_latency_us_, RESAMPLED_LATENCY_US);
}

}  // namespace
