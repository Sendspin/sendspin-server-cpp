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

/// @file test_host_view_internals.cpp
/// @brief A host view's latency slew, set going through the latency a live input owes, and its
/// outbox as a host call leaves it before posting. Compiled with -fno-access-control
/// (tests/CMakeLists.txt).

#include "clock.h"
#include "host_view.h"
#include "host_view_fixture.h"
#include "job_view.h"
#include "test_recorder.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <numbers>
#include <thread>
#include <vector>

namespace {

using sendspin::server::DrainOutcome;
using sendspin::server::HostView;
using sendspin::server::InputId;
using sendspin::server::InputState;
using sendspin::server::now_us;
using sendspin::server::SampleFormat;
using sendspin::server::WriteResult;
using sendspin::server::test::HostViewTest;
using sendspin::server::test::job_view_of;
using sendspin::server::test::Recorder;
using Bytes = std::vector<uint8_t>;

constexpr uint32_t RATE = HostViewTest::RATE;

/// A frame's time at 48 kHz, rounded up
constexpr int64_t FRAME_US = (1000000 + RATE - 1) / RATE;

/// A 200 Hz tone at half scale at frame k, from its peak, as a 16-bit sample
int16_t tone_at(double frame) {
    return static_cast<int16_t>(
        std::lround(16384.0 * std::cos(2.0 * std::numbers::pi * 200.0 * frame / RATE)));
}

/// Frames [from, from + count) of the tone, 16-bit stereo
Bytes tone_pcm(uint64_t from, uint64_t count) {
    Bytes pcm(count * 4);
    for (uint64_t k = 0; k < count; ++k) {
        const int16_t sample = tone_at(static_cast<double>(from + k));
        std::memcpy(&pcm[(k * 4)], &sample, 2);
        std::memcpy(&pcm[(k * 4) + 2], &sample, 2);
    }
    return pcm;
}

// A live input whose group lead fell owes the excess its writes land past a fresh anchor; a write
// that owes it plays LOWERING_SLEW_PPM fast, or as much slower as plays out only what it owes: it
// stages fewer positions and reports those taken, and `G_eff` follows its lead down by the time
// taken out, never below `G_next` nor the input's least lead, against which what is owed is also
// measured. A window of writes is seeded behind the input, and each row's write closes none; the
// anchor's half second keeps the excess above any row's debt unless the thread runs that late.
TEST_F(HostViewTest, PlaysALiveInputFastWhileItOwesLatency) {
    using sendspin::server::sched::LOWERING_SLEW_PPM;
    constexpr uint64_t WRITE = 2 * RATE;
    constexpr int64_t WRITE_US = 2000000;
    constexpr uint64_t ANCHOR = RATE / 2;
    struct Row {
        const char* name;
        /// How far the group lead falls after the anchor, the input's least lead, the debt seeded
        /// and the frames the anchoring write holds
        int64_t fall_us;
        int64_t min_lead_us;
        int64_t debt_us;
        uint64_t anchor_frames;
        /// The time the slew takes out of the write
        int64_t slewed_us;
    };
    const Row rows[] = {
        {"Control: nothing owed stages the write as it came", 200000, 0, 0, ANCHOR, 0},
        {"Owing more than the write's share plays at the slew rate", 200000, 0, 1000000, ANCHOR,
         WRITE_US * LOWERING_SLEW_PPM / 1000000},
        // All but a frame of it, for the part frame a frame drop carries: 80 us, 40 ppm
        {"Owing less plays only that out", 200000, 0, 101, ANCHOR, 101 - FRAME_US},
        {"G_eff stops at G_next", 50, 0, 1000000, ANCHOR, WRITE_US * LOWERING_SLEW_PPM / 1000000},
        {"G_eff stops at the least lead", 200000, LEAD_US - 50, 1000000, ANCHOR,
         WRITE_US * LOWERING_SLEW_PPM / 1000000},
        // Twenty frames staged, fewer than the ASRC's filter spans
        {"A short history starts the slew from what there is", 200000, 0, 1000000, 20,
         WRITE_US * LOWERING_SLEW_PPM / 1000000},
        // A frame's anchor and a millisecond's wait: its first sample lands under the least lead,
        // however late the thread, and 200 ms past G_next
        {"What is owed is measured against the least lead", 200000, LEAD_US, 1000000, 1, 0},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        HostView& view = this->make_slewing(row.min_lead_us);
        const Bytes anchor = tone_pcm(0, row.anchor_frames);
        const Bytes pcm = tone_pcm(row.anchor_frames, WRITE);
        ASSERT_EQ(view.write(anchor.data(), row.anchor_frames).frames, row.anchor_frames);
        if (row.anchor_frames == 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        view.set_group_params({LEAD_US - row.fall_us, 0, SLEW_AHEAD_US});
        view.lowering_ = {now_us(), std::numeric_limits<int64_t>::max(), 0, row.debt_us};
        const WriteResult result = view.write(pcm.data(), WRITE);
        // Admission admits the whole write, so all it lacks is what the slew took out; the
        // positions staged are what it reports taken
        EXPECT_EQ(result.state, InputState::OPEN);
        const sendspin::server::JobView job = job_view_of(view);
        EXPECT_EQ(job.head, row.anchor_frames + result.frames);
        const auto missing = static_cast<int64_t>(WRITE - result.frames);
        const int64_t least_lead = std::max(LEAD_US - row.fall_us, row.min_lead_us);
        if (row.slewed_us == 0) {
            EXPECT_EQ(missing, 0);
            EXPECT_EQ(job.group_lead_eff_us, LEAD_US);
            Bytes staged;
            ASSERT_TRUE(view.read(job.run.epoch, row.anchor_frames, WRITE, staged));
            EXPECT_EQ(staged, pcm);
            continue;
        }
        // A frame's rounding either way, and what the ASRC's filter holds where it runs
        EXPECT_GE(missing, (row.slewed_us / FRAME_US) - 1) << missing;
        EXPECT_LE(missing, (row.slewed_us * RATE / 1000000) + 2 +
                               static_cast<int64_t>(sendspin::server::LIVE_SLEW_BLOCK_FRAMES))
            << missing;
        EXPECT_GE(job.group_lead_eff_us,
                  std::max(least_lead, LEAD_US - row.slewed_us - FRAME_US - 1));
        EXPECT_LE(job.group_lead_eff_us,
                  std::max(least_lead, LEAD_US - row.slewed_us + (2 * FRAME_US) + 1));
        // Staged frame j is the input's frame at anchor + j times the rate, the slew starting
        // where the anchor's frames ended: through the ASRC to its filter's error and a 16-bit
        // step, by dropped frames one input frame as it came within a frame of it
        Bytes staged;
        ASSERT_TRUE(view.read(job.run.epoch, row.anchor_frames, result.frames, staged));
        const double ratio = 1.0 + (static_cast<double>(row.slewed_us) / WRITE_US);
        for (uint64_t j = 0; j < result.frames; ++j) {
            int16_t sample = 0;
            std::memcpy(&sample, &staged[j * 4], 2);
            const double at = static_cast<double>(row.anchor_frames) + (j * ratio);
            if (sendspin::server::profile::SOURCE_ASRC) {
                ASSERT_NEAR(sample, tone_at(at), 70) << j;
            } else {
                const auto low = static_cast<uint64_t>(at);
                ASSERT_TRUE(sample == tone_at(static_cast<double>(low)) ||
                            sample == tone_at(static_cast<double>(low + 1)))
                    << j;
            }
        }
    }
}

/// Frames [from, from + count) of the tone, float stereo
Bytes tone_floats(uint64_t from, uint64_t count) {
    Bytes pcm(count * 8);
    for (uint64_t k = 0; k < count; ++k) {
        const auto sample = static_cast<float>(tone_at(static_cast<double>(from + k)) / 32768.0);
        std::memcpy(&pcm[(k * 8)], &sample, 4);
        std::memcpy(&pcm[(k * 8) + 4], &sample, 4);
    }
    return pcm;
}

// A format set while a live input slews starts a new segment, and the slew starts over in it, in
// the new format, from the segment's first frame: the next write's staged audio is the tone at the
// slew's rate from where that write began, in floats, with none of the old segment's frames
TEST_F(HostViewTest, StartsTheSlewOverInANewFormat) {
    using sendspin::server::sched::LOWERING_SLEW_PPM;
    constexpr uint64_t ANCHOR = RATE / 2;
    HostView& view = this->make_slewing(0);
    const Bytes anchor = tone_pcm(0, ANCHOR);
    const Bytes before = tone_pcm(ANCHOR, RATE);
    // From a peak of the tone, as the slew's first frame primes it
    const Bytes after = tone_floats(ANCHOR + RATE, RATE);
    ASSERT_EQ(view.write(anchor.data(), ANCHOR).frames, ANCHOR);
    view.set_group_params({LEAD_US - 200000, 0, SLEW_AHEAD_US});
    view.lowering_ = {now_us(), std::numeric_limits<int64_t>::max(), 0, 1000000};
    ASSERT_LT(view.write(before.data(), RATE).frames, RATE);
    const uint64_t segment = job_view_of(view).head;
    ASSERT_TRUE(view.set_format({RATE, 2, SampleFormat::F32}));
    const WriteResult result = view.write(after.data(), RATE);
    ASSERT_EQ(result.state, InputState::OPEN);
    ASSERT_LT(result.frames, RATE);
    const sendspin::server::JobView job = job_view_of(view);
    ASSERT_EQ(job.head, segment + result.frames);
    Bytes staged;
    ASSERT_TRUE(view.read(job.run.epoch, segment, result.frames, staged));
    const double ratio = 1.0 + (static_cast<double>(LOWERING_SLEW_PPM) * 1e-6);
    for (uint64_t j = 0; j < result.frames; ++j) {
        float sample = 0.0F;
        std::memcpy(&sample, &staged[j * 8], 4);
        const double at = static_cast<double>(ANCHOR + RATE) + (j * ratio);
        if (sendspin::server::profile::SOURCE_ASRC) {
            ASSERT_NEAR(sample * 32768.0, tone_at(at), 70) << j;
        } else {
            const auto low = static_cast<uint64_t>(at);
            const auto exact = [&](uint64_t k) {
                return sample == static_cast<float>(tone_at(static_cast<double>(k)) / 32768.0);
            };
            ASSERT_TRUE(exact(low) || exact(low + 1)) << j;
        }
    }
}

// The write that takes up set_format()'s slew frees nothing: the slew it replaced stays in the view
// until core frees it (free_retired_slew()), which leaves alone a slew set_format() made that no
// write has taken up yet. White-box: HostView::slew_, next_slew_.
TEST_F(HostViewTest, LeavesTheSlewAFormatReplacedForCore) {
    HostView& view = this->make_slewing(0);
    ASSERT_EQ(this->write(CELL).frames, CELL);
    const uint8_t* replaced = view.slew_.output();
    ASSERT_TRUE(view.set_format({RATE, 2, SampleFormat::F32}));
    const uint8_t* made = view.next_slew_.output();
    view.free_retired_slew();
    ASSERT_EQ(view.next_slew_.output(), made);
    const Bytes pcm(CELL * 8, 0);
    ASSERT_EQ(view.write(pcm.data(), CELL).frames, CELL);
    EXPECT_EQ(view.slew_.output(), made);
    EXPECT_EQ(view.next_slew_.output(), replaced);
    view.free_retired_slew();
    EXPECT_FALSE(view.next_slew_.ready());
    EXPECT_EQ(view.slew_.output(), made);
}

// While a write runs through the slew, between the step that takes it up and the steps that stage
// its blocks, the input is FULL: another write takes nothing, and a writer waiting for room is
// woken as the slewed write ends, the input OPEN again
TEST_F(HostViewTest, HoldsOffWritersWhileAWriteSlews) {
    constexpr uint64_t ANCHOR = RATE / 2;
    HostView& view = this->make_slewing(0);
    const Bytes anchor = tone_pcm(0, ANCHOR);
    const Bytes pcm = tone_pcm(ANCHOR, RATE);
    ASSERT_EQ(view.write(anchor.data(), ANCHOR).frames, ANCHOR);
    const HostView::SlewedWrite slewed = view.begin_slewed(RATE, 300, InputState::OPEN);
    EXPECT_EQ(view.state(), InputState::FULL);
    const WriteResult between = view.write(pcm.data(), RATE);
    EXPECT_EQ(between.frames, 0U);
    EXPECT_EQ(between.state, InputState::FULL);
    // No timeout: only the slewed write's end wakes it
    InputState woken = InputState::FULL;
    std::thread waiter([&] { woken = view.wait_writable(std::numeric_limits<int64_t>::max()); });
    EXPECT_EQ(view.write_slewed(pcm.data(), slewed).state, InputState::OPEN);
    waiter.join();
    EXPECT_EQ(woken, InputState::OPEN);
    EXPECT_EQ(view.state(), InputState::OPEN);
}

// A write runs through the slew with the mutex released, so it takes up the slew in one step and
// stages its blocks in others. A write made in between takes nothing (FULL), whatever the call
// before it left. A flush, pause, format, drain, close or server stop in between ends the
// staging, so nothing more lands past what that call left, and the write reports the state the
// call left and takes no slew time into `G_eff`. After a flush the next writes take the slew up
// again.
TEST_F(HostViewTest, EndsASlewedWriteACallOverran) {
    constexpr uint64_t ANCHOR = RATE / 2;
    struct Row {
        const char* name;
        std::function<void(HostView&)> call;
        InputState state;
    };
    const Row rows[] = {
        {"a flush", [](HostView& view) { view.flush(); }, InputState::OPEN},
        {"a pause", [](HostView& view) { (void)view.pause(); }, InputState::OPEN},
        {"a format",
         [](HostView& view) { ASSERT_TRUE(view.set_format({RATE, 2, SampleFormat::F32})); },
         InputState::OPEN},
        {"a drain", [](HostView& view) { (void)view.drain(); }, InputState::DRAINING},
        {"a close", [](HostView& view) { view.close(); }, InputState::CLOSED},
        {"a server stop", [](HostView& view) { (void)view.server_stop(); },
         InputState::STOP_LATCHED},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        HostView& view = this->make_slewing(0);
        const Bytes anchor = tone_pcm(0, ANCHOR);
        const Bytes pcm = tone_pcm(ANCHOR, RATE);
        ASSERT_EQ(view.write(anchor.data(), ANCHOR).frames, ANCHOR);
        // So that G_eff could follow a slew down
        view.set_group_params({LEAD_US - 200000, 0, SLEW_AHEAD_US});
        const HostView::SlewedWrite slewed = view.begin_slewed(RATE, 300, InputState::OPEN);
        row.call(view);
        const sendspin::server::JobView before = job_view_of(view);
        const WriteResult between = view.write(pcm.data(), RATE);
        EXPECT_EQ(between.frames, 0U);
        EXPECT_EQ(between.state,
                  row.state == InputState::CLOSED ? InputState::CLOSED : InputState::FULL);
        const WriteResult result = view.write_slewed(pcm.data(), slewed);
        EXPECT_EQ(result.frames, 0U);
        EXPECT_EQ(result.state, row.state);
        const sendspin::server::JobView after = job_view_of(view);
        EXPECT_EQ(after.head, before.head);
        EXPECT_EQ(after.group_lead_eff_us, before.group_lead_eff_us);
    }
    // The flush's epoch anchors with the next write, and the one after takes the slew up again
    HostView& view = this->make_slewing(0);
    const Bytes anchor = tone_pcm(0, ANCHOR);
    const Bytes pcm = tone_pcm(ANCHOR, RATE);
    ASSERT_EQ(view.write(anchor.data(), ANCHOR).frames, ANCHOR);
    const HostView::SlewedWrite slewed = view.begin_slewed(RATE, 300, InputState::OPEN);
    view.flush();
    ASSERT_EQ(view.write_slewed(pcm.data(), slewed).frames, 0U);
    ASSERT_EQ(view.write(anchor.data(), ANCHOR).frames, ANCHOR);
    EXPECT_EQ(view.write(pcm.data(), RATE).frames, RATE);
    EXPECT_EQ(job_view_of(view).head, ANCHOR + RATE);
}

// One live input slewing under a writer while another thread flushes, pauses and resumes, sets
// formats and finally closes it, each call landing wherever the writer is: every write reports no
// more than it was given, some writes are played fast, and the input still takes writes until it
// closes. Bounded, a few hundred milliseconds; its point is the ThreadSanitizer run.
TEST_F(HostViewTest, SlewsUnderCallsFromAnotherThread) {
    HostView& view = this->make_slewing(0);
    view.set_group_params({LEAD_US - 200000, 0, SLEW_AHEAD_US});
    std::atomic<bool> done{false};
    // Writes the slew played fast, so the run shows it engaged
    uint64_t slewed = 0;
    std::thread writer([&] {
        const Bytes pcm = tone_floats(0, RATE / 10);
        while (!done.load()) {
            {
                // Owing a second, so most writes go through the slew
                const std::lock_guard<std::mutex> lock(view.mutex_);
                view.lowering_ = {now_us(), std::numeric_limits<int64_t>::max(), 0, 1000000};
            }
            // Either format's frames fit the buffer: S16 reads half of it
            const WriteResult result = view.write(pcm.data(), RATE / 10);
            ASSERT_LE(result.frames, RATE / 10);
            if (result.state == InputState::OPEN && result.frames < RATE / 10) {
                ++slewed;
            }
            std::this_thread::yield();
        }
    });
    for (int i = 0; i < 1000; ++i) {
        switch (i % 4) {
            case 0:
                view.flush();
                break;
            case 1:
                (void)view.pause();
                view.resume();
                break;
            case 2:
                (void)view.set_format(
                    {RATE, 2, (i % 8 == 2) ? SampleFormat::F32 : SampleFormat::S16});
                break;
            default:
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                break;
        }
    }
    view.close();
    done.store(true);
    writer.join();
    EXPECT_GT(slewed, 0U);
    EXPECT_EQ(view.write(tone_pcm(0, CELL).data(), CELL).state, InputState::CLOSED);
}

// What a live epoch owes ends with it, and so does its slew: the epoch a flush, or a pause and
// resume, begins, and its writes, owe nothing of the one before and stage their audio as it came,
// bit for bit. Owed a second, the old epoch's debt would slew the new epoch's 1 s write, and an
// ASRC left running would resample it, starting with the frames it held from the old epoch.
TEST_F(HostViewTest, EndsALiveInputsSlewWithItsEpoch) {
    constexpr uint64_t ANCHOR = RATE / 10;
    for (const bool flush : {true, false}) {
        SCOPED_TRACE(flush ? "a flush" : "a pause and resume");
        HostView& view = this->make_slewing(0);
        ASSERT_EQ(this->write(ANCHOR).frames, ANCHOR);
        view.set_group_params({LEAD_US - 200000, 0, SLEW_AHEAD_US});
        view.lowering_ = {now_us(), std::numeric_limits<int64_t>::max(), 0, 1000000};
        ASSERT_LT(this->write(RATE).frames, RATE);
        view.lowering_.debt_us = 1000000;
        if (flush) {
            view.flush();
            // The new epoch's first write anchors it
            ASSERT_EQ(this->write(ANCHOR).frames, ANCHOR);
        } else {
            (void)view.pause();
            ASSERT_EQ(this->write(ANCHOR).frames, ANCHOR);
            view.resume();
        }
        const uint64_t from = job_view_of(view).head;
        const Bytes pcm = tone_pcm(ANCHOR, RATE);
        ASSERT_EQ(view.write(pcm.data(), RATE).frames, RATE);
        const sendspin::server::JobView job = job_view_of(view);
        EXPECT_EQ(job.head, from + RATE);
        EXPECT_EQ(job.group_lead_eff_us, LEAD_US - 200000);
        Bytes staged;
        ASSERT_TRUE(view.read(job.run.epoch, from, RATE, staged));
        EXPECT_EQ(staged, pcm);
    }
}

// A host call that queued on_drained and has not posted it when the input closes, and the
// dispatcher then finishes, still has it delivered: close() posts too
TEST_F(HostViewTest, DeliversWhatAHostCallQueuedWhenTheInputCloses) {
    HostView& view = this->make();
    view.outbox_.push_back({false, {}, DrainOutcome::PLAYED});
    view.outbox_posted_ = true;
    view.close();
    this->dispatcher->finish();
    this->dispatcher->join();
    EXPECT_EQ(this->recorder.count(Recorder::drained(InputId{2}, DrainOutcome::PLAYED)), 1U);
}

// A delivery swaps the outbox with the list it delivers from, so once both have grown a callback
// queued under the mutex allocates nothing
TEST_F(HostViewTest, KeepsTheOutboxCapacityAcrossDeliveries) {
    HostView& view = this->make();
    for (int i = 0; i < 2; ++i) {
        view.outbox_.push_back({false, {}, DrainOutcome::PLAYED});
        view.deliver_outbox();
    }
    EXPECT_EQ(view.outbox_.size(), 0U);
    EXPECT_GT(view.outbox_.capacity(), 0U);
    EXPECT_EQ(this->recorder.count(Recorder::drained(InputId{2}, DrainOutcome::PLAYED)), 2U);
}

}  // namespace
