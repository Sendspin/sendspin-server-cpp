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
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
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

/// A live input anchored with a lead of LEAD_US and A_US of input, its lead then fallen by FALL_US,
/// and a write of C_US e_us after the anchor that closes a window whose least excess, net of cuts,
/// was least_us: the frames cut from the write, and `G_eff` after it, `G_next` plus the write's
/// excess less the cut (LEAD_US when nothing is cut)
constexpr int64_t FALL_US = 200000;
constexpr int64_t A_US = 60000;
constexpr int64_t C_US = 140000;

struct Lowered {
    int64_t cut_frames;
    int64_t lead_eff_us;
};

Lowered lowered(int64_t e_us, int64_t least_us) {
    const int64_t next = LEAD_US - FALL_US;
    const int64_t excess = A_US + FALL_US - e_us;
    // The window's least, the closing write's included, is cut once it reaches the minimum
    const int64_t least = std::min(least_us, excess);
    const int64_t debt = least >= sendspin::server::sched::LOWERING_MIN_US ? least : 0;
    const auto frames = std::min<int64_t>(C_US * RATE / 1000000, debt * RATE / 1000000);
    if (frames == 0) {
        return {0, LEAD_US};
    }
    const int64_t cut_us = frames * 1000000 / RATE;
    return {frames, std::max(next, std::min(LEAD_US, next + excess - cut_us))};
}

// A live input whose group lead fell lands every write later than a fresh anchor would place it.
// Once a window of writes kept that excess, the write that closes it is cut from its end, up to the
// window's least excess, and the effective lead falls to where the cut leaves the writes that
// follow: never below the lead its players need, never above where it was. Positions count only
// what was taken. The write-ahead is W_live's, so admission takes the whole write; the bounds take
// the thread's time between the anchor and the write from the clock, however late it ran.
TEST_F(HostViewTest, LowersALiveInputsLatencyOnceItsLeadFalls) {
    // A frame's rounding of the cut, at 48 kHz
    constexpr int64_t ROUNDING_US = (1000000 + RATE - 1) / RATE + 1;
    struct Row {
        const char* name;
        int64_t least_us;
    };
    const Row rows[] = {
        // The whole write is cut, and G_eff falls to G_next plus what it owed past it
        {"The window owes the whole excess", std::numeric_limits<int64_t>::max()},
        // 40 ms cut leaves this write 180 ms past G_next, above where G_eff stands: G_eff stays
        {"The window owes less than this write's excess", sendspin::server::sched::LOWERING_MIN_US},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        HostView& view = this->make_live();
        ASSERT_EQ(this->write(A_US * RATE / 1000000).frames, A_US * RATE / 1000000);
        const int64_t anchored = view.time_of(0) - LEAD_US;
        view.set_group_params({LEAD_US - FALL_US, 0, LIVE_AHEAD_US});
        // A window of writes is behind the input, its least excess, counted net of cuts, the row's
        view.lowering_ = {now_us() - sendspin::server::sched::LOWERING_WINDOW_US, row.least_us, 0,
                          0};
        const int64_t before = now_us();
        const WriteResult result = this->write(C_US * RATE / 1000000);
        const int64_t after = now_us();
        EXPECT_EQ(result.state, InputState::OPEN);
        // The later the write, the less it owes: what it cut and G_eff after it lie between their
        // values at the clock's readings around it
        const Lowered late = lowered(after - anchored, row.least_us);
        const Lowered early = lowered(before - anchored, row.least_us);
        const auto cut = static_cast<int64_t>(C_US * RATE / 1000000 - result.frames);
        EXPECT_GE(cut, late.cut_frames - 1);
        EXPECT_LE(cut, early.cut_frames + 1);
        const int64_t lead_eff = view.job_view().group_lead_eff_us;
        EXPECT_GE(lead_eff, late.lead_eff_us - ROUNDING_US);
        EXPECT_LE(lead_eff, early.lead_eff_us + ROUNDING_US);
        EXPECT_EQ(view.job_view().head, (A_US * RATE / 1000000) + result.frames);
    }
}

// What a live epoch owes ends with it: the epoch a flush begins, and its writes, owe nothing of
// the one before. Owed a second, the old epoch's debt would cut the new epoch's writes up to their
// excess.
TEST_F(HostViewTest, EndsALiveInputsLoweringWithItsEpoch) {
    HostView& view = this->make_live();
    ASSERT_EQ(this->write(CELL).frames, CELL);
    view.lowering_.debt_us = 1000000;
    view.flush();
    ASSERT_EQ(this->write(CELL).frames, CELL);
    EXPECT_EQ(this->write(CELL).frames, CELL);
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
