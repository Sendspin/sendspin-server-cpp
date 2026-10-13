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

/// @file test_sched_chunk.cpp
/// @brief The chunk header and send_ahead, producibility and format selection, and packing

#include "sched/chunk.h"
#include "sched/constants.h"
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace {

using sendspin::server::sched::AudioFormat;
using sendspin::server::sched::Codec;
using sendspin::server::sched::FormatShape;
using sendspin::server::sched::FrameView;
using sendspin::server::sched::PackLimits;
namespace sched = sendspin::server::sched;

constexpr int64_t MS = 1000;
constexpr size_t MAX_MESSAGE_BYTES = 16384;

constexpr AudioFormat PCM44{Codec::PCM, 44100, 2, 16};
constexpr AudioFormat PCM48_24{Codec::PCM, 48000, 2, 24};
constexpr AudioFormat FLAC48{Codec::FLAC, 48000, 2, 16};

constexpr int64_t T0 = 1000 * MS;

/// Frames first..first + (count - 1) * cells, each of bytes and cells * 20 ms, T0 at 1 s
std::vector<FrameView> frames(int64_t first, size_t count, int64_t bytes, uint32_t cells = 1) {
    std::vector<FrameView> out;
    for (size_t i = 0; i < count; ++i) {
        const int64_t index = first + static_cast<int64_t>(i) * cells;
        out.push_back({index, T0 + index * 20 * MS, cells * 20 * MS, bytes});
    }
    return out;
}

TEST(SchedChunk, SendAheadSaturates) {
    struct Row {
        int64_t d;
        uint32_t field;
    };
    const Row rows[] = {
        {-1, 0},
        {0, 0},
        {1, 1},
        {0xFFFFFFFE, 0xFFFFFFFE},
        {0xFFFFFFFF, 0xFFFFFFFF},
        {0x100000000, 0xFFFFFFFF},
        // Control: 250 ms
        {250000, 250000},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.d);
        EXPECT_EQ(sched::send_ahead_field(1000000 + row.d, 1000000), row.field);
    }
}

// A0 at ts 560 ms stamped at 1.120 ms carries send_ahead 558,880
TEST(SchedChunk, HeaderLayout) {
    std::array<uint8_t, 13> header{};
    sched::write_chunk_header(header, 560000, sched::send_ahead_field(560000, 1120));
    const std::array<uint8_t, 13> expected = {4,    0,    0,    0,    0,    0,   0x08,
                                              0x8B, 0x80, 0x00, 0x08, 0x87, 0x20};
    EXPECT_EQ(header, expected);
}

TEST(SchedChunk, Producible) {
    FormatShape at_limit;
    at_limit.format = FLAC48;
    at_limit.layout = {1, 960, true};
    at_limit.encoder_available = true;
    at_limit.max_frame_bytes = 65519 - 13;
    FormatShape over_limit = at_limit;
    over_limit.max_frame_bytes += 1;
    FormatShape flac32 = at_limit;
    flac32.format.bit_depth = 32;
    flac32.max_frame_bytes = 1000;
    FormatShape flac24 = flac32;
    flac24.format.bit_depth = 24;
    FormatShape no_encoder = flac24;
    no_encoder.encoder_available = false;

    struct Row {
        const char* name;
        FormatShape shape;
        int64_t capacity;
        bool producible;
    };
    const Row rows[] = {
        {"Control: PCM 48/24/2 with C 200,000", sched::pcm_shape(PCM48_24), 200000, true},
        {"B_f + 13 equal to 65,519", at_limit, 100000, true},
        {"B_f + 13 one over 65,519", over_limit, 100000, false},
        {"B_f + 13 equal to C", sched::pcm_shape(PCM44), 3541, true},
        {"B_f + 13 one over C", sched::pcm_shape(PCM44), 3540, false},
        {"FLAC at 32 bits", flac32, 100000, false},
        {"FLAC at 24 bits", flac24, 100000, true},
        {"no encoder for the codec", no_encoder, 100000, false},
        {"11,025 Hz: frames of two cells", sched::pcm_shape({Codec::PCM, 11025, 2, 16}), 100000,
         true},
        {"8,010 Hz: frames of five cells", sched::pcm_shape({Codec::PCM, 8010, 2, 16}), 100000,
         true},
        {"44,101 Hz: the frame exceeds a chunk", sched::pcm_shape({Codec::PCM, 44101, 2, 16}),
         10000000, false},
        {"44,110 Hz: five cells, still within a chunk",
         sched::pcm_shape({Codec::PCM, 44110, 2, 16}), 100000, true},
        {"PCM bit depth 0", sched::pcm_shape({Codec::PCM, 48000, 2, 0}), 100000, false},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sched::producible(row.shape, row.capacity), row.producible);
    }
    // Cases a and c: PCM frames are exact
    EXPECT_EQ(sched::pcm_shape(PCM44).max_frame_bytes, 3528);
    EXPECT_EQ(sched::pcm_shape(PCM48_24).max_frame_bytes, 5760);
    // The cap is max_message_bytes unless one frame needs more
    EXPECT_EQ(sched::message_cap(sched::pcm_shape(PCM44), MAX_MESSAGE_BYTES), 16384);
    EXPECT_EQ(sched::message_cap(at_limit, MAX_MESSAGE_BYTES), 65519);
}

// Spec player/v1.md: bit_depth is ignored for Opus, so it never tells two Opus formats apart
TEST(SchedChunk, OpusFormatsIgnoreBitDepth) {
    EXPECT_EQ((AudioFormat{Codec::OPUS, 48000, 2, 16}), (AudioFormat{Codec::OPUS, 48000, 2, 0}));
    EXPECT_NE((AudioFormat{Codec::PCM, 48000, 2, 16}), (AudioFormat{Codec::PCM, 48000, 2, 24}));
    // Control: everything else still counts for Opus
    EXPECT_NE((AudioFormat{Codec::OPUS, 48000, 2, 16}), (AudioFormat{Codec::OPUS, 48000, 1, 16}));
}

TEST(SchedChunk, SelectFormat) {
    const std::vector<FormatShape> supported = {sched::pcm_shape(PCM48_24),
                                                sched::pcm_shape(PCM44)};
    struct Row {
        const char* name;
        std::optional<AudioFormat> preferred;
        int64_t capacity;
        std::optional<uint32_t> native_rate;
        std::optional<AudioFormat> selected;
    };
    const Row rows[] = {
        {"Control: no preference gives the first entry", std::nullopt, 100000, std::nullopt,
         PCM48_24},
        {"a producible preference wins", PCM44, 100000, std::nullopt, PCM44},
        {"a preference not listed is ignored", FLAC48, 100000, std::nullopt, PCM48_24},
        // 5,773 B frames do not fit 5,000; 3,541 B frames do
        {"the first entry too big for the capacity", std::nullopt, 5000, std::nullopt, PCM44},
        {"a preference too big falls back", PCM48_24, 5000, std::nullopt, PCM44},
        {"nothing fits", std::nullopt, 3000, std::nullopt, std::nullopt},
        // spec player/v1.md: a server MAY select another entry to match the native rate
        {"an entry at the native rate before a higher one", std::nullopt, 100000, 44100, PCM44},
        {"a preference before the native rate", PCM48_24, 100000, 44100, PCM48_24},
        {"no entry at the native rate gives the first", std::nullopt, 100000, 96000, PCM48_24},
        {"an entry at the native rate too big falls back", std::nullopt, 3541, 48000, PCM44},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sched::select_format(supported, row.preferred, row.capacity, row.native_rate),
                  row.selected);
    }
}

TEST(SchedChunk, PackChunk) {
    PackLimits pcm;
    pcm.message_cap = 16384;
    pcm.room_bytes = 100000;

    PackLimits room_d = pcm;
    room_d.room_bytes = 11974;
    PackLimits exact = pcm;
    exact.room_bytes = 13 + 3 * 3528;
    PackLimits opus = pcm;
    opus.codec = Codec::OPUS;
    PackLimits switching = pcm;
    switching.switch_point = 2;
    PackLimits final_index = pcm;
    final_index.final_index = 2;
    PackLimits two_cells = pcm;
    two_cells.cells = 2;
    PackLimits five_cells = pcm;
    five_cells.cells = 5;
    PackLimits long_target = pcm;
    long_target.target_cells = 10;
    // Frame 0 is at T0: slack 49,999 and 50,000
    PackLimits nearly_late = pcm;
    nearly_late.now_us = T0 - 49999;
    PackLimits solo_edge = pcm;
    solo_edge.now_us = T0 - 50000;
    PackLimits od_late = pcm;
    od_late.now_us = T0 - 60000;
    od_late.output_delay_us = 10001;

    std::vector<FrameView> gap = frames(0, 2, 100);
    gap.push_back({3, T0 + 60 * MS, 20 * MS, 100});
    // The timeline shifts by 30 ms from frame 2 on
    std::vector<FrameView> shifted = frames(0, 6, 100);
    for (size_t i = 2; i < shifted.size(); ++i) {
        shifted[i].ts_us += 30 * MS;
    }

    struct Row {
        const char* name;
        std::vector<FrameView> frames;
        PackLimits limits;
        size_t n;
    };
    const Row rows[] = {
        // B's chunks are 5 frames of 2,000 B
        {"Control: B's 5 frames", frames(0, 6, 2000), pcm, 5},
        // 3,528 B frames under a 16,384 B cap
        {"cap-limited (A: 4)", frames(0, 6, 3528), pcm, 4},
        // Room 11,974
        {"room-limited (d: 3)", frames(0, 6, 3528), room_d, 3},
        {"exact fit bytes == limit", frames(0, 6, 3528), exact, 3},
        {"n_target-limited", frames(0, 7, 100), pcm, 5},
        {"Opus takes one frame", frames(0, 3, 100), opus, 1},
        {"slack below S_solo goes alone", frames(0, 6, 100), nearly_late, 1},
        {"slack at S_solo does not", frames(0, 6, 100), solo_edge, 5},
        {"od counts against the slack", frames(0, 6, 100), od_late, 1},
        {"stops at a switch point", frames(0, 6, 100), switching, 2},
        {"stops after the final index", frames(0, 6, 100), final_index, 3},
        {"a missing frame mid-run", gap, pcm, 2},
        {"a timeline shift ends the chunk", shifted, pcm, 2},
        {"m = 2 packs two 40 ms frames", frames(0, 4, 100, 2), two_cells, 2},
        {"m = 2 steps k by 2", frames(0, 4, 100, 1), two_cells, 1},
        {"m = 5 packs one", frames(0, 3, 100, 5), five_cells, 1},
        {"never past 150 ms", frames(0, 10, 100), long_target, 7},
        {"no frames", {}, pcm, 0},
        {"the first frame does not fit the room", frames(0, 2, 3528),
         [&] {
             PackLimits small = pcm;
             small.room_bytes = 3540;
             return small;
         }(),
         0},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sched::pack_chunk(row.frames, row.limits), row.n);
    }
}

// A113 packs k452 and the short final k453 into 5,357 B, 30.295 ms
TEST(SchedChunk, DrainedEpochFinalFramePacksWithEarlierOnes) {
    std::vector<FrameView> tail = {{452, 21600 * MS, 20 * MS, 3528},
                                   {453, 21620 * MS, 10295, 1816}};
    PackLimits limits;
    limits.message_cap = 16384;
    limits.room_bytes = 100000;
    limits.now_us = 21120 * MS;
    limits.final_index = 453;
    ASSERT_EQ(sched::pack_chunk(tail, limits), 2U);
    EXPECT_EQ(13 + tail[0].payload_bytes + tail[1].payload_bytes, 5357);
    EXPECT_EQ(tail[0].duration_us + tail[1].duration_us, 30295);
}

// A chunk short of n_max for want of frames or room waits for them while its first frame's slack
// exceeds the player's S_pack (160 ms, or a full chunk and M_pump past its min_buffer_ms when that
// is more, and at least 500 ms for a buffered input), until ts - od - S_pack; one whole in itself
// (n_max, the message cap, the final index, the switch point, Opus) goes now, as does one with
// slack at or under S_pack. Frame 0 is at T0 = 1 s; now is 0 unless a row says otherwise.
TEST(SchedChunk, HoldsAShortChunkWhileItHasTheSlack) {
    // S_pack = max(S_solo, mb) + n_target * F + M_pump: 160 ms at the defaults, a full chunk past
    // the player's min_buffer_ms when that is more, and the configured M_pump; a buffered input's
    // player waits to S_buf, 500 ms, at the least
    constexpr auto LIVE = sendspin::server::InputKind::LIVE;
    constexpr auto BUFFERED = sendspin::server::InputKind::BUFFERED;
    EXPECT_EQ(sched::PACK_HOLD_SLACK_US, 160 * MS);
    EXPECT_EQ(sched::pack_hold_slack_us(0, 10 * MS, LIVE), 160 * MS);
    EXPECT_EQ(sched::pack_hold_slack_us(50 * MS, 10 * MS, LIVE), 160 * MS);
    EXPECT_EQ(sched::pack_hold_slack_us(150 * MS, 10 * MS, LIVE), 260 * MS);
    EXPECT_EQ(sched::pack_hold_slack_us(500 * MS, 10 * MS, LIVE), 610 * MS);
    EXPECT_EQ(sched::pack_hold_slack_us(0, 20 * MS, LIVE), 170 * MS);
    // Control: S_buf for a buffered input's player, whose own S_pack is less
    EXPECT_EQ(sched::BUFFERED_HOLD_SLACK_US, 500 * MS);
    EXPECT_EQ(sched::pack_hold_slack_us(150 * MS, 10 * MS, BUFFERED), 500 * MS);
    EXPECT_EQ(sched::pack_hold_slack_us(390 * MS, 10 * MS, BUFFERED), 500 * MS);
    EXPECT_EQ(sched::pack_hold_slack_us(500 * MS, 10 * MS, BUFFERED), 610 * MS);
    PackLimits pcm;
    pcm.message_cap = 16384;
    pcm.room_bytes = 100000;
    const auto with = [&](auto change) {
        PackLimits limits = pcm;
        change(limits);
        return limits;
    };
    const int64_t hold = T0 - sched::PACK_HOLD_SLACK_US;

    struct Row {
        const char* name;
        std::vector<FrameView> frames;
        PackLimits limits;
        int64_t until;
        /// The message's bytes with the next frame, for a stop at the room
        int64_t bytes_with_next;
    };
    const Row rows[] = {
        {"Control: five frames go now", frames(0, 6, 2000), pcm, sched::NEVER, 0},
        {"two frames produced wait for more", frames(0, 2, 2000), pcm, hold, 0},
        {"room for three waits for room", frames(0, 6, 3528),
         with([](PackLimits& l) { l.room_bytes = 11974; }), hold, 13 + (4 * 3528)},
        {"the cap allows four: whole", frames(0, 6, 3528), pcm, sched::NEVER, 0},
        {"at the final index: whole", frames(0, 3, 100),
         with([](PackLimits& l) { l.final_index = 2; }), sched::NEVER, 0},
        {"a final index further on still waits", frames(0, 3, 100),
         with([](PackLimits& l) { l.final_index = 9; }), hold, 0},
        {"at a timeline shift: whole",
         [] {
             std::vector<FrameView> shifted = frames(0, 3, 100);
             shifted[2].ts_us += 30 * MS;
             return shifted;
         }(),
         pcm, sched::NEVER, 0},
        {"at the switch point: whole", frames(0, 2, 100),
         with([](PackLimits& l) { l.switch_point = 2; }), sched::NEVER, 0},
        {"Opus takes one packet: whole", frames(0, 1, 100),
         with([](PackLimits& l) { l.codec = Codec::OPUS; }), sched::NEVER, 0},
        {"slack S_pack + 1 waits 1 us", frames(0, 2, 100),
         with([](PackLimits& l) { l.now_us = T0 - sched::PACK_HOLD_SLACK_US - 1; }),
         T0 - sched::PACK_HOLD_SLACK_US, 0},
        {"slack S_pack goes now", frames(0, 2, 100),
         with([](PackLimits& l) { l.now_us = T0 - sched::PACK_HOLD_SLACK_US; }), sched::NEVER, 0},
        {"a min_buffer_ms of 500 ms waits only to 610 ms of slack", frames(0, 2, 100),
         with([](PackLimits& l) {
             l.hold_slack_us = sched::pack_hold_slack_us(500 * MS, 10 * MS, LIVE);
         }),
         T0 - (610 * MS), 0},
        {"slack at a 500 ms player's S_pack goes now", frames(0, 2, 100), with([](PackLimits& l) {
             l.hold_slack_us = 610 * MS;
             l.now_us = T0 - (610 * MS);
         }),
         sched::NEVER, 0},
        {"od counts against the slack", frames(0, 2, 100),
         with([](PackLimits& l) { l.output_delay_us = 300 * MS; }), hold - (300 * MS), 0},
        {"a first frame that does not fit is blocked, not held", frames(0, 2, 3528),
         with([](PackLimits& l) { l.room_bytes = 3540; }), sched::NEVER, 13 + 3528},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const sched::PackPlan plan = sched::plan_chunk(row.frames, row.limits);
        EXPECT_EQ(plan.frames, sched::pack_chunk(row.frames, row.limits));
        EXPECT_EQ(plan.bytes_with_next, row.bytes_with_next);
        EXPECT_EQ(sched::pack_hold_until(plan, row.frames.front().ts_us, row.limits), row.until);
    }
}

}  // namespace
