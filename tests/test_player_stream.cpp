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

/// @file test_player_stream.cpp
/// @brief PlayerStream driven over a fake group with the clock passed in: what its markers and
/// cursor emit around a switch in place, and how long its cursor holds a short chunk

#include "player_stream.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using sendspin::server::AudioFormat;
using sendspin::server::ClientState;
using sendspin::server::InputFormat;
using sendspin::server::PlayerState;
using sendspin::server::PlayerStream;
using sendspin::server::PlayerSupport;
using sendspin::server::SampleFormat;
using sendspin::server::StreamSource;
namespace sched = sendspin::server::sched;

constexpr int64_t T0 = 1000000;
constexpr int64_t START_US = 500000;
constexpr int64_t LAST_CELL = 10;
constexpr size_t PAYLOAD = 100;
constexpr sched::EpochId EPOCH{1};
const sched::AudioFormat PCM16{sched::Codec::PCM, 48000, 2, 16};

/// A group playing a draining epoch whose last input sample is in LAST_CELL, or an epoch still
/// taking input, with one ring per format
class FakeGroup : public StreamSource {
public:
    FakeGroup()
        : anchor_(sched::make_anchor(T0, 0, 48000, 2)),
          ring16_(sched::InstanceId{1}, 1),
          ring24_(sched::InstanceId{2}, 1) {}

    Playback playback() const override {
        Playback playback;
        playback.playing = true;
        playback.run = sched::RunId{1};
        playback.epoch = EPOCH;
        playback.anchor = &this->anchor_;
        playback.draining = this->draining_;
        playback.last_cell = LAST_CELL;
        playback.input = InputFormat{48000, 2, SampleFormat::S16};
        return playback;
    }

    const sched::FrameRing* ring_for(const sched::AudioFormat& format) const override {
        return format == PCM16 ? &this->ring16_ : &this->ring24_;
    }

    const sched::FrameRing* visualizer_ring(uint64_t /*key*/) const override {
        return nullptr;
    }

    void on_stream_changed() override {}

    /// Restarts ring at first and appends frames from first through last; -1 for none. The last
    /// is the instance's final frame unless the epoch goes on.
    static void fill(sched::FrameRing& ring, int64_t first, int64_t last, bool final = true) {
        ring.restart(EPOCH, first);
        for (int64_t k = first; k <= last; ++k) {
            sched::Frame frame;
            frame.epoch = EPOCH;
            frame.index = k;
            frame.ts_us = T0 + (k * sched::CELL_US);
            frame.duration_us = sched::CELL_US;
            frame.samples = 960;
            frame.payload.assign(PAYLOAD, 0);
            EXPECT_TRUE(ring.append(std::move(frame), START_US));
        }
        if (final) {
            ring.set_final_index(last);
        }
    }

    sched::Anchor anchor_;
    sched::FrameRing ring16_;
    sched::FrameRing ring24_;
    bool draining_{true};
};

/// What a marker at the head emits for the player role alone, and with which want
struct MarkerPlan {
    sched::StreamAction action{sched::StreamAction::NONE};
    sched::Want want;
};

MarkerPlan plan_marker(const PlayerStream& stream, const StreamSource* group, sched::RoleSet roles,
                       int64_t now_us) {
    MarkerPlan plan;
    plan.want = stream.want(group, roles, now_us);
    plan.action = sched::reconcile_stream(stream.sent(), plan.want, stream.preconditions());
    return plan;
}

/// What one stream sent: the cell of each chunk's frames and its format's bit depth, in-place
/// starts, and whether a stream/end followed
struct Sent {
    std::vector<std::pair<int64_t, uint32_t>> cells;
    size_t in_place{0};
    bool ended{false};
};

/// Starts a 16-bit stream on group, has the operator set 24 bits, then runs the pump's steps with
/// a simulated clock until the stream ends, for at most a few seconds of it
Sent run(FakeGroup& group) {
    PlayerStream stream;
    PlayerSupport support;
    support.supported_formats = {{sendspin::server::Codec::PCM, 2, 48000, 16},
                                 {sendspin::server::Codec::PCM, 2, 48000, 24}};
    support.buffer_capacity = 1000000;
    stream.set_support(support);
    stream.on_activation_written(true);
    ClientState state;
    state.available = true;
    state.player = PlayerState{};
    stream.on_client_state(state);
    stream.set_pending_kind(sched::TriggerKind::JOIN);
    const sched::RoleSet roles = sched::RoleSet::of(sched::StreamRole::PLAYER);

    Sent sent;
    MarkerPlan plan = plan_marker(stream, &group, roles, START_US);
    EXPECT_EQ(plan.action, sched::StreamAction::START);
    stream.commit_marker(&group, plan.action, plan.want, START_US);
    stream.set_format_override(AudioFormat{sendspin::server::Codec::PCM, 2, 48000, 24});

    int64_t now = START_US;
    for (int step = 0; step < 500 && !sent.ended; ++step) {
        if (stream.prepare(&group, 0, now)) {
            // The drain watch's marker
            plan = plan_marker(stream, &group, roles, now);
            sent.ended = plan.action == sched::StreamAction::END;
            stream.commit_marker(&group, plan.action, plan.want, now);
            continue;
        }
        const sched::AudioCandidate candidate = stream.candidate(&group, 16384, now);
        if (!candidate.ready) {
            const int64_t wake = std::min(candidate.room_time_us, stream.drain_watch_us(&group, 0));
            now = wake == sched::NEVER ? now + sched::CELL_US : std::max(wake, now + 1);
            continue;
        }
        if (stream.announce_due(&group)) {
            EXPECT_TRUE(stream.commit_announce(&group, now).has_value());
            ++sent.in_place;
            continue;
        }
        const uint32_t depth = stream.cursor_format().bit_depth;
        const sendspin::server::PackedChunk chunk = stream.take_chunk(&group, 16384, now);
        EXPECT_FALSE(chunk.plaintext.bytes.empty());
        const size_t frames = (chunk.plaintext.bytes.size() - sched::CHUNK_HEADER_BYTES) / PAYLOAD;
        for (size_t i = 0; i < frames; ++i) {
            sent.cells.emplace_back(((chunk.ts_us - T0) / sched::CELL_US) + static_cast<int64_t>(i),
                                    depth);
        }
    }
    return sent;
}

// A switch planned in a draining epoch whose target instance ends below the switch point (it
// started past the drain position, so its final index is -1) never happens: the stream plays its
// own format to its last frame, then ends, with no in-place stream/start. Control: a target that
// holds the frames from the switch point on takes the stream there.
TEST(PlayerStreamSwitch, EndsInItsOwnFormatWhenTheTargetEndsBelowTheSwitchPoint) {
    struct Row {
        const char* name;
        int64_t target_first;
        int64_t target_last;
        size_t in_place;
        int64_t first_wide;
    };
    const Row rows[] = {
        {"the target ends below the switch point", 9, -1, 0, LAST_CELL + 1},
        {"Control: the target holds the switch point", 9, LAST_CELL, 1, 9},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        FakeGroup group;
        FakeGroup::fill(group.ring16_, 0, LAST_CELL);
        FakeGroup::fill(group.ring24_, row.target_first, row.target_last);
        const Sent sent = run(group);
        EXPECT_TRUE(sent.ended);
        EXPECT_EQ(sent.in_place, row.in_place);
        // Every cell once, in order, through the last one
        ASSERT_EQ(sent.cells.size(), static_cast<size_t>(LAST_CELL + 1));
        for (size_t i = 0; i < sent.cells.size(); ++i) {
            EXPECT_EQ(sent.cells[i].first, static_cast<int64_t>(i));
            EXPECT_EQ(sent.cells[i].second, static_cast<int64_t>(i) < row.first_wide ? 16U : 24U)
                << i;
        }
    }
}

// A chunk held for the ledger's room wakes the pump when the ledger has room for one more frame,
// not only once its slack falls to S_pack. Eight 5-frame chunks of 100-byte frames fill the ledger
// but for two frames' room (a capacity that still holds one real 48 kHz frame, so the format is
// producible); the ninth chunk waits, and its wake is the first chunk's completion, 200 ms before
// its own S_pack (S_buf, 500 ms, for this buffered input).
TEST(PlayerStreamPacking, WakesAChunkHeldForRoomWhenTheLedgerHasIt) {
    constexpr int64_t CHUNK_BYTES = sched::CHUNK_HEADER_BYTES + (5 * PAYLOAD);
    FakeGroup group;
    FakeGroup::fill(group.ring16_, 0, 60);
    PlayerStream stream;
    PlayerSupport support;
    support.supported_formats = {{sendspin::server::Codec::PCM, 2, 48000, 16}};
    support.buffer_capacity =
        static_cast<uint32_t>((8 * CHUNK_BYTES) + sched::CHUNK_HEADER_BYTES + (2 * PAYLOAD));
    stream.set_support(support);
    stream.on_activation_written(true);
    ClientState state;
    state.available = true;
    state.player = PlayerState{};
    stream.on_client_state(state);
    stream.set_pending_kind(sched::TriggerKind::JOIN);
    const sched::RoleSet roles = sched::RoleSet::of(sched::StreamRole::PLAYER);
    const MarkerPlan plan = plan_marker(stream, &group, roles, START_US);
    ASSERT_EQ(plan.action, sched::StreamAction::START);
    stream.commit_marker(&group, plan.action, plan.want, START_US);

    for (int64_t chunk = 0; chunk < 8; ++chunk) {
        stream.prepare(&group, 0, START_US);
        ASSERT_TRUE(stream.candidate(&group, 16384, START_US).ready) << chunk;
        const sendspin::server::PackedChunk taken = stream.take_chunk(&group, 16384, START_US);
        ASSERT_EQ(taken.ts_us, T0 + (chunk * 5 * sched::CELL_US));
        ASSERT_EQ(static_cast<int64_t>(taken.plaintext.bytes.size()), CHUNK_BYTES);
        // The trace's audio_bytes: the five frames' audio, counted apart from the header
        EXPECT_EQ(taken.audio_bytes, 5 * PAYLOAD);
    }
    stream.prepare(&group, 0, START_US);
    const sched::AudioCandidate held = stream.candidate(&group, 16384, START_US);
    EXPECT_FALSE(held.ready);
    // The first chunk completes at the end of its audio, T0 + 100 ms; S_pack is 500 ms before the
    // held chunk's T0 + 800 ms
    EXPECT_EQ(held.room_time_us, T0 + (5 * sched::CELL_US));
    EXPECT_LT(held.room_time_us, T0 + (40 * sched::CELL_US) - sched::BUFFERED_HOLD_SLACK_US);
    // Once it has room, it goes with all five frames
    const int64_t room = held.room_time_us;
    stream.prepare(&group, 0, room);
    ASSERT_TRUE(stream.candidate(&group, 16384, room).ready);
    const sendspin::server::PackedChunk ninth = stream.take_chunk(&group, 16384, room);
    EXPECT_EQ(ninth.ts_us, T0 + (40 * sched::CELL_US));
    EXPECT_EQ(static_cast<int64_t>(ninth.plaintext.bytes.size()), CHUNK_BYTES);
}

// spec player/v1.md "Server Audio Send Constraints": a chunk short of n_target waits for more
// frames only while its first frame keeps S_buf, or the player's min_buffer_ms with five cells and
// `M_pump` to spare when that is more (pack_hold_slack_us()), from the client/state the player
// reported. Two of five frames are produced, 800 ms into a buffered epoch that goes on.
TEST(PlayerStreamPacking, HoldsAShortChunkNoLaterThanThePlayersMinBuffer) {
    constexpr int64_t FIRST = 40;
    constexpr int64_t MS = 1000;
    struct Row {
        const char* name;
        uint32_t min_buffer_ms;
        int64_t slack_us;
    };
    const Row rows[] = {
        {"Control: a min_buffer_ms under S_buf holds to S_buf", 150, sched::BUFFERED_HOLD_SLACK_US},
        {"a min_buffer_ms over S_buf holds to it", 600,
         (600 * MS) + (sched::TARGET_CHUNK_CELLS * sched::CELL_US) +
             sched::timing_defaults::PUMP_MARGIN_US},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        FakeGroup group;
        group.draining_ = false;
        FakeGroup::fill(group.ring16_, FIRST, FIRST + 1, false);
        PlayerStream stream;
        PlayerSupport support;
        support.supported_formats = {{sendspin::server::Codec::PCM, 2, 48000, 16}};
        support.buffer_capacity = 1000000;
        stream.set_support(support);
        stream.on_activation_written(true);
        ClientState state;
        state.available = true;
        state.player = PlayerState{};
        state.player->min_buffer_ms = row.min_buffer_ms;
        stream.on_client_state(state);
        stream.set_pending_kind(sched::TriggerKind::JOIN);
        const MarkerPlan plan =
            plan_marker(stream, &group, sched::RoleSet::of(sched::StreamRole::PLAYER), START_US);
        ASSERT_EQ(plan.action, sched::StreamAction::START);
        stream.commit_marker(&group, plan.action, plan.want, START_US);

        stream.prepare(&group, 0, START_US);
        const sched::AudioCandidate held = stream.candidate(&group, 16384, START_US);
        EXPECT_FALSE(held.ready);
        EXPECT_EQ(held.room_time_us, T0 + (FIRST * sched::CELL_US) - row.slack_us);
    }
}

}  // namespace
