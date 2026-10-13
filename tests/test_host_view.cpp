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

/// @file test_host_view.cpp
/// @brief A group input's host view on its own: admission, anchoring in the host call, the
/// provisional times, readiness and what core does to it. White-box: sets the outbox as a host call
/// leaves it before posting (see tests/CMakeLists.txt).

#include "clock.h"
#include "dispatcher.h"
#include "host_view.h"
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
#include <memory>
#include <mutex>
#include <numbers>
#include <thread>
#include <vector>

namespace {

using sendspin::server::DrainOutcome;
using sendspin::server::GroupId;
using sendspin::server::GroupParams;
using sendspin::server::HostView;
using sendspin::server::InputFormat;
using sendspin::server::InputId;
using sendspin::server::InputOptions;
using sendspin::server::InputState;
using sendspin::server::now_us;
using sendspin::server::SampleFormat;
using sendspin::server::WriteResult;
using sendspin::server::test::Recorder;
using Bytes = std::vector<uint8_t>;

constexpr uint32_t RATE = 48000;
constexpr int64_t LEAD_US = 300000;
constexpr int64_t AHEAD_US = 1000000;
constexpr uint64_t AHEAD_FRAMES = RATE;
constexpr int64_t LIVE_AHEAD_US = 200000;
/// W_live widened to 3 s, so that one write holds 2 s, which the slew takes enough frames out of
/// to count
constexpr int64_t SLEW_AHEAD_US = 3000000;
/// A cell of frames
constexpr uint64_t CELL = RATE / 50;

class HostViewTest : public testing::Test {
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
        const Bytes pcm(frames * 4, 1);
        return this->view->write(pcm.data(), frames);
    }

    Recorder recorder;
    std::shared_ptr<sendspin::server::Dispatcher> dispatcher =
        std::make_shared<sendspin::server::Dispatcher>();
    std::shared_ptr<HostView> view;
};

// The write that begins a run anchors it at now plus the group lead in the call itself, and admits
// the write-ahead from there; a view with no write-ahead admits nothing
TEST_F(HostViewTest, AnchorsTheRunInTheWriteThatBeginsIt) {
    HostView& view = this->make({LEAD_US, 0, 0});
    EXPECT_EQ(this->write(10).state, InputState::FULL);
    EXPECT_EQ(view.wait_writable(0), InputState::FULL);

    view.set_group_params({LEAD_US, 0, AHEAD_US});
    EXPECT_EQ(view.wait_writable(0), InputState::OPEN);
    const int64_t before = now_us();
    const WriteResult result = this->write(2 * AHEAD_FRAMES);
    const int64_t after = now_us();
    EXPECT_EQ(result.frames, AHEAD_FRAMES);
    EXPECT_EQ(result.state, InputState::FULL);
    EXPECT_GE(view.time_of(0), before + LEAD_US);
    EXPECT_LE(view.time_of(0), after + LEAD_US);
    // A second's audio later on the timeline
    EXPECT_EQ(view.time_of(RATE) - view.time_of(0), 1000000);
    const int64_t latency = view.latency_us();
    EXPECT_LE(latency, LEAD_US + AHEAD_US);
    EXPECT_GE(latency, LEAD_US + AHEAD_US - (now_us() - before));
}

// A pause before anything played pauses at the epoch's start, and the resume anchors a new epoch
// from it with a fresh lead
TEST_F(HostViewTest, PausesAndResumesFromThePausedPosition) {
    HostView& view = this->make();
    ASSERT_EQ(this->write(RATE / 2).frames, RATE / 2);
    EXPECT_EQ(view.pause(), 0U);
    // Frozen at the paused position plus the write-ahead
    EXPECT_EQ(this->write(AHEAD_FRAMES).frames, AHEAD_FRAMES / 2);
    const int64_t before = now_us();
    view.resume();
    EXPECT_GE(view.time_of(0), before + LEAD_US);
}

// A live input's first write admits W_live from the anchor. Paused, it takes every write the
// staging ring holds (up to 3 s here), and its latency is the group lead: the head would play a
// lead from now. Its resume anchors the new run at the head, a lead from the resume, where the
// buffered resume above anchors at the paused position.
TEST_F(HostViewTest, TakesALiveInputWhilePausedAndResumesAtTheHead) {
    HostView& view = this->make_live();
    const WriteResult first = this->write(RATE);
    EXPECT_EQ(first.frames, RATE / 5);
    EXPECT_EQ(first.state, InputState::FULL);
    const uint64_t paused = view.pause();
    EXPECT_LE(paused, RATE / 5);
    uint64_t head = RATE / 5;
    // Whole seconds past W_live that the ring holds, so that none of them is cut short
    const int64_t seconds =
        std::min<int64_t>(3, (sendspin::server::STAGING_RING_US - LIVE_AHEAD_US) / 1000000);
    ASSERT_GE(seconds, 2);
    for (int64_t second = 0; second < seconds; ++second) {
        const WriteResult result = this->write(RATE);
        EXPECT_EQ(result.frames, RATE);
        EXPECT_EQ(result.state, InputState::OPEN);
        head += RATE;
    }
    EXPECT_EQ(view.latency_us(), LEAD_US);
    const int64_t paused_before = now_us();
    const int64_t provisional = view.time_of(head);
    EXPECT_GE(provisional, paused_before + LEAD_US);
    EXPECT_LE(provisional, now_us() + LEAD_US);

    const int64_t before = now_us();
    view.resume();
    const int64_t after = now_us();
    EXPECT_GE(view.time_of(head), before + LEAD_US);
    EXPECT_LE(view.time_of(head), after + LEAD_US);
    // Anchored: a second of input later is a second later on the timeline
    EXPECT_EQ(view.time_of(head + RATE) - view.time_of(head), 1000000);
    // The new run admits W_live from the head, and what has played since the resume
    const WriteResult resumed = this->write(RATE);
    const auto played = static_cast<uint64_t>((now_us() - before) * RATE / 1000000);
    EXPECT_EQ(resumed.state, InputState::FULL);
    EXPECT_GE(resumed.frames, RATE / 5);
    EXPECT_LE(resumed.frames, (RATE / 5) + played + 1);
}

// Once a live input's next frame is late, it admits what the live rebase leaves room for, `W + L_p
// - J_in` past the head, here 220 ms; a buffered input's rebase, restoring the full lead, would
// leave `W`. The sleep lets the frame go late.
TEST_F(HostViewTest, AdmitsAStalledLiveInputAsIfRebased) {
    HostView& view = this->make_live();
    ASSERT_EQ(this->write(RATE / 50).frames, RATE / 50);
    // The pipelines' k*, the next frame, with L_p of a 20 ms frame and the 20 ms jitter
    view.publish_shift_index(view.job_view().run.epoch, 1, 40000);
    std::this_thread::sleep_for(std::chrono::microseconds(LEAD_US + 50000));
    const WriteResult result = this->write(RATE);
    EXPECT_EQ(result.frames, uint64_t{RATE} * 220 / 1000);
    EXPECT_EQ(result.state, InputState::FULL);
}

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
        const sendspin::server::JobView job = view.job_view();
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
    const uint64_t segment = view.job_view().head;
    ASSERT_TRUE(view.set_format({RATE, 2, SampleFormat::F32}));
    const WriteResult result = view.write(after.data(), RATE);
    ASSERT_EQ(result.state, InputState::OPEN);
    ASSERT_LT(result.frames, RATE);
    const sendspin::server::JobView job = view.job_view();
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
        const sendspin::server::JobView before = view.job_view();
        const WriteResult between = view.write(pcm.data(), RATE);
        EXPECT_EQ(between.frames, 0U);
        EXPECT_EQ(between.state,
                  row.state == InputState::CLOSED ? InputState::CLOSED : InputState::FULL);
        const WriteResult result = view.write_slewed(pcm.data(), slewed);
        EXPECT_EQ(result.frames, 0U);
        EXPECT_EQ(result.state, row.state);
        const sendspin::server::JobView after = view.job_view();
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
    EXPECT_EQ(view.job_view().head, ANCHOR + RATE);
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

// A live input with a least lead above the group's anchors there, takes that lead for its
// provisional times before then, and keeps a raised lead above it as any
TEST_F(HostViewTest, AnchorsALiveInputAtItsLeastLead) {
    constexpr int64_t LEAST_US = LEAD_US + 100000;
    HostView& view = this->make_slewing(LEAST_US);
    const int64_t provisional = now_us();
    EXPECT_GE(view.time_of(0), provisional + LEAST_US);
    const int64_t before = now_us();
    ASSERT_EQ(this->write(CELL).frames, CELL);
    const int64_t after = now_us();
    EXPECT_GE(view.time_of(0), before + LEAST_US);
    EXPECT_LE(view.time_of(0), after + LEAST_US);
    EXPECT_EQ(view.job_view().group_lead_eff_us, LEAST_US);
    view.set_group_params({LEAST_US + 200000, 0, SLEW_AHEAD_US});
    EXPECT_EQ(view.job_view().group_lead_eff_us, LEAST_US + 200000);
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
        const uint64_t from = view.job_view().head;
        const Bytes pcm = tone_pcm(ANCHOR, RATE);
        ASSERT_EQ(view.write(pcm.data(), RATE).frames, RATE);
        const sendspin::server::JobView job = view.job_view();
        EXPECT_EQ(job.head, from + RATE);
        EXPECT_EQ(job.group_lead_eff_us, LEAD_US - 200000);
        Bytes staged;
        ASSERT_TRUE(view.read(job.run.epoch, from, RATE, staged));
        EXPECT_EQ(staged, pcm);
    }
}

// MAX_ANCHOR_SHIFTS: releasing the ring folds the shifts below the cell it then holds first into
// S_base, so an epoch takes a shift a cell for longer than the cap, and every time from the cell
// kept on reads as before the fold. 8 kHz mono: 160 frames a cell.
TEST_F(HostViewTest, FoldsTheShiftsBelowTheReleasedRing) {
    constexpr uint64_t CELL_FRAMES = 160;
    constexpr int64_t SHIFTS = sendspin::server::MAX_ANCHOR_SHIFTS + 100;
    HostView& view = this->make({LEAD_US, 0, 30000000}, {8000, 1, SampleFormat::S16});
    ASSERT_EQ(this->write(CELL_FRAMES).frames, CELL_FRAMES);
    const sendspin::server::sched::EpochId epoch = view.job_view().run.epoch;
    const uint64_t far = CELL_FRAMES * 2 * SHIFTS;
    const int64_t far_before = view.time_of(far);
    for (int64_t index = 0; index < SHIFTS; ++index) {
        ASSERT_TRUE(view.add_shift(epoch, index, 1)) << index;
        const auto next = static_cast<uint64_t>(index + 1) * CELL_FRAMES;
        const int64_t kept = view.time_of(next);
        view.release_before(epoch, next);
        ASSERT_EQ(view.time_of(next), kept) << index;
        ASSERT_EQ(this->write(CELL_FRAMES).frames, CELL_FRAMES) << index;
    }
    EXPECT_EQ(view.time_of(far), far_before + SHIFTS);
}

// Control for the above: with nothing released, the anchor holds MAX_ANCHOR_SHIFTS and refuses
// the next
TEST_F(HostViewTest, RefusesAShiftPastTheCapWithNothingReleased) {
    HostView& view = this->make({LEAD_US, 0, 30000000}, {8000, 1, SampleFormat::S16});
    ASSERT_EQ(this->write(160).frames, 160U);
    const sendspin::server::sched::EpochId epoch = view.job_view().run.epoch;
    const auto cap = static_cast<int64_t>(sendspin::server::MAX_ANCHOR_SHIFTS);
    for (int64_t index = 0; index < cap; ++index) {
        ASSERT_TRUE(view.add_shift(epoch, index, 1)) << index;
    }
    EXPECT_FALSE(view.add_shift(epoch, cap, 1));
}

// set_format() during a run starts a segment at the head. Positions and the timeline continue, each
// segment at its own rate; write() then takes the new format, admitted at its rate; the pipelines
// read each segment in its own format, never across the boundary. A second set_format() with
// nothing written since takes the format in place. A staged drain refuses it.
TEST_F(HostViewTest, ChangesFormatWithinARunAsANewSegment) {
    HostView& view = this->make();
    ASSERT_EQ(this->write(RATE / 2).frames, RATE / 2);
    const int64_t anchored = view.time_of(0);
    EXPECT_TRUE(view.set_format({44100, 1, SampleFormat::S24}));
    // Half a second at 48 kHz, then a second at 44.1 kHz
    EXPECT_EQ(view.time_of(RATE / 2) - anchored, 500000);
    EXPECT_EQ(view.time_of((RATE / 2) + 44100) - anchored, 1500000);

    // The head is half a second into the timeline of a second's write-ahead: about half a second
    // at 44.1 kHz fits, plus what played meanwhile
    const Bytes mono(44100 * 3, 0x11);
    const int64_t before = now_us();
    const WriteResult result = view.write(mono.data(), 44100);
    const int64_t after = now_us();
    EXPECT_EQ(result.state, InputState::FULL);
    const int64_t playing = anchored - LEAD_US;
    EXPECT_GE(result.frames, static_cast<size_t>((before - playing + 500000) * 44100 / 1000000));
    EXPECT_LE(result.frames, static_cast<size_t>((after - playing + 500000) * 44100 / 1000000) + 1);

    const sendspin::server::JobView job = view.job_view();
    ASSERT_EQ(job.segments.size(), 2U);
    EXPECT_EQ(job.segments[1].position, RATE / 2);
    EXPECT_EQ(job.segments[1].format, (InputFormat{44100, 1, SampleFormat::S24}));
    ASSERT_EQ(job.anchor.segments.size(), 2U);
    EXPECT_EQ(job.anchor.segments[1].rate, 44100U);
    Bytes out;
    // Control: ten stereo 16-bit frames before the boundary
    EXPECT_TRUE(view.read(job.run.epoch, (RATE / 2) - 10, 10, out));
    EXPECT_EQ(out, Bytes(40, 1));
    EXPECT_TRUE(view.read(job.run.epoch, RATE / 2, 10, out));
    EXPECT_EQ(out, Bytes(30, 0x11));
    EXPECT_FALSE(view.read(job.run.epoch, (RATE / 2) - 5, 10, out));

    // Nothing written since: the format changes in place, with no third segment
    EXPECT_TRUE(view.set_format({48000, 2, SampleFormat::S16}));
    EXPECT_TRUE(view.set_format({96000, 2, SampleFormat::F32}));
    EXPECT_EQ(view.job_view().segments.size(), 3U);
    EXPECT_EQ(view.job_view().anchor.segments.back().rate, 96000U);
    view.drain();
    EXPECT_FALSE(view.set_format({48000, 2, SampleFormat::S16}));
}

// set_format() while paused starts a segment at the head, which the provisional times and the
// resume's anchor take; after a stop, it is the next run's format and positions restart. An invalid
// format is refused.
TEST_F(HostViewTest, ChangesFormatWhilePausedAndBetweenRuns) {
    HostView& view = this->make();
    ASSERT_EQ(this->write(RATE / 2).frames, RATE / 2);
    EXPECT_EQ(view.pause(), 0U);
    EXPECT_TRUE(view.set_format({44100, 1, SampleFormat::S24}));
    // Frozen at the paused position plus the write-ahead, at the head's rate: 44,100 - 24,000
    const Bytes mono(44100 * 3, 0x22);
    EXPECT_EQ(view.write(mono.data(), 44100).frames, 20100U);
    // Provisional, as if resumed now: half a second at 48 kHz, then floor(20,100 / 44,100 s)
    EXPECT_EQ(view.latency_us(), LEAD_US + 500000 + 455782);
    view.resume();
    const int64_t anchored = view.time_of(0);
    EXPECT_EQ(view.time_of(RATE / 2) - anchored, 500000);
    EXPECT_EQ(view.time_of((RATE / 2) + 44100) - anchored, 1500000);
    EXPECT_EQ(view.job_view().anchor.segments.size(), 2U);

    view.stop();
    EXPECT_TRUE(view.set_format({44100, 2, SampleFormat::S16}));
    EXPECT_FALSE(view.set_format({0, 2, SampleFormat::S16}));
    // Positions restart at 0 after the stop, in one segment
    EXPECT_LE(view.latency_us(), LEAD_US);
    EXPECT_EQ(view.job_view().segments.size(), 1U);
    EXPECT_EQ(this->write(1).state, InputState::OPEN);
}

// set_format() sizes the staging ring for the new format, so admission is not cut short by a ring
// sized for the old one, and what the ring held is still read back as written. An 8 kHz mono ring
// holds 23,000 frames of 48 kHz stereo floats past 4,000 written; the write-ahead admits at least
// 24,000.
TEST_F(HostViewTest, GrowsTheRingForALargerFormatKeepingWhatItHolds) {
    HostView& view = this->make({LEAD_US, 0, AHEAD_US}, {8000, 1, SampleFormat::S16});
    Bytes first(4000 * 2);
    for (size_t i = 0; i < first.size(); ++i) {
        first[i] = static_cast<uint8_t>(i * 13);
    }
    ASSERT_EQ(view.write(first.data(), 4000).frames, 4000U);
    EXPECT_TRUE(view.set_format({RATE, 2, SampleFormat::F32}));
    const Bytes wide(size_t{RATE} * 8, 0x33);
    const WriteResult result = view.write(wide.data(), RATE);
    EXPECT_GE(result.frames, 24000U);
    const sendspin::server::sched::EpochId epoch = view.job_view().run.epoch;
    Bytes out;
    EXPECT_TRUE(view.read(epoch, 0, 4000, out));
    EXPECT_EQ(out, first);
    EXPECT_TRUE(view.read(epoch, 4000 + result.frames - 10, 10, out));
    EXPECT_EQ(out, Bytes(80, 0x33));
}

// What the ring holds across its wrap moves to the same counted offsets in the larger ring.
// STAGING_RING_US of 8 kHz mono is a ring of `ring` frames (96,000 on the host profile, 24,000 on
// the embedded one); with what is before `ring - 16,000` released, `ring + 4,000` written wrap it,
// and the last 20,000 read back as written after the growth.
TEST_F(HostViewTest, GrowsTheRingKeepingWhatWrapsInIt) {
    constexpr uint64_t ring = sendspin::server::STAGING_RING_US * 8000 / 1000000;
    static_assert(ring > 20000);
    HostView& view = this->make({LEAD_US, 0, 20000000}, {8000, 1, SampleFormat::S16});
    Bytes pcm(size_t{ring + 4000} * 2);
    for (size_t i = 0; i < pcm.size(); ++i) {
        pcm[i] = static_cast<uint8_t>((i * 7919) >> 5);
    }
    ASSERT_EQ(view.write(pcm.data(), ring - 6000).frames, ring - 6000);
    const sendspin::server::sched::EpochId epoch = view.job_view().run.epoch;
    view.release_before(epoch, ring - 16000);
    ASSERT_EQ(view.write(pcm.data() + ((ring - 6000) * 2), 10000).frames, 10000U);
    ASSERT_TRUE(view.set_format({RATE, 2, SampleFormat::F32}));
    Bytes out;
    ASSERT_TRUE(view.read(epoch, ring - 16000, 20000, out));
    EXPECT_TRUE(std::equal(out.begin(), out.end(), pcm.begin() + ((ring - 16000) * 2)));
}

// MAX_STAGED_SEGMENTS: the staging ring holds that many segments at once, so a set_format() past
// it is refused; once core releases the input before the later ones, it is taken again
TEST_F(HostViewTest, RefusesASegmentPastTheStagedCap) {
    HostView& view = this->make();
    const Bytes pcm(16, 1);
    for (size_t i = 1; i < sendspin::server::MAX_STAGED_SEGMENTS; ++i) {
        ASSERT_EQ(view.write(pcm.data(), 1).frames, 1U);
        ASSERT_TRUE(view.set_format({i % 2 == 1 ? 44100U : RATE, 2, SampleFormat::S16})) << i;
    }
    ASSERT_EQ(view.write(pcm.data(), 1).frames, 1U);
    EXPECT_FALSE(view.set_format({RATE, 1, SampleFormat::S16}));
    // Control: core releases what played, which ends the first segments
    view.release_before(view.job_view().run.epoch, 4);
    EXPECT_TRUE(view.set_format({RATE, 1, SampleFormat::S16}));
    // Those from position 4 on, and the new one, in the staging list and the playing anchor alike
    const sendspin::server::JobView job = view.job_view();
    EXPECT_EQ(job.segments.size(), sendspin::server::MAX_STAGED_SEGMENTS - 3);
    EXPECT_EQ(job.anchor.segments.size(), sendspin::server::MAX_STAGED_SEGMENTS - 3);
}

// A write cut short is answered by on_writable once one cell fits, and not before
TEST_F(HostViewTest, AnswersAShortWriteWhenACellFits) {
    HostView& view = this->make();
    EXPECT_EQ(view.poll_writable(now_us()), sendspin::server::sched::NEVER);
    const int64_t before = now_us();
    EXPECT_EQ(this->write(2 * AHEAD_FRAMES).state, InputState::FULL);
    const int64_t after = now_us();
    // One cell of the timeline after the anchor, whose time the write took
    const int64_t ready = view.poll_writable(before);
    EXPECT_GE(ready, before + 20000);
    EXPECT_LE(ready, after + 20000);
    EXPECT_EQ(view.poll_writable(ready - 1), ready);
    EXPECT_EQ(this->recorder.count("writable 1 2"), 0U);
    EXPECT_EQ(view.poll_writable(ready), sendspin::server::sched::NEVER);
    this->recorder.wait_for("writable 1 2");
    EXPECT_EQ(view.poll_writable(ready), sendspin::server::sched::NEVER);
    EXPECT_EQ(this->recorder.count("writable 1 2"), 1U);
}

// Completion delivered for the draining epoch ends the run with PLAYED; a flush first leaves
// nothing for it, and closing the input resolves a pending drain as STOPPED
TEST_F(HostViewTest, ResolvesEachDrainOnceWhoeverComesFirst) {
    HostView& view = this->make();
    ASSERT_GT(this->write(RATE).frames, 0U);
    const sendspin::server::sched::EpochId epoch = view.job_view().run.epoch;
    view.drain();
    EXPECT_EQ(this->write(1).state, InputState::DRAINING);
    view.flush();
    EXPECT_FALSE(view.complete_drain(epoch));
    this->recorder.wait_for(Recorder::drained(InputId{2}, DrainOutcome::FLUSHED));

    ASSERT_GT(this->write(RATE).frames, 0U);
    const sendspin::server::sched::EpochId flushed = view.job_view().run.epoch;
    view.drain();
    EXPECT_FALSE(view.complete_drain(epoch));
    EXPECT_TRUE(view.complete_drain(flushed));
    EXPECT_FALSE(view.complete_drain(flushed));
    view.release_drained();
    this->recorder.wait_for(Recorder::drained(InputId{2}, DrainOutcome::PLAYED));
    // The run ended: the next write begins another
    EXPECT_EQ(this->write(1).state, InputState::OPEN);

    view.drain();
    view.close();
    this->recorder.wait_for(Recorder::drained(InputId{2}, DrainOutcome::STOPPED));
    EXPECT_EQ(this->write(1).state, InputState::CLOSED);
    EXPECT_TRUE(view.gone());
}

// drain() calls made before the drain resolves join it, and its resolution answers each once, in
// the order they were made: three, past the room for two the view's queue of drain ids starts with
TEST_F(HostViewTest, AnswersEveryDrainCallItsResolutionJoins) {
    HostView& view = this->make();
    ASSERT_GT(this->write(RATE).frames, 0U);
    const sendspin::server::sched::EpochId epoch = view.job_view().run.epoch;
    std::vector<sendspin::server::DrainId> ids;
    for (int i = 0; i < 3; ++i) {
        ids.push_back(view.drain());
    }
    EXPECT_TRUE(view.complete_drain(epoch));
    view.release_drained();
    // Every callback the release posted has run once the dispatch thread ends
    this->dispatcher->finish();
    this->dispatcher->join();
    const std::vector<std::pair<sendspin::server::DrainId, DrainOutcome>> drains =
        this->recorder.drains();
    ASSERT_EQ(drains.size(), ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        EXPECT_EQ(drains[i].first, ids[i]) << i;
        EXPECT_EQ(drains[i].second, DrainOutcome::PLAYED) << i;
    }
}

// state() shows what the next write() would report, including a stop by the server and a closed
// input, without a write
TEST_F(HostViewTest, ReportsItsStateWithoutAWrite) {
    HostView& view = this->make({LEAD_US, 0, 0});
    EXPECT_EQ(view.state(), InputState::FULL);
    view.set_group_params({LEAD_US, 0, AHEAD_US});
    EXPECT_EQ(view.state(), InputState::OPEN);
    ASSERT_GT(this->write(RATE / 2).frames, 0U);
    view.drain();
    EXPECT_EQ(view.state(), InputState::DRAINING);
    view.flush();
    EXPECT_EQ(view.state(), InputState::OPEN);
    ASSERT_GT(this->write(RATE / 2).frames, 0U);
    EXPECT_TRUE(view.server_stop().applied);
    EXPECT_EQ(view.state(), InputState::STOP_LATCHED);
    view.stop();
    EXPECT_EQ(view.state(), InputState::OPEN);
    view.close();
    EXPECT_EQ(view.state(), InputState::CLOSED);

    HostView& released = this->make();
    released.release();
    EXPECT_EQ(released.state(), InputState::CLOSED);
}

// A host call made since the last write() ends a wait, which returns the state as it ends,
// whether the call came while the waiter waited or before it began. The view admits nothing, so
// only the call can end the wait.
TEST_F(HostViewTest, EndsAWaitOnAHostCallFromAnotherThread) {
    struct Row {
        const char* name;
        std::function<void(HostView&)> act;
        InputState state;
    };
    const Row rows[] = {
        {"flush", [](HostView& view) { view.flush(); }, InputState::FULL},
        {"drain", [](HostView& view) { view.drain(); }, InputState::FULL},
        {"pause", [](HostView& view) { view.pause(); }, InputState::FULL},
        {"resume", [](HostView& view) { view.resume(); }, InputState::FULL},
        {"stop", [](HostView& view) { view.stop(); }, InputState::FULL},
        {"set_format", [](HostView& view) { view.set_format({RATE, 2, SampleFormat::S16}); },
         InputState::FULL},
        {"release", [](HostView& view) { view.release(); }, InputState::CLOSED},
        {"the server closing it", [](HostView& view) { view.close(); }, InputState::CLOSED},
    };
    // Control: with no call, the wait ends at its timeout, still full
    EXPECT_EQ(this->make({LEAD_US, 0, 0}).wait_writable(1000), InputState::FULL);
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        HostView& view = this->make({LEAD_US, 0, 0});
        ASSERT_EQ(this->write(1).state, InputState::FULL);
        InputState state = InputState::OPEN;
        std::thread waiter(
            [&] { state = view.wait_writable(std::numeric_limits<int64_t>::max()); });
        row.act(view);
        waiter.join();
        EXPECT_EQ(state, row.state);
    }
}

// A wait answers each host call once: after a wait returned for a pause(), the next one waits
// out its timeout, and a flush() from another thread still ends a wait
TEST_F(HostViewTest, WaitsAgainOnceAWaitAnsweredACall) {
    constexpr int64_t WAIT_US = 200000;
    HostView& view = this->make({LEAD_US, 0, 0});
    ASSERT_EQ(this->write(1).state, InputState::FULL);
    view.pause();
    EXPECT_EQ(view.wait_writable(std::numeric_limits<int64_t>::max()), InputState::FULL);
    const int64_t before = now_us();
    EXPECT_EQ(view.wait_writable(WAIT_US), InputState::FULL);
    EXPECT_GE(now_us() - before, WAIT_US);

    InputState state = InputState::OPEN;
    std::thread waiter([&] { state = view.wait_writable(std::numeric_limits<int64_t>::max()); });
    view.flush();
    waiter.join();
    EXPECT_EQ(state, InputState::FULL);
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

}  // namespace
