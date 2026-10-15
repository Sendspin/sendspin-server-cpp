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

/// @file test_visualizer_stream.cpp
/// @brief One session's visualizer stream over a fake group: the cursor's order and skips, the
/// ledger's pacing (spec roles/visualizer/v1.md "client/hello visualizer@v1 support object") and
/// a configuration change in place.

#include "fragmentation.h"
#include "visualizer_stream.h"
#include "wire_messages.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using sendspin::server::ClientState;
using sendspin::server::OutboundPlaintext;
using sendspin::server::StreamSource;
using sendspin::server::VisualizerConfig;
using sendspin::server::VisualizerRequest;
using sendspin::server::VisualizerStream;
using sendspin::server::VisualizerSupport;
namespace sched = sendspin::server::sched;
namespace type = sendspin::server::visualizer_type;
namespace wire = sendspin::server::test::wire;

constexpr int64_t T0 = 1000000;
constexpr sched::EpochId EPOCH{1};
constexpr int64_t CELLS = 20;
const VisualizerRequest LOUDNESS{type::LOUDNESS, 50, std::nullopt};
const VisualizerRequest F_PEAK{type::F_PEAK, 50, std::nullopt};
/// Onsets only: cells with no message while nothing fires
const VisualizerRequest PEAK{type::PEAK, 50, std::nullopt};
/// A capacity every message here fits, so the requests negotiate to their own types
constexpr uint32_t CAPACITY = 1000;

/// One message of type at ts: its type byte, its timestamp and zero data
void append_message(std::vector<uint8_t>& out, uint8_t id, size_t bytes, int64_t ts) {
    out.push_back(id);
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<uint8_t>(static_cast<uint64_t>(ts) >> shift));
    }
    out.resize(out.size() + bytes - 9, 0);
}

/// A playing group with one ring per configuration: loudness (11 bytes) at each cell's start,
/// f_peak (13 bytes) 10 ms into it
class FakeGroup : public StreamSource {
public:
    FakeGroup() : anchor_(sched::make_anchor(T0, 0, 48000, 2)) {
        for (int64_t cell = 0; cell < CELLS; ++cell) {
            this->append(this->loudness_, cell, 16, 11, 0);
            this->append(this->f_peak_, cell, 18, 13, 10000);
        }
    }

    Playback playback() const override {
        Playback playback;
        playback.playing = true;
        playback.run = sched::RunId{1};
        playback.epoch = EPOCH;
        playback.anchor = &this->anchor_;
        playback.draining = this->last_cell.has_value();
        playback.last_cell = this->last_cell;
        playback.group_lead_us = this->group_lead_us;
        return playback;
    }

    const sched::FrameRing* ring_for(const sched::AudioFormat& /*format*/) const override {
        return nullptr;
    }

    const sched::FrameRing* visualizer_ring(uint64_t key) const override {
        const auto key_of = [](const VisualizerRequest& request) {
            return sendspin::server::config_key(
                sendspin::server::negotiate_visualizer(request, CAPACITY));
        };
        if (key == key_of(LOUDNESS)) {
            return &this->loudness_;
        }
        if (key == key_of(PEAK)) {
            return &this->peak_;
        }
        return key == key_of(F_PEAK) && this->f_peak_ready ? &this->f_peak_ : nullptr;
    }

    /// The onset-only ring's cells 0 to cells - 1, none with a message
    void fill_peak(int64_t cells) {
        this->peak_.restart(EPOCH, 0);
        for (int64_t cell = 0; cell < cells; ++cell) {
            sched::Frame frame;
            frame.epoch = EPOCH;
            frame.index = cell;
            frame.ts_us = T0 + (cell * sched::CELL_US);
            EXPECT_TRUE(this->peak_.append(std::move(frame), 0));
        }
    }

    void on_stream_changed() override {}

    bool f_peak_ready{false};
    /// Set: the epoch drains, its last input sample in this cell
    std::optional<int64_t> last_cell;
    /// `G`; 0, below `J_min`, unless a test sets it
    int64_t group_lead_us{0};

private:
    static void append(sched::FrameRing& ring, int64_t cell, uint8_t id, size_t bytes,
                       int64_t offset_us) {
        if (cell == 0) {
            ring.restart(EPOCH, 0);
        }
        sched::Frame frame;
        frame.epoch = EPOCH;
        frame.index = cell;
        frame.ts_us = T0 + (cell * sched::CELL_US) + offset_us;
        append_message(frame.payload, id, bytes, frame.ts_us);
        EXPECT_TRUE(ring.append(std::move(frame), 0));
    }

    sched::Anchor anchor_;
    sched::FrameRing loudness_{sched::InstanceId{1}, 1};
    sched::FrameRing f_peak_{sched::InstanceId{2}, 1};
    sched::FrameRing peak_{sched::InstanceId{3}, 1};
};

/// A visualizer stream of capacity, its role written and its request in, started at stamp_us by a
/// trigger of kind
VisualizerStream started(FakeGroup& group, uint32_t capacity, int64_t stamp_us,
                         sched::TriggerKind kind = sched::TriggerKind::GROUP) {
    VisualizerStream stream;
    stream.set_support(VisualizerSupport{capacity});
    stream.on_activation_written(true);
    ClientState state;
    state.available = true;
    state.visualizer = LOUDNESS;
    stream.on_client_state(state);
    stream.set_pending_kind(kind);
    const sched::Want want =
        stream.want(&group, sched::RoleSet::of(sched::StreamRole::VISUALIZER), stamp_us);
    EXPECT_EQ(sched::reconcile_stream(stream.sent(), want, stream.preconditions()),
              sched::StreamAction::START);
    stream.commit_marker(&group, sched::StreamAction::START, want, stamp_us);
    return stream;
}

int64_t ts_of(const OutboundPlaintext& message) {
    return wire::timestamp_at(message.bytes);
}

/// One message taken: its timestamp and when
struct Taken {
    int64_t ts{0};
    int64_t at{0};
};

/// Runs the pump's steps with a simulated clock from now_us until the ring is spent
std::vector<Taken> drain(VisualizerStream& stream, FakeGroup& group, int64_t now_us) {
    std::vector<Taken> taken;
    for (int step = 0; step < 1000; ++step) {
        stream.prepare(&group, 0, now_us);
        const sched::AudioCandidate candidate = stream.candidate(&group, now_us);
        if (candidate.ready) {
            const OutboundPlaintext message = stream.take_message(&group);
            EXPECT_EQ(ts_of(message), candidate.deadline_us);
            taken.push_back({ts_of(message), now_us});
            continue;
        }
        if (candidate.room_time_us == sched::NEVER) {
            break;
        }
        EXPECT_GT(candidate.room_time_us, now_us);
        now_us = candidate.room_time_us;
    }
    return taken;
}

// From a start, every message with `ts` after it, once, in order; a message waits while the count
// of those sent with `ts` still ahead and its own size would pass the capacity, until the oldest of
// them passes its time
TEST(VisualizerStream, SendsEachMessageOnceWithinTheLedger) {
    FakeGroup group;
    // Two loudness messages
    VisualizerStream stream = started(group, 22, T0 - 1000000);
    const std::vector<Taken> taken = drain(stream, group, T0 - 1000000);
    ASSERT_EQ(taken.size(), static_cast<size_t>(CELLS));
    for (size_t i = 0; i < taken.size(); ++i) {
        EXPECT_EQ(taken[i].ts, T0 + (static_cast<int64_t>(i) * sched::CELL_US));
        EXPECT_LT(taken[i].at, taken[i].ts) << i;
        if (i >= 2) {
            // The one two before had to pass its time first, and that is when it went
            EXPECT_EQ(taken[i].at, taken[i - 2].ts) << i;
        } else {
            EXPECT_EQ(taken[i].at, T0 - 1000000) << i;
        }
    }
}

// A message has room for the tag its encryption appends, so that the encryption does not reallocate
// it
TEST(VisualizerStream, LeavesRoomForTheTransportTag) {
    FakeGroup group;
    VisualizerStream stream = started(group, CAPACITY, T0 - 1000000);
    stream.prepare(&group, 0, T0 - 1000000);
    ASSERT_TRUE(stream.candidate(&group, T0 - 1000000).ready);
    const OutboundPlaintext message = stream.take_message(&group);
    ASSERT_FALSE(message.bytes.empty());
    EXPECT_GE(message.bytes.capacity() - message.bytes.size(),
              sendspin::server::TRANSPORT_TAG_BYTES);
}

// A visualizer starts while its first message, its startup lead past now, is still at or before the
// last cell's start; one microsecond later it does not. The startup lead of the join here is
// `J_min`, or the group lead above it.
TEST(VisualizerStream, StartsInADrainWhileItsStartupLeadStillReachesTheLastCell) {
    constexpr int64_t J_MIN = sched::timing_defaults::JOIN_MIN_US;
    constexpr int64_t G = 150000;
    // The last cell, 10, starts at T0 + 200 ms
    constexpr int64_t LAST_TS = T0 + (10 * sched::CELL_US);
    struct Row {
        const char* name;
        int64_t group_lead;
        int64_t now;
        sched::StreamAction action;
    };
    const Row rows[] = {
        {"Control: J_min before the last cell", 0, LAST_TS - J_MIN, sched::StreamAction::START},
        {"one microsecond later", 0, LAST_TS - J_MIN + 1, sched::StreamAction::NONE},
        {"the group lead before the last cell", G, LAST_TS - G, sched::StreamAction::START},
        {"one microsecond later than the group lead", G, LAST_TS - G + 1,
         sched::StreamAction::NONE},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        FakeGroup group;
        group.last_cell = 10;
        group.group_lead_us = row.group_lead;
        VisualizerStream stream;
        stream.set_support(VisualizerSupport{CAPACITY});
        stream.on_activation_written(true);
        ClientState state;
        state.available = true;
        state.visualizer = LOUDNESS;
        stream.on_client_state(state);
        stream.set_pending_kind(sched::TriggerKind::JOIN);
        const sched::Want want =
            stream.want(&group, sched::RoleSet::of(sched::StreamRole::VISUALIZER), row.now);
        EXPECT_EQ(sched::reconcile_stream(stream.sent(), want, stream.preconditions()), row.action);
    }
}

// A stream's first message is the first at least its startup lead past its start's stamp: `J_min`
// after a group trigger, and after a join the group lead `G` if more; a message whose time passed
// is skipped. The loudness messages are 20 ms apart from T0.
TEST(VisualizerStream, StartsItsStartupLeadPastItsStampAndSkipsLateMessages) {
    constexpr int64_t J_MIN = sched::timing_defaults::JOIN_MIN_US;
    static_assert(J_MIN == 100000);
    struct Row {
        const char* name;
        int64_t stamp;
        sched::TriggerKind kind;
        int64_t now;
        int64_t first_ts;
        int64_t group_lead{0};
    };
    constexpr auto GROUP = sched::TriggerKind::GROUP;
    constexpr auto JOIN = sched::TriggerKind::JOIN;
    const Row rows[] = {
        {"Control: a start well before the first message", T0 - 1000000, GROUP, T0 - 1000000, T0},
        {"a group start J_min before the first message", T0 - J_MIN, GROUP, T0 - J_MIN, T0},
        {"a group start inside the messages", T0 + 50000, GROUP, T0 + 50000, T0 + 160000},
        {"a join inside the messages", T0 + 50000, JOIN, T0 + 50000, T0 + 160000},
        {"a floor on a message takes it", T0 + 40000, JOIN, T0 + 40000, T0 + 140000},
        {"one microsecond past it does not", T0 + 40001, JOIN, T0 + 40001, T0 + 160000},
        {"a clock past the floor: only messages after it", T0, GROUP, T0 + 200000, T0 + 220000},
        {"a group lead below J_min", T0 + 40000, JOIN, T0 + 40000, T0 + 140000, 60000},
        {"a group lead above J_min", T0 + 40000, JOIN, T0 + 40000, T0 + 240000, 200000},
        {"a group trigger: J_min whatever the group lead", T0 + 40000, GROUP, T0 + 40000,
         T0 + 140000, 200000},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        FakeGroup group;
        group.group_lead_us = row.group_lead;
        VisualizerStream stream = started(group, CAPACITY, row.stamp, row.kind);
        const std::vector<Taken> taken = drain(stream, group, row.now);
        ASSERT_FALSE(taken.empty());
        EXPECT_EQ(taken.front().ts, row.first_ts);
    }
}

// spec roles/visualizer/v1.md "client/state visualizer object" and "Visualizer messages": a
// changed configuration goes out as a stream/start in place once the new messages reach past the
// last one sent, with no reset of the count, and timestamps go on from there
TEST(VisualizerStream, ChangesItsConfigurationInPlaceWithoutGoingBack) {
    FakeGroup group;
    VisualizerStream stream = started(group, 1000, T0 - 1000000);
    int64_t now = T0 - 1000000;
    std::vector<int64_t> sent;
    for (int i = 0; i < 5; ++i) {
        stream.prepare(&group, 0, now);
        ASSERT_TRUE(stream.candidate(&group, now).ready);
        sent.push_back(ts_of(stream.take_message(&group)));
    }
    ClientState state;
    state.available = true;
    state.visualizer = F_PEAK;
    EXPECT_FALSE(stream.on_client_state(state));
    stream.prepare(&group, 0, now);
    // Its analysis has no ring yet: nothing goes, neither old messages nor the start
    EXPECT_FALSE(stream.candidate(&group, now).ready);
    EXPECT_FALSE(stream.announce_due(&group));
    group.f_peak_ready = true;
    stream.prepare(&group, 0, now);
    ASSERT_TRUE(stream.candidate(&group, now).ready);
    ASSERT_TRUE(stream.announce_due(&group));
    const std::optional<OutboundPlaintext> start = stream.commit_announce(&group, now);
    ASSERT_TRUE(start);
    const std::string json(start->bytes.begin() + 1, start->bytes.end());
    EXPECT_NE(json.find(R"("visualizer":{"types":["f_peak"],"rate_max":50})"), std::string::npos)
        << json;
    EXPECT_EQ(json.find("player"), std::string::npos);
    stream.prepare(&group, 0, now);
    ASSERT_TRUE(stream.candidate(&group, now).ready);
    const OutboundPlaintext next = stream.take_message(&group);
    EXPECT_EQ(next.bytes.front(), 18);
    // The first f_peak after the last loudness, 10 ms into its cell
    EXPECT_EQ(ts_of(next), sent.back() + 10000);
}

// spec roles/visualizer/v1.md "client/state visualizer object": a changed configuration gets its
// stream/start even when its cells carry no message (onsets only, none firing), once its analysis
// holds a cell past the last message sent; the client is never left with the old configuration
TEST(VisualizerStream, AnnouncesAConfigurationWithNoMessagesYet) {
    FakeGroup group;
    VisualizerStream stream = started(group, 1000, T0 - 1000000);
    const int64_t now = T0 - 1000000;
    for (int i = 0; i < 5; ++i) {
        stream.prepare(&group, 0, now);
        ASSERT_TRUE(stream.candidate(&group, now).ready);
        stream.take_message(&group);
    }
    ClientState state;
    state.available = true;
    state.visualizer = PEAK;
    stream.on_client_state(state);
    // Control: its cells reach only the last message sent, cell 4's
    group.fill_peak(5);
    stream.prepare(&group, 0, now);
    EXPECT_FALSE(stream.announce_due(&group));
    EXPECT_FALSE(stream.candidate(&group, now).ready);
    group.fill_peak(6);
    stream.prepare(&group, 0, now);
    ASSERT_TRUE(stream.announce_due(&group));
    ASSERT_TRUE(stream.candidate(&group, now).ready);
    const std::optional<OutboundPlaintext> start = stream.commit_announce(&group, now);
    ASSERT_TRUE(start);
    const std::string json(start->bytes.begin() + 1, start->bytes.end());
    EXPECT_NE(json.find(R"("visualizer":{"types":["peak"],"rate_max":50})"), std::string::npos)
        << json;
}

}  // namespace
